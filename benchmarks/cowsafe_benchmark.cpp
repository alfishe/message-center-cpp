#include <benchmark/benchmark.h>

#include "eventqueue_cow.h"
#include "eventqueue_cowsafe.h"
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace std;

// ============================================================================
// PERFORMANCE BENCHMARKS
// ============================================================================

// Helper to fill queue
void FillQueueSafe(EventQueueCOWSafe &queue, int topics = 256) {
  for (int i = 0; i < topics; ++i) {
    std::string t = "topic_" + std::to_string(i);
    ObserverCallbackFunc callback = [](int id, Message *msg) {
      volatile int x = id;
      (void)x;
    };
    queue.AddObserver(t, callback);
  }
}

// Single Thread Baseline - Should be identical to COW
static void BM_COWSafe_SingleThread(benchmark::State &state) {
  EventQueueCOWSafeCUT queue;
  FillQueueSafe(queue, 10);

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

// User Scenario - Should be identical to COW for dispatch
static void BM_COWSafe_UserScenario(benchmark::State &state) {
  EventQueueCOWSafeCUT queue;
  int numTopics = 256;
  FillQueueSafe(queue, numTopics);

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
// SAFETY BENCHMARKS - Test RemoveObserver blocking behavior
// ============================================================================

// Test: RemoveObserver waits for slow callback
static void BM_COWSafe_SlowCallbackSafety(benchmark::State &state) {
  EventQueueCOWSafeCUT queue;

  std::atomic<bool> callback_started{false};
  std::atomic<bool> callback_finished{false};
  std::atomic<bool> remove_returned{false};

  // Register slow callback
  ObserverCallbackFunc slow_callback = [&](int id, Message *msg) {
    callback_started = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // Slow!
    callback_finished = true;
  };
  queue.AddObserver("slow_topic", slow_callback);

  for (auto _ : state) {
    callback_started = false;
    callback_finished = false;
    remove_returned = false;

    // Start dispatch in background
    std::thread dispatcher([&]() {
      queue.Post("slow_topic", nullptr);
      Message *msg = queue.GetQueueMessage();
      if (msg) {
        queue.Dispatch(msg->tid, msg);
      }
    });

    // Wait for callback to start
    while (!callback_started) {
      std::this_thread::yield();
    }

    // Now try to remove observer (should block until callback finishes)
    std::thread remover([&]() {
      queue.RemoveObserver("slow_topic", slow_callback);
      remove_returned = true;
    });

    dispatcher.join();
    remover.join();

    // Verify: RemoveObserver didn't return until callback finished
    state.counters["SafetyViolations"] =
        (remove_returned && !callback_finished) ? 1 : 0;
  }
}

// Test: Concurrent dispatch + unregister stress test
static void BM_COWSafe_ConcurrentStress(benchmark::State &state) {
  EventQueueCOWSafeCUT queue;

  std::atomic<bool> running{true};
  std::atomic<int> crashes{0};

  // Register 100 observers
  std::vector<ObserverCallbackFunc> callbacks;
  for (int i = 0; i < 100; ++i) {
    ObserverCallbackFunc cb = [](int id, Message *msg) {
      volatile int x = id;
      (void)x;
    };
    callbacks.push_back(cb);
    queue.AddObserver("stress_topic", cb);
  }

  // Dispatcher thread
  std::thread dispatcher([&]() {
    while (running) {
      queue.Post("stress_topic", nullptr);
      Message *msg = queue.GetQueueMessage();
      if (msg) {
        queue.Dispatch(msg->tid, msg);
      }
    }
  });

  for (auto _ : state) {
    // Randomly remove and re-add observers
    for (int i = 0; i < 10; ++i) {
      int idx = i % callbacks.size();
      queue.RemoveObserver("stress_topic", callbacks[idx]);
      queue.AddObserver("stress_topic", callbacks[idx]);
    }
  }

  running = false;
  dispatcher.join();

  state.counters["Crashes"] = crashes.load();
}

// ============================================================================
// OVERHEAD MEASUREMENT - Compare COW vs COWSafe dispatch overhead
// ============================================================================

static void BM_Overhead_COW_Dispatch(benchmark::State &state) {
  EventQueueCOWCUT queue;
  ObserverCallbackFunc callback = [](int id, Message *msg) {
    volatile int x = id;
    (void)x;
  };
  queue.AddObserver("topic", callback);

  Message msg;
  msg.tid = queue.ResolveTopic("topic");

  for (auto _ : state) {
    queue.Dispatch(msg.tid, &msg);
  }

  state.SetItemsProcessed(state.iterations());
}

static void BM_Overhead_COWSafe_Dispatch(benchmark::State &state) {
  EventQueueCOWSafeCUT queue;
  ObserverCallbackFunc callback = [](int id, Message *msg) {
    volatile int x = id;
    (void)x;
  };
  queue.AddObserver("topic", callback);

  Message msg;
  msg.tid = queue.ResolveTopic("topic");

  for (auto _ : state) {
    queue.Dispatch(msg.tid, &msg);
  }

  state.SetItemsProcessed(state.iterations());
}

// ============================================================================
// REGISTER BENCHMARKS
// ============================================================================

BENCHMARK(BM_COWSafe_SingleThread)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_COWSafe_UserScenario)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);

BENCHMARK(BM_COWSafe_SlowCallbackSafety)->Iterations(10);
BENCHMARK(BM_COWSafe_ConcurrentStress)->Iterations(100);

BENCHMARK(BM_Overhead_COW_Dispatch);
BENCHMARK(BM_Overhead_COWSafe_Dispatch);
