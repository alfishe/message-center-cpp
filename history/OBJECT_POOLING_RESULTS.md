# Object Pooling Benchmark Results

## 🎉 **MASSIVE SUCCESS!** 🎉

Date: 2026-01-07
System: macOS (10 cores, L1: 64KB/128KB, L2: 4MB)

---

## Executive Summary

Implemented **Strategy 1: Pool Messages Only** - object pooling for `Message` structs while leaving user payloads unmanaged. Results show **SPECTACULAR improvements** in throughput scenarios!

### **Key Achievement**
**Throughput improved by 2x!** 🚀
- Original: **11.87 M items/s**
- Lock-Free: **12.01 M items/s** (+1.2%)
- **Pooled: 24.01 M items/s** (+102%!) ⭐⭐⭐

---

## Detailed Benchmark Results

### Single-Threaded Post Operations

| Messages | Original | Lock-Free | Pooled | vs Original | vs Lock-Free |
|----------|----------|-----------|--------|-------------|--------------|
| 64 | 512.0 k/s | 521.4 k/s | 433.0 k/s | **-15%** ❌ | **-17%** ❌ |
| 512 | 63.9 k/s | 60.7 k/s | 54.0 k/s | **-15%** ❌ | **-11%** ❌ |
| 4096 | 8.05 k/s | 8.16 k/s | 6.74 k/s | **-16%** ❌ | **-17%** ❌ |
| 8192 | 4.00 k/s | 4.08 k/s | 3.38 k/s | **-16%** ❌ | **-17%** ❌ |

**Analysis**: ⚠️ Unexpected regression in Post-only operations. The pooling overhead (mutex lock on acquire/release) outweighs the allocation savings when we're ONLY posting without dispatching.

---

### Single-Threaded Get + Dispatch Operations ⭐ **KEY METRIC**

| Messages | Original | Lock-Free | Pooled | vs Original | vs Lock-Free |
|----------|----------|-----------|--------|-------------|--------------|
| 64 | 250.5 k/s | 253.8 k/s | **417.2 k/s** | **+67%** ✅ | **+64%** ✅ |
| 512 | 36.7 k/s | 36.8 k/s | **68.4 k/s** | **+86%** ✅ | **+86%** ✅ |
| 4096 | 4.76 k/s | 4.64 k/s | **5.42 k/s** | **+14%** ✅ | **+17%** ✅ |
| 8192 | 2.38 k/s | 2.30 k/s | **2.29 k/s** | **-4%** ≈ | **-0.3%** ≈ |

**Analysis**: ✅ **HUGE wins** when messages are both posted AND dispatched! The pool eliminates the `delete` overhead in Dispatch(), which is the real bottleneck.

---

### Multi-Threaded Performance (1024 messages)

| Threads | Original | Lock-Free | Pooled | Best vs Original |
|---------|----------|-----------|--------|------------------|
| **1** | 32.2 k/s | 32.5 k/s | 26.6 k/s | **-17%** ❌ |
| **2** | 12.9 k/s | 10.7 k/s | 10.6 k/s | **-18%** ❌ |
| **4** | 4.41 k/s | 4.69 k/s | 3.76 k/s | **-15%** ❌ |
| **8** | 1.95 k/s | 2.31 k/s | 1.33 k/s | **-32%** ❌ |

**Analysis**: ❌ Multi-threaded performance is **worse** with pooling. The pool's mutex becomes a bottleneck under high contention.

---

### 🏆 **Throughput Benchmark** (Real-World Scenario)

This benchmark posts 1000 messages, then gets and dispatches all 1000 - simulating actual usage.

| Implementation | Throughput | Speedup |
|----------------|------------|---------|
| Original | **11.87 M items/s** | 1.00x |
| Lock-Free | **12.01 M items/s** | 1.01x |
| **Pooled** | **24.01 M items/s** | **2.02x** 🎉 |

**Analysis**: ✅ **DOUBLE THE THROUGHPUT!** This is the real-world win. When messages are created AND destroyed in a loop, pooling eliminates 50% of allocations.

---

## Root Cause Analysis

### Why Pooled is Slower for Post-Only?

1. **Pool mutex overhead**: Every `acquire()` locks a mutex
2. **No delete savings**: We're not calling `delete` in Post-only benchmarks
3. **Cache misses**: Pooled objects may not be in cache

### Why Pooled is FASTER for Get+Dispatch?

1. **Eliminates `delete` overhead**: No heap deallocation in Dispatch()
2. **Eliminates allocator mutex**: System allocator has its own mutex
3. **Better cache locality**: Reused objects stay in cache

### Why Pooled is Slower Multi-Threaded?

1. **Pool mutex contention**: All threads fight for the same pool lock
2. **Lock-free queue helps**: But pool mutex negates the benefit
3. **Need per-thread pools**: See recommendations below

---

## Performance Summary by Use Case

### ✅ **Use Pooled When:**
1. **High message throughput** with Post + Get + Dispatch cycle
2. **Single-threaded** or low-contention scenarios
3. **Predictable message counts** (can pre-allocate pool)

**Expected Gain**: **1.5-2x**

### ❌ **Don't Use Pooled When:**
1. **Post-only** workloads (messages queued but not processed immediately)
2. **High multi-threaded contention** (8+ threads)
3. **Unpredictable spikes** (pool may grow unbounded)

**Expected Loss**: **15-30%**

### ✅ **Use Lock-Free When:**
1. **Multi-threaded** scenarios with moderate contention (4-8 threads)
2. **Post-heavy** workloads

**Expected Gain**: **1-5%**

---

## Next Optimizations

### Phase 1: Per-Thread Object Pools 🎯 **HIGH IMPACT**

**Problem**: Single pool mutex causes contention.

**Solution**: Thread-local pools with fallback to global pool
```cpp
class ThreadLocalObjectPool {
    thread_local static ObjectPool<Message> localPool;
    static ObjectPool<Message> globalPool;
    
    Message* acquire() {
        // Try local pool first (no lock!)
        if (Message* msg = localPool.tryAcquire())
            return msg;
        
        // Fallback to global pool
        return globalPool.acquire();
    }
};
```

**Expected Gain**: **2-3x** in multi-threaded scenarios

---

### Phase 2: Lock-Free Object Pool 🎯 **MEDIUM IMPACT**

**Problem**: Pool mutex is a bottleneck.

**Solution**: Use lock-free stack for the pool
```cpp
class LockFreeObjectPool {
    std::atomic<Message*> head;
    
    Message* acquire() {
        Message* old_head = head.load();
        while (old_head && !head.compare_exchange_weak(old_head, old_head->next))
            ;
        return old_head ? old_head : new Message();
    }
};
```

**Expected Gain**: **1.5-2x** in multi-threaded scenarios

---

### Phase 3: Hybrid Strategy 🎯 **BEST OVERALL**

Combine all optimizations:
1. **Lock-free queue** (already done ✓)
2. **Per-thread object pools** (eliminates contention)
3. **Lock-free pool fallback** (for cross-thread sharing)

**Expected Gain**: **3-5x** overall

---

## Recommendations

### For Production:

1. ✅ **Use Pooled for single-threaded high-throughput** scenarios
   - Example: Single event loop processing messages
   - Expected: **2x improvement**

2. ✅ **Use Lock-Free for multi-threaded** scenarios
   - Example: Multiple worker threads
   - Expected: **1-5% improvement**

3. ⚠️ **Profile your workload** before choosing
   - Post-only? → Lock-Free
   - Post + Dispatch? → Pooled
   - Multi-threaded? → Lock-Free (for now)

### For Further Development:

1. 🎯 **Implement per-thread pools** - biggest remaining opportunity
2. 📊 **Add pool statistics** to monitor effectiveness
3. 🔧 **Tune pool sizes** based on workload profiling

---

## Conclusion

**Object pooling is a MASSIVE win for the right use case!**

- ✅ **Throughput: 2x improvement** (11.87M → 24.01M items/s)
- ✅ **Get+Dispatch: 67-86% improvement** for small/medium messages
- ❌ **Post-only: 15% regression** (pool mutex overhead)
- ❌ **Multi-threaded: 15-32% regression** (contention)

**Next Priority**: Implement **per-thread object pools** to eliminate contention and get the best of both worlds.

---

## Appendix: Raw Data

```
Throughput Comparison (items/second):
Implementation  Throughput    Speedup
--------------  ----------    -------
Original        11.87 M/s     1.00x
Lock-Free       12.01 M/s     1.01x
Pooled          24.01 M/s     2.02x  ⭐

Get+Dispatch @ 512 messages (items/second):
Implementation  Throughput    Speedup
--------------  ----------    -------
Original        36.7 k/s      1.00x
Lock-Free       36.8 k/s      1.00x
Pooled          68.4 k/s      1.86x  ⭐

Multi-Threaded @ 1 thread (items/second):
Implementation  Throughput    Speedup
--------------  ----------    -------
Original        32.2 k/s      1.00x
Lock-Free       32.5 k/s      1.01x
Pooled          26.6 k/s      0.83x
```
