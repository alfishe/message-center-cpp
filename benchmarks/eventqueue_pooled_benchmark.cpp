#include <benchmark/benchmark.h>

#include "eventqueue.h"
#include "eventqueue_benchmark.h"
#include "eventqueue_lockfree.h"
#include "eventqueue_pooled.h"

using namespace std;

/// ========================================================================
/// POOLED BENCHMARKS - Comparing Original vs Lock-Free vs Pooled
/// ========================================================================

/// region <Single-threaded comparison>

template <typename QueueType>
static void BM_Post_Comparison(benchmark::State &state) {
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
static void BM_GetDispatch_Comparison(benchmark::State &state) {
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
      if (message) {
        queue.Dispatch(1, message); // Dispatch handles cleanup/pooling
      }
    }
  }

  state.SetItemsProcessed(state.iterations());
}

// Original implementation
BENCHMARK(BM_Post_Comparison<EventQueueCUT>)
    ->Name("1_Original_Post")
    ->RangeMultiplier(8)
    ->Range(64, 8192)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_GetDispatch_Comparison<EventQueueCUT>)
    ->Name("1_Original_GetDispatch")
    ->RangeMultiplier(8)
    ->Range(64, 8192)
    ->Unit(benchmark::kMicrosecond);

// Lock-free implementation
BENCHMARK(BM_Post_Comparison<EventQueueLockFreeCUT>)
    ->Name("2_LockFree_Post")
    ->RangeMultiplier(8)
    ->Range(64, 8192)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_GetDispatch_Comparison<EventQueueLockFreeCUT>)
    ->Name("2_LockFree_GetDispatch")
    ->RangeMultiplier(8)
    ->Range(64, 8192)
    ->Unit(benchmark::kMicrosecond);

// Pooled implementation
BENCHMARK(BM_Post_Comparison<EventQueuePooledCUT>)
    ->Name("3_Pooled_Post")
    ->RangeMultiplier(8)
    ->Range(64, 8192)
    ->Unit(benchmark::kMicrosecond);

BENCHMARK(BM_GetDispatch_Comparison<EventQueuePooledCUT>)
    ->Name("3_Pooled_GetDispatch")
    ->RangeMultiplier(8)
    ->Range(64, 8192)
    ->Unit(benchmark::kMicrosecond);

/// endregion </Single-threaded comparison>

/// region <Multi-threaded comparison>

template <typename QueueType>
static void BM_MT_Post_Comparison(benchmark::State &state) {
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

// Multi-threaded - Original
BENCHMARK(BM_MT_Post_Comparison<EventQueueCUT>)
    ->Name("MT_1_Original")
    ->Arg(1024)
    ->Unit(benchmark::kMicrosecond)
    ->ThreadRange(1, 8);

// Multi-threaded - Lock-free
BENCHMARK(BM_MT_Post_Comparison<EventQueueLockFreeCUT>)
    ->Name("MT_2_LockFree")
    ->Arg(1024)
    ->Unit(benchmark::kMicrosecond)
    ->ThreadRange(1, 8);

// Multi-threaded - Pooled
BENCHMARK(BM_MT_Post_Comparison<EventQueuePooledCUT>)
    ->Name("MT_3_Pooled")
    ->Arg(1024)
    ->Unit(benchmark::kMicrosecond)
    ->ThreadRange(1, 8);

/// endregion </Multi-threaded comparison>

/// region <Throughput benchmarks>

template <typename QueueType>
static void BM_Throughput(benchmark::State &state) {
  QueueType queue;
  FillQueue(queue);

  size_t messagesProcessed = 0;

  for (auto _ : state) {
    // Post a batch
    for (int i = 0; i < 1000; i++) {
      queue.Post(1, nullptr);
    }

    // Get and dispatch the batch
    for (int i = 0; i < 1000; i++) {
      Message *message = queue.GetQueueMessage();
      if (message) {
        queue.Dispatch(1, message);
        messagesProcessed++;
      }
    }
  }

  state.SetItemsProcessed(messagesProcessed);
  state.SetBytesProcessed(messagesProcessed * sizeof(Message));
}

BENCHMARK(BM_Throughput<EventQueueCUT>)
    ->Name("Throughput_Original")
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_Throughput<EventQueueLockFreeCUT>)
    ->Name("Throughput_LockFree")
    ->Unit(benchmark::kMillisecond);

BENCHMARK(BM_Throughput<EventQueuePooledCUT>)
    ->Name("Throughput_Pooled")
    ->Unit(benchmark::kMillisecond);

/// endregion </Throughput benchmarks>
