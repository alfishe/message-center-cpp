#include <benchmark/benchmark.h>

#include "eventqueue_benchmark.h"
#include "eventqueue_ultimate.h"
#include "queuestats.h"
#include <atomic>
#include <thread>
#include <vector>

using namespace std;

/// ========================================================================
/// ULTIMATE EVENTQUEUE BENCHMARKS
/// Testing lock-free + object pooling + per-thread pools
/// ========================================================================

/// Single-threaded consumer (ultimate version)
static void BM_Ultimate_SingleThread(benchmark::State &state) {
  EventQueueUltimateCUT queue;
  FillQueue(queue);

  QueueStats stats;
  std::atomic<bool> running{true};

  std::thread consumer([&]() {
    while (running) {
      Message *msg = queue.GetQueueMessage();
      if (msg) {
        stats.recordDequeue();
        queue.Dispatch(msg->tid, msg);
      } else {
        std::this_thread::yield();
      }
    }
  });

  size_t messagesProduced = 0;

  for (auto _ : state) {
    for (int i = 0; i < 1000; i++) {
      queue.Post(1, nullptr);
      stats.recordEnqueue();
      messagesProduced++;
    }

    while (queue.GetQueueSize() > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  }

  running = false;
  consumer.join();

  state.SetItemsProcessed(messagesProduced);
  state.counters["MaxDepth"] = stats.getMaxDepth();
  state.counters["Threads"] = 1;
  state.counters["LocalPool"] = queue.GetLocalPoolSize();
  state.counters["GlobalPool"] = queue.GetGlobalPoolSize();
}

/// Fixed pool - 2 consumer threads (ultimate version)
static void BM_Ultimate_FixedPool2(benchmark::State &state) {
  EventQueueUltimateCUT queue;
  FillQueue(queue);

  QueueStats stats;
  std::atomic<bool> running{true};
  std::vector<std::thread> consumers;

  for (int i = 0; i < 2; i++) {
    consumers.emplace_back([&]() {
      while (running) {
        Message *msg = queue.GetQueueMessage();
        if (msg) {
          stats.recordDequeue();
          queue.Dispatch(msg->tid, msg);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  size_t messagesProduced = 0;

  for (auto _ : state) {
    for (int i = 0; i < 1000; i++) {
      queue.Post(1, nullptr);
      stats.recordEnqueue();
      messagesProduced++;
    }

    while (queue.GetQueueSize() > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  }

  running = false;
  for (auto &c : consumers)
    c.join();

  state.SetItemsProcessed(messagesProduced);
  state.counters["MaxDepth"] = stats.getMaxDepth();
  state.counters["Threads"] = 2;
}

/// Fixed pool - 4 consumer threads (ultimate version)
static void BM_Ultimate_FixedPool4(benchmark::State &state) {
  EventQueueUltimateCUT queue;
  FillQueue(queue);

  QueueStats stats;
  std::atomic<bool> running{true};
  std::vector<std::thread> consumers;

  for (int i = 0; i < 4; i++) {
    consumers.emplace_back([&]() {
      while (running) {
        Message *msg = queue.GetQueueMessage();
        if (msg) {
          stats.recordDequeue();
          queue.Dispatch(msg->tid, msg);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  size_t messagesProduced = 0;

  for (auto _ : state) {
    for (int i = 0; i < 1000; i++) {
      queue.Post(1, nullptr);
      stats.recordEnqueue();
      messagesProduced++;
    }

    while (queue.GetQueueSize() > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  }

  running = false;
  for (auto &c : consumers)
    c.join();

  state.SetItemsProcessed(messagesProduced);
  state.counters["MaxDepth"] = stats.getMaxDepth();
  state.counters["Threads"] = 4;
}

/// Fixed pool - 8 consumer threads (ultimate version)
static void BM_Ultimate_FixedPool8(benchmark::State &state) {
  EventQueueUltimateCUT queue;
  FillQueue(queue);

  QueueStats stats;
  std::atomic<bool> running{true};
  std::vector<std::thread> consumers;

  for (int i = 0; i < 8; i++) {
    consumers.emplace_back([&]() {
      while (running) {
        Message *msg = queue.GetQueueMessage();
        if (msg) {
          stats.recordDequeue();
          queue.Dispatch(msg->tid, msg);
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  size_t messagesProduced = 0;

  for (auto _ : state) {
    for (int i = 0; i < 1000; i++) {
      queue.Post(1, nullptr);
      stats.recordEnqueue();
      messagesProduced++;
    }

    while (queue.GetQueueSize() > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
  }

  running = false;
  for (auto &c : consumers)
    c.join();

  state.SetItemsProcessed(messagesProduced);
  state.counters["MaxDepth"] = stats.getMaxDepth();
  state.counters["Threads"] = 8;
}

BENCHMARK(BM_Ultimate_SingleThread)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Ultimate_FixedPool2)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Ultimate_FixedPool4)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_Ultimate_FixedPool8)->Unit(benchmark::kMillisecond);
