#include "../src/eventqueue.h"
#include "../src/eventqueue_broadcast.h"
#include "../src/eventqueue_fast.h"
#include "../src/eventqueue_batch.h"
#include "../src/eventqueue_emulator.h"
#include "../src/mpmc_queue.h"
#include <algorithm>
#include <atomic>
#include <benchmark/benchmark.h>
#include <chrono>
#include <thread>
#include <vector>

// Real-world scenario: Emulator message passing
// - 256 topics (emulator instances)
// - 5 observers per topic (UI, logger, debugger, profiler, network)
// - 60 Hz update rate per instance
// - Multiple producer threads (emulator cores)
// - Multiple consumer threads (dispatch workers)

// Thread-local latency buffer for Fast queue benchmarks
static thread_local std::vector<int64_t> *tls_fastLatency = nullptr;

static double Percentile(const std::vector<int64_t> &sorted, double pct) {
  if (sorted.empty())
    return 0.0;
  size_t idx = static_cast<size_t>(pct * (sorted.size() - 1));
  return static_cast<double>(sorted[idx]);
}

static void ReportLatencyCounters(benchmark::State &state,
                                   std::vector<int64_t> &latenciesNs) {
  if (latenciesNs.empty())
    return;

  std::sort(latenciesNs.begin(), latenciesNs.end());
  auto us = [](double ns) { return ns / 1000.0; };

  state.counters["latency_min_us"] = us((double)latenciesNs.front());
  state.counters["latency_p50_us"] = us(Percentile(latenciesNs, 0.50));
  state.counters["latency_p90_us"] = us(Percentile(latenciesNs, 0.90));
  state.counters["latency_p99_us"] = us(Percentile(latenciesNs, 0.99));
  state.counters["latency_p999_us"] = us(Percentile(latenciesNs, 0.999));
  state.counters["latency_max_us"] = us((double)latenciesNs.back());

  double sum = 0;
  for (auto v : latenciesNs)
    sum += (double)v;
  state.counters["latency_mean_us"] = us(sum / latenciesNs.size());
  state.counters["latency_samples"] = (double)latenciesNs.size();
}

// Fast queue callback for latency capture
static void FastLatencyObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  if (tls_fastLatency && data && size >= sizeof(std::chrono::steady_clock::time_point)) {
    auto ts = *reinterpret_cast<const std::chrono::steady_clock::time_point *>(data);
    auto elapsed = std::chrono::steady_clock::now() - ts;
    tls_fastLatency->push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  }
}

static void FastNoopObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  benchmark::DoNotOptimize(tid);
}

// =============================================================================
// EventQueueBroadcastFast benchmarks (best general-purpose variant)
// =============================================================================

template <int PayloadSize = 32>
static void BM_BroadcastFast(benchmark::State &state) {
  EventQueueBroadcastFast<65536> queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;
  const int NUM_PRODUCERS = 2;
  const int NUM_CONSUMERS = 8;

  std::vector<uint16_t> topicIds;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topicIds.push_back(queue.registerTopic());
  }

  for (uint16_t id : topicIds) {
    queue.addObserver(id, FastLatencyObserver, nullptr);
    for (int j = 1; j < OBSERVERS_PER_TOPIC; j++) {
      queue.addObserver(id, FastNoopObserver, nullptr);
    }
  }

  std::atomic<int64_t> totalMessages{0};
  std::atomic<bool> running{true};
  std::vector<int64_t> allLatenciesNs;

  for (auto _ : state) {
    running = true;
    totalMessages = 0;
    std::vector<std::vector<int64_t>> perConsumerLatencies(NUM_CONSUMERS);

    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
      producers.emplace_back([&, i]() {
        int topicsPerProducer = NUM_TOPICS / NUM_PRODUCERS;
        int startTopic = i * topicsPerProducer;
        int endTopic = (i == NUM_PRODUCERS - 1) ? NUM_TOPICS : startTopic + topicsPerProducer;

        alignas(8) char payloadData[PayloadSize];

        for (int iter = 0; iter < 100; iter++) {
          for (int t = startTopic; t < endTopic; t++) {
            *reinterpret_cast<std::chrono::steady_clock::time_point *>(payloadData) =
                std::chrono::steady_clock::now();
            queue.post(topicIds[t], payloadData, PayloadSize);
            totalMessages++;
          }
        }
      });
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
      consumers.emplace_back([&, i]() {
        tls_fastLatency = &perConsumerLatencies[i];
        while (running || !queue.empty()) {
          if (!queue.dispatchOne()) {
            std::this_thread::yield();
          }
        }
        tls_fastLatency = nullptr;
      });
    }

    for (auto &t : producers) {
      t.join();
    }

    std::this_thread::sleep_for(std::chrono::microseconds(100));
    running = false;

    for (auto &t : consumers) {
      t.join();
    }

    for (auto &buf : perConsumerLatencies) {
      allLatenciesNs.insert(allLatenciesNs.end(), buf.begin(), buf.end());
    }
  }

  state.SetItemsProcessed(totalMessages.load());
  state.counters["payload_bytes"] = PayloadSize;
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

BENCHMARK_TEMPLATE(BM_BroadcastFast, 16)->Iterations(20)->Name("BM_Fast_16B");
BENCHMARK_TEMPLATE(BM_BroadcastFast, 32)->Iterations(20)->Name("BM_Fast_32B");
BENCHMARK_TEMPLATE(BM_BroadcastFast, 48)->Iterations(20)->Name("BM_Fast_48B");

// =============================================================================
// EventQueueBroadcastFastBatch benchmarks (best tail latency)
// =============================================================================

static void BatchLatencyObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  if (tls_fastLatency && data && size >= sizeof(std::chrono::steady_clock::time_point)) {
    auto ts = *reinterpret_cast<const std::chrono::steady_clock::time_point *>(data);
    auto elapsed = std::chrono::steady_clock::now() - ts;
    tls_fastLatency->push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  }
}

static void BatchNoopObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  benchmark::DoNotOptimize(tid);
}

template <int PayloadSize = 32>
static void BM_FastBatch_BatchDispatch(benchmark::State &state) {
  EventQueueBroadcastFastBatch<65536> queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;
  const int NUM_PRODUCERS = 2;
  const int NUM_CONSUMERS = 8;

  std::vector<uint16_t> topicIds;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topicIds.push_back(queue.registerTopic());
  }

  for (uint16_t id : topicIds) {
    queue.addObserver(id, BatchLatencyObserver, nullptr);
    for (int j = 1; j < OBSERVERS_PER_TOPIC; j++) {
      queue.addObserver(id, BatchNoopObserver, nullptr);
    }
  }

  std::atomic<int64_t> totalMessages{0};
  std::atomic<bool> running{true};
  std::vector<int64_t> allLatenciesNs;

  for (auto _ : state) {
    running = true;
    totalMessages = 0;
    std::vector<std::vector<int64_t>> perConsumerLatencies(NUM_CONSUMERS);

    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
      producers.emplace_back([&, i]() {
        int topicsPerProducer = NUM_TOPICS / NUM_PRODUCERS;
        int startTopic = i * topicsPerProducer;
        int endTopic = (i == NUM_PRODUCERS - 1) ? NUM_TOPICS : startTopic + topicsPerProducer;

        alignas(8) char payloadData[PayloadSize];

        for (int iter = 0; iter < 100; iter++) {
          for (int t = startTopic; t < endTopic; t++) {
            *reinterpret_cast<std::chrono::steady_clock::time_point *>(payloadData) =
                std::chrono::steady_clock::now();
            queue.post(topicIds[t], payloadData, PayloadSize);
            totalMessages++;
          }
        }
      });
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
      consumers.emplace_back([&, i]() {
        tls_fastLatency = &perConsumerLatencies[i];
        while (running || !queue.empty()) {
          if (queue.dispatchBatch(32) == 0) {
            std::this_thread::yield();
          }
        }
        tls_fastLatency = nullptr;
      });
    }

    for (auto &t : producers) {
      t.join();
    }

    std::this_thread::sleep_for(std::chrono::microseconds(100));
    running = false;

    for (auto &t : consumers) {
      t.join();
    }

    for (auto &buf : perConsumerLatencies) {
      allLatenciesNs.insert(allLatenciesNs.end(), buf.begin(), buf.end());
    }
  }

  state.SetItemsProcessed(totalMessages.load());
  state.counters["dispatch_mode"] = 1;
  state.counters["payload_bytes"] = PayloadSize;
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

BENCHMARK_TEMPLATE(BM_FastBatch_BatchDispatch, 32)->Iterations(20)->Name("BM_Batch_32B");

// =============================================================================
// EventQueueEmulator benchmarks (best for emulator workloads)
// =============================================================================

static void EmuLatencyObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  if (tls_fastLatency && data && size >= sizeof(std::chrono::steady_clock::time_point)) {
    auto ts = *reinterpret_cast<const std::chrono::steady_clock::time_point *>(data);
    auto elapsed = std::chrono::steady_clock::now() - ts;
    tls_fastLatency->push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  }
}

static void EmuNoopObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  benchmark::DoNotOptimize(tid);
}

// Emulator dual-queue benchmark - simulates mixed critical/bulk workload
static void BM_Emulator_DualQueue(benchmark::State &state) {
  EventQueueEmulator<4096, 16384> queue;

  // Critical topics (fast queue)
  uint16_t vblank = queue.registerTopic(TopicPriority::Critical);
  uint16_t audioSync = queue.registerTopic(TopicPriority::Critical);
  uint16_t inputKey = queue.registerTopic(TopicPriority::Critical);

  // Normal topics (bulk queue)
  uint16_t cpuTrace = queue.registerTopic(TopicPriority::Normal);
  uint16_t frameReady = queue.registerTopic(TopicPriority::Normal);

  // Add observers
  queue.addObserver(vblank, EmuLatencyObserver, nullptr);
  queue.addObserver(audioSync, EmuLatencyObserver, nullptr);
  queue.addObserver(inputKey, EmuLatencyObserver, nullptr);
  queue.addObserver(cpuTrace, EmuNoopObserver, nullptr);
  queue.addObserver(frameReady, EmuNoopObserver, nullptr);

  std::atomic<int64_t> totalMessages{0};
  std::atomic<bool> running{true};
  std::vector<int64_t> allLatenciesNs;

  for (auto _ : state) {
    running = true;
    totalMessages = 0;
    std::vector<int64_t> consumerLatencies;

    // Emulator thread - posts critical events at ~60Hz + audio at ~735Hz
    std::thread emulatorThread([&]() {
      alignas(8) char payload[32];

      for (int frame = 0; frame < 600; frame++) {
        // VBlank
        *reinterpret_cast<std::chrono::steady_clock::time_point *>(payload) =
            std::chrono::steady_clock::now();
        queue.postFast(vblank, payload, 32);
        totalMessages++;

        // Audio syncs per frame (~12 per frame for 44100Hz / 60Hz)
        for (int a = 0; a < 12; a++) {
          *reinterpret_cast<std::chrono::steady_clock::time_point *>(payload) =
              std::chrono::steady_clock::now();
          queue.postFast(audioSync, payload, 16);
          totalMessages++;
        }

        // Random input events
        if (frame % 10 == 0) {
          *reinterpret_cast<std::chrono::steady_clock::time_point *>(payload) =
              std::chrono::steady_clock::now();
          queue.postFast(inputKey, payload, 8);
          totalMessages++;
        }

        // Frame pointer (bulk)
        queue.postBulk(frameReady, payload, 32);
        totalMessages++;

        // CPU trace (bulk, higher volume)
        for (int t = 0; t < 100; t++) {
          queue.postBulk(cpuTrace, payload, 32);
          totalMessages++;
        }
      }
    });

    // Consumer thread - priority dispatch
    std::thread consumerThread([&]() {
      tls_fastLatency = &consumerLatencies;
      while (running || !queue.empty()) {
        if (queue.dispatchPriority() == 0) {
          std::this_thread::yield();
        }
      }
      tls_fastLatency = nullptr;
    });

    emulatorThread.join();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    running = false;
    consumerThread.join();

    allLatenciesNs.insert(allLatenciesNs.end(),
                          consumerLatencies.begin(), consumerLatencies.end());
  }

  state.SetItemsProcessed(totalMessages.load());
  state.counters["queue_type"] = 2;
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Single-queue baseline for emulator comparison
static void BM_Emulator_SingleQueue(benchmark::State &state) {
  EventQueueBroadcastFast<65536> queue;

  uint16_t vblank = queue.registerTopic();
  uint16_t audioSync = queue.registerTopic();
  uint16_t inputKey = queue.registerTopic();
  uint16_t cpuTrace = queue.registerTopic();
  uint16_t frameReady = queue.registerTopic();

  queue.addObserver(vblank, FastLatencyObserver, nullptr);
  queue.addObserver(audioSync, FastLatencyObserver, nullptr);
  queue.addObserver(inputKey, FastLatencyObserver, nullptr);
  queue.addObserver(cpuTrace, FastNoopObserver, nullptr);
  queue.addObserver(frameReady, FastNoopObserver, nullptr);

  std::atomic<int64_t> totalMessages{0};
  std::atomic<bool> running{true};
  std::vector<int64_t> allLatenciesNs;

  for (auto _ : state) {
    running = true;
    totalMessages = 0;
    std::vector<int64_t> consumerLatencies;

    std::thread emulatorThread([&]() {
      alignas(8) char payload[32];

      for (int frame = 0; frame < 600; frame++) {
        *reinterpret_cast<std::chrono::steady_clock::time_point *>(payload) =
            std::chrono::steady_clock::now();
        queue.post(vblank, payload, 32);
        totalMessages++;

        for (int a = 0; a < 12; a++) {
          *reinterpret_cast<std::chrono::steady_clock::time_point *>(payload) =
              std::chrono::steady_clock::now();
          queue.post(audioSync, payload, 16);
          totalMessages++;
        }

        if (frame % 10 == 0) {
          *reinterpret_cast<std::chrono::steady_clock::time_point *>(payload) =
              std::chrono::steady_clock::now();
          queue.post(inputKey, payload, 8);
          totalMessages++;
        }

        queue.post(frameReady, payload, 32);
        totalMessages++;

        for (int t = 0; t < 100; t++) {
          queue.post(cpuTrace, payload, 32);
          totalMessages++;
        }
      }
    });

    std::thread consumerThread([&]() {
      tls_fastLatency = &consumerLatencies;
      while (running || !queue.empty()) {
        if (!queue.dispatchOne()) {
          std::this_thread::yield();
        }
      }
      tls_fastLatency = nullptr;
    });

    emulatorThread.join();
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    running = false;
    consumerThread.join();

    allLatenciesNs.insert(allLatenciesNs.end(),
                          consumerLatencies.begin(), consumerLatencies.end());
  }

  state.SetItemsProcessed(totalMessages.load());
  state.counters["queue_type"] = 1;
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Emulator workload comparison
BENCHMARK(BM_Emulator_SingleQueue)->Iterations(10)->Name("BM_Emu_SingleQueue");
BENCHMARK(BM_Emulator_DualQueue)->Iterations(10)->Name("BM_Emu_DualQueue");

BENCHMARK_MAIN();
