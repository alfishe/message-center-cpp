/**
 * @file rework_regression_test.cpp
 * @brief Regression tests for the review-driven rework:
 *  - ObjectPoolLockFree grows lazily (no up-front commit of maxSize) and still
 *    hands out unique objects / handles overflow correctly.
 *  - MessageCenter / MessageCenterFast: stop() requested from inside an observer
 *    callback is deferred (no self-join, no detach) and the thread is joined later.
 *  - MessageCenterFast: no lost wakeups - a message posted to a sleeping worker is
 *    delivered well before the worker's wait_for timeout.
 */

#include <gtest/gtest.h>

#include "eventqueue_batch.h"
#include "eventqueue_broadcast.h"
#include "eventqueue_emulator.h"
#include "eventqueue_fast.h"
#include "messagecenter.h"
#include "messagecenter_fast.h"
#include "objectpool.h"
#include "objectpool_lockfree.h"

#include <atomic>
#include <chrono>
#include <set>
#include <thread>
#include <vector>

namespace {
struct Big {
    char bytes[256];
};
} // namespace

TEST(ReworkRegression, ObjectPoolLockFree_GrowsLazily) {
    // Large maxSize must not commit memory up front
    ObjectPoolLockFree<Big> pool(0, 1u << 20);
    EXPECT_EQ(pool.allocated(), 0u);

    Big* a = pool.acquire();
    ASSERT_NE(a, nullptr);
    // One chunk allocated on demand - far below maxSize
    EXPECT_GT(pool.allocated(), 0u);
    EXPECT_LE(pool.allocated(), 64u);
    pool.release(a);

    // Released slot is reused, no further growth
    size_t before = pool.allocated();
    for (int i = 0; i < 1000; ++i) {
        pool.release(pool.acquire());
    }
    EXPECT_EQ(pool.allocated(), before);
}

TEST(ReworkRegression, ObjectPoolLockFree_InitialSizeAndOverflow) {
    ObjectPoolLockFree<Big> pool(10, 64);
    EXPECT_GE(pool.allocated(), 10u);   // pre-warmed (rounded to chunk)
    EXPECT_LE(pool.allocated(), 64u);

    // Exhaust the pooled capacity and go into overflow
    std::vector<Big*> objs;
    std::set<Big*> unique;
    for (int i = 0; i < 200; ++i) {
        Big* p = pool.acquire();
        ASSERT_NE(p, nullptr);
        objs.push_back(p);
        unique.insert(p);
    }
    EXPECT_EQ(unique.size(), objs.size());
    EXPECT_EQ(pool.allocated(), 200u);  // 64 pooled + 136 overflow

    for (Big* p : objs) {
        pool.release(p);
    }
    EXPECT_EQ(pool.allocated(), 64u);   // overflow nodes freed, pooled slots retained
}

TEST(ReworkRegression, ObjectPoolLockFree_ProducerConsumerHandoff) {
    // Emulator pattern: one thread acquires, another releases
    ObjectPoolLockFree<Big> pool(0, 1024);
    MPMCQueue<Big*, 1024> q;
    const int N = 50000;
    std::atomic<int> consumed{0};

    std::thread consumer([&]() {
        Big* p = nullptr;
        while (consumed.load(std::memory_order_relaxed) < N) {
            if (q.try_pop(p)) {
                EXPECT_EQ(p->bytes[0], 'x');
                pool.release(p);
                consumed.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    for (int i = 0; i < N; ++i) {
        Big* p = pool.acquire();
        p->bytes[0] = 'x';
        while (!q.try_push(p)) {
            std::this_thread::yield();
        }
    }
    consumer.join();
    EXPECT_LE(pool.allocated(), 1024u + 64u);
}

TEST(ReworkRegression, MessageCenterFast_StopFromCallbackIsDeferred) {
    std::atomic<int> delivered{0};
    {
        MessageCenterFast mc;
        mc.start();
        mc.addObserver("stop", [&](uint16_t, const void*, size_t) {
            delivered.fetch_add(1);
            mc.stop(); // must not self-join / detach
        });
        ASSERT_TRUE(mc.post("stop", nullptr, 0));

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (delivered.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(delivered.load(), 1);
        // Destructor (on the test thread) joins the worker
    }
}

TEST(ReworkRegression, MessageCenter_StopFromCallbackIsDeferred) {
    struct Payload : public MessagePayload {};
    std::atomic<int> delivered{0};
    {
        MessageCenterCUT& mc = MessageCenterCUT::DefaultMessageCenter(true);
        int topic = mc.RegisterTopic("stop_rework");
        ObserverCallbackFunc cb = [&](int, Message*) {
            delivered.fetch_add(1);
            mc.Stop(); // must not self-join / detach
        };
        uint64_t id = mc.AddObserver("stop_rework", cb);
        mc.Post(topic, new Payload(), true);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (delivered.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(delivered.load(), 1);
        mc.RemoveObserverById("stop_rework", id);
        // Dispose (on the test thread) joins the worker
        MessageCenterCUT::DisposeDefaultMessageCenter();
    }
}

TEST(ReworkRegression, MessageCenterFast_NoLostWakeups) {
    // Each post goes to a worker that has gone to sleep. With a lost wakeup the
    // message would only be picked up by the 10 ms wait_for timeout.
    MessageCenterFast mc;
    std::atomic<int> delivered{0};
    mc.addObserver("ping", [&](uint16_t, const void*, size_t) { delivered.fetch_add(1); });
    mc.start();

    const int ROUNDS = 200;
    int slow = 0;
    for (int i = 0; i < ROUNDS; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(500)); // let worker fall asleep
        int target = delivered.load() + 1;
        auto t0 = std::chrono::steady_clock::now();
        ASSERT_TRUE(mc.post("ping", nullptr, 0));
        while (delivered.load() < target) {
            std::this_thread::yield();
        }
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        if (us >= 9000) {
            ++slow;
        }
    }
    mc.stop();
    // Allow scheduler noise on loaded CI / sanitizers, but not systematic timeouts
    EXPECT_LE(slow, ROUNDS / 50);
}


namespace {
struct CountedPayload : public MessagePayload {
    std::atomic<int>* alive;
    explicit CountedPayload(std::atomic<int>* a) : alive(a) {
        alive->fetch_add(1, std::memory_order_relaxed);
    }
    ~CountedPayload() override {
        alive->fetch_sub(1, std::memory_order_relaxed);
    }
};
} // namespace

TEST(ReworkRegression, EventQueue_MaxQueueSizeBoundsAndCleansUpPayloads) {
    std::atomic<int> alive{0};
    {
        EventQueueCUT q;
        q.SetMaxQueueSize(8);
        EXPECT_EQ(q.GetMaxQueueSize(), 8u);
        int topic = q.RegisterTopic("bounded");

        int accepted = 0;
        for (int i = 0; i < 20; ++i) {
            if (q.Post(topic, new CountedPayload(&alive), true)) {
                ++accepted;
            }
        }
        EXPECT_EQ(accepted, 8);
        EXPECT_EQ(q.GetQueueSize(), 8u);
        // Rejected payloads with autoCleanupPayload=true must be freed immediately
        EXPECT_EQ(alive.load(), 8);

        // Posting to an invalid topic with autoCleanupPayload=true must also free immediately
        EXPECT_FALSE(q.Post(-1, new CountedPayload(&alive), true));
        EXPECT_EQ(alive.load(), 8);
    }
    // Queue disposal frees remaining 8 payloads
    EXPECT_EQ(alive.load(), 0);
}

TEST(ReworkRegression, EventQueue_PostBatchAndBoundedRejection) {
    std::atomic<int> alive{0};
    std::atomic<int> delivered{0};
    {
        EventQueueCUT q;
        q.SetMaxQueueSize(10);
        int topic = q.RegisterTopic("batch_topic");
        q.AddObserver("batch_topic", ObserverCallbackFunc([&](int, Message*) {
            delivered.fetch_add(1, std::memory_order_relaxed);
        }));

        std::vector<MessagePayload*> batch(16);
        for (size_t i = 0; i < batch.size(); ++i) {
            batch[i] = new CountedPayload(&alive);
        }

        size_t enqueued = q.PostBatch(topic, batch.data(), batch.size(), true);
        EXPECT_EQ(enqueued, 10u);
        EXPECT_EQ(q.GetQueueSize(), 10u);
        // 6 rejected payloads must already be deleted
        EXPECT_EQ(alive.load(), 10);

        while (Message* msg = q.GetQueueMessage()) {
            q.Dispatch(msg->tid, msg);
        }
        EXPECT_EQ(delivered.load(), 10);
        EXPECT_EQ(alive.load(), 0);
    }
}

TEST(ReworkRegression, LockFreeQueues_TryPostOnFullReturnsFalseWithoutLivelock) {
    // Single-threaded saturation test: tryPost must return false once the ring buffer is full
    // and release both the pooled envelope and any RefCountedPayload without leaking.
    {
        EventQueueBroadcast<16> q;
        uint16_t t = q.registerTopic("full");
        q.addObserver(t, [](uint16_t, const void*, size_t) {});
        std::vector<char> large(128, 0x5A);
        size_t ok = 0;
        for (int i = 0; i < 64; ++i) {
            if (q.tryPost(t, large.data(), large.size())) {
                ++ok;
            }
        }
        EXPECT_EQ(ok, 16u);
        EXPECT_EQ(q.dispatchAll(), 16u);
    }
    {
        EventQueueBroadcastFast<16> q;
        uint16_t t = q.registerTopic();
        q.addObserver(t, [](uint16_t, const void*, size_t, void*) {});
        std::vector<char> large(128, 0x5A);
        size_t ok = 0;
        for (int i = 0; i < 64; ++i) {
            if (q.tryPost(t, large.data(), large.size())) {
                ++ok;
            }
        }
        EXPECT_EQ(ok, 16u);
        EXPECT_EQ(q.dispatchAll(), 16u);
    }
    {
        EventQueueBroadcastFastBatch<16> q;
        uint16_t t = q.registerTopic();
        q.addObserver(t, [](uint16_t, const void*, size_t, void*) {});
        std::vector<char> large(128, 0x5A);
        size_t ok = 0;
        for (int i = 0; i < 64; ++i) {
            if (q.tryPost(t, large.data(), large.size())) {
                ++ok;
            }
        }
        EXPECT_EQ(ok, 16u);
        EXPECT_EQ(q.dispatchAll(), 16u);
    }
    {
        EventQueueEmulator<16, 16> q;
        uint16_t crit = q.registerTopic(TopicPriority::Critical);
        uint16_t bulk = q.registerTopic(TopicPriority::Normal);
        q.addObserver(crit, [](uint16_t, const void*, size_t, void*) {});
        q.addObserver(bulk, [](uint16_t, const void*, size_t, void*) {});
        uint32_t smallVal = 42;
        std::vector<char> large(128, 0x5A);
        size_t okFast = 0, okBulk = 0;
        for (int i = 0; i < 64; ++i) {
            if (q.tryPost(crit, &smallVal, sizeof(smallVal))) ++okFast;
            if (q.tryPost(bulk, large.data(), large.size())) ++okBulk;
        }
        EXPECT_EQ(okFast, 16u);
        EXPECT_EQ(okBulk, 16u);
        EXPECT_EQ(q.dispatchAll(), 32u);
    }
}

TEST(ReworkRegression, SmallStackFootprintAndLazySegmentGrowth) {
    // 1. All queue classes must have a compact stack footprint (< 2 KB)
    EXPECT_LT(sizeof(MessageCenterFast), 2048u);
    EXPECT_LT(sizeof(EventQueueBroadcastFast<>), 2048u);
    EXPECT_LT(sizeof(EventQueueBroadcastFastBatch<>), 2048u);
    EXPECT_LT(sizeof(EventQueueBroadcast<>), 2048u);
    EXPECT_LT(sizeof(EventQueueEmulator<>), 2048u);

    // 2. MPMCQueue<int, 65536> starts with 1 segment (64 KB) and stays at 1 segment
    //    across 100,000 messages during normal work (backlog < 1024).
    MPMCQueue<int, 65536> q;
    EXPECT_EQ(q.allocated_segments(), 1u);

    for (int frame = 0; frame < 1000; ++frame) {
        for (int i = 0; i < 100; ++i) {
            ASSERT_TRUE(q.try_push(frame * 100 + i));
        }
        for (int i = 0; i < 100; ++i) {
            int out = -1;
            ASSERT_TRUE(q.try_pop(out));
            EXPECT_EQ(out, frame * 100 + i);
        }
    }
    EXPECT_EQ(q.allocated_segments(), 1u);

    // 3. On a 10,000-message backlog spike, it grows lazily to 10 segments (ceil(10000 / 1024))
    //    and recycles those segments on a subsequent spike without allocating more.
    for (int i = 0; i < 10000; ++i) {
        ASSERT_TRUE(q.try_push(i));
    }
    size_t segsAfterFirstSpike = q.allocated_segments();
    EXPECT_EQ(segsAfterFirstSpike, 10u);

    for (int i = 0; i < 10000; ++i) {
        int out = -1;
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_TRUE(q.empty());

    // Second 10,000-message spike reuses recycled segments (zero new allocations)
    for (int i = 0; i < 10000; ++i) {
        ASSERT_TRUE(q.try_push(i + 50000));
    }
    EXPECT_EQ(q.allocated_segments(), segsAfterFirstSpike);
    for (int i = 0; i < 10000; ++i) {
        int out = -1;
        ASSERT_TRUE(q.try_pop(out));
        EXPECT_EQ(out, i + 50000);
    }
    EXPECT_TRUE(q.empty());
}

TEST(ReworkRegression, CoordinatedPoolAndQueue_NoDoSForOccasionalBiggerPayloads) {
    EventQueueEmulator<4096, 16384> emu2;
    uint16_t crit2 = emu2.registerTopic(TopicPriority::Critical);
    struct Counters {
        std::atomic<int> small{0};
        std::atomic<int> big{0};
    } c;
    emu2.addObserver(
        crit2,
        [](uint16_t, const void* data, size_t size, void* u) {
            auto* cnt = static_cast<Counters*>(u);
            if (size <= EMU_INLINE_MAX) {
                cnt->small.fetch_add(1, std::memory_order_relaxed);
            } else {
                const auto* bytes = static_cast<const unsigned char*>(data);
                if (bytes[0] == 0xAA && bytes[size - 1] == 0x55) {
                    cnt->big.fetch_add(1, std::memory_order_relaxed);
                }
            }
        },
        &c);

    // 1. Posting an occasional bigger payload (> 48B) on a Critical topic via post() / tryPost()
    //    must succeed (routed through bulk queue) rather than being denied service.
    std::vector<unsigned char> bigPayload(512, 0x11);
    bigPayload.front() = 0xAA;
    bigPayload.back() = 0x55;
    uint32_t smallPayload = 0x12345678u;

    for (int i = 0; i < 200; ++i) {
        ASSERT_TRUE(emu2.tryPost(crit2, &smallPayload, sizeof(smallPayload)));
    }
    ASSERT_TRUE(emu2.tryPost(crit2, bigPayload.data(), bigPayload.size()));
    ASSERT_TRUE(emu2.post(crit2, bigPayload.data(), bigPayload.size()));

    // 2. Even with 200 fast messages queued ahead, a single dispatchPriority() call
    //    must service at least one bulk (bigger-payload) message without starving it.
    size_t firstPass = emu2.dispatchPriority(64);
    EXPECT_EQ(firstPass, 65u); // 64 fast + 1 bulk
    EXPECT_EQ(c.small.load(), 64);
    EXPECT_EQ(c.big.load(), 1);

    // Drain remainder
    while (!emu2.empty()) {
        emu2.dispatchPriority(64);
    }
    EXPECT_EQ(c.small.load(), 200);
    EXPECT_EQ(c.big.load(), 2);
}

TEST(ReworkRegression, BatchQueue_MultiInstanceIsolationAndCrossThreadDestruction) {
    // 1. Interleaving postBatched() across two instances on the same thread
    //    must never cross-contaminate queues or pools.
    EventQueueBroadcastFastBatch<1024> q1;
    EventQueueBroadcastFastBatch<1024> q2;
    uint16_t t1 = q1.registerTopic();
    uint16_t t2 = q2.registerTopic();

    int sum1 = 0;
    int sum2 = 0;
    q1.addObserver(t1, [](uint16_t, const void* d, size_t, void* u) {
        *static_cast<int*>(u) += *static_cast<const int*>(d);
    }, &sum1);
    q2.addObserver(t2, [](uint16_t, const void* d, size_t, void* u) {
        *static_cast<int*>(u) += *static_cast<const int*>(d);
    }, &sum2);

    for (int i = 0; i < 10; ++i) {
        int v1 = 1;
        int v2 = 100;
        q1.postBatched(t1, &v1, sizeof(v1));
        q2.postBatched(t2, &v2, sizeof(v2));
    }
    q1.flushBatch();
    q2.flushBatch();

    EXPECT_EQ(q1.dispatchBatch(64), 10u);
    EXPECT_EQ(q2.dispatchBatch(64), 10u);
    EXPECT_EQ(sum1, 10);
    EXPECT_EQ(sum2, 1000);

    // 2. Destroying an instance while another thread holds unflushed batched
    //    messages (including > 48B managed payloads) must clean up safely without
    //    leaks or dangling thread_local pointers.
    auto* dynamicQ = new EventQueueBroadcastFastBatch<1024>();
    uint16_t dt = dynamicQ->registerTopic();
    dynamicQ->addObserver(dt, [](uint16_t, const void*, size_t, void*) {}, nullptr);

    std::atomic<bool> buffered{false};
    std::atomic<bool> canExit{false};
    std::thread producer([&]() {
        std::vector<char> big(128, 0x42);
        for (int i = 0; i < 5; ++i) {
            dynamicQ->postBatched(dt, big.data(), big.size());
        }
        buffered.store(true, std::memory_order_release);
        while (!canExit.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Posting to q1 after dynamicQ was destroyed on main thread must not UAF
        int v1 = 7;
        q1.postBatched(t1, &v1, sizeof(v1));
        q1.flushBatch();
    });

    while (!buffered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    delete dynamicQ;
    canExit.store(true, std::memory_order_release);
    producer.join();

    EXPECT_EQ(q1.dispatchBatch(64), 1u);
    EXPECT_EQ(sum1, 17);
}

TEST(ReworkRegression, NullptrLargePayload_SafeAcrossAllFlavors) {
    // Posting nullptr with size > 48B must zero-initialize safely instead of SIGSEGV
    EventQueueBroadcast<1024> bcast;
    uint16_t tb = bcast.registerTopic("null_big");
    int count = 0;
    bcast.addObserver(tb, [&](uint16_t, const void* data, size_t sz) {
        ASSERT_NE(data, nullptr);
        EXPECT_EQ(sz, 128u);
        EXPECT_EQ(static_cast<const unsigned char*>(data)[0], 0u);
        ++count;
    });
    EXPECT_TRUE(bcast.post(tb, nullptr, 128));
    EXPECT_EQ(bcast.dispatchAll(), 1u);
    EXPECT_EQ(count, 1);

    EventQueueBroadcastFast<1024> fast;
    uint16_t tf = fast.registerTopic();
    fast.addObserver(tf, [](uint16_t, const void* data, size_t sz, void* u) {
        if (data && sz == 128 && static_cast<const unsigned char*>(data)[127] == 0u)
            ++(*static_cast<int*>(u));
    }, &count);
    EXPECT_TRUE(fast.post(tf, nullptr, 128));
    EXPECT_EQ(fast.dispatchAll(), 1u);
    EXPECT_EQ(count, 2);

    EventQueueEmulator<1024, 1024> emu;
    uint16_t te = emu.registerTopic(TopicPriority::Critical);
    emu.addObserver(te, [](uint16_t, const void* data, size_t sz, void* u) {
        if (data && sz == 128 && static_cast<const unsigned char*>(data)[64] == 0u)
            ++(*static_cast<int*>(u));
    }, &count);
    EXPECT_TRUE(emu.post(te, nullptr, 128));
    EXPECT_EQ(emu.dispatchPriority(64), 1u);
    EXPECT_EQ(count, 3);
}

TEST(ReworkRegression, TopicRegistrationOverflow_ClampsWithoutWrapAround) {
    EventQueueEmulator<1024, 1024> emu;
    for (size_t i = 0; i < EMU_MAX_TOPICS; ++i) {
        EXPECT_EQ(emu.registerTopic(), static_cast<uint16_t>(i));
    }
    // Calling registerTopic() 70,000 more times (past 65,536 uint16_t wrap boundary)
    // must always return UINT16_MAX and never wrap around to topic 0.
    for (int i = 0; i < 70000; ++i) {
        EXPECT_EQ(emu.registerTopic(), UINT16_MAX);
    }
}

TEST(ReworkRegression, RestartAfterCallbackSelfStop_WorksCleanly) {
    // 1. MessageCenterFast: callback calls stop(), then main thread calls start() directly
    {
        MessageCenterFast mc;
        mc.start();
        uint16_t t = mc.registerTopic("restart.fast");
        std::atomic<int> received{0};
        mc.addObserver(t, [&](uint16_t, const void*, size_t) {
            if (received.fetch_add(1) == 0) {
                mc.stop(); // Deferred self-stop on worker thread
            }
        });

        int v = 1;
        ASSERT_TRUE(mc.post(t, &v, sizeof(v)));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (received.load() < 1 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(received.load(), 1);

        // Calling start() directly (without an intermediate external stop()) must
        // join the exited worker and spawn a fresh worker thread.
        mc.start();
        ASSERT_TRUE(mc.post(t, &v, sizeof(v)));
        mc.flush(1000);
        EXPECT_EQ(received.load(), 2);
        mc.stop();
    }

    // 2. Classic MessageCenter: callback calls Stop(), then main thread calls Start() directly
    {
        MessageCenter::DisposeDefaultMessageCenter();
        MessageCenterCUT& mc = MessageCenterCUT::DefaultMessageCenter(true);
        std::atomic<int> received{0};
        mc.AddObserver("restart.classic", ObserverCallbackFunc([&](int, Message*) {
            if (received.fetch_add(1) == 0) {
                mc.Stop(); // Deferred self-stop on worker thread
            }
        }));

        ASSERT_TRUE(mc.Post("restart.classic", nullptr, false));
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!mc.m_stopped.load() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(received.load(), 1);

        // Calling Start() directly must join the old thread and start a new worker
        mc.Start();
        ASSERT_TRUE(mc.Post("restart.classic", nullptr, false));
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (received.load() < 2 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        EXPECT_EQ(received.load(), 2);
        MessageCenterCUT::DisposeDefaultMessageCenter();
    }
}

namespace {
struct SmallTypedEvent {
    uint32_t id;
    uint64_t timestamp;
};

struct LargeTypedEvent {
    uint64_t seq;
    uint8_t payload[128];
};

void recordSmallAndLargeCb(uint16_t, const void* data, size_t size, void* user) {
    uint64_t* acc = static_cast<uint64_t*>(user);
    if (size == sizeof(SmallTypedEvent)) {
        const SmallTypedEvent* e = static_cast<const SmallTypedEvent*>(data);
        *acc += e->id + e->timestamp;
    } else if (size == sizeof(LargeTypedEvent)) {
        const LargeTypedEvent* e = static_cast<const LargeTypedEvent*>(data);
        *acc += e->seq + e->payload[0] + e->payload[127];
    } else if (size == 0) {
        *acc += 1;
    }
}
} // namespace

TEST(ReworkRegression, TypedPayloadHelpersAndNonBlockingTryPost) {
    SmallTypedEvent smallEvt{10, 20};
    LargeTypedEvent largeEvt{};
    largeEvt.seq = 100;
    largeEvt.payload[0] = 5;
    largeEvt.payload[127] = 7;

    // 1. EventQueueBroadcastFast
    {
        EventQueueBroadcastFast<64> q;
        uint16_t t = q.registerTopic();
        uint64_t sum = 0;
        q.addObserver(t, &recordSmallAndLargeCb, &sum);

        EXPECT_TRUE(q.post(t, smallEvt));
        EXPECT_TRUE(q.tryPost(t, largeEvt));
        EXPECT_TRUE(q.tryPost(t));
        EXPECT_EQ(q.dispatchAll(), 3u);
        EXPECT_EQ(sum, 30u + 112u + 1u);
    }

    // 2. EventQueueBroadcastFastBatch
    {
        EventQueueBroadcastFastBatch<64> q;
        uint16_t t = q.registerTopic();
        uint64_t sum = 0;
        q.addObserver(t, &recordSmallAndLargeCb, &sum);

        EXPECT_TRUE(q.post(t, smallEvt));
        EXPECT_TRUE(q.tryPost(t, largeEvt));
        EXPECT_TRUE(q.postBatched(t, smallEvt));
        EXPECT_TRUE(q.postBatched(t));
        q.flushBatch();
        EXPECT_EQ(q.dispatchAll(), 4u);
        EXPECT_EQ(sum, 30u + 112u + 30u + 1u);
    }

    // 3. EventQueueEmulator
    {
        EventQueueEmulator<64, 64> q;
        uint16_t crit = q.registerTopic(TopicPriority::Critical);
        uint16_t norm = q.registerTopic(TopicPriority::Normal);
        uint64_t sum = 0;
        q.addObserver(crit, &recordSmallAndLargeCb, &sum);
        q.addObserver(norm, &recordSmallAndLargeCb, &sum);

        EXPECT_TRUE(q.postFast(crit, smallEvt));
        EXPECT_TRUE(q.tryPostFast(crit, smallEvt));
        EXPECT_TRUE(q.tryPostFast(crit));
        EXPECT_TRUE(q.post(crit, largeEvt));    // Auto-routes >48B to bulk queue
        EXPECT_TRUE(q.tryPost(crit, largeEvt)); // Auto-routes >48B to bulk queue
        EXPECT_TRUE(q.postBulk(norm, largeEvt));
        EXPECT_TRUE(q.tryPostBulk(norm, smallEvt));
        EXPECT_EQ(q.dispatchAll(), 7u);
        EXPECT_EQ(sum, 30u + 30u + 1u + 112u + 112u + 112u + 30u);
    }

    // 4. EventQueueBroadcast
    {
        EventQueueBroadcast<64> q;
        uint16_t t = q.registerTopic("typed.bcast");
        uint64_t sum = 0;
        q.addObserver(t, [&](uint16_t, const void* data, size_t size) {
            recordSmallAndLargeCb(t, data, size, &sum);
        });

        EXPECT_TRUE(q.post(t, smallEvt));
        EXPECT_TRUE(q.tryPost(t, largeEvt));
        EXPECT_TRUE(q.post(std::string("typed.bcast"), smallEvt));
        EXPECT_TRUE(q.tryPost(std::string("typed.bcast"), largeEvt));
        EXPECT_TRUE(q.tryPost(t));
        EXPECT_EQ(q.dispatchAll(), 5u);
        EXPECT_EQ(sum, 30u + 112u + 30u + 112u + 1u);
    }

    // 5. MessageCenterFast
    {
        MessageCenterFast mc;
        mc.start();
        uint16_t t = mc.registerTopic("typed.mcfast");
        std::atomic<uint64_t> sum{0};
        mc.addObserver(t, [&](uint16_t tid, const void* data, size_t size) {
            uint64_t local = 0;
            recordSmallAndLargeCb(tid, data, size, &local);
            sum.fetch_add(local, std::memory_order_relaxed);
        });

        EXPECT_TRUE(mc.post(t, smallEvt));
        EXPECT_TRUE(mc.tryPost(t, largeEvt));
        EXPECT_TRUE(mc.post(std::string("typed.mcfast"), smallEvt));
        EXPECT_TRUE(mc.tryPost(std::string("typed.mcfast"), largeEvt));
        EXPECT_TRUE(mc.tryPost(t));
        mc.flush(1000);
        mc.stop();
        EXPECT_EQ(sum.load(), 30u + 112u + 30u + 112u + 1u);
    }
}

TEST(ReworkRegression, GetTopicByIdZeroAndOutOfRangePostAndObjectPoolClear) {
    // 1. EventQueue::GetTopicByID(0) returns first topic name and Post rejects >= MAX_TOPICS
    {
        EventQueue q;
        int t0 = q.RegisterTopic("first.topic");
        int t1 = q.RegisterTopic("second.topic");
        ASSERT_EQ(t0, 0);
        ASSERT_EQ(t1, 1);
        EXPECT_EQ(q.GetTopicByID(0), "first.topic");
        EXPECT_EQ(q.GetTopicByID(1), "second.topic");
        EXPECT_EQ(q.GetTopicByID(2), "");
        EXPECT_EQ(q.GetTopicByID(-1), "");

        std::atomic<int> destroyed{0};
        struct Tracked : public MessagePayload {
            std::atomic<int>* d;
            explicit Tracked(std::atomic<int>* ptr) : d(ptr) {}
            ~Tracked() override { d->fetch_add(1, std::memory_order_relaxed); }
        };

        EXPECT_FALSE(q.Post(static_cast<int>(MAX_TOPICS), new Tracked(&destroyed), true));
        EXPECT_EQ(destroyed.load(), 1);

        MessagePayload* batch[2] = {new Tracked(&destroyed), new Tracked(&destroyed)};
        EXPECT_EQ(q.PostBatch(static_cast<int>(MAX_TOPICS), batch, 2, true), 0u);
        EXPECT_EQ(destroyed.load(), 3);
    }

    // 2. ObjectPool::clear() preserves allocated() count for checked-out objects
    {
        ObjectPool<int> pool(2, 2);
        EXPECT_EQ(pool.size(), 2u);
        EXPECT_EQ(pool.allocated(), 2u);

        int* a = pool.acquire();
        EXPECT_EQ(pool.size(), 1u);
        EXPECT_EQ(pool.allocated(), 2u);

        pool.clear();
        EXPECT_EQ(pool.size(), 0u);
        EXPECT_EQ(pool.allocated(), 1u); // 'a' is still checked out

        int* b = pool.acquire();
        int* c = pool.acquire();
        EXPECT_EQ(pool.allocated(), 3u);

        pool.release(b);
        pool.release(c);
        pool.release(a); // Pool is at maxSize (2), so 'a' is deleted without underflowing
        EXPECT_EQ(pool.size(), 2u);
        EXPECT_EQ(pool.allocated(), 2u);
    }

    // 3. MessageCenterFast nullptr payload zero-initialization (inline & managed)
    {
        MessageCenterFast mc;
        mc.start();
        uint16_t t = mc.registerTopic("null.zero");
        std::atomic<int> verified{0};
        mc.addObserver(t, [&](uint16_t, const void* data, size_t size) {
            const uint8_t* bytes = static_cast<const uint8_t*>(data);
            bool allZero = (bytes != nullptr);
            for (size_t i = 0; i < size && allZero; ++i) {
                if (bytes[i] != 0) allZero = false;
            }
            if (allZero) verified.fetch_add(1, std::memory_order_relaxed);
        });

        EXPECT_TRUE(mc.post(t, nullptr, 32));
        EXPECT_TRUE(mc.post(t, nullptr, 128));
        EXPECT_TRUE(mc.tryPost(t, nullptr, 32));
        EXPECT_TRUE(mc.tryPost(t, nullptr, 128));
        mc.flush(1000);
        mc.stop();
        EXPECT_EQ(verified.load(), 4);
    }

    // 4. EventQueueBroadcast::dispatchBatch partial and full batch drains
    {
        EventQueueBroadcast<64> q;
        uint16_t t = q.registerTopic("batch.drain");
        uint64_t sum = 0;
        q.addObserver(t, [&](uint16_t, const void* data, size_t size) {
            if (size == sizeof(uint32_t)) {
                sum += *static_cast<const uint32_t*>(data);
            }
        });
        for (uint32_t i = 1; i <= 50; ++i) {
            EXPECT_TRUE(q.post(t, i));
        }
        EXPECT_EQ(q.dispatchBatch(10), 10u);
        EXPECT_EQ(q.dispatchBatch(35), 35u);
        EXPECT_EQ(q.dispatchAll(), 5u);
        EXPECT_EQ(sum, (50u * 51u) / 2u);
    }

    // 5. ObjectPoolLockFree::acquireBatch + releaseBatch & MPMCQueue::try_push_batch / try_pop_batch
    {
        ObjectPoolLockFree<uint64_t> pool(16, 128);
        uint64_t* items[48] = {};
        pool.acquireBatch(items, 48); // Crosses initial 64-slot chunk and grows
        for (size_t i = 0; i < 48; ++i) {
            ASSERT_NE(items[i], nullptr);
            *items[i] = i + 100;
        }
        pool.releaseBatch(items, 48);

        MPMCQueue<uint32_t, 2048> q; // 2 segments of 1024
        uint32_t inBatch[64];
        uint32_t outBatch[64];
        for (uint32_t b = 0; b < 24; ++b) { // 24 * 64 = 1536 items (crosses segment boundary)
            for (uint32_t i = 0; i < 64; ++i) {
                inBatch[i] = b * 64 + i;
            }
            EXPECT_EQ(q.try_push_batch(inBatch, 64), 64u);
        }
        uint32_t expected = 0;
        while (!q.empty()) {
            size_t popped = q.try_pop_batch(outBatch, 64);
            ASSERT_GT(popped, 0u);
            for (size_t i = 0; i < popped; ++i) {
                EXPECT_EQ(outBatch[i], expected++);
            }
        }
        EXPECT_EQ(expected, 1536u);
    }

    // 6. MessageCenterFast no-observer fast path, observerCount(), and topicCount()
    {
        MessageCenterFast mc;
        uint16_t tNoObs = mc.registerTopic("no.observers");
        uint16_t tWithObs = mc.registerTopic("with.observers");
        EXPECT_EQ(mc.topicCount(), 2u);
        EXPECT_EQ(mc.observerCount(tNoObs), 0u);
        EXPECT_EQ(mc.observerCount(tWithObs), 0u);

        // Posting to a topic with 0 observers must succeed without enqueuing
        int val = 42;
        EXPECT_TRUE(mc.post(tNoObs, val));
        EXPECT_TRUE(mc.tryPost(tNoObs, val));
        EXPECT_EQ(mc.pending(), 0u);

        std::atomic<int> count{0};
        mc.addObserver(tWithObs, [&](uint16_t, const void*, size_t) {
            count.fetch_add(1, std::memory_order_relaxed);
        });
        EXPECT_EQ(mc.observerCount(tWithObs), 1u);
        mc.start();
        EXPECT_TRUE(mc.post(tWithObs, val));
        mc.flush(1000);
        mc.stop();
        EXPECT_EQ(count.load(), 1);
    }
}

namespace {
void incrementIntCb(uint16_t, const void*, size_t, void* user) {
    ++(*static_cast<int*>(user));
}
} // namespace

TEST(ReworkRegression, LockFreeQueues_RemoveObserverClearObserversAndEmuTopics) {
    // 1. EventQueueBroadcast: removeObserver by ID and clearObservers
    {
        EventQueueBroadcast<64> q;
        uint16_t t = q.registerTopic("obs.bcast");
        int c1 = 0, c2 = 0;
        uint64_t id1 = q.addObserver(t, [&](uint16_t, const void*, size_t) { ++c1; });
        uint64_t id2 = q.addObserver("obs.bcast", [&](uint16_t, const void*, size_t) { ++c2; });
        EXPECT_NE(id1, 0u);
        EXPECT_NE(id2, 0u);
        EXPECT_EQ(q.observerCount(t), 2u);

        EXPECT_TRUE(q.post(t, 1));
        EXPECT_EQ(q.dispatchAll(), 1u);
        EXPECT_EQ(c1, 1);
        EXPECT_EQ(c2, 1);

        EXPECT_TRUE(q.removeObserver(t, id1));
        EXPECT_FALSE(q.removeObserver(t, id1)); // already removed
        EXPECT_EQ(q.observerCount(t), 1u);

        EXPECT_TRUE(q.post(t, 2));
        EXPECT_EQ(q.dispatchAll(), 1u);
        EXPECT_EQ(c1, 1);
        EXPECT_EQ(c2, 2);

        q.clearObservers("obs.bcast");
        EXPECT_EQ(q.observerCount(t), 0u);
        EXPECT_TRUE(q.post(t, 3));
        EXPECT_EQ(q.dispatchAll(), 0u); // no observers -> fast no-op
        EXPECT_EQ(c2, 2);
    }

    // 2. EventQueueBroadcastFast: removeObserver(callback, userData) and clearObservers
    {
        EventQueueBroadcastFast<64> q;
        uint16_t t = q.registerTopic();
        int c1 = 0, c2 = 0;
        EXPECT_TRUE(q.addObserver(t, &incrementIntCb, &c1));
        EXPECT_TRUE(q.addObserver(t, &incrementIntCb, &c2));
        EXPECT_EQ(q.observerCount(t), 2u);

        EXPECT_TRUE(q.removeObserver(t, &incrementIntCb, &c1));
        EXPECT_FALSE(q.removeObserver(t, &incrementIntCb, &c1));
        EXPECT_EQ(q.observerCount(t), 1u);

        EXPECT_TRUE(q.post(t, 1));
        EXPECT_EQ(q.dispatchAll(), 1u);
        EXPECT_EQ(c1, 0);
        EXPECT_EQ(c2, 1);

        q.clearObservers(t);
        EXPECT_EQ(q.observerCount(t), 0u);
    }

    // 3. EventQueueBroadcastFastBatch: removeObserver(callback, userData) and clearObservers
    {
        EventQueueBroadcastFastBatch<64> q;
        uint16_t t = q.registerTopic();
        int c1 = 0, c2 = 0;
        EXPECT_TRUE(q.addObserver(t, &incrementIntCb, &c1));
        EXPECT_TRUE(q.addObserver(t, &incrementIntCb, &c2));
        EXPECT_EQ(q.observerCount(t), 2u);

        EXPECT_TRUE(q.removeObserver(t, &incrementIntCb, &c1));
        EXPECT_EQ(q.observerCount(t), 1u);

        EXPECT_TRUE(q.postBatched(t, 1));
        q.flushBatch();
        EXPECT_EQ(q.dispatchAll(), 1u);
        EXPECT_EQ(c1, 0);
        EXPECT_EQ(c2, 1);

        q.clearObservers(t);
        EXPECT_EQ(q.observerCount(t), 0u);
    }

    // 4. EventQueueEmulator: EmuTopics default priorities, collision avoidance, removeObserver
    {
        EventQueueEmulator<64, 64> q;
        EXPECT_EQ(q.getTopicPriority(EmuTopics::VBLANK), TopicPriority::Critical);
        EXPECT_EQ(q.getTopicPriority(EmuTopics::AUDIO_SYNC), TopicPriority::Critical);
        EXPECT_EQ(q.getTopicPriority(EmuTopics::FRAME_READY), TopicPriority::Critical);
        EXPECT_EQ(q.getTopicPriority(EmuTopics::CPU_TRACE), TopicPriority::Normal);
        EXPECT_EQ(q.getTopicPriority(EmuTopics::SCREENSHOT), TopicPriority::Normal);

        int vblankCount = 0, shotCount = 0;
        EXPECT_TRUE(q.addObserver(EmuTopics::VBLANK, &incrementIntCb, &vblankCount));
        EXPECT_TRUE(q.addObserver(EmuTopics::SCREENSHOT, &incrementIntCb, &shotCount));

        // Subsequent registerTopic() must start after EmuTopics::SCREENSHOT (8)
        uint16_t dynamicTopic = q.registerTopic(TopicPriority::Critical);
        EXPECT_GT(dynamicTopic, EmuTopics::SCREENSHOT);

        // Auto-routing post(EmuTopics::VBLANK, ...) must route to the fast queue
        EXPECT_TRUE(q.post(EmuTopics::VBLANK, 123));
        EXPECT_EQ(q.fastQueueSize(), 1u);
        EXPECT_EQ(q.bulkQueueSize(), 0u);
        EXPECT_TRUE(q.dispatchFast());
        EXPECT_EQ(vblankCount, 1);

        EXPECT_TRUE(q.removeObserver(EmuTopics::VBLANK, &incrementIntCb, &vblankCount));
        EXPECT_EQ(q.observerCount(EmuTopics::VBLANK), 0u);
        q.clearObservers(EmuTopics::SCREENSHOT);
        EXPECT_EQ(q.observerCount(EmuTopics::SCREENSHOT), 0u);
    }

    // 5. MessageCenterFast: removeObserver by ID and clearObservers
    {
        MessageCenterFast mc;
        mc.start();
        uint16_t t = mc.registerTopic("obs.mcfast");
        std::atomic<int> c1{0}, c2{0};
        uint64_t id1 = mc.addObserver(t, [&](uint16_t, const void*, size_t) {
            c1.fetch_add(1, std::memory_order_relaxed);
        });
        uint64_t id2 = mc.addObserver("obs.mcfast", [&](uint16_t, const void*, size_t) {
            c2.fetch_add(1, std::memory_order_relaxed);
        });
        EXPECT_EQ(mc.observerCount(t), 2u);

        EXPECT_TRUE(mc.removeObserver(t, id1));
        EXPECT_EQ(mc.observerCount(t), 1u);

        EXPECT_TRUE(mc.post(t, 42));
        mc.flush(1000);
        EXPECT_EQ(c1.load(), 0);
        EXPECT_EQ(c2.load(), 1);

        mc.clearObservers("obs.mcfast");
        EXPECT_EQ(mc.observerCount(t), 0u);
        EXPECT_TRUE(mc.post(t, 42));
        mc.flush(1000);
        mc.stop();
        EXPECT_EQ(c2.load(), 1);
        (void)id2;
    }
}
