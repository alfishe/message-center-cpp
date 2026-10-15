/**
 * @file comprehensive_benchmark.cpp
 * @brief Structured, Comprehensive Benchmarking Suite for message-center-cpp.
 *
 * Covers:
 * 1. Topic Registration & Lookup Latency / Throughput (Hit & Miss).
 * 2. Observer Addition & Removal Scaling (IDs vs Pointers vs Class Methods).
 * 3. Single-Threaded Post & Dispatch Latency & Throughput across all queue variants:
 *    - EventQueue (Classic single mutex)
 *    - EventQueueBroadcastFast (Lock-free MPMC, inline payload)
 *    - EventQueueBroadcastFast (Lock-free MPMC, refcounted managed payload)
 *    - EventQueueBroadcastFastBatch (Batched post and dispatch)
 *    - EventQueueEmulator (Dual priority queue: Critical vs Normal)
 * 4. Multi-Threaded Producer Scaling & Contention (1 to 64 threads).
 * 5. Fan-Out Scaling (1, 4, 16, 64 observers per topic).
 * 6. Latency Percentiles (P50, P90, P99, P99.9).
 */

#include <benchmark/benchmark.h>

#include "eventqueue.h"
#include "messagecenter.h"
#include "eventqueue_broadcast.h"
#include "eventqueue_fast.h"
#include "eventqueue_batch.h"
#include "eventqueue_emulator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

// Test payloads
struct SmallPayload32 {
    uint64_t id;
    uint64_t timestamp;
    uint64_t data1;
    uint64_t data2;
};

struct LargePayload256 {
    uint64_t id;
    char buffer[248];
};

} // namespace

// ============================================================================
// Suite 1: Topic Registration & Resolution
// ============================================================================

static void BM_Comprehensive_TopicRegister(benchmark::State& state) {
    EventQueueCUT queue;
    queue.init();
    int count = state.range(0);
    std::vector<std::string> names;
    names.reserve(count);
    for (int i = 0; i < count; i++) {
        names.push_back("topic_benchmark_" + std::to_string(i));
    }

    for (auto _ : state) {
        for (int i = 0; i < count; i++) {
            queue.RegisterTopic(names[i]);
        }
        queue.ClearTopics();
    }
    state.SetItemsProcessed(state.iterations() * count);
}
BENCHMARK(BM_Comprehensive_TopicRegister)->Arg(10)->Arg(100)->Arg(500);

static void BM_Comprehensive_TopicResolveHit(benchmark::State& state) {
    EventQueueCUT queue;
    queue.init();
    int count = state.range(0);
    std::vector<std::string> names;
    names.reserve(count);
    for (int i = 0; i < count; i++) {
        std::string n = "topic_hit_" + std::to_string(i);
        queue.RegisterTopic(n);
        names.push_back(n);
    }

    size_t idx = 0;
    for (auto _ : state) {
        benchmark::DoNotOptimize(queue.ResolveTopic(names[idx % count]));
        idx++;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Comprehensive_TopicResolveHit)->Arg(10)->Arg(100)->Arg(500);

static void BM_Comprehensive_TopicResolveMiss(benchmark::State& state) {
    EventQueueCUT queue;
    queue.init();
    for (int i = 0; i < 100; i++) {
        queue.RegisterTopic("topic_exist_" + std::to_string(i));
    }

    std::string missing = "topic_missing_xyz";
    for (auto _ : state) {
        benchmark::DoNotOptimize(queue.ResolveTopic(missing));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Comprehensive_TopicResolveMiss);

// ============================================================================
// Suite 2: Observer Registration & Removal
// ============================================================================

static void BM_Comprehensive_ObserverAddRemove(benchmark::State& state) {
    EventQueueCUT queue;
    queue.init();
    const std::string topic = "bench_obs_add_remove";
    queue.RegisterTopic(topic);

    auto callback = ObserverCallbackFunc([](int, Message*) {});

    for (auto _ : state) {
        state.PauseTiming();
        std::vector<uint64_t> ids;
        ids.reserve(state.range(0));
        state.ResumeTiming();

        for (int i = 0; i < state.range(0); i++) {
            ids.push_back(queue.AddObserver(topic, callback));
        }

        for (int i = 0; i < state.range(0); i++) {
            queue.RemoveObserverById(topic, ids[i]);
        }
    }
    state.SetItemsProcessed(state.iterations() * state.range(0) * 2);
}
BENCHMARK(BM_Comprehensive_ObserverAddRemove)->Arg(10)->Arg(100)->Arg(1000);

// ============================================================================
// Suite 3: Architecture Comparison (Single Producer, Single Consumer)
// ============================================================================

// 3.1 EventQueueBroadcastFast (Inline 32B)
static void BM_ArchCompare_BroadcastFast_Inline32(benchmark::State& state) {
    EventQueueBroadcastFast<65536> queue;
    uint16_t tid = queue.registerTopic();
    std::atomic<uint64_t> count{0};
    queue.addObserver(tid, [](uint16_t, const void*, size_t, void* ctx) {
        auto* c = static_cast<std::atomic<uint64_t>*>(ctx);
        c->fetch_add(1, std::memory_order_relaxed);
    }, &count);

    SmallPayload32 payload{1, 2, 3, 4};

    for (auto _ : state) {
        queue.post(tid, &payload, sizeof(payload));
        queue.dispatchOne();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ArchCompare_BroadcastFast_Inline32);

// 3.2 EventQueueBroadcastFast (Managed 256B RefCounted)
static void BM_ArchCompare_BroadcastFast_Managed256(benchmark::State& state) {
    EventQueueBroadcastFast<65536> queue;
    uint16_t tid = queue.registerTopic();
    std::atomic<uint64_t> count{0};
    queue.addObserver(tid, [](uint16_t, const void*, size_t, void* ctx) {
        auto* c = static_cast<std::atomic<uint64_t>*>(ctx);
        c->fetch_add(1, std::memory_order_relaxed);
    }, &count);

    LargePayload256 payload{1, {0}};

    for (auto _ : state) {
        queue.post(tid, &payload, sizeof(payload));
        queue.dispatchOne();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ArchCompare_BroadcastFast_Managed256);

// 3.3 EventQueueBroadcastFastBatch (Inline 32B, Batch 32)
static void BM_ArchCompare_BroadcastBatch_Inline32(benchmark::State& state) {
    EventQueueBroadcastFastBatch<65536> queue;
    uint16_t tid = queue.registerTopic();
    std::atomic<uint64_t> count{0};
    queue.addObserver(tid, [](uint16_t, const void*, size_t, void* ctx) {
        auto* c = static_cast<std::atomic<uint64_t>*>(ctx);
        c->fetch_add(1, std::memory_order_relaxed);
    }, &count);

    SmallPayload32 payload{1, 2, 3, 4};

    for (auto _ : state) {
        for (int i = 0; i < 32; i++) {
            queue.postBatched(tid, &payload, sizeof(payload));
        }
        queue.flushBatch();
        queue.dispatchBatch(32);
    }
    state.SetItemsProcessed(state.iterations() * 32);
}
BENCHMARK(BM_ArchCompare_BroadcastBatch_Inline32);

// 3.4 EventQueueEmulator (Dual Queue Priority)
static void BM_ArchCompare_Emulator_CriticalPriority(benchmark::State& state) {
    EventQueueEmulator<4096, 16384> queue;
    uint16_t critTopic = queue.registerTopic(TopicPriority::Critical);
    std::atomic<uint64_t> count{0};
    queue.addObserver(critTopic, [](uint16_t, const void*, size_t, void* ctx) {
        auto* c = static_cast<std::atomic<uint64_t>*>(ctx);
        c->fetch_add(1, std::memory_order_relaxed);
    }, &count);

    SmallPayload32 payload{1, 2, 3, 4};

    for (auto _ : state) {
        queue.postFast(critTopic, &payload, sizeof(payload));
        queue.dispatchFast();
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ArchCompare_Emulator_CriticalPriority);

// ============================================================================
// Suite 4: Fan-Out Scaling
// ============================================================================

static void BM_Comprehensive_FanOut_BroadcastFast(benchmark::State& state) {
    int numObservers = state.range(0);
    EventQueueBroadcastFast<65536> queue;
    uint16_t tid = queue.registerTopic();

    std::atomic<uint64_t> count{0};
    for (int i = 0; i < numObservers; i++) {
        queue.addObserver(tid, [](uint16_t, const void*, size_t, void* ctx) {
            auto* c = static_cast<std::atomic<uint64_t>*>(ctx);
            c->fetch_add(1, std::memory_order_relaxed);
        }, &count);
    }

    SmallPayload32 payload{1, 2, 3, 4};

    for (auto _ : state) {
        queue.post(tid, &payload, sizeof(payload));
        queue.dispatchOne();
    }
    state.SetItemsProcessed(state.iterations() * numObservers);
}
BENCHMARK(BM_Comprehensive_FanOut_BroadcastFast)->Arg(1)->Arg(4)->Arg(16);

// ============================================================================
// Suite 5: Emulator-Pattern Workloads
// Retro emulators typically post a burst of events during a frame (CPU/chipset
// emulation thread) and drain them at the frame boundary - often on the same
// thread, or handed to one consumer (audio/UI) thread.
// ============================================================================

static void emuCountCb(uint16_t, const void*, size_t, void* ctx) {
    ++*static_cast<uint64_t*>(ctx);
}

// Same-thread frame: N critical events (e.g. raster/audio/input) + N/8 bulk events, then drain
static void BM_Emulator_FrameBurst_SameThread(benchmark::State& state) {
    const int perFrame = static_cast<int>(state.range(0));
    EventQueueEmulator<4096, 16384> queue;
    uint16_t crit = queue.registerTopic(TopicPriority::Critical);
    uint16_t bulk = queue.registerTopic(TopicPriority::Normal);
    uint64_t count = 0;
    queue.addObserver(crit, emuCountCb, &count);
    queue.addObserver(bulk, emuCountCb, &count);

    SmallPayload32 payload{1, 2, 3, 4};
    for (auto _ : state) {
        for (int i = 0; i < perFrame; ++i) {
            queue.postFast(crit, &payload, sizeof(payload));
            if ((i & 7) == 0)
                queue.postBulk(bulk, &payload, sizeof(payload));
        }
        queue.dispatchAll();
    }
    benchmark::DoNotOptimize(count);
    state.SetItemsProcessed(state.iterations() * (perFrame + (perFrame + 7) / 8));
}
BENCHMARK(BM_Emulator_FrameBurst_SameThread)->Arg(64)->Arg(1024);

// Emulation thread produces, a separate consumer thread dispatches
static void BM_Emulator_ProducerConsumer(benchmark::State& state) {
    EventQueueEmulator<4096, 16384> queue;
    uint16_t crit = queue.registerTopic(TopicPriority::Critical);
    uint64_t count = 0;
    queue.addObserver(crit, emuCountCb, &count);

    std::atomic<bool> stop{false};
    std::thread consumer([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            if (queue.dispatchFast() == false)
                std::this_thread::yield();
        }
        while (queue.dispatchFast()) {
        }
    });

    SmallPayload32 payload{1, 2, 3, 4};
    for (auto _ : state) {
        queue.postFast(crit, &payload, sizeof(payload));
    }
    stop.store(true);
    consumer.join();
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Emulator_ProducerConsumer)->UseRealTime();

// Raw pool hot path (single thread acquire+release)
static void BM_ObjectPoolLockFree_AcquireRelease(benchmark::State& state) {
    ObjectPoolLockFree<SmallPayload32> pool(256, 4096);
    for (auto _ : state) {
        SmallPayload32* p = pool.acquire();
        benchmark::DoNotOptimize(p);
        pool.release(p);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ObjectPoolLockFree_AcquireRelease);


// Classic MessageCenter batch posting (amortizes lock & wakeup across a frame burst)
static void BM_MessageCenter_PostBatch(benchmark::State& state) {
    const size_t batchSize = static_cast<size_t>(state.range(0));
    auto& mc = MessageCenter::DefaultMessageCenter();
    int topicId = mc.RegisterTopic("bench.post.batch");
    std::atomic<size_t> delivered{0};
    mc.AddObserver("bench.post.batch", ObserverCallbackFunc([&](int, Message*) {
        delivered.fetch_add(1, std::memory_order_relaxed);
    }));

    std::vector<MessagePayload*> batch(batchSize, nullptr);
    for (auto _ : state) {
        mc.PostBatch(topicId, batch.data(), batchSize, false);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (delivered.load(std::memory_order_relaxed) < state.iterations() * batchSize &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    MessageCenter::DisposeDefaultMessageCenter();
    state.SetItemsProcessed(state.iterations() * batchSize);
}
BENCHMARK(BM_MessageCenter_PostBatch)->Arg(16)->Arg(64)->Arg(256)->Iterations(2000);
