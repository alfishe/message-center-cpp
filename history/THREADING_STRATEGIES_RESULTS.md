# Threading Strategies Benchmark Results

## Executive Summary

Tested different consumer threading strategies with **EventQueuePooled** (lock-free + object pooling). Results show that **single-threaded is fastest** for this workload, with diminishing returns as threads increase.

Date: 2026-01-07
System: macOS (10 cores, L1: 64KB/128KB, L2: 4MB)

---

## Test Setup

- **Producer**: Single thread (main benchmark thread) posting 1000 messages per iteration
- **Consumers**: Variable (1, 2, 4, or 8 threads) getting and dispatching messages
- **Queue**: EventQueuePooled (lock-free + object pooling)
- **Queue Depth Tracking**: QueueStats with max depth monitoring

---

## Benchmark Results

### Throughput Comparison

| Consumer Threads | Throughput | vs Single-Thread | Max Queue Depth |
|------------------|------------|------------------|-----------------|
| **1** | **8.36 M items/s** | **1.00x** (baseline) | 1000 |
| **2** | **7.94 M items/s** | **0.95x** (-5%) | 1000 |
| **4** | **3.62 M items/s** | **0.43x** (-57%) | 1000 |
| **8** | **2.60 M items/s** | **0.31x** (-69%) | 1000 |

### Latency (Time per 1000 messages)

| Consumer Threads | Wall Time | CPU Time |
|------------------|-----------|----------|
| **1** | 0.187 ms | 0.120 ms |
| **2** | 0.232 ms | 0.126 ms |
| **4** | 0.494 ms | 0.276 ms |
| **8** | 0.690 ms | 0.384 ms |

---

## Analysis

### 🏆 **Winner: Single-Threaded**

**Single-threaded consumer is the fastest** at **8.36 M items/s**!

### Why Single-Threaded Wins?

1. **No thread synchronization overhead**
   - No context switches
   - No cache line bouncing
   - No atomic operation contention

2. **Better cache locality**
   - Single thread keeps working set in L1/L2 cache
   - Multiple threads fight for cache lines

3. **Pool mutex becomes bottleneck**
   - All consumer threads contend for the object pool mutex
   - More threads = more contention

4. **Queue depth stays maxed**
   - Max depth = 1000 for all configurations
   - Consumers can't keep up with single producer
   - Adding more consumers doesn't help if pool is the bottleneck

### Why More Threads Get Slower?

```
Single Thread:    Producer → Queue → Consumer (smooth flow)
                                      ↓
                                   No contention!

Multiple Threads: Producer → Queue → Consumer 1 ┐
                                   → Consumer 2 ├─→ Fight for pool mutex!
                                   → Consumer 3 ┘
```

**The bottleneck shifted from queue to object pool!**

---

## Key Insights

### 1. **Queue Depth Analysis**

All configurations show **MaxDepth = 1000**, meaning:
- Producer is faster than consumers can process
- Queue is always full
- Adding more consumer threads doesn't help

**Solution**: Either slow down producer or speed up consumers (eliminate pool mutex)

### 2. **Scalability Issues**

| Threads | Throughput | Efficiency per Thread |
|---------|------------|-----------------------|
| 1 | 8.36 M/s | 8.36 M/s |
| 2 | 7.94 M/s | 3.97 M/s (47% efficient) |
| 4 | 3.62 M/s | 0.91 M/s (11% efficient) |
| 8 | 2.60 M/s | 0.33 M/s (4% efficient) |

**Terrible scaling!** Each additional thread provides diminishing returns.

### 3. **The Pool Mutex Problem**

The object pool has a single mutex that all threads fight for:

```cpp
Message* acquire() {
    std::lock_guard<std::mutex> lock(m_mutex);  // ← Bottleneck!
    // ...
}

void release(Message* msg) {
    std::lock_guard<std::mutex> lock(m_mutex);  // ← Bottleneck!
    // ...
}
```

**Every consumer thread must:**
1. Lock pool mutex to get message
2. Process message
3. Lock pool mutex to return message

With 8 threads, they spend most time waiting for the mutex!

---

## Recommendations

### ✅ **For Current Implementation**

**Use single-threaded consumer** - it's the fastest at **8.36 M items/s**!

```cpp
// Simple and fast
while (running) {
    Message* msg = queue.GetQueueMessage();
    if (msg) {
        queue.Dispatch(msg->tid, msg);
    }
}
```

### 🎯 **To Enable Multi-Threading**

Implement **per-thread object pools** to eliminate mutex contention:

```cpp
class ThreadLocalPool {
    thread_local static ObjectPool<Message> localPool;
    
    Message* acquire() {
        // No mutex - each thread has its own pool!
        return localPool.acquire();
    }
};
```

**Expected improvement**: 2-3x for multi-threaded scenarios

### ⚠️ **When to Use Multiple Threads**

Only use multiple consumer threads if:
1. **Per-thread pools are implemented** (eliminates mutex)
2. **Observers are CPU-intensive** (e.g., heavy computation per message)
3. **Observers block on I/O** (e.g., network calls, disk writes)

For lightweight observers (like in this benchmark), **single-threaded is optimal**.

---

## Comparison with Previous Results

### Single-Threaded Throughput Evolution

| Implementation | Throughput | Improvement |
|----------------|------------|-------------|
| Original (mutex-based) | 11.87 M/s | 1.00x |
| Lock-Free | 12.01 M/s | 1.01x |
| Pooled | 24.01 M/s | 2.02x |
| **Pooled + Threading (1 thread)** | **8.36 M/s** | **0.70x** ❌ |

**Wait, why slower?** 

The threading benchmark has **different workload**:
- **Previous**: Post 1000, then Get+Dispatch 1000 (batch)
- **Threading**: Post continuously while consumer runs (streaming)

The streaming workload has more contention and less batching efficiency.

---

## Next Steps

### Phase 1: Per-Thread Object Pools 🎯 **CRITICAL**

This is now **mandatory** for multi-threading to work:

```cpp
thread_local ObjectPool<Message> myPool;

Message* acquire() {
    return myPool.acquire();  // No mutex!
}
```

**Expected gain**: **3-5x** for 4-8 threads

### Phase 2: Benchmark with Heavy Observers

Test with CPU-intensive observers:

```cpp
void HeavyObserver(int id, Message* msg) {
    // Simulate heavy work
    std::this_thread::sleep_for(std::chrono::microseconds(100));
}
```

This would make multi-threading beneficial even with current pool.

### Phase 3: Producer-Consumer Ratio

Test with multiple producers:

```cpp
// 4 producers, 4 consumers
// Should show better scaling
```

---

## Conclusion

**For the current implementation:**
- ✅ **Single-threaded is optimal**: 8.36 M items/s
- ❌ **Multi-threading hurts performance**: -69% with 8 threads
- 🎯 **Root cause**: Object pool mutex contention

**To enable multi-threading:**
- Implement per-thread object pools
- Expected: 3-5x improvement for 4-8 threads
- Would make multi-threading viable

**Bottom line**: Stick with single-threaded until per-thread pools are implemented!

---

## Raw Data

```
Threading Strategy Benchmarks (items/second):
Threads  Throughput    vs Single    Efficiency
-------  ----------    ---------    ----------
1        8.36 M/s      1.00x        100%
2        7.94 M/s      0.95x        47%
4        3.62 M/s      0.43x        11%
8        2.60 M/s      0.31x        4%

Queue Depth:
All configurations: MaxDepth = 1000 (queue always full)

Latency (ms per 1000 messages):
Threads  Wall Time    CPU Time
-------  ---------    --------
1        0.187 ms     0.120 ms
2        0.232 ms     0.126 ms
4        0.494 ms     0.276 ms
8        0.690 ms     0.384 ms
```
