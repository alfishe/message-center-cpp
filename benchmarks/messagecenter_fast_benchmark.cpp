// MessageCenterFast vs MessageCenter Benchmarks
// Compares lock-free variant with original mutex-based

#include <benchmark/benchmark.h>
#include "messagecenter.h"
#include "messagecenter_fast.h"
#include <atomic>
#include <thread>
#include <vector>

MessageCenterFast* MessageCenterFast::s_instance = nullptr;

// Small payload for fair comparison
struct BenchPayload {
    uint32_t id;
    uint32_t value;
    char data[24];
};

// ============================================================================
// Original MessageCenter Benchmarks
// ============================================================================

static void BM_Original_Post_Small(benchmark::State& state) {
    auto& mc = MessageCenter::DefaultMessageCenter();
    int topicId = mc.RegisterTopic("bench.original.small");

    std::atomic<int> count{0};
    mc.AddObserver("bench.original.small", ObserverCallbackFunc([&count](int id, Message* msg) {
        count++;
    }));

    BenchPayload payload{1, 42, {}};

    for (auto _ : state) {
        mc.Post(topicId, nullptr, false);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MessageCenter::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Original_Post_Small)->Iterations(100000);

static void BM_Original_Latency(benchmark::State& state) {
    auto& mc = MessageCenter::DefaultMessageCenter();
    int topicId = mc.RegisterTopic("bench.original.latency");

    std::atomic<bool> received{false};
    std::atomic<int64_t> receiveTime{0};

    mc.AddObserver("bench.original.latency", ObserverCallbackFunc([&](int id, Message* msg) {
        receiveTime = std::chrono::steady_clock::now().time_since_epoch().count();
        received = true;
    }));

    std::vector<int64_t> latencies;
    latencies.reserve(state.max_iterations);

    for (auto _ : state) {
        received = false;
        auto start = std::chrono::steady_clock::now().time_since_epoch().count();
        mc.Post(topicId, nullptr, false);

        while (!received) {
            std::this_thread::yield();
        }
        latencies.push_back(receiveTime - start);
    }

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        state.counters["p50_us"] = latencies[latencies.size() / 2] / 1000.0;
        state.counters["p99_us"] = latencies[latencies.size() * 99 / 100] / 1000.0;
    }

    MessageCenter::DisposeDefaultMessageCenter();
}
BENCHMARK(BM_Original_Latency)->Iterations(10000);

// ============================================================================
// MessageCenterFast Benchmarks
// ============================================================================

static void BM_Fast_Post_Small(benchmark::State& state) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();
    uint16_t topicId = mc.registerTopic("bench.fast.small");

    std::atomic<int> count{0};
    mc.addObserver(topicId, [&count](uint16_t t, const void* data, size_t size) {
        count++;
    });

    BenchPayload payload{1, 42, {}};

    for (auto _ : state) {
        mc.post(topicId, &payload, sizeof(payload));
    }

    mc.flush();
    MessageCenterFast::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Fast_Post_Small)->Iterations(100000);

static void BM_Fast_Post_Large(benchmark::State& state) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();
    uint16_t topicId = mc.registerTopic("bench.fast.large");

    std::atomic<int> count{0};
    mc.addObserver(topicId, [&count](uint16_t t, const void* data, size_t size) {
        count++;
    });

    char largeData[1024];
    memset(largeData, 0xAB, sizeof(largeData));

    for (auto _ : state) {
        mc.post(topicId, largeData, sizeof(largeData));
    }

    mc.flush();
    MessageCenterFast::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Fast_Post_Large)->Iterations(100000);

static void BM_Fast_Latency(benchmark::State& state) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();
    uint16_t topicId = mc.registerTopic("bench.fast.latency");

    std::atomic<bool> received{false};
    std::atomic<int64_t> receiveTime{0};

    mc.addObserver(topicId, [&](uint16_t t, const void* data, size_t size) {
        receiveTime = std::chrono::steady_clock::now().time_since_epoch().count();
        received = true;
    });

    std::vector<int64_t> latencies;
    latencies.reserve(state.max_iterations);

    BenchPayload payload{1, 42, {}};

    for (auto _ : state) {
        received = false;
        auto start = std::chrono::steady_clock::now().time_since_epoch().count();
        mc.post(topicId, &payload, sizeof(payload));

        while (!received) {
            std::this_thread::yield();
        }
        latencies.push_back(receiveTime - start);
    }

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        state.counters["p50_us"] = latencies[latencies.size() / 2] / 1000.0;
        state.counters["p99_us"] = latencies[latencies.size() * 99 / 100] / 1000.0;
    }

    MessageCenterFast::DisposeDefaultMessageCenter();
}
BENCHMARK(BM_Fast_Latency)->Iterations(10000);

static void BM_Fast_MultiProducer(benchmark::State& state) {
    const int numThreads = state.range(0);
    const int msgsPerThread = 10000;

    auto& mc = MessageCenterFast::DefaultMessageCenter();
    uint16_t topicId = mc.registerTopic("bench.fast.multiproducer");

    std::atomic<int> count{0};
    mc.addObserver(topicId, [&count](uint16_t t, const void* data, size_t size) {
        count++;
    });

    BenchPayload payload{1, 42, {}};

    for (auto _ : state) {
        std::vector<std::thread> threads;

        for (int t = 0; t < numThreads; t++) {
            threads.emplace_back([&mc, topicId, &payload, msgsPerThread]() {
                for (int i = 0; i < msgsPerThread; i++) {
                    mc.post(topicId, &payload, sizeof(payload));
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }
    }

    mc.flush();
    MessageCenterFast::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations() * numThreads * msgsPerThread);
}
BENCHMARK(BM_Fast_MultiProducer)->Arg(2)->Arg(4)->Arg(8)->Unit(benchmark::kMillisecond);

static void BM_Fast_FanOut(benchmark::State& state) {
    const int numObservers = state.range(0);

    auto& mc = MessageCenterFast::DefaultMessageCenter();
    uint16_t topicId = mc.registerTopic("bench.fast.fanout");

    std::atomic<int> count{0};
    for (int i = 0; i < numObservers; i++) {
        mc.addObserver(topicId, [&count](uint16_t t, const void* data, size_t size) {
            count++;
        });
    }

    BenchPayload payload{1, 42, {}};

    for (auto _ : state) {
        mc.post(topicId, &payload, sizeof(payload));
    }

    mc.flush();
    MessageCenterFast::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations() * numObservers);
}
BENCHMARK(BM_Fast_FanOut)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16);

// ============================================================================
// Head-to-Head Comparison
// ============================================================================

static void BM_Compare_Original_Throughput(benchmark::State& state) {
    auto& mc = MessageCenter::DefaultMessageCenter();
    int topicId = mc.RegisterTopic("bench.compare.original");

    std::atomic<int> count{0};
    mc.AddObserver("bench.compare.original", ObserverCallbackFunc([&count](int id, Message* msg) {
        count++;
    }));

    for (auto _ : state) {
        mc.Post(topicId, nullptr, false);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    state.counters["delivered"] = count.load();
    MessageCenter::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Compare_Original_Throughput)->Iterations(500000);

static void BM_Compare_Fast_Throughput(benchmark::State& state) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();
    uint16_t topicId = mc.registerTopic("bench.compare.fast");

    std::atomic<int> count{0};
    mc.addObserver(topicId, [&count](uint16_t t, const void* data, size_t size) {
        count++;
    });

    int value = 42;
    for (auto _ : state) {
        mc.post(topicId, &value, sizeof(value));
    }

    mc.flush();
    state.counters["delivered"] = count.load();
    MessageCenterFast::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Compare_Fast_Throughput)->Iterations(500000);
