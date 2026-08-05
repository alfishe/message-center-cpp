// MessageCenter Benchmarks
// Tests latency and throughput for common use cases

#include <benchmark/benchmark.h>
#include "messagecenter.h"
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>

// Simple payload types for benchmarking
struct SmallPayload : public MessagePayload {
    uint32_t id;
    uint32_t value;
    SmallPayload(uint32_t i, uint32_t v) : id(i), value(v) {}
};

struct MediumPayload : public MessagePayload {
    uint32_t id;
    char data[64];
    MediumPayload(uint32_t i) : id(i) { memset(data, 0, sizeof(data)); }
};

struct LargePayload : public MessagePayload {
    uint32_t id;
    char data[1024];
    LargePayload(uint32_t i) : id(i) { memset(data, 0, sizeof(data)); }
};

// Benchmark: Post throughput (fire-and-forget)
static void BM_MessageCenter_Post_Small(benchmark::State& state) {
    auto& mc = MessageCenter::DefaultMessageCenter();
    int topicId = mc.RegisterTopic("bench.small");

    std::atomic<int> count{0};
    mc.AddObserver("bench.small", ObserverCallbackFunc([&count](int id, Message* msg) {
        count++;
    }));

    for (auto _ : state) {
        mc.Post(topicId, new SmallPayload(1, 42));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MessageCenter::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MessageCenter_Post_Small)->Iterations(100000);

// Benchmark: Post with medium payload
static void BM_MessageCenter_Post_Medium(benchmark::State& state) {
    auto& mc = MessageCenter::DefaultMessageCenter();
    int topicId = mc.RegisterTopic("bench.medium");

    std::atomic<int> count{0};
    mc.AddObserver("bench.medium", ObserverCallbackFunc([&count](int id, Message* msg) {
        count++;
    }));

    for (auto _ : state) {
        mc.Post(topicId, new MediumPayload(1));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MessageCenter::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MessageCenter_Post_Medium)->Iterations(100000);

// Benchmark: Post with large payload
static void BM_MessageCenter_Post_Large(benchmark::State& state) {
    auto& mc = MessageCenter::DefaultMessageCenter();
    int topicId = mc.RegisterTopic("bench.large");

    std::atomic<int> count{0};
    mc.AddObserver("bench.large", ObserverCallbackFunc([&count](int id, Message* msg) {
        count++;
    }));

    for (auto _ : state) {
        mc.Post(topicId, new LargePayload(1));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MessageCenter::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MessageCenter_Post_Large)->Iterations(100000);

// Benchmark: Multiple observers (fan-out)
static void BM_MessageCenter_FanOut(benchmark::State& state) {
    const int numObservers = state.range(0);

    auto& mc = MessageCenter::DefaultMessageCenter();
    int topicId = mc.RegisterTopic("bench.fanout");

    std::atomic<int> count{0};
    for (int i = 0; i < numObservers; i++) {
        mc.AddObserver("bench.fanout", ObserverCallbackFunc([&count](int id, Message* msg) {
            count++;
        }));
    }

    for (auto _ : state) {
        mc.Post(topicId, new SmallPayload(1, 42));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MessageCenter::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations() * numObservers);
}
BENCHMARK(BM_MessageCenter_FanOut)->Arg(1)->Arg(2)->Arg(4)->Arg(8)->Arg(16);

// Benchmark: Multiple topics
static void BM_MessageCenter_MultiTopic(benchmark::State& state) {
    const int numTopics = state.range(0);

    auto& mc = MessageCenter::DefaultMessageCenter();
    std::vector<int> topicIds;

    std::atomic<int> count{0};
    for (int i = 0; i < numTopics; i++) {
        std::string topicName = "bench.topic" + std::to_string(i);
        topicIds.push_back(mc.RegisterTopic(topicName));
        mc.AddObserver(topicName, ObserverCallbackFunc([&count](int id, Message* msg) {
            count++;
        }));
    }

    int idx = 0;
    for (auto _ : state) {
        mc.Post(topicIds[idx % numTopics], new SmallPayload(1, 42));
        idx++;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    MessageCenter::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MessageCenter_MultiTopic)->Arg(4)->Arg(16)->Arg(64)->Arg(256);

// Benchmark: Multi-producer (concurrent posts)
static void BM_MessageCenter_MultiProducer(benchmark::State& state) {
    const int numThreads = state.range(0);
    const int msgsPerThread = 10000;

    auto& mc = MessageCenter::DefaultMessageCenter();
    int topicId = mc.RegisterTopic("bench.multiproducer");

    std::atomic<int> count{0};
    mc.AddObserver("bench.multiproducer", ObserverCallbackFunc([&count](int id, Message* msg) {
        count++;
    }));

    for (auto _ : state) {
        std::vector<std::thread> threads;

        for (int t = 0; t < numThreads; t++) {
            threads.emplace_back([&mc, topicId, msgsPerThread]() {
                for (int i = 0; i < msgsPerThread; i++) {
                    mc.Post(topicId, new SmallPayload(i, 42));
                }
            });
        }

        for (auto& t : threads) {
            t.join();
        }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    MessageCenter::DisposeDefaultMessageCenter();

    state.SetItemsProcessed(state.iterations() * numThreads * msgsPerThread);
}
BENCHMARK(BM_MessageCenter_MultiProducer)->Arg(2)->Arg(4)->Arg(8)->Unit(benchmark::kMillisecond);

// Benchmark: String-based topic resolution
static void BM_MessageCenter_TopicResolution(benchmark::State& state) {
    auto& mc = MessageCenter::DefaultMessageCenter();

    for (int i = 0; i < 100; i++) {
        mc.RegisterTopic("topic.category" + std::to_string(i));
    }

    for (auto _ : state) {
        benchmark::DoNotOptimize(mc.ResolveTopic("topic.category50"));
    }

    MessageCenter::DisposeDefaultMessageCenter();
}
BENCHMARK(BM_MessageCenter_TopicResolution);

// Benchmark: End-to-end latency (post -> dispatch -> callback)
static void BM_MessageCenter_Latency(benchmark::State& state) {
    auto& mc = MessageCenter::DefaultMessageCenter();
    int topicId = mc.RegisterTopic("bench.latency");

    std::atomic<bool> received{false};
    std::atomic<int64_t> latencyNs{0};

    mc.AddObserver("bench.latency", ObserverCallbackFunc([&received, &latencyNs](int id, Message* msg) {
        auto* payload = static_cast<SmallPayload*>(msg->obj);
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        latencyNs = now - payload->value;
        received = true;
    }));

    std::vector<int64_t> latencies;
    latencies.reserve(state.max_iterations);

    for (auto _ : state) {
        received = false;
        auto start = std::chrono::steady_clock::now().time_since_epoch().count();
        mc.Post(topicId, new SmallPayload(1, static_cast<uint32_t>(start)));

        while (!received) {
            std::this_thread::yield();
        }
        latencies.push_back(latencyNs);
    }

    if (!latencies.empty()) {
        std::sort(latencies.begin(), latencies.end());
        state.counters["p50_us"] = latencies[latencies.size() / 2] / 1000.0;
        state.counters["p99_us"] = latencies[latencies.size() * 99 / 100] / 1000.0;
    }

    MessageCenter::DisposeDefaultMessageCenter();
}
BENCHMARK(BM_MessageCenter_Latency)->Iterations(10000);
