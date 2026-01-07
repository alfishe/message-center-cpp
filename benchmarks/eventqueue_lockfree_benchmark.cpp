#include <benchmark/benchmark.h>

#include "eventqueue.h"
#include "eventqueue_benchmark.h"
#include "eventqueue_lockfree.h"
#include <sstream>
#include <string>

using namespace std;

/// ========================================================================
/// LOCK-FREE BENCHMARKS - Comparing original vs lock-free implementation
/// ========================================================================

/// region <Single-threaded TPS benchmarks - Lock-free>

static void BM_LF_PostSingleTopicTPS(benchmark::State &state) {
  EventQueueLockFreeCUT queue;
  FillQueue(queue);

  for (auto _ : state) {
    for (int i = 0; i < state.range(0); i++) {
      queue.Post(1, nullptr);
    }
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_LF_PostSingleTopicTPS)
    ->Range(1, 1 << 18)
    ->Unit(benchmark::kMillisecond);

static void BM_LF_GetMessageSingleTopicTPS(benchmark::State &state) {
  EventQueueLockFreeCUT queue;
  FillQueue(queue);

  for (auto _ : state) {
    state.PauseTiming();
    for (int i = 0; i < state.range(0); i++) {
      queue.Post(1, nullptr);
    }
    state.ResumeTiming();

    for (int i = 0; i < state.range(0); i++) {
      Message *message = queue.GetQueueMessage();
      if (message && message->tid) {
        // Process message
        delete message;
      }
    }
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_LF_GetMessageSingleTopicTPS)
    ->Range(1, 1 << 18)
    ->Unit(benchmark::kMillisecond);

/// endregion </Single-threaded TPS benchmarks - Lock-free>

/// region <Multi-threaded TPS benchmarks - Lock-free>

static void BM_LF_MT_PostSingleTopicTPS(benchmark::State &state) {
  static EventQueueLockFreeCUT queue;

  if (state.thread_index() == 0) {
    // Only initialize once for all threads
    FillQueue(queue);
  }

  for (auto _ : state) {
    for (int i = 0; i < state.range(0); i++) {
      queue.Post(1, nullptr);
    }
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_LF_MT_PostSingleTopicTPS)
    ->Range(1, 1 << 18)
    ->Unit(benchmark::kMillisecond)
    ->ThreadRange(1, std::thread::hardware_concurrency());

static void BM_LF_MT_GetMessageSingleTopicTPS(benchmark::State &state) {
  static EventQueueLockFreeCUT queue;

  if (state.thread_index() == 0) {
    FillQueue(queue);
  }

  for (auto _ : state) {
    state.PauseTiming();
    for (int i = 0; i < state.range(0); i++) {
      queue.Post(1, nullptr);
    }
    state.ResumeTiming();

    for (int i = 0; i < state.range(0); i++) {
      Message *message = queue.GetQueueMessage();
      if (message && message->tid) {
        delete message;
      }
    }
  }

  state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_LF_MT_GetMessageSingleTopicTPS)
    ->Range(1, 1 << 18)
    ->Unit(benchmark::kMillisecond)
    ->ThreadRange(1, std::thread::hardware_concurrency());

/// endregion </Multi-threaded TPS benchmarks - Lock-free>

/// region <Comparison benchmarks - Original vs Lock-free>

// Helper function to run Post benchmark for both implementations
template <typename QueueType>
static void BM_Compare_Post(benchmark::State &state) {
  QueueType queue;
  FillQueue(queue);

  for (auto _ : state) {
    for (int i = 0; i < state.range(0); i++) {
      queue.Post(1, nullptr);
    }
  }

  state.SetItemsProcessed(state.iterations());
}

template <typename QueueType>
static void BM_Compare_Get(benchmark::State &state) {
  QueueType queue;
  FillQueue(queue);

  for (auto _ : state) {
    state.PauseTiming();
    for (int i = 0; i < state.range(0); i++) {
      queue.Post(1, nullptr);
    }
    state.ResumeTiming();

    for (int i = 0; i < state.range(0); i++) {
      Message *message = queue.GetQueueMessage();
      if (message && message->tid) {
        delete message;
      }
    }
  }

  state.SetItemsProcessed(state.iterations());
}

// Original implementation benchmarks
BENCHMARK(BM_Compare_Post<EventQueueCUT>)
    ->Name("Original_Post")
    ->RangeMultiplier(8)
    ->Range(8, 8 << 10)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_Compare_Get<EventQueueCUT>)
    ->Name("Original_Get")
    ->RangeMultiplier(8)
    ->Range(8, 8 << 10)
    ->Unit(benchmark::kMicrosecond);

// Lock-free implementation benchmarks
BENCHMARK(BM_Compare_Post<EventQueueLockFreeCUT>)
    ->Name("LockFree_Post")
    ->RangeMultiplier(8)
    ->Range(8, 8 << 10)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_Compare_Get<EventQueueLockFreeCUT>)
    ->Name("LockFree_Get")
    ->RangeMultiplier(8)
    ->Range(8, 8 << 10)
    ->Unit(benchmark::kMicrosecond);

/// endregion </Comparison benchmarks>

/// region <Multi-threaded comparison>

template <typename QueueType>
static void BM_MT_Compare_Post(benchmark::State &state) {
  static QueueType queue;

  if (state.thread_index() == 0) {
    FillQueue(queue);
  }

  for (auto _ : state) {
    for (int i = 0; i < state.range(0); i++) {
      queue.Post(1, nullptr);
    }
  }

  state.SetItemsProcessed(state.iterations());
}

// Multi-threaded comparison - Original
BENCHMARK(BM_MT_Compare_Post<EventQueueCUT>)
    ->Name("MT_Original_Post")
    ->Arg(1024)
    ->Unit(benchmark::kMicrosecond)
    ->ThreadRange(1, std::thread::hardware_concurrency());

// Multi-threaded comparison - Lock-free
BENCHMARK(BM_MT_Compare_Post<EventQueueLockFreeCUT>)
    ->Name("MT_LockFree_Post")
    ->Arg(1024)
    ->Unit(benchmark::kMicrosecond)
    ->ThreadRange(1, std::thread::hardware_concurrency());

/// endregion </Multi-threaded comparison>
