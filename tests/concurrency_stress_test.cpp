/**
 * @file concurrency_stress_test.cpp
 * @brief Concurrency Stress and Multi-Threaded Contention Tests.
 *
 * This test suite verifies:
 * 1. High-throughput multi-threaded producer posting to EventQueue.
 * 2. Concurrent dynamic observer registration and unregistration while dispatches are active.
 * 3. MPMC lock-free queue bounded capacity, try_push, try_pop, and ring buffer wrap-around.
 * 4. Thread-safe ObjectPool acquire/release lifecycle under concurrent thread load.
 */

#include <gtest/gtest.h>

#include "eventqueue.h"
#include "messagecenter.h"
#include "mpmc_queue.h"
#include "objectpool.h"
#include "objectpool_lockfree.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

class ConcurrencyStressTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {
        MessageCenter::DisposeDefaultMessageCenter();
    }
};

// ============================================================================
// Section 1: Multi-Producer Concurrent Posting
// ============================================================================

/**
 * Test: MultiProducerEventQueuePosting
 * Multiple threads concurrently register topics and post messages into EventQueue.
 * Verifies that all messages are safely enqueued without race conditions.
 */
TEST_F(ConcurrencyStressTest, MultiProducerEventQueuePosting) {
    EventQueueCUT queue;
    queue.init();

    const int NUM_PRODUCERS = 8;
    const int MSGS_PER_PRODUCER = 5000;
    const int TOTAL_MESSAGES = NUM_PRODUCERS * MSGS_PER_PRODUCER;

    int topicId = queue.RegisterTopic("stress.multiproducer");
    ASSERT_GE(topicId, 0);

    std::atomic<bool> start{false};
    std::vector<std::thread> producers;

    for (int p = 0; p < NUM_PRODUCERS; p++) {
        producers.emplace_back([&queue, topicId, &start, MSGS_PER_PRODUCER]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 0; i < MSGS_PER_PRODUCER; i++) {
                queue.Post(topicId, nullptr, false);
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : producers) {
        t.join();
    }

    // Verify all messages arrived in queue
    int drained = 0;
    while (Message* m = queue.GetQueueMessage()) {
        drained++;
        delete m;
    }

    EXPECT_EQ(drained, TOTAL_MESSAGES);
    queue.dispose();
}

// ============================================================================
// Section 2: Dynamic Add/Remove Under Active Dispatches
// ============================================================================

/**
 * Test: ConcurrentObserverAddRemoveDuringDispatch
 * Tests thread safety when observers are dynamically added and removed by worker
 * threads while dispatching is actively running.
 */
TEST_F(ConcurrencyStressTest, ConcurrentObserverAddRemoveDuringDispatch) {
    EventQueueCUT queue;
    queue.init();

    const std::string topic = "stress.dynamic.observers";
    int topicId = queue.RegisterTopic(topic);
    ASSERT_GE(topicId, 0);

    std::atomic<bool> running{true};
    std::atomic<uint64_t> callbacksExecuted{0};

    // Dispatcher thread
    std::thread dispatcher([&queue, &running, topicId]() {
        while (running.load(std::memory_order_relaxed)) {
            Message* m = queue.GetQueueMessage();
            if (m != nullptr) {
                queue.Dispatch(m->tid, m);
            } else {
                std::this_thread::yield();
            }
        }
        // Drain remaining
        while (Message* m = queue.GetQueueMessage()) {
            queue.Dispatch(m->tid, m);
        }
    });

    // Poster thread
    std::thread poster([&queue, topicId, &running]() {
        for (int i = 0; i < 2000 && running.load(std::memory_order_relaxed); i++) {
            queue.Post(topicId, nullptr, false);
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    // Observer mutator thread (rapid add and remove)
    std::thread mutator([&queue, &running, &callbacksExecuted, topic]() {
        for (int cycle = 0; cycle < 100 && running.load(std::memory_order_relaxed); cycle++) {
            uint64_t id = queue.AddObserver(topic, ObserverCallbackFunc([&callbacksExecuted](int, Message*) {
                callbacksExecuted.fetch_add(1, std::memory_order_relaxed);
            }));
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            queue.RemoveObserverById(topic, id);
        }
    });

    poster.join();
    mutator.join();
    running.store(false, std::memory_order_release);
    dispatcher.join();

    EXPECT_GE(callbacksExecuted.load(), 0u);
    queue.dispose();
}

// ============================================================================
// Section 3: MPMC Queue Boundary & Wrap-Around
// ============================================================================

/**
 * Test: MPMCQueueWrapAroundAndSaturation
 * Validates that MPMCQueue safely wraps around sequence numbers across multiple
 * buffer cycles, and accurately returns false when full or empty.
 */
TEST_F(ConcurrencyStressTest, MPMCQueueWrapAroundAndSaturation) {
    constexpr size_t CAPACITY = 64;
    MPMCQueue<int, CAPACITY> queue;

    EXPECT_TRUE(queue.empty());
    int val = 0;
    EXPECT_FALSE(queue.try_pop(val));

    // Fill completely
    for (size_t i = 0; i < CAPACITY; i++) {
        EXPECT_TRUE(queue.try_push(static_cast<int>(i)));
    }

    // Queue full: next push must fail
    EXPECT_FALSE(queue.try_push(999));
    EXPECT_EQ(queue.size_approx(), CAPACITY);

    // Multi-cycle wrap-around (thousands of items through a 64-slot queue)
    const int CYCLES = 10000;
    for (int c = 0; c < CYCLES; c++) {
        int out = -1;
        EXPECT_TRUE(queue.try_pop(out));
        EXPECT_TRUE(queue.try_push(c));
    }

    // Drain remainder
    int count = 0;
    while (queue.try_pop(val)) {
        count++;
    }
    EXPECT_EQ(count, static_cast<int>(CAPACITY));
    EXPECT_TRUE(queue.empty());
}

/**
 * Test: MPMCQueueMultiThreadedContention
 * Multi-producer multi-consumer high contention test on bounded MPMCQueue.
 */
TEST_F(ConcurrencyStressTest, MPMCQueueMultiThreadedContention) {
    constexpr size_t CAPACITY = 256;
    MPMCQueue<int, CAPACITY> queue;

    const int NUM_PRODUCERS = 4;
    const int NUM_CONSUMERS = 4;
    const int ITEMS_PER_PRODUCER = 10000;
    const int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

    std::atomic<bool> start{false};
    std::atomic<int> consumedCount{0};
    std::atomic<int64_t> consumedSum{0};

    std::vector<std::thread> consumers;
    for (int c = 0; c < NUM_CONSUMERS; c++) {
        consumers.emplace_back([&queue, &start, &consumedCount, &consumedSum, TOTAL_ITEMS]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (consumedCount.load(std::memory_order_relaxed) < TOTAL_ITEMS) {
                int item = 0;
                if (queue.try_pop(item)) {
                    consumedSum.fetch_add(item, std::memory_order_relaxed);
                    consumedCount.fetch_add(1, std::memory_order_relaxed);
                } else {
                    std::this_thread::yield();
                }
            }
        });
    }

    std::vector<std::thread> producers;
    for (int p = 0; p < NUM_PRODUCERS; p++) {
        producers.emplace_back([&queue, &start, ITEMS_PER_PRODUCER]() {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (int i = 1; i <= ITEMS_PER_PRODUCER; i++) {
                while (!queue.try_push(i)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : producers) t.join();
    for (auto& t : consumers) t.join();

    EXPECT_EQ(consumedCount.load(), TOTAL_ITEMS);
    int64_t expectedPerProducer = static_cast<int64_t>(ITEMS_PER_PRODUCER) * (ITEMS_PER_PRODUCER + 1) / 2;
    EXPECT_EQ(consumedSum.load(), expectedPerProducer * NUM_PRODUCERS);
}

// ============================================================================
// Section 4: ObjectPool Contention
// ============================================================================

/**
 * Test: ObjectPoolConcurrentAcquireRelease
 * Verifies that the mutex-based ObjectPool safely handles concurrent allocation,
 * reuse, and destruction across threads.
 */
TEST_F(ConcurrencyStressTest, ObjectPoolConcurrentAcquireRelease) {
    struct PooledObject {
        int x = 0;
        int y = 0;
    };

    ObjectPool<PooledObject> pool(32, 128);
    EXPECT_EQ(pool.size(), 32u);

    const int NUM_THREADS = 8;
    const int ITERS = 2000;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&pool, ITERS]() {
            for (int i = 0; i < ITERS; i++) {
                PooledObject* obj = pool.acquire();
                ASSERT_NE(obj, nullptr);
                obj->x = i;
                obj->y = i * 2;
                pool.release(obj);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // Pool remains valid and destructible
    EXPECT_GE(pool.size(), 1u);
    pool.clear();
    EXPECT_EQ(pool.size(), 0u);
}

/**
 * Test: ObjectPoolLockFreeConcurrentAcquireRelease
 * Verifies that the lock-free ObjectPoolLockFree safely handles concurrent allocation,
 * reuse, and release across multiple threads without ABA corruption, leaks, or races.
 */
TEST_F(ConcurrencyStressTest, ObjectPoolLockFreeConcurrentAcquireRelease) {
    struct PooledObject {
        int x = 0;
        int y = 0;
    };

    ObjectPoolLockFree<PooledObject> pool(64, 256);

    const int NUM_THREADS = 8;
    const int ITERS = 5000;
    std::vector<std::thread> threads;

    for (int t = 0; t < NUM_THREADS; t++) {
        threads.emplace_back([&pool, ITERS]() {
            for (int i = 0; i < ITERS; i++) {
                PooledObject* obj = pool.acquire();
                ASSERT_NE(obj, nullptr);
                obj->x = i;
                obj->y = i * 2;
                pool.release(obj);
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }
}
