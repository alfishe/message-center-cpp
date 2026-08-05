#include "../src/eventqueue.h"
#include "../src/eventqueue_broadcast.h"
#include "../src/eventqueue_broadcast_fast.h"
#include "../src/eventqueue_broadcast_fast_tl.h"
#include "../src/eventqueue_broadcast_fast_batch.h"
#include "../src/eventqueue_broadcast_fast_prefetch.h"
#include "../src/eventqueue_emulator.h"
#include "../src/eventqueue_lockfree.h"
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

// Payload carrying the Post() timestamp, used by the latency benchmarks
// below to measure end-to-end Post -> Dispatch(observer) time.
struct LatencyPayload : public MessagePayload {
  std::chrono::steady_clock::time_point postTime;
};

// Thread-local sample buffer. Each producing/consuming thread sets this to
// its own buffer so LatencyCaptureObserver can record without locking.
static thread_local std::vector<int64_t> *tls_latencyBuffer = nullptr;

static void LatencyCaptureObserver(int id, Message *msg) {
  if (!tls_latencyBuffer)
    return;

  auto *payload = static_cast<LatencyPayload *>(msg->obj);
  auto elapsed = std::chrono::steady_clock::now() - payload->postTime;
  tls_latencyBuffer->push_back(
      std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
}

// Stand-in for the other 4 subscribers (UI, logger, debugger, network) that
// don't participate in latency capture. A free function (rather than a
// capture-less lambda) avoids ambiguity between the ObserverCallback and
// ObserverCallbackFunc AddObserver() overloads.
static void NoopObserver(int id, Message *msg) {
  (void)msg;
  benchmark::DoNotOptimize(id);
}

// sorted[] must already be sorted ascending
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

static void BM_RealWorld_SingleThread(benchmark::State &state) {
  EventQueueCUT queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;

  // Setup topics
  std::vector<std::string> topics;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topics.push_back("emulator_" + std::to_string(i));
  }

  // Register observers (simulating UI, logger, debugger, etc.)
  std::atomic<int> callbackCount{0};
  for (const auto &topic : topics) {
    for (int i = 0; i < OBSERVERS_PER_TOPIC; i++) {
      queue.AddObserver(topic, [&callbackCount](int id, Message *msg) {
        callbackCount++;
        // Simulate minimal work
        benchmark::DoNotOptimize(callbackCount.load());
      });
    }
  }

  int64_t totalMessages = 0;

  for (auto _ : state) {
    // Post messages (simulate 60 Hz × 256 instances = 15,360 events/sec)
    for (const auto &topic : topics) {
      queue.Post(topic, nullptr);
      totalMessages++;
    }

    // Dispatch all messages
    while (true) {
      Message *msg = queue.GetQueueMessage();
      if (!msg)
        break;

      queue.Dispatch(msg->tid, msg);
    }
  }

  state.SetItemsProcessed(totalMessages);
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages, benchmark::Counter::kIsRate);
  state.counters["callbacks/sec"] =
      benchmark::Counter(callbackCount.load(), benchmark::Counter::kIsRate);
}

static void BM_RealWorld_MultiThread_Producers(benchmark::State &state) {
  EventQueueCUT queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;
  const int NUM_PRODUCERS = state.range(0);

  // Setup topics
  std::vector<std::string> topics;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topics.push_back("emulator_" + std::to_string(i));
  }

  // Register observers
  std::atomic<int> callbackCount{0};
  for (const auto &topic : topics) {
    for (int i = 0; i < OBSERVERS_PER_TOPIC; i++) {
      queue.AddObserver(topic, [&callbackCount](int id, Message *msg) {
        callbackCount++;
        benchmark::DoNotOptimize(callbackCount.load());
      });
    }
  }

  std::atomic<int64_t> totalMessages{0};
  std::atomic<bool> running{true};

  for (auto _ : state) {
    running = true;

    // Producer threads (simulate multiple emulator cores)
    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
      producers.emplace_back([&, i]() {
        int topicsPerProducer = NUM_TOPICS / NUM_PRODUCERS;
        int startTopic = i * topicsPerProducer;
        int endTopic = (i == NUM_PRODUCERS - 1)
                           ? NUM_TOPICS
                           : startTopic + topicsPerProducer;

        for (int iter = 0; iter < 10; iter++) {
          for (int t = startTopic; t < endTopic; t++) {
            queue.Post(topics[t], nullptr);
            totalMessages++;
          }
        }
      });
    }

    // Consumer thread (dispatch worker)
    std::thread consumer([&]() {
      while (running) {
        Message *msg = queue.GetQueueMessage();
        if (msg) {
          queue.Dispatch(msg->tid, msg);
        } else {
          std::this_thread::yield();
        }
      }
    });

    // Wait for producers
    for (auto &t : producers) {
      t.join();
    }

    // Drain queue
    while (true) {
      Message *msg = queue.GetQueueMessage();
      if (!msg)
        break;
      queue.Dispatch(msg->tid, msg);
    }

    running = false;
    consumer.join();
  }

  state.SetItemsProcessed(totalMessages.load());
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  state.counters["callbacks/sec"] =
      benchmark::Counter(callbackCount.load(), benchmark::Counter::kIsRate);
}

static void BM_RealWorld_MultiThread_Full(benchmark::State &state) {
  EventQueueCUT queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;
  const int NUM_PRODUCERS = 8; // 8 emulator cores
  const int NUM_CONSUMERS = 4; // 4 dispatch workers

  // Setup topics
  std::vector<std::string> topics;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topics.push_back("emulator_" + std::to_string(i));
  }

  // Register observers
  std::atomic<int> callbackCount{0};
  for (const auto &topic : topics) {
    for (int i = 0; i < OBSERVERS_PER_TOPIC; i++) {
      queue.AddObserver(topic, [&callbackCount](int id, Message *msg) {
        callbackCount++;
        benchmark::DoNotOptimize(callbackCount.load());
      });
    }
  }

  std::atomic<int64_t> totalMessages{0};
  std::atomic<bool> running{true};

  for (auto _ : state) {
    running = true;
    callbackCount = 0;
    totalMessages = 0;

    // Producer threads
    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
      producers.emplace_back([&, i]() {
        int topicsPerProducer = NUM_TOPICS / NUM_PRODUCERS;
        int startTopic = i * topicsPerProducer;
        int endTopic = (i == NUM_PRODUCERS - 1)
                           ? NUM_TOPICS
                           : startTopic + topicsPerProducer;

        for (int iter = 0; iter < 100; iter++) {
          for (int t = startTopic; t < endTopic; t++) {
            queue.Post(topics[t], nullptr);
            totalMessages++;
          }
        }
      });
    }

    // Consumer threads
    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
      consumers.emplace_back([&]() {
        while (running) {
          Message *msg = queue.GetQueueMessage();
          if (msg) {
            queue.Dispatch(msg->tid, msg);
          } else {
            std::this_thread::yield();
          }
        }
      });
    }

    // Wait for producers
    for (auto &t : producers) {
      t.join();
    }

    // Drain queue
    while (true) {
      Message *msg = queue.GetQueueMessage();
      if (!msg)
        break;
      queue.Dispatch(msg->tid, msg);
    }

    running = false;
    for (auto &t : consumers) {
      t.join();
    }
  }

  state.SetItemsProcessed(totalMessages.load());
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  state.counters["callbacks/sec"] =
      benchmark::Counter(callbackCount.load(), benchmark::Counter::kIsRate);
  state.counters["throughput_vs_requirement"] = benchmark::Counter(
      totalMessages.load() / 15360.0, benchmark::Counter::kAvgThreads);
}

static void BM_RealWorld_60Hz_Simulation(benchmark::State &state) {
  EventQueueCUT queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;

  // Setup topics
  std::vector<std::string> topics;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topics.push_back("emulator_" + std::to_string(i));
  }

  // Register observers
  std::atomic<int> callbackCount{0};
  for (const auto &topic : topics) {
    for (int i = 0; i < OBSERVERS_PER_TOPIC; i++) {
      queue.AddObserver(topic, [&callbackCount](int id, Message *msg) {
        callbackCount++;
        benchmark::DoNotOptimize(callbackCount.load());
      });
    }
  }

  int64_t totalMessages = 0;

  for (auto _ : state) {
    // Simulate one frame (1/60 second) of all 256 instances
    for (const auto &topic : topics) {
      queue.Post(topic, nullptr);
      totalMessages++;
    }

    // Dispatch all messages for this frame
    int dispatched = 0;
    while (dispatched < NUM_TOPICS) {
      Message *msg = queue.GetQueueMessage();
      if (msg) {
        queue.Dispatch(msg->tid, msg);
        dispatched++;
      }
    }
  }

  state.SetItemsProcessed(totalMessages);
  state.counters["frames/sec"] =
      benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages, benchmark::Counter::kIsRate);
  state.counters["callbacks/sec"] =
      benchmark::Counter(callbackCount.load(), benchmark::Counter::kIsRate);
}

// Measures end-to-end Post() -> Dispatch(observer) latency, single-threaded.
// One observer per topic timestamps itself; the other 4 simulate subscriber
// work as in the throughput benchmarks above.
static void BM_RealWorld_Latency_SingleThread(benchmark::State &state) {
  EventQueueCUT queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;

  std::vector<std::string> topics;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topics.push_back("emulator_" + std::to_string(i));
  }

  for (const auto &topic : topics) {
    queue.AddObserver(topic, LatencyCaptureObserver);
    for (int i = 1; i < OBSERVERS_PER_TOPIC; i++) {
      queue.AddObserver(topic, NoopObserver);
    }
  }

  std::vector<int64_t> latenciesNs;
  latenciesNs.reserve(200000);
  tls_latencyBuffer = &latenciesNs;

  int64_t totalMessages = 0;

  for (auto _ : state) {
    for (const auto &topic : topics) {
      auto *payload = new LatencyPayload();
      payload->postTime = std::chrono::steady_clock::now();
      queue.Post(topic, payload, /*autoCleanupPayload=*/true);
      totalMessages++;
    }

    while (true) {
      Message *msg = queue.GetQueueMessage();
      if (!msg)
        break;
      queue.Dispatch(msg->tid, msg);
    }
  }

  tls_latencyBuffer = nullptr;

  state.SetItemsProcessed(totalMessages);
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages, benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, latenciesNs);
}

// Measures end-to-end Post() -> Dispatch(observer) latency under the same
// 8-producer / 4-consumer contention as BM_RealWorld_MultiThread_Full.
// Each consumer thread owns its own sample buffer (via tls_latencyBuffer)
// so latency capture never adds cross-thread lock contention on the hot path.
static void BM_RealWorld_Latency_MultiThread_Full(benchmark::State &state) {
  EventQueueCUT queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;
  const int NUM_PRODUCERS = 8;
  const int NUM_CONSUMERS = 4;

  std::vector<std::string> topics;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topics.push_back("emulator_" + std::to_string(i));
  }

  for (const auto &topic : topics) {
    queue.AddObserver(topic, LatencyCaptureObserver);
    for (int i = 1; i < OBSERVERS_PER_TOPIC; i++) {
      queue.AddObserver(topic, NoopObserver);
    }
  }

  std::atomic<int64_t> totalMessages{0};
  std::atomic<bool> running{true};
  std::vector<int64_t> allLatenciesNs;

  for (auto _ : state) {
    running = true;
    totalMessages = 0;

    std::vector<std::vector<int64_t>> perConsumerLatencies(NUM_CONSUMERS);
    std::vector<int64_t> drainLatencies;

    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
      producers.emplace_back([&, i]() {
        int topicsPerProducer = NUM_TOPICS / NUM_PRODUCERS;
        int startTopic = i * topicsPerProducer;
        int endTopic = (i == NUM_PRODUCERS - 1)
                           ? NUM_TOPICS
                           : startTopic + topicsPerProducer;

        for (int iter = 0; iter < 100; iter++) {
          for (int t = startTopic; t < endTopic; t++) {
            auto *payload = new LatencyPayload();
            payload->postTime = std::chrono::steady_clock::now();
            queue.Post(topics[t], payload, /*autoCleanupPayload=*/true);
            totalMessages++;
          }
        }
      });
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
      consumers.emplace_back([&, i]() {
        tls_latencyBuffer = &perConsumerLatencies[i];
        while (running) {
          Message *msg = queue.GetQueueMessage();
          if (msg) {
            queue.Dispatch(msg->tid, msg);
          } else {
            std::this_thread::yield();
          }
        }
        tls_latencyBuffer = nullptr;
      });
    }

    for (auto &t : producers) {
      t.join();
    }

    tls_latencyBuffer = &drainLatencies;
    while (true) {
      Message *msg = queue.GetQueueMessage();
      if (!msg)
        break;
      queue.Dispatch(msg->tid, msg);
    }
    tls_latencyBuffer = nullptr;

    running = false;
    for (auto &t : consumers) {
      t.join();
    }

    for (auto &buf : perConsumerLatencies) {
      allLatenciesNs.insert(allLatenciesNs.end(), buf.begin(), buf.end());
    }
    allLatenciesNs.insert(allLatenciesNs.end(), drainLatencies.begin(),
                           drainLatencies.end());
  }

  state.SetItemsProcessed(totalMessages.load());
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Lock-free queue variant: single-threaded latency
static void BM_LockFree_Latency_SingleThread(benchmark::State &state) {
  EventQueueLockFreeCUT<65536> queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;

  std::vector<std::string> topics;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topics.push_back("emulator_" + std::to_string(i));
  }

  for (const auto &topic : topics) {
    queue.AddObserver(topic, LatencyCaptureObserver);
    for (int i = 1; i < OBSERVERS_PER_TOPIC; i++) {
      queue.AddObserver(topic, NoopObserver);
    }
  }

  std::vector<int64_t> latenciesNs;
  latenciesNs.reserve(200000);
  tls_latencyBuffer = &latenciesNs;

  int64_t totalMessages = 0;

  for (auto _ : state) {
    for (const auto &topic : topics) {
      auto *payload = new LatencyPayload();
      payload->postTime = std::chrono::steady_clock::now();
      queue.Post(topic, payload, /*autoCleanupPayload=*/true);
      totalMessages++;
    }

    while (true) {
      Message *msg = queue.GetQueueMessage();
      if (!msg)
        break;
      queue.Dispatch(msg->tid, msg);
    }
  }

  tls_latencyBuffer = nullptr;

  state.SetItemsProcessed(totalMessages);
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages, benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, latenciesNs);
}

// Lock-free queue variant: multi-threaded latency (8P + 4C)
static void BM_LockFree_Latency_MultiThread_Full(benchmark::State &state) {
  EventQueueLockFreeCUT<65536> queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;
  const int NUM_PRODUCERS = 8;
  const int NUM_CONSUMERS = 4;

  std::vector<std::string> topics;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topics.push_back("emulator_" + std::to_string(i));
  }

  for (const auto &topic : topics) {
    queue.AddObserver(topic, LatencyCaptureObserver);
    for (int i = 1; i < OBSERVERS_PER_TOPIC; i++) {
      queue.AddObserver(topic, NoopObserver);
    }
  }

  std::atomic<int64_t> totalMessages{0};
  std::atomic<bool> running{true};
  std::vector<int64_t> allLatenciesNs;

  for (auto _ : state) {
    running = true;
    totalMessages = 0;

    std::vector<std::vector<int64_t>> perConsumerLatencies(NUM_CONSUMERS);
    std::vector<int64_t> drainLatencies;

    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
      producers.emplace_back([&, i]() {
        int topicsPerProducer = NUM_TOPICS / NUM_PRODUCERS;
        int startTopic = i * topicsPerProducer;
        int endTopic = (i == NUM_PRODUCERS - 1)
                           ? NUM_TOPICS
                           : startTopic + topicsPerProducer;

        for (int iter = 0; iter < 100; iter++) {
          for (int t = startTopic; t < endTopic; t++) {
            auto *payload = new LatencyPayload();
            payload->postTime = std::chrono::steady_clock::now();
            queue.Post(topics[t], payload, /*autoCleanupPayload=*/true);
            totalMessages++;
          }
        }
      });
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
      consumers.emplace_back([&, i]() {
        tls_latencyBuffer = &perConsumerLatencies[i];
        while (running) {
          Message *msg = queue.GetQueueMessage();
          if (msg) {
            queue.Dispatch(msg->tid, msg);
          } else {
            std::this_thread::yield();
          }
        }
        tls_latencyBuffer = nullptr;
      });
    }

    for (auto &t : producers) {
      t.join();
    }

    tls_latencyBuffer = &drainLatencies;
    while (true) {
      Message *msg = queue.GetQueueMessage();
      if (!msg)
        break;
      queue.Dispatch(msg->tid, msg);
    }
    tls_latencyBuffer = nullptr;

    running = false;
    for (auto &t : consumers) {
      t.join();
    }

    for (auto &buf : perConsumerLatencies) {
      allLatenciesNs.insert(allLatenciesNs.end(), buf.begin(), buf.end());
    }
    allLatenciesNs.insert(allLatenciesNs.end(), drainLatencies.begin(),
                           drainLatencies.end());
  }

  state.SetItemsProcessed(totalMessages.load());
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Measures processing latency only (dequeue -> dispatch complete), not queuing delay
static void BM_LockFree_ProcessingLatency(benchmark::State &state) {
  EventQueueLockFreeCUT<65536> queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;

  std::vector<int> topicIds;
  for (int i = 0; i < NUM_TOPICS; i++) {
    std::string topic = "emulator_" + std::to_string(i);
    int id = queue.RegisterTopic(topic);
    topicIds.push_back(id);
  }

  std::atomic<int64_t> callbackCount{0};
  for (int i = 0; i < NUM_TOPICS; i++) {
    std::string topic = "emulator_" + std::to_string(i);
    for (int j = 0; j < OBSERVERS_PER_TOPIC; j++) {
      queue.AddObserver(topic, [&callbackCount](int id, Message *msg) {
        callbackCount++;
        benchmark::DoNotOptimize(callbackCount.load());
      });
    }
  }

  std::vector<int64_t> processingLatenciesNs;
  processingLatenciesNs.reserve(300000);

  int64_t totalMessages = 0;

  for (auto _ : state) {
    // Pre-fill queue with messages
    for (int id : topicIds) {
      queue.Post(id, nullptr, false);
    }

    // Measure processing latency only
    while (true) {
      Message *msg = queue.GetQueueMessage();
      if (!msg)
        break;

      auto start = std::chrono::steady_clock::now();
      queue.Dispatch(msg->tid, msg);
      auto elapsed = std::chrono::steady_clock::now() - start;

      processingLatenciesNs.push_back(
          std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
      totalMessages++;
    }
  }

  state.SetItemsProcessed(totalMessages);
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages, benchmark::Counter::kIsRate);
  state.counters["callbacks/sec"] =
      benchmark::Counter(callbackCount.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, processingLatenciesNs);
}

// Raw MPMC queue benchmark - measures pure queue latency without EventQueue overhead
static void BM_RawMPMC_Latency_MultiThread(benchmark::State &state) {
  MPMCQueue<std::chrono::steady_clock::time_point, 65536> queue;

  const int NUM_PRODUCERS = 8;
  const int NUM_CONSUMERS = 4;
  const int MSGS_PER_PRODUCER = 25600; // 256 topics * 100 iterations

  std::atomic<int64_t> totalMessages{0};
  std::atomic<bool> running{true};
  std::vector<int64_t> allLatenciesNs;

  for (auto _ : state) {
    running = true;
    totalMessages = 0;

    std::vector<std::vector<int64_t>> perConsumerLatencies(NUM_CONSUMERS);

    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
      producers.emplace_back([&]() {
        for (int j = 0; j < MSGS_PER_PRODUCER; j++) {
          auto now = std::chrono::steady_clock::now();
          while (!queue.try_push(now)) {
            std::this_thread::yield();
          }
          totalMessages++;
        }
      });
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
      consumers.emplace_back([&, i]() {
        std::chrono::steady_clock::time_point ts;
        while (running || !queue.empty()) {
          if (queue.try_pop(ts)) {
            auto elapsed = std::chrono::steady_clock::now() - ts;
            perConsumerLatencies[i].push_back(
                std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed)
                    .count());
          } else {
            std::this_thread::yield();
          }
        }
      });
    }

    for (auto &t : producers) {
      t.join();
    }

    // Brief drain
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    running = false;

    for (auto &t : consumers) {
      t.join();
    }

    for (auto &buf : perConsumerLatencies) {
      allLatenciesNs.insert(allLatenciesNs.end(), buf.begin(), buf.end());
    }
  }

  state.SetItemsProcessed(totalMessages.load());
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Register benchmarks
BENCHMARK(BM_RealWorld_SingleThread)->MinTime(2.0);
BENCHMARK(BM_RealWorld_MultiThread_Producers)
    ->Arg(2)
    ->Arg(4)
    ->Arg(8)
    ->MinTime(2.0);
BENCHMARK(BM_RealWorld_MultiThread_Full)->MinTime(2.0);
BENCHMARK(BM_RealWorld_60Hz_Simulation)->MinTime(2.0);
BENCHMARK(BM_RealWorld_Latency_SingleThread)->Iterations(50);
BENCHMARK(BM_RealWorld_Latency_MultiThread_Full)->Iterations(20);
BENCHMARK(BM_LockFree_Latency_SingleThread)->Iterations(50);
BENCHMARK(BM_LockFree_Latency_MultiThread_Full)->Iterations(20);
BENCHMARK(BM_RawMPMC_Latency_MultiThread)->Iterations(20);
BENCHMARK(BM_LockFree_ProcessingLatency)->Iterations(100);

// Balanced producer/consumer ratio (4P + 4C) to reduce queuing delay
static void BM_LockFree_Latency_Balanced(benchmark::State &state) {
  EventQueueLockFreeCUT<65536> queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;
  const int NUM_PRODUCERS = 4;
  const int NUM_CONSUMERS = 4;

  std::vector<std::string> topics;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topics.push_back("emulator_" + std::to_string(i));
  }

  for (const auto &topic : topics) {
    queue.AddObserver(topic, LatencyCaptureObserver);
    for (int i = 1; i < OBSERVERS_PER_TOPIC; i++) {
      queue.AddObserver(topic, NoopObserver);
    }
  }

  std::atomic<int64_t> totalMessages{0};
  std::atomic<bool> running{true};
  std::vector<int64_t> allLatenciesNs;

  for (auto _ : state) {
    running = true;
    totalMessages = 0;

    std::vector<std::vector<int64_t>> perConsumerLatencies(NUM_CONSUMERS);
    std::vector<int64_t> drainLatencies;

    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
      producers.emplace_back([&, i]() {
        int topicsPerProducer = NUM_TOPICS / NUM_PRODUCERS;
        int startTopic = i * topicsPerProducer;
        int endTopic = (i == NUM_PRODUCERS - 1)
                           ? NUM_TOPICS
                           : startTopic + topicsPerProducer;

        for (int iter = 0; iter < 100; iter++) {
          for (int t = startTopic; t < endTopic; t++) {
            auto *payload = new LatencyPayload();
            payload->postTime = std::chrono::steady_clock::now();
            queue.Post(topics[t], payload, true);
            totalMessages++;
          }
        }
      });
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
      consumers.emplace_back([&, i]() {
        tls_latencyBuffer = &perConsumerLatencies[i];
        while (running) {
          Message *msg = queue.GetQueueMessage();
          if (msg) {
            queue.Dispatch(msg->tid, msg);
          } else {
            std::this_thread::yield();
          }
        }
        tls_latencyBuffer = nullptr;
      });
    }

    for (auto &t : producers) {
      t.join();
    }

    tls_latencyBuffer = &drainLatencies;
    while (true) {
      Message *msg = queue.GetQueueMessage();
      if (!msg)
        break;
      queue.Dispatch(msg->tid, msg);
    }
    tls_latencyBuffer = nullptr;

    running = false;
    for (auto &t : consumers) {
      t.join();
    }

    for (auto &buf : perConsumerLatencies) {
      allLatenciesNs.insert(allLatenciesNs.end(), buf.begin(), buf.end());
    }
    allLatenciesNs.insert(allLatenciesNs.end(), drainLatencies.begin(),
                           drainLatencies.end());
  }

  state.SetItemsProcessed(totalMessages.load());
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

BENCHMARK(BM_LockFree_Latency_Balanced)->Iterations(20);

// Thread-local latency capture for broadcast benchmarks
static thread_local std::vector<int64_t> *tls_broadcastLatency = nullptr;

// Fire-and-forget broadcast queue benchmark
// Tests unified post() API with auto memory management
// More consumers than producers (2P + 8C) for low queuing delay
template <size_t PayloadSize>
static void BM_Broadcast_FireAndForget(benchmark::State &state) {
  EventQueueBroadcast<65536> queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;
  const int NUM_PRODUCERS = 2;
  const int NUM_CONSUMERS = 8;

  std::vector<uint16_t> topicIds;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topicIds.push_back(queue.registerTopic("topic_" + std::to_string(i)));
  }

  for (uint16_t id : topicIds) {
    for (int j = 0; j < OBSERVERS_PER_TOPIC; j++) {
      queue.addObserver(id, [](uint16_t tid, const void *data, size_t size) {
        if (tls_broadcastLatency && data && size >= sizeof(std::chrono::steady_clock::time_point)) {
          auto postTime = *reinterpret_cast<const std::chrono::steady_clock::time_point *>(data);
          auto elapsed = std::chrono::steady_clock::now() - postTime;
          tls_broadcastLatency->push_back(
              std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        }
        benchmark::DoNotOptimize(tid);
      });
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
            // Stamp time at start of payload
            *reinterpret_cast<std::chrono::steady_clock::time_point *>(payloadData) =
                std::chrono::steady_clock::now();

            // Fire and forget - queue handles memory automatically
            queue.post(topicIds[t], payloadData, PayloadSize);
            totalMessages++;
          }
        }
      });
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
      consumers.emplace_back([&, i]() {
        tls_broadcastLatency = &perConsumerLatencies[i];
        while (running || !queue.empty()) {
          if (!queue.dispatchOne()) {
            std::this_thread::yield();
          }
        }
        tls_broadcastLatency = nullptr;
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
  state.counters["mode"] = (PayloadSize <= 48) ? 0 : 1; // 0=inline, 1=managed
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Test fire-and-forget with various payload sizes
BENCHMARK_TEMPLATE(BM_Broadcast_FireAndForget, 16)->Iterations(20)->Name("BM_Broadcast_16B");
BENCHMARK_TEMPLATE(BM_Broadcast_FireAndForget, 32)->Iterations(20)->Name("BM_Broadcast_32B");
BENCHMARK_TEMPLATE(BM_Broadcast_FireAndForget, 48)->Iterations(20)->Name("BM_Broadcast_48B");
BENCHMARK_TEMPLATE(BM_Broadcast_FireAndForget, 64)->Iterations(20)->Name("BM_Broadcast_64B");
BENCHMARK_TEMPLATE(BM_Broadcast_FireAndForget, 256)->Iterations(20)->Name("BM_Broadcast_256B");
BENCHMARK_TEMPLATE(BM_Broadcast_FireAndForget, 1024)->Iterations(20)->Name("BM_Broadcast_1KB");
BENCHMARK_TEMPLATE(BM_Broadcast_FireAndForget, 4096)->Iterations(20)->Name("BM_Broadcast_4KB");

// Emulator-style pointer+metadata pattern
// Simulates frame buffer / audio buffer passing with zero-copy
struct FrameBufferRef {
  void *pixels;
  uint16_t width;
  uint16_t height;
  uint8_t format;
  uint8_t padding;
  uint32_t frameNum;
  std::chrono::steady_clock::time_point postTime; // For latency measurement
};
static_assert(sizeof(FrameBufferRef) <= 48, "FrameBufferRef must fit inline");

static void BM_Broadcast_PointerMetadata(benchmark::State &state) {
  EventQueueBroadcast<65536> queue;

  const int NUM_TOPICS = 64;  // 64 emulator instances
  const int OBSERVERS_PER_TOPIC = 3; // UI, logger, debugger
  const int NUM_PRODUCERS = 2;
  const int NUM_CONSUMERS = 8;

  std::vector<uint16_t> topicIds;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topicIds.push_back(queue.registerTopic("emu_" + std::to_string(i)));
  }

  for (uint16_t id : topicIds) {
    for (int j = 0; j < OBSERVERS_PER_TOPIC; j++) {
      queue.addObserver(id, [](uint16_t tid, const void *data, size_t size) {
        if (tls_broadcastLatency && data && size == sizeof(FrameBufferRef)) {
          auto *ref = reinterpret_cast<const FrameBufferRef *>(data);
          auto elapsed = std::chrono::steady_clock::now() - ref->postTime;
          tls_broadcastLatency->push_back(
              std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
          // Simulate minimal frame processing
          benchmark::DoNotOptimize(ref->pixels);
        }
      });
    }
  }

  // Simulated frame buffers (one per topic)
  std::vector<std::vector<uint8_t>> frameBuffers(NUM_TOPICS);
  for (auto &buf : frameBuffers) {
    buf.resize(320 * 256 * 2); // 160KB per frame (320x256 RGB565)
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

        uint32_t frameNum = 0;
        for (int iter = 0; iter < 100; iter++) {
          for (int t = startTopic; t < endTopic; t++) {
            FrameBufferRef ref;
            ref.pixels = frameBuffers[t].data();
            ref.width = 320;
            ref.height = 256;
            ref.format = 1; // RGB565
            ref.frameNum = frameNum++;
            ref.postTime = std::chrono::steady_clock::now();

            queue.postRef(topicIds[t], ref);
            totalMessages++;
          }
        }
      });
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
      consumers.emplace_back([&, i]() {
        tls_broadcastLatency = &perConsumerLatencies[i];
        while (running || !queue.empty()) {
          if (!queue.dispatchOne()) {
            std::this_thread::yield();
          }
        }
        tls_broadcastLatency = nullptr;
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
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  state.counters["metadata_bytes"] = sizeof(FrameBufferRef);
  ReportLatencyCounters(state, allLatenciesNs);
}

BENCHMARK(BM_Broadcast_PointerMetadata)->Iterations(20);

// Large payload benchmark: test up to 10MB payloads
template <size_t PayloadKB>
static void BM_Broadcast_LargePayload(benchmark::State &state) {
  EventQueueBroadcast<4096> queue; // Smaller queue for large payloads

  const int NUM_TOPICS = 4;
  const int OBSERVERS_PER_TOPIC = 2;
  const int NUM_PRODUCERS = 1;
  const int NUM_CONSUMERS = 4;
  const size_t PAYLOAD_SIZE = PayloadKB * 1024;

  std::vector<uint16_t> topicIds;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topicIds.push_back(queue.registerTopic("large_" + std::to_string(i)));
  }

  for (uint16_t id : topicIds) {
    for (int j = 0; j < OBSERVERS_PER_TOPIC; j++) {
      queue.addObserver(id, [](uint16_t tid, const void *data, size_t size) {
        if (tls_broadcastLatency && data && size >= sizeof(std::chrono::steady_clock::time_point)) {
          auto postTime = *reinterpret_cast<const std::chrono::steady_clock::time_point *>(data);
          auto elapsed = std::chrono::steady_clock::now() - postTime;
          tls_broadcastLatency->push_back(
              std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
        }
        // Simulate touching the data (cache effects)
        benchmark::DoNotOptimize(*reinterpret_cast<const char *>(data));
        benchmark::DoNotOptimize(*(reinterpret_cast<const char *>(data) + size - 1));
      });
    }
  }

  std::atomic<int64_t> totalMessages{0};
  std::atomic<int64_t> totalBytes{0};
  std::atomic<bool> running{true};
  std::vector<int64_t> allLatenciesNs;

  for (auto _ : state) {
    running = true;
    totalMessages = 0;
    totalBytes = 0;
    std::vector<std::vector<int64_t>> perConsumerLatencies(NUM_CONSUMERS);

    std::vector<std::thread> producers;
    for (int i = 0; i < NUM_PRODUCERS; i++) {
      producers.emplace_back([&, i]() {
        std::vector<char> payload(PAYLOAD_SIZE);

        for (int iter = 0; iter < 20; iter++) {
          for (int t = 0; t < NUM_TOPICS; t++) {
            *reinterpret_cast<std::chrono::steady_clock::time_point *>(payload.data()) =
                std::chrono::steady_clock::now();

            queue.post(topicIds[t], payload.data(), PAYLOAD_SIZE);
            totalMessages++;
            totalBytes += PAYLOAD_SIZE;
          }
        }
      });
    }

    std::vector<std::thread> consumers;
    for (int i = 0; i < NUM_CONSUMERS; i++) {
      consumers.emplace_back([&, i]() {
        tls_broadcastLatency = &perConsumerLatencies[i];
        while (running || !queue.empty()) {
          if (!queue.dispatchOne()) {
            std::this_thread::yield();
          }
        }
        tls_broadcastLatency = nullptr;
      });
    }

    for (auto &t : producers) {
      t.join();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    running = false;

    for (auto &t : consumers) {
      t.join();
    }

    for (auto &buf : perConsumerLatencies) {
      allLatenciesNs.insert(allLatenciesNs.end(), buf.begin(), buf.end());
    }
  }

  state.SetItemsProcessed(totalMessages.load());
  state.SetBytesProcessed(totalBytes.load());
  state.counters["payload_KB"] = PayloadKB;
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  state.counters["MB/sec"] =
      benchmark::Counter(totalBytes.load() / (1024.0 * 1024.0), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Test large payloads from 64KB to 10MB
BENCHMARK_TEMPLATE(BM_Broadcast_LargePayload, 64)->Iterations(10)->Name("BM_Large_64KB");
BENCHMARK_TEMPLATE(BM_Broadcast_LargePayload, 256)->Iterations(10)->Name("BM_Large_256KB");
BENCHMARK_TEMPLATE(BM_Broadcast_LargePayload, 1024)->Iterations(10)->Name("BM_Large_1MB");
BENCHMARK_TEMPLATE(BM_Broadcast_LargePayload, 4096)->Iterations(5)->Name("BM_Large_4MB");
BENCHMARK_TEMPLATE(BM_Broadcast_LargePayload, 10240)->Iterations(3)->Name("BM_Large_10MB");

// ============================================================================
// FAST BROADCAST QUEUE - No std::function, no shared_ptr, raw function pointers
// ============================================================================

// Thread-local for fast queue latency
static thread_local std::vector<int64_t> *tls_fastLatency = nullptr;

static void FastLatencyObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  if (tls_fastLatency && data && size >= sizeof(std::chrono::steady_clock::time_point)) {
    auto postTime = *reinterpret_cast<const std::chrono::steady_clock::time_point *>(data);
    auto elapsed = std::chrono::steady_clock::now() - postTime;
    tls_fastLatency->push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  }
  benchmark::DoNotOptimize(tid);
}

static void FastNoopObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  benchmark::DoNotOptimize(tid);
  benchmark::DoNotOptimize(data);
}

template <size_t PayloadSize>
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

// Compare Fast vs Regular at same payload sizes
BENCHMARK_TEMPLATE(BM_BroadcastFast, 32)->Iterations(20)->Name("BM_Fast_32B");

// Compare wait strategies: spin vs yield vs no-wait
enum class WaitStrategy { BusySpin, Yield, NoWait };

template <WaitStrategy Strategy>
static void BM_Fast_WaitStrategy(benchmark::State &state) {
  EventQueueBroadcastFast<65536> queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;
  const int NUM_PRODUCERS = 2;
  const int NUM_CONSUMERS = 8;
  const size_t PAYLOAD_SIZE = 32;

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

        alignas(8) char payloadData[PAYLOAD_SIZE];

        for (int iter = 0; iter < 100; iter++) {
          for (int t = startTopic; t < endTopic; t++) {
            *reinterpret_cast<std::chrono::steady_clock::time_point *>(payloadData) =
                std::chrono::steady_clock::now();
            queue.post(topicIds[t], payloadData, PAYLOAD_SIZE);
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
            // Different wait strategies
            if constexpr (Strategy == WaitStrategy::BusySpin) {
#if defined(__aarch64__)
              asm volatile("yield");
#else
              __builtin_ia32_pause();
#endif
            } else if constexpr (Strategy == WaitStrategy::Yield) {
              std::this_thread::yield();
            }
            // NoWait: immediately retry (tightest loop)
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
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

BENCHMARK_TEMPLATE(BM_Fast_WaitStrategy, WaitStrategy::BusySpin)->Iterations(20)->Name("BM_Wait_BusySpin");
BENCHMARK_TEMPLATE(BM_Fast_WaitStrategy, WaitStrategy::Yield)->Iterations(20)->Name("BM_Wait_Yield");
BENCHMARK_TEMPLATE(BM_Fast_WaitStrategy, WaitStrategy::NoWait)->Iterations(20)->Name("BM_Wait_NoWait");

// Thread-local pool callback adapters - same signature as FastCallback
static void TLLatencyObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  if (tls_fastLatency && data && size >= sizeof(std::chrono::steady_clock::time_point)) {
    auto ts = *reinterpret_cast<const std::chrono::steady_clock::time_point *>(data);
    auto elapsed = std::chrono::steady_clock::now() - ts;
    tls_fastLatency->push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  }
}

static void TLNoopObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  benchmark::DoNotOptimize(tid);
}

// Compare lock-free pool vs thread-local pool
template <int PayloadSize = 32>
static void BM_FastTL_ThreadLocalPool(benchmark::State &state) {
  EventQueueBroadcastFastTL<65536> queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;
  const int NUM_PRODUCERS = 2;
  const int NUM_CONSUMERS = 8;

  std::vector<uint16_t> topicIds;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topicIds.push_back(queue.registerTopic());
  }

  for (uint16_t id : topicIds) {
    queue.addObserver(id, TLLatencyObserver, nullptr);
    for (int j = 1; j < OBSERVERS_PER_TOPIC; j++) {
      queue.addObserver(id, TLNoopObserver, nullptr);
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
  state.counters["pool_type"] = 1; // 1 = thread-local
  state.counters["payload_bytes"] = PayloadSize;
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Lock-free vs Thread-local pool comparison
BENCHMARK_TEMPLATE(BM_BroadcastFast, 32)->Iterations(20)->Name("BM_Pool_LockFree_32B");
BENCHMARK_TEMPLATE(BM_FastTL_ThreadLocalPool, 32)->Iterations(20)->Name("BM_Pool_ThreadLocal_32B");

// Batch callback adapters
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

// Batch dispatch benchmark - process messages in batches for better cache locality
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
          // Use batch dispatch instead of single dispatch
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
  state.counters["dispatch_mode"] = 1; // 1 = batch
  state.counters["payload_bytes"] = PayloadSize;
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Single dispatch vs batch dispatch comparison
BENCHMARK_TEMPLATE(BM_BroadcastFast, 32)->Iterations(20)->Name("BM_Dispatch_Single_32B");
BENCHMARK_TEMPLATE(BM_FastBatch_BatchDispatch, 32)->Iterations(20)->Name("BM_Dispatch_Batch_32B");

// Prefetch callback adapters
static void PFLatencyObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  if (tls_fastLatency && data && size >= sizeof(std::chrono::steady_clock::time_point)) {
    auto ts = *reinterpret_cast<const std::chrono::steady_clock::time_point *>(data);
    auto elapsed = std::chrono::steady_clock::now() - ts;
    tls_fastLatency->push_back(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  }
}

static void PFNoopObserver(uint16_t tid, const void *data, size_t size, void *userData) {
  benchmark::DoNotOptimize(tid);
}

// Prefetch batch dispatch benchmark
template <int PayloadSize = 32>
static void BM_FastPrefetch_BatchDispatch(benchmark::State &state) {
  EventQueueBroadcastFastPrefetch<65536> queue;

  const int NUM_TOPICS = 256;
  const int OBSERVERS_PER_TOPIC = 5;
  const int NUM_PRODUCERS = 2;
  const int NUM_CONSUMERS = 8;

  std::vector<uint16_t> topicIds;
  for (int i = 0; i < NUM_TOPICS; i++) {
    topicIds.push_back(queue.registerTopic());
  }

  for (uint16_t id : topicIds) {
    queue.addObserver(id, PFLatencyObserver, nullptr);
    for (int j = 1; j < OBSERVERS_PER_TOPIC; j++) {
      queue.addObserver(id, PFNoopObserver, nullptr);
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
          if (queue.dispatchBatchPrefetch(8) == 0) {
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
  state.counters["prefetch"] = 1;
  state.counters["payload_bytes"] = PayloadSize;
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Batch vs Prefetch comparison
BENCHMARK_TEMPLATE(BM_FastBatch_BatchDispatch, 32)->Iterations(20)->Name("BM_Batch_NoPrefetch_32B");
BENCHMARK_TEMPLATE(BM_FastPrefetch_BatchDispatch, 32)->Iterations(20)->Name("BM_Batch_Prefetch_32B");

// Emulator callback adapters
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
    std::vector<int64_t> criticalLatencies;
    std::vector<int64_t> consumerLatencies;

    // Emulator thread - posts critical events at ~60Hz + audio at ~735Hz (44100/60)
    std::thread emulatorThread([&]() {
      alignas(8) char payload[32];

      for (int frame = 0; frame < 600; frame++) {  // 10 seconds at 60Hz
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
  state.counters["queue_type"] = 2; // dual queue
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Single-queue baseline for comparison
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
  state.counters["queue_type"] = 1; // single queue
  state.counters["messages/sec"] =
      benchmark::Counter(totalMessages.load(), benchmark::Counter::kIsRate);
  ReportLatencyCounters(state, allLatenciesNs);
}

// Emulator workload comparison
BENCHMARK(BM_Emulator_SingleQueue)->Iterations(10)->Name("BM_Emu_SingleQueue");
BENCHMARK(BM_Emulator_DualQueue)->Iterations(10)->Name("BM_Emu_DualQueue");

BENCHMARK_MAIN();
