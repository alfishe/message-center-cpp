#include <benchmark/benchmark.h>

#include "eventqueue_cowsafe.h"
#include "eventqueue_simple.h"
#include <atomic>
#include <thread>
#include <vector>

using namespace std;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

void FillQueueSimple(EventQueueSimple &queue, int topics = 256) {
  for (int i = 0; i < topics; ++i) {
    std::string t = "topic_" + std::to_string(i);
    ObserverCallbackFunc callback = [](int id, Message *msg) {
      volatile int x = id;
      (void)x;
    };
    queue.AddObserver(t, callback);
  }
}

void FillQueueCOWSafe(EventQueueCOWSafe &queue, int topics = 256) {
  for (int i = 0; i < topics; ++i) {
    std::string t = "topic_" + std::to_string(i);
    ObserverCallbackFunc callback = [](int id, Message *msg) {
      volatile int x = id;
      (void)x;
    };
    queue.AddObserver(t, callback);
  }
}

// ============================================================================
// SINGLE-THREADED BENCHMARKS
// ============================================================================

static void BM_Simple_SingleThread(benchmark::State &state) {
  EventQueueSimpleCUT queue;
  FillQueueSimple(queue, 10);

  std::atomic<bool> running{true};
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

  size_t produced = 0;
  for (auto _ : state) {
    for (int i = 0; i < 1000; i++) {
      queue.Post(1, nullptr);
      produced++;
    }
    while (queue.GetQueueSize() > 0)
      std::this_thread::yield();
  }

  running = false;
  consumer.join();

  state.SetItemsProcessed(produced);
}

static void BM_COWSafe_SingleThread(benchmark::State &state) {
  EventQueueCOWSafeCUT queue;
  FillQueueCOWSafe(queue, 10);

  std::atomic<bool> running{true};
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

  size_t produced = 0;
  for (auto _ : state) {
    for (int i = 0; i < 1000; i++) {
      queue.Post(1, nullptr);
      produced++;
    }
    while (queue.GetQueueSize() > 0)
      std::this_thread::yield();
  }

  running = false;
  consumer.join();

  state.SetItemsProcessed(produced);
}

// ============================================================================
// MULTI-THREADED BENCHMARKS (User Scenario)
// ============================================================================

static void BM_Simple_UserScenario(benchmark::State &state) {
  EventQueueSimpleCUT queue;
  int numTopics = 256;
  FillQueueSimple(queue, numTopics);

  std::atomic<bool> running{true};
  std::atomic<size_t> consumed{0};

  // 4 Consumers
  std::vector<std::thread> consumers;
  for (int i = 0; i < 4; ++i) {
    consumers.emplace_back([&]() {
      while (running) {
        Message *msg = queue.GetQueueMessage();
        if (msg) {
          queue.Dispatch(msg->tid, msg);
          consumed++;
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  // 8 Producers
  std::vector<std::thread> producers;
  for (int i = 0; i < 8; ++i) {
    producers.emplace_back([&]() {
      int id = 0;
      while (running) {
        if (queue.GetQueueSize() < 10000) {
          queue.Post(id % numTopics, nullptr);
          id++;
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto _ : state) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  running = false;
  for (auto &t : producers)
    t.join();
  for (auto &t : consumers)
    t.join();

  state.SetItemsProcessed(consumed);
}

static void BM_COWSafe_UserScenario(benchmark::State &state) {
  EventQueueCOWSafeCUT queue;
  int numTopics = 256;
  FillQueueCOWSafe(queue, numTopics);

  std::atomic<bool> running{true};
  std::atomic<size_t> consumed{0};

  // 4 Consumers
  std::vector<std::thread> consumers;
  for (int i = 0; i < 4; ++i) {
    consumers.emplace_back([&]() {
      while (running) {
        Message *msg = queue.GetQueueMessage();
        if (msg) {
          queue.Dispatch(msg->tid, msg);
          consumed++;
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  // 8 Producers
  std::vector<std::thread> producers;
  for (int i = 0; i < 8; ++i) {
    producers.emplace_back([&]() {
      int id = 0;
      while (running) {
        if (queue.GetQueueSize() < 10000) {
          queue.Post(id % numTopics, nullptr);
          id++;
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto _ : state) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  running = false;
  for (auto &t : producers)
    t.join();
  for (auto &t : consumers)
    t.join();

  state.SetItemsProcessed(consumed);
}

// ============================================================================
// REGISTER BENCHMARKS
// ============================================================================

BENCHMARK(BM_Simple_SingleThread)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_COWSafe_SingleThread)->Unit(benchmark::kMillisecond);

BENCHMARK(BM_Simple_UserScenario)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
BENCHMARK(BM_COWSafe_UserScenario)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
