/**
 * @file fuzz_regression_test.cpp
 * @brief Deterministic Property-Based & Fuzz Regression Tests.
 *
 * This test suite executes randomized state-machine sequences against EventQueue
 * using fixed seeds, verifying that:
 * 1. Random interleavings of RegisterTopic, AddObserver, Post, Dispatch, RemoveObserverById,
 *    and ResolveTopic maintain state invariants without memory corruption or asserts.
 * 2. Unregistered or nonexistent topic resolutions return -1 safely.
 * 3. Clearing topics resets ID lookup without crashing active observer collections.
 * 4. Rapid post/drain cycles handle arbitrary burst patterns.
 */

#include <gtest/gtest.h>

#include "eventqueue.h"
#include "eventqueue_broadcast.h"
#include "eventqueue_fast.h"
#include "eventqueue_batch.h"
#include "eventqueue_emulator.h"
#include "messagecenter_fast.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

class FuzzRegressionTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================================
// Deterministic Pseudo-Random State Machine Test
// ============================================================================

TEST_F(FuzzRegressionTest, RandomizedEventQueueStateMachine) {
    EventQueueCUT queue;
    queue.init();

    // Use deterministic seed for reproducible execution across test runs
    std::mt19937 rng(1337);
    std::uniform_int_distribution<int> opDist(0, 5);
    std::uniform_int_distribution<int> topicDist(0, 15);

    std::vector<std::string> topicNames;
    for (int i = 0; i < 16; i++) {
        topicNames.push_back("fuzz_topic_" + std::to_string(i));
    }

    struct ActiveObserver {
        std::string topic;
        uint64_t id;
    };
    std::vector<ActiveObserver> activeObservers;
    std::atomic<uint64_t> deliveryCount{0};

    const int NUM_STEPS = 5000;

    for (int step = 0; step < NUM_STEPS; step++) {
        int op = opDist(rng);
        int tIdx = topicDist(rng);
        const std::string& topic = topicNames[tIdx];

        switch (op) {
            case 0: { // RegisterTopic
                int tid = queue.RegisterTopic(topic);
                EXPECT_GE(tid, 0);
                break;
            }
            case 1: { // ResolveTopic
                int tid = queue.ResolveTopic(topic);
                // Valid topic ID or -1 if not yet registered
                EXPECT_TRUE(tid >= -1);
                break;
            }
            case 2: { // AddObserver (lambda)
                uint64_t obsId = queue.AddObserver(topic, ObserverCallbackFunc([&deliveryCount](int, Message*) {
                    deliveryCount.fetch_add(1, std::memory_order_relaxed);
                }));
                EXPECT_NE(obsId, 0u);
                activeObservers.push_back({topic, obsId});
                break;
            }
            case 3: { // RemoveObserverById
                if (!activeObservers.empty()) {
                    std::uniform_int_distribution<size_t> obsIndexDist(0, activeObservers.size() - 1);
                    size_t idx = obsIndexDist(rng);
                    queue.RemoveObserverById(activeObservers[idx].topic, activeObservers[idx].id);
                    activeObservers.erase(activeObservers.begin() + idx);
                }
                break;
            }
            case 4: { // Post
                queue.Post(topic, nullptr, false);
                break;
            }
            case 5: { // Drain a few messages
                for (int d = 0; d < 3; d++) {
                    Message* m = queue.GetQueueMessage();
                    if (m != nullptr) {
                        queue.Dispatch(m->tid, m);
                    }
                }
                break;
            }
        }
    }

    // Drain all remaining queued messages
    while (Message* m = queue.GetQueueMessage()) {
        queue.Dispatch(m->tid, m);
    }

    // Clean up all remaining active observers
    for (const auto& obs : activeObservers) {
        queue.RemoveObserverById(obs.topic, obs.id);
    }

    queue.dispose();
}

/**
 * Test: TopicResolutionBoundaryFuzz
 * Tests edge case strings for RegisterTopic and ResolveTopic.
 */
TEST_F(FuzzRegressionTest, TopicResolutionBoundaryFuzz) {
    EventQueue queue;
    queue.init();

    // Empty string
    EXPECT_EQ(queue.ResolveTopic(""), -1);
    EXPECT_EQ(queue.RegisterTopic(""), -1);

    // Single character
    int tidA = queue.RegisterTopic("a");
    EXPECT_GE(tidA, 0);
    EXPECT_EQ(queue.ResolveTopic("a"), tidA);

    // Very long string
    std::string longTopic(500, 'z');
    int tidLong = queue.RegisterTopic(longTopic);
    EXPECT_GE(tidLong, 0);
    EXPECT_EQ(queue.ResolveTopic(longTopic), tidLong);

    // Special characters
    std::string specialTopic = "topic/with/slashes.and:colons#hashes@at!exclamation";
    int tidSpecial = queue.RegisterTopic(specialTopic);
    EXPECT_GE(tidSpecial, 0);
    EXPECT_EQ(queue.ResolveTopic(specialTopic), tidSpecial);

    queue.dispose();
}

/**
 * Test: ClearTopicsAndReRegister
 * Validates that ClearTopics resets the topic map without crashing when
 * re-registering topics.
 */
TEST_F(FuzzRegressionTest, ClearTopicsAndReRegister) {
    EventQueue queue;
    queue.init();

    for (int i = 0; i < 50; i++) {
        int id = queue.RegisterTopic("topic_" + std::to_string(i));
        EXPECT_EQ(id, i);
    }

    queue.ClearTopics();

    // After ClearTopics, previously registered topics should no longer resolve
    EXPECT_EQ(queue.ResolveTopic("topic_0"), -1);

    // Re-register: topic numbering should reset to 0
    int newId = queue.RegisterTopic("topic_0");
    EXPECT_EQ(newId, 0);

    queue.dispose();
}

/**
 * Test: RandomizedBroadcastQueueStateMachine
 * Fuzzes EventQueueBroadcast with mixed inline/managed payloads, multiple observers,
 * dynamic registrations, and interleaving post/dispatch calls.
 */
TEST_F(FuzzRegressionTest, RandomizedBroadcastQueueStateMachine) {
    EventQueueBroadcast<1024> queue;
    std::mt19937 rng(4242);
    std::uniform_int_distribution<int> opDist(0, 4);
    std::uniform_int_distribution<int> sizeDist(0, 128);

    std::vector<uint16_t> topicIds;
    for (int i = 0; i < 8; i++) {
        topicIds.push_back(queue.registerTopic("bcast_fuzz_" + std::to_string(i)));
    }

    std::atomic<uint64_t> receivedCount{0};
    for (uint16_t tid : topicIds) {
        queue.addObserver(tid, [&receivedCount](uint16_t, const void*, size_t) {
            receivedCount.fetch_add(1, std::memory_order_relaxed);
        });
    }

    char buffer[256];
    std::memset(buffer, 0xAB, sizeof(buffer));

    const int NUM_STEPS = 2000;
    for (int step = 0; step < NUM_STEPS; step++) {
        int op = opDist(rng);
        uint16_t tid = topicIds[step % topicIds.size()];
        size_t sz = static_cast<size_t>(sizeDist(rng));

        switch (op) {
            case 0: // Post inline
                queue.post(tid, buffer, std::min(sz, size_t(48)));
                break;
            case 1: // Post managed (>48 bytes)
                queue.post(tid, buffer, std::max(sz, size_t(49)));
                break;
            case 2: // Post empty
                queue.post(tid);
                break;
            case 3: // Dispatch one
                queue.dispatchOne();
                break;
            case 4: // Dispatch all
                queue.dispatchAll();
                break;
        }
    }
    queue.dispatchAll();
}

/**
 * Test: RandomizedBroadcastFastQueueStateMachine
 * Fuzzes EventQueueBroadcastFast with mixed inline and managed payloads,
 * rapid post cycles, and single/all dispatches.
 */
TEST_F(FuzzRegressionTest, RandomizedBroadcastFastQueueStateMachine) {
    EventQueueBroadcastFast<1024> queue;
    std::mt19937 rng(54321);
    std::uniform_int_distribution<int> opDist(0, 3);
    std::uniform_int_distribution<int> sizeDist(0, 128);

    std::vector<uint16_t> topicIds;
    for (int i = 0; i < 8; i++) {
        topicIds.push_back(queue.registerTopic());
    }

    std::atomic<uint64_t> receivedCount{0};
    for (uint16_t tid : topicIds) {
        queue.addObserver(tid, [](uint16_t, const void*, size_t, void* u) {
            auto* counter = static_cast<std::atomic<uint64_t>*>(u);
            counter->fetch_add(1, std::memory_order_relaxed);
        }, &receivedCount);
    }

    char buffer[256];
    std::memset(buffer, 0xCD, sizeof(buffer));

    const int NUM_STEPS = 2000;
    for (int step = 0; step < NUM_STEPS; step++) {
        int op = opDist(rng);
        uint16_t tid = topicIds[step % topicIds.size()];
        size_t sz = static_cast<size_t>(sizeDist(rng));

        switch (op) {
            case 0:
                queue.post(tid, buffer, std::min(sz, size_t(48)));
                break;
            case 1:
                queue.post(tid, buffer, std::max(sz, size_t(49)));
                break;
            case 2:
                queue.dispatchOne();
                break;
            case 3:
                queue.dispatchAll();
                break;
        }
    }
    queue.dispatchAll();
}

/**
 * Test: RandomizedBroadcastBatchQueueStateMachine
 * Fuzzes EventQueueBroadcastFastBatch with batch flushing and dispatching.
 */
TEST_F(FuzzRegressionTest, RandomizedBroadcastBatchQueueStateMachine) {
    EventQueueBroadcastFastBatch<1024> queue;
    std::mt19937 rng(99999);
    std::uniform_int_distribution<int> opDist(0, 4);
    std::uniform_int_distribution<int> sizeDist(0, 128);

    std::vector<uint16_t> topicIds;
    for (int i = 0; i < 8; i++) {
        topicIds.push_back(queue.registerTopic());
    }

    std::atomic<uint64_t> receivedCount{0};
    for (uint16_t tid : topicIds) {
        queue.addObserver(tid, [](uint16_t, const void*, size_t, void* u) {
            auto* counter = static_cast<std::atomic<uint64_t>*>(u);
            counter->fetch_add(1, std::memory_order_relaxed);
        }, &receivedCount);
    }

    char buffer[256];
    std::memset(buffer, 0xEF, sizeof(buffer));

    const int NUM_STEPS = 2000;
    for (int step = 0; step < NUM_STEPS; step++) {
        int op = opDist(rng);
        uint16_t tid = topicIds[step % topicIds.size()];
        size_t sz = static_cast<size_t>(sizeDist(rng));

        switch (op) {
            case 0:
                queue.post(tid, buffer, std::min(sz, size_t(48)));
                break;
            case 1:
                queue.post(tid, buffer, std::max(sz, size_t(49)));
                break;
            case 2:
                queue.flushBatch();
                break;
            case 3:
                queue.dispatchBatch(16);
                break;
            case 4:
                queue.dispatchAll();
                break;
        }
    }
    queue.flushBatch();
    queue.dispatchAll();
}

/**
 * Test: RandomizedEmulatorDualQueueStateMachine
 * Fuzzes EventQueueEmulator mixed critical fast queue and normal bulk queue.
 */
TEST_F(FuzzRegressionTest, RandomizedEmulatorDualQueueStateMachine) {
    EventQueueEmulator<512, 1024> queue;
    std::mt19937 rng(77711);
    std::uniform_int_distribution<int> opDist(0, 4);
    std::uniform_int_distribution<int> sizeDist(1, 100);

    uint16_t critTopic = queue.registerTopic(TopicPriority::Critical);
    uint16_t bulkTopic = queue.registerTopic(TopicPriority::Normal);

    std::atomic<uint64_t> critCount{0};
    std::atomic<uint64_t> bulkCount{0};

    queue.addObserver(critTopic, [](uint16_t, const void*, size_t, void* u) {
        static_cast<std::atomic<uint64_t>*>(u)->fetch_add(1, std::memory_order_relaxed);
    }, &critCount);

    queue.addObserver(bulkTopic, [](uint16_t, const void*, size_t, void* u) {
        static_cast<std::atomic<uint64_t>*>(u)->fetch_add(1, std::memory_order_relaxed);
    }, &bulkCount);

    char buffer[256];
    std::memset(buffer, 0x5A, sizeof(buffer));

    const int NUM_STEPS = 2000;
    for (int step = 0; step < NUM_STEPS; step++) {
        int op = opDist(rng);
        size_t sz = static_cast<size_t>(sizeDist(rng));

        switch (op) {
            case 0:
                queue.postFast(critTopic, buffer, std::min(sz, size_t(48)));
                break;
            case 1:
                queue.postBulk(bulkTopic, buffer, sz);
                break;
            case 2:
                queue.dispatchFast();
                break;
            case 3:
                queue.dispatchBulk();
                break;
            case 4:
                queue.dispatchPriority();
                break;
        }
    }
    queue.dispatchAll();
}

/**
 * Test: RandomizedMessageCenterFastStateMachine
 * Fuzzes MessageCenterFast with dynamic topics, observers, and payload sizes.
 */
TEST_F(FuzzRegressionTest, RandomizedMessageCenterFastStateMachine) {
    MessageCenterFast mc;
    mc.start();

    std::mt19937 rng(123456);
    std::uniform_int_distribution<int> opDist(0, 2);
    std::uniform_int_distribution<int> sizeDist(1, 200);

    std::vector<uint16_t> topicIds;
    for (int i = 0; i < 8; i++) {
        topicIds.push_back(mc.registerTopic("mc_fast_fuzz_" + std::to_string(i)));
    }

    std::atomic<uint64_t> receivedCount{0};
    for (uint16_t tid : topicIds) {
        mc.addObserver(tid, [&receivedCount](uint16_t, const void*, size_t) {
            receivedCount.fetch_add(1, std::memory_order_relaxed);
        });
    }

    char buffer[512];
    std::memset(buffer, 0x7E, sizeof(buffer));

    const int NUM_STEPS = 1000;
    for (int step = 0; step < NUM_STEPS; step++) {
        int op = opDist(rng);
        uint16_t tid = topicIds[step % topicIds.size()];
        size_t sz = static_cast<size_t>(sizeDist(rng));

        switch (op) {
            case 0:
                mc.post(tid, buffer, std::min(sz, size_t(64)));
                break;
            case 1:
                mc.post(tid, buffer, std::max(sz, size_t(65)));
                break;
            case 2:
                if (step % 50 == 0) {
                    mc.flush(500);
                }
                break;
        }
    }

    mc.flush(2000);
    mc.stop();
}
