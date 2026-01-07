#include <benchmark/benchmark.h>

#include "eventqueue_cow.h"
#include "queuestats.h"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace std;

// Helper to fill queue with 256 topics
void FillQueueCOW(EventQueueCOW &queue, int topics = 256) {
  for (int i = 0; i < topics; ++i) {
    std::string t = "topic_" + std::to_string(i);
    // Add 1 cheap observer per topic
    ObserverCallbackFunc callback = [](int id, Message *msg) {
      volatile int x = id;
      (void)x;
    };
    queue.AddObserver(t, callback);
  }
}

// Single Thread Baseline
static void BM_COW_SingleThread(benchmark::State &state) {
  EventQueueCOWCUT queue;
  FillQueueCOW(queue, 10); // Standard 10 topics

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

// User Scenario: 256 Topics, 8 Producers, 4 Consumers
static void BM_COW_UserScenario(benchmark::State &state) {
  EventQueueCOWCUT queue;
  int numTopics = 256;
  FillQueueCOW(queue, numTopics);

  std::atomic<bool> running{true};
  std::atomic<size_t> consumed{0};

  // 4 Consumers (Readers)
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

  // 8 Producers (Writers)
  std::vector<std::thread> producers;
  for (int i = 0; i < 8; ++i) {
    producers.emplace_back([&]() {
      int id = 0;
      while (running) {
        if (queue.GetQueueSize() <
            10000) { // Throttling to prevent memory explosion if consumers slow
          queue.Post(id % numTopics, nullptr);
          id++;
        } else {
          std::this_thread::yield();
        }
      }
    });
  }

  // Run for strict time
  auto start = std::chrono::high_resolution_clock::now();
  for (auto _ : state) {
    // Just wait
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  auto end = std::chrono::high_resolution_clock::now();

  running = false;
  for (auto &t : producers)
    t.join();
  for (auto &t : consumers)
    t.join();

  // Calculate rate
  auto duration =
      std::chrono::duration_cast<std::chrono::seconds>(end - start).count();
  // Items processed is consumption
  state.SetItemsProcessed(consumed);
}

BENCHMARK(BM_COW_SingleThread)->Unit(benchmark::kMillisecond);
BENCHMARK(BM_COW_UserScenario)
    ->UseRealTime()
    ->Unit(benchmark::kMillisecond)
    ->MinTime(2.0);
