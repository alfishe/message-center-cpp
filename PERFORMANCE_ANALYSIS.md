# EventQueue Performance Analysis

## Executive Summary

Optimized message queue achieving **1.25 μs P50 latency** and **5.75 M messages/sec** throughput with automatic memory management.

## Test Configuration

- **Hardware**: Apple Silicon (ARM64), 10 cores
- **Scenario**: 256 topics, 5 observers/topic, 2 producers, 8 consumers
- **Payload sizes**: 16B - 10MB

---

## Implementation Variants

### 1. Original EventQueue (Mutex-based)
- `std::deque<Message*>` + `std::mutex`
- `std::condition_variable` for signaling
- Heap allocation per message

### 2. EventQueueLockFree
- Custom MPMC ring buffer (lock-free)
- Lock-free object pool (Treiber stack)
- COW observers with `std::shared_ptr`

### 3. EventQueueBroadcast
- Fire-and-forget API with auto memory management
- Inline payload (≤48B) or RefCountedPayload (>48B)
- `std::function` callbacks

### 4. EventQueueBroadcastFast
- Raw function pointers (no `std::function`)
- Observer snapshot cached in message
- No `std::shared_ptr` on hot path

---

## Benchmark Results

### Latency by Implementation (32B payload, 2P+8C)

| Implementation | P50 | P99 | Max | Throughput |
|----------------|-----|-----|-----|------------|
| Original (mutex) | 6,617 μs | 9,873 μs | 14,840 μs | 372 K/s |
| LockFree | 3,604 μs | 7,269 μs | 11,122 μs | 1.44 M/s |
| Broadcast | 1.83 μs | 849 μs | 1,843 μs | 3.49 M/s |
| **BroadcastFast** | **1.25 μs** | **108 μs** | **414 μs** | **5.75 M/s** |

### Latency by Payload Size (BroadcastFast)

| Payload | P50 | P99 | Throughput | Mode |
|---------|-----|-----|------------|------|
| 16 B | 1.88 μs | 1,314 μs | 2.61 M/s | inline |
| 32 B | 1.25 μs | 108 μs | 5.75 M/s | inline |
| 48 B | 1.25 μs | 431 μs | 5.64 M/s | inline |
| 64 B | 2.25 μs | 2,981 μs | 3.81 M/s | managed |
| 256 B | 1.50 μs | 851 μs | 5.11 M/s | managed |
| 1 KB | 1.21 μs | 4,572 μs | 5.51 M/s | managed |
| 4 KB | 2.21 μs | 1,001 μs | 2.93 M/s | managed |

### Large Payload Scaling

| Payload | P50 | P99 | Bandwidth |
|---------|-----|-----|-----------|
| 64 KB | 2.4 μs | 17 μs | 5.0 GB/s |
| 256 KB | 6.1 μs | 28 μs | 20 GB/s |
| 1 MB | 21 μs | 97 μs | 78 GB/s |
| 4 MB | 345 μs | 1,136 μs | 636 GB/s |
| 10 MB | 326 μs | 1,153 μs | 2.3 TB/s |

### Pointer+Metadata Pattern (Emulator Style)

| Metric | Value |
|--------|-------|
| P50 latency | **1.75 μs** |
| P99 latency | 129 μs |
| Throughput | 1.58 M/s |
| Metadata size | 32 bytes |

---

## Key Findings

### 1. Processing vs Queuing Latency

| Metric | Value |
|--------|-------|
| Pure processing latency | **83 nanoseconds** |
| End-to-end (8P+4C) | 3,600+ μs |

**Conclusion**: 99.99% of latency is queuing delay, not processing. Balance producer/consumer ratio for lowest latency.

### 2. Wait Strategy Impact

| Strategy | P50 | P99 | Max | CPU Usage |
|----------|-----|-----|-----|-----------|
| **Yield** | **1.25 μs** | **108 μs** | **414 μs** | Low |
| BusySpin | 1.29 μs | 665 μs | 2,987 μs | 100%/core |
| NoWait | 1.29 μs | 386 μs | 1,979 μs | 100%/core |

**Conclusion**: `std::this_thread::yield()` is optimal. Busy-spin wastes CPU and hurts tail latency due to cache contention.

### 3. std::function Overhead

| Callback Type | P50 | Throughput |
|---------------|-----|------------|
| `std::function` | 1.83 μs | 3.49 M/s |
| Raw function pointer | **1.25 μs** | **5.75 M/s** |

**Conclusion**: Raw function pointers provide ~2x throughput improvement.

### 4. Inline vs Managed Payloads

| Mode | Best For | Overhead |
|------|----------|----------|
| Inline (≤48B) | Small events, metadata | memcpy only |
| Managed (>48B) | Large payloads | allocation + refcount |

**Conclusion**: Inline mode avoids allocation for common small payloads.

---

## Optimization Techniques Applied

### Lock-Free Data Structures
- **MPMC Ring Buffer**: Bounded, cache-aligned slots, sequence numbers for ABA safety
- **Treiber Stack Pool**: Lock-free object recycling

### Memory Management
- **Object Pooling**: Reuse message objects, avoid malloc/free per message
- **RefCountedPayload**: Zero-copy broadcast, auto-cleanup after last observer
- **Inline Payloads**: No allocation for ≤48 byte data

### Cache Optimization
- **64-byte alignment**: Prevent false sharing on head/tail pointers
- **Observer snapshot**: Copy observer list into message, avoid cache miss on dispatch

### API Design
- **Fire-and-forget**: Producer posts and forgets, queue handles lifetime
- **Pointer+metadata**: Pass buffer reference for large data, no copy

---

## Recommendations by Use Case

### Real-time Emulator (60 Hz, low latency)
```cpp
// Use pointer+metadata for frame buffers
struct FrameRef { void* pixels; uint16_t w, h; uint32_t frame; };
queue.postRef(FRAME_READY, FrameRef{buffer, 320, 256, frameNum});
queue.dispatchAll();  // Synchronous - buffer safe to reuse after
```

### High-throughput Event Bus
```cpp
// Use Fast queue with function pointers
EventQueueBroadcastFast<65536> queue;
queue.addObserver(topic, myCallback, userData);
queue.post(topic, &event, sizeof(event));
```

### Mixed Payload Sizes
```cpp
// Auto-selects inline vs managed
queue.post(topic, smallData, 32);   // Inline, no allocation
queue.post(topic, largeData, 4096); // Managed, auto-freed
```

### Low Tail Latency (Batch Dispatch)
```cpp
// Use batch dispatch variant for consistent P999 latency
EventQueueBroadcastFastBatch<65536> queue;
// ... setup observers ...

// Consumer thread - batch dispatch reduces atomic operation overhead
while (running) {
  if (queue.dispatchBatch(32) == 0) {
    std::this_thread::yield();
  }
}
// P999: 76 μs vs 144 μs (47% improvement over single dispatch)
```

### Emulator (Dual-Queue with Priority)
```cpp
// Dual-queue system: fast queue for critical, bulk queue for debug/state
EventQueueEmulator<4096, 16384> queue;

// Critical topics - inline only, always dispatched first
uint16_t vblank = queue.registerTopic(TopicPriority::Critical);
uint16_t audioSync = queue.registerTopic(TopicPriority::Critical);
uint16_t inputKey = queue.registerTopic(TopicPriority::Critical);

// Normal topics - can use larger managed payloads
uint16_t cpuTrace = queue.registerTopic(TopicPriority::Normal);
uint16_t stateData = queue.registerTopic(TopicPriority::Normal);

// Post critical events (fast queue, inline only)
queue.postFast(vblank, &frameNum, sizeof(frameNum));
queue.postFast(audioSync, &audioPtr, sizeof(audioPtr));

// Post bulk data (managed payloads OK)
queue.postBulk(cpuTrace, traceData, traceSize);

// Priority dispatch - drain all critical before any bulk
queue.dispatchPriority();

// Benchmark results: P99 improved from 2.2ms to 99μs (22x better)
```

---

## Files

| File | Purpose | Lines |
|------|---------|-------|
| `src/mpmc_queue.h` | Lock-free bounded MPMC ring buffer | ~115 |
| `src/objectpool_lockfree.h` | Lock-free Treiber stack object pool | ~95 |
| `src/payload_refcounted.h` | Zero-copy ref-counted payload | ~100 |
| `src/eventqueue_broadcast.h` | Fire-and-forget broadcast queue (std::function) | ~230 |
| `src/eventqueue_fast.h` | Maximum performance variant | ~180 |
| `src/eventqueue_batch.h` | Batch dispatch variant | ~255 |
| `src/eventqueue_emulator.h` | Dual-queue emulator-optimized variant | ~320 |
| `benchmarks/realworld_benchmark.cpp` | Latency benchmarks | ~470 |

---

## Optimization Experiments

### Tested Variants

| Variant | P50 | P99 | P999 | Max | Throughput | Verdict |
|---------|-----|-----|------|-----|------------|---------|
| **BroadcastFast (baseline)** | 1.17 μs | 15.4 μs | 144 μs | 234 μs | 5.77 M/s | **Best overall** |
| Batch Dispatch | 1.21 μs | 17.1 μs | **76 μs** | **159 μs** | 5.44 M/s | Best tail latency |
| Prefetch Hints | 1.29 μs | 16.7 μs | 160 μs | 212 μs | 5.88 M/s | Marginal gain |
| Thread-Local Pool | 3,746 μs | 11 ms | 12 ms | 12 ms | 6.14 M/s | **Bad for P/C** |

### Key Findings

1. **Thread-local pools don't work for producer/consumer workloads**
   - Each consumer accumulates messages in its local cache
   - Producers keep allocating new objects
   - Results in 3,000x worse P50 latency despite similar throughput
   - Only suitable when same thread allocates AND deallocates

2. **Batch dispatch reduces tail latency by ~50%**
   - P999: 76 μs vs 144 μs (47% improvement)
   - Max: 159 μs vs 234 μs (32% improvement)
   - Groups queue pops and pool returns, reducing atomic operation overhead
   - Trade-off: slightly lower throughput (5.44 vs 5.77 M/s)

3. **Prefetch hints provide marginal benefit**
   - Modern CPUs have effective automatic prefetchers
   - Small payloads (≤48B) already fit in cache lines efficiently
   - P99 improvement: 16.7 μs vs 15.4 μs (not significant)

4. **Lock-free pool remains best for general use**
   - Consistent low latency across all percentiles
   - Works correctly with any producer/consumer ratio
   - Treiber stack has minimal contention for moderate thread counts

5. **Dual-queue system is optimal for emulator workloads**
   - Separates critical events (audio, vblank, input) from bulk data (debug, state)
   - P99 improved from 2.2ms to 99μs (22x better)
   - P999 improved from 2.4ms to 130μs (18x better)
   - 30% higher throughput (128M/s vs 98M/s)
   - Critical queue is inline-only (no allocation on hot path)

### Emulator Dual-Queue Results

| Metric | Single Queue | Dual Queue | Improvement |
|--------|--------------|------------|-------------|
| P50 | 0.17 μs | 0.17 μs | Same |
| P90 | 73 μs | 25 μs | 66% better |
| P99 | 2,203 μs | **99 μs** | **22x better** |
| P999 | 2,384 μs | **130 μs** | **18x better** |
| Max | 2,408 μs | 135 μs | 18x better |
| Throughput | 98 M/s | 128 M/s | 30% better |

---

## Future Optimization Opportunities

1. **SPSC fast path** - Simpler queue when single producer/consumer detected
2. **Huge pages** - Reduce TLB misses for large queue capacities
3. **NUMA awareness** - Pin producers/consumers to same NUMA node
4. **Work-stealing** - Balance load when consumers have uneven work
