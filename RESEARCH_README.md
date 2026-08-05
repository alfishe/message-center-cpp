# EventQueue Optimization Research

This branch contains all experimental work from the EventQueue performance optimization effort. It documents various approaches tested, their results, and conclusions about what works best for different use cases.

## Executive Summary

| Variant | P50 | P99 | Throughput | Verdict |
|---------|-----|-----|------------|---------|
| Original (mutex) | 6,617 μs | 9,873 μs | 372 K/s | Baseline |
| **BroadcastFast** | **1.25 μs** | **108 μs** | **5.75 M/s** | **Best general** |
| BroadcastFastBatch | 1.21 μs | 76 μs | 5.44 M/s | Best tail latency |
| **EmulatorDualQueue** | 0.17 μs | **99 μs** | 128 M/s | **Best for emulators** |
| BroadcastFastTL | 3,746 μs | 11,004 μs | 6.14 M/s | Failed (P/C mismatch) |
| BroadcastFastPrefetch | 1.29 μs | 160 μs | 5.88 M/s | Marginal improvement |

---

## File Reference

### Core Infrastructure

#### `src/mpmc_queue.h`
**Purpose**: Lock-free bounded Multi-Producer Multi-Consumer ring buffer.

**Why**: The original `std::deque` + `std::mutex` approach had high contention. A lock-free ring buffer eliminates mutex overhead entirely.

**Key Design**:
- Fixed-size power-of-2 capacity (compile-time)
- 64-byte aligned slots to prevent false sharing
- Sequence numbers for ABA problem avoidance
- Separate cache lines for head/tail pointers

**Usage**:
```cpp
MPMCQueue<Message*, 65536> queue;

// Producer
while (!queue.try_push(msg)) {
  std::this_thread::yield();
}

// Consumer
Message* msg;
if (queue.try_pop(msg)) {
  // process msg
}
```

**Benchmark**: Pure MPMC throughput ~15M ops/sec with 8 threads.

---

#### `src/objectpool_lockfree.h`
**Purpose**: Lock-free object pool using Treiber stack algorithm.

**Why**: Frequent `new`/`delete` of message objects caused allocation overhead. Pooling reuses objects.

**Key Design**:
- Treiber stack (lock-free LIFO)
- Configurable initial/max capacity
- Falls back to `new` when pool exhausted
- Thread-safe acquire/release

**Usage**:
```cpp
ObjectPoolLockFree<Message> pool(256, 8192);  // initial=256, max=8192

Message* msg = pool.acquire();  // Get from pool or new
// ... use msg ...
pool.release(msg);              // Return to pool
```

**Benchmark**: ~50ns per acquire/release pair vs ~200ns for new/delete.

---

#### `src/objectpool_threadlocal.h`
**Purpose**: Per-thread object pool to eliminate pool contention.

**Why**: Hypothesis that thread-local pools would eliminate atomic operations on the hot path.

**Key Design**:
- Each thread gets its own free list (up to 64 threads)
- No atomics on acquire/release within same thread
- 256 objects cached per thread

**Result**: **FAILED for producer/consumer workloads**. 
- Producers allocate, consumers release → objects accumulate on consumer threads
- P50 latency: 3,746 μs (vs 1.25 μs for lock-free pool)
- Only suitable when same thread allocates AND deallocates

**Usage** (not recommended for P/C):
```cpp
ObjectPoolThreadLocal<Message> pool;
// Only use if same thread does acquire() and release()
```

---

#### `src/payload_refcounted.h`
**Purpose**: Reference-counted payload for zero-copy broadcast to multiple observers.

**Why**: Broadcasting to N observers would otherwise require N copies. Ref-counting allows single allocation shared by all.

**Key Design**:
- Flexible array member for inline data storage
- `addRefs(count)` for broadcast setup
- `release()` decrements and auto-frees when zero
- Single allocation for header + data

**Usage**:
```cpp
// Create with data copy
RefCountedPayload* payload = RefCountedPayload::create(data, size);

// Broadcast to 5 observers
payload->addRefs(4);  // Already has refcount=1

// Each observer calls release() when done
payload->release();  // Last one frees memory
```

---

### Queue Variants

#### `src/eventqueue_lockfree.h`
**Purpose**: First lock-free EventQueue attempt using MPMC + lock-free pool.

**Why**: Direct replacement for original mutex-based EventQueue with same API.

**Key Design**:
- Same API as original EventQueue
- Uses MPMCQueue for message passing
- Lock-free object pool for messages
- COW observer lists with `std::shared_ptr`

**Result**: Superseded by Broadcast variants which have better memory management.

**Usage**:
```cpp
EventQueueLockFreeCUT<65536> queue;
queue.AddObserver("topic", [](int id, Message* msg) { ... });
queue.Post("topic", payload, ownsPayload);
queue.Dispatch(queue.GetQueueMessage());
```

---

#### `src/eventqueue_broadcast.h`
**Purpose**: Fire-and-forget broadcast queue with automatic memory management.

**Why**: Original API required manual payload lifecycle management. This variant copies data and auto-frees.

**Key Design**:
- Auto-selects inline (≤48B) or managed (>48B) mode
- Producer posts and forgets - queue handles cleanup
- `std::function` callbacks for flexibility
- String-based topic names with ID lookup

**Usage**:
```cpp
EventQueueBroadcast<65536> queue;
uint16_t topic = queue.registerTopic("frame_ready");
queue.addObserver(topic, [](uint16_t id, const void* data, size_t size) {
  // data is valid only during callback
});

queue.post(topic, frameData, sizeof(frameData));  // Copied, auto-freed
queue.dispatchOne();
```

**Benchmark**: P50=1.83μs, 3.49M msg/sec (slower than Fast variant due to std::function).

---

#### `src/eventqueue_broadcast_fast.h`
**Purpose**: Maximum performance broadcast queue.

**Why**: `std::function` has overhead (virtual call, potential heap). Raw function pointers are faster.

**Key Design**:
- Raw function pointers instead of `std::function`
- Observer snapshot cached in message (no lookup on dispatch)
- No `std::shared_ptr` on hot path
- Numeric topic IDs only (no string lookup)

**Usage**:
```cpp
void myCallback(uint16_t topicId, const void* data, size_t size, void* userData) {
  // Process event
}

EventQueueBroadcastFast<65536> queue;
uint16_t topic = queue.registerTopic();
queue.addObserver(topic, myCallback, userData);
queue.post(topic, data, size);
queue.dispatchOne();
```

**Benchmark**: P50=1.25μs, P99=108μs, 5.75M msg/sec. **Best general-purpose variant.**

---

#### `src/eventqueue_broadcast_fast_batch.h`
**Purpose**: Batch dispatch variant for better tail latency.

**Why**: Single dispatch has overhead per message. Batching amortizes atomic operations.

**Key Design**:
- `dispatchBatch(N)` processes up to N messages at once
- Groups queue pops and pool returns
- Thread-local batch buffer for producer-side batching

**Usage**:
```cpp
EventQueueBroadcastFastBatch<65536> queue;
// ... setup ...

// Consumer - batch dispatch
while (running) {
  if (queue.dispatchBatch(32) == 0) {
    std::this_thread::yield();
  }
}
```

**Benchmark**: P999=76μs vs 144μs (47% improvement). Slightly lower throughput (5.44M vs 5.75M).

---

#### `src/eventqueue_broadcast_fast_prefetch.h`
**Purpose**: Software prefetch hints to hide memory latency.

**Why**: Hypothesis that prefetching next message while processing current would improve performance.

**Key Design**:
- `__builtin_prefetch` for next message in batch
- Prefetch both message struct and payload data

**Result**: **Marginal improvement**. Modern CPUs have effective automatic prefetchers. Not worth the complexity.

**Benchmark**: Max latency improved slightly (186μs vs 221μs), throughput unchanged.

---

#### `src/eventqueue_broadcast_fast_tl.h`
**Purpose**: Thread-local pool variant to eliminate pool contention.

**Why**: Test if per-thread pools improve performance.

**Result**: **FAILED**. Same issue as `objectpool_threadlocal.h` - producers and consumers are different threads.

**Benchmark**: P50=3,746μs (3000x worse than lock-free pool).

---

#### `src/eventqueue_emulator.h`
**Purpose**: Dual-queue system optimized for emulator workloads.

**Why**: Emulators have mixed criticality - audio/input need low latency, debug traces can wait.

**Key Design**:
- **Fast queue**: Inline-only (≤48B), zero allocation, always dispatched first
- **Bulk queue**: Managed payloads OK, processed when fast queue empty
- Topic-level priority assignment
- Pre-defined topic IDs for common events (VBLANK, AUDIO_SYNC, INPUT_KEY)

**Dispatch strategies**:
```cpp
queue.dispatchPriority();  // Drain all fast, then one bulk
queue.dispatchAll();       // Drain fast, then drain bulk  
queue.dispatchRatio(4);    // 4 fast per 1 bulk (prevents starvation)
```

**Usage**:
```cpp
EventQueueEmulator<4096, 16384> queue;

// Critical - uses fast queue
uint16_t vblank = queue.registerTopic(TopicPriority::Critical);
uint16_t audio = queue.registerTopic(TopicPriority::Critical);

// Normal - uses bulk queue
uint16_t trace = queue.registerTopic(TopicPriority::Normal);

queue.addObserver(vblank, vblankHandler, nullptr);
queue.addObserver(audio, audioHandler, nullptr);
queue.addObserver(trace, traceHandler, nullptr);

// Emulator loop
queue.postFast(vblank, &frameNum, sizeof(frameNum));
queue.postFast(audio, &audioSync, sizeof(audioSync));
queue.postBulk(trace, cpuState, sizeof(cpuState));

queue.dispatchPriority();  // Audio/vblank first!
```

**Benchmark**: 
| Metric | Single Queue | Dual Queue | Improvement |
|--------|--------------|------------|-------------|
| P99 | 2,203 μs | 99 μs | **22x better** |
| P999 | 2,384 μs | 130 μs | **18x better** |
| Throughput | 98 M/s | 128 M/s | 30% better |

**Best variant for emulator use cases.**

---

### Benchmarks

#### `benchmarks/realworld_benchmark.cpp`
**Purpose**: Comprehensive latency and throughput benchmarks.

**Contents**:
- `BM_RealWorld_*`: Original EventQueue benchmarks
- `BM_LockFree_*`: Lock-free variant benchmarks
- `BM_Broadcast_*`: Broadcast queue with various payload sizes
- `BM_BroadcastFast`: Fast variant comparison
- `BM_Fast_WaitStrategy`: Yield vs busy-spin vs no-wait
- `BM_Pool_*`: Lock-free vs thread-local pool comparison
- `BM_Dispatch_*`: Single vs batch dispatch
- `BM_Batch_*`: Batch with/without prefetch
- `BM_Emu_*`: Emulator dual-queue vs single-queue

**Running**:
```bash
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make message-center-benchmark

# Run specific benchmarks
./bin/message-center-benchmark --benchmark_filter="BM_Fast"
./bin/message-center-benchmark --benchmark_filter="BM_Emu"
```

---

### Documentation

#### `PERFORMANCE_ANALYSIS.md`
Complete performance analysis with:
- Benchmark methodology
- Results tables
- Optimization techniques explained
- Recommendations by use case
- Future optimization opportunities

---

## Key Learnings

### What Worked

1. **Lock-free MPMC ring buffer** - Eliminates mutex contention, 15x throughput improvement
2. **Object pooling** - Avoids malloc/free overhead, ~4x faster than new/delete
3. **Inline small payloads** - No allocation for ≤48B, covers 90% of events
4. **Raw function pointers** - 2x throughput vs std::function
5. **Observer snapshot in message** - Avoids lookup on dispatch
6. **Dual-queue priority** - 22x P99 improvement for mixed-criticality workloads
7. **Yield vs busy-spin** - yield() beats busy-spin for tail latency

### What Failed

1. **Thread-local pools** - Catastrophic for producer/consumer (objects accumulate on wrong thread)
2. **Software prefetch** - Modern CPUs prefetch automatically, manual hints add overhead
3. **Busy-spin wait** - Wastes CPU cycles, hurts tail latency due to cache contention

### Design Principles

1. **Separate hot and cold paths** - Critical events should never touch slow code
2. **Avoid allocation on hot path** - Inline payloads, pre-allocated pools
3. **Cache line alignment** - 64-byte alignment prevents false sharing
4. **Copy-on-write for observers** - Registration is rare, dispatch is hot
5. **Snapshot state into message** - Avoid lookups during dispatch

---

## Recommended Variants for Production

| Use Case | Recommended Variant | Why |
|----------|---------------------|-----|
| General event bus | `EventQueueBroadcastFast` | Best throughput, good latency |
| Low tail latency | `EventQueueBroadcastFastBatch` | 47% better P999 |
| Emulator | `EventQueueEmulator` | 22x better P99 for critical events |
| Simple/flexible API | `EventQueueBroadcast` | std::function, string topics |

---

## Files to Keep vs Exclude

### Keep (production-ready)
- `mpmc_queue.h` - Core infrastructure
- `objectpool_lockfree.h` - Best general pool
- `payload_refcounted.h` - Zero-copy broadcast
- `eventqueue_broadcast.h` - Flexible API variant
- `eventqueue_broadcast_fast.h` - Best performance
- `eventqueue_broadcast_fast_batch.h` - Best tail latency
- `eventqueue_emulator.h` - Best for emulators
- `realworld_benchmark.cpp` - Benchmarks
- `PERFORMANCE_ANALYSIS.md` - Documentation

### Exclude (experimental/failed)
- `objectpool_threadlocal.h` - Failed for P/C
- `eventqueue_broadcast_fast_tl.h` - Failed for P/C
- `eventqueue_broadcast_fast_prefetch.h` - Marginal gains
- `eventqueue_lockfree.h` - Superseded by broadcast variants
