# MessageCenter Performance Optimization

## Performance Comparison

### Final Benchmark Results

| Metric | Original | MessageCenterFast | Winner |
|--------|----------|-------------------|--------|
| Post throughput | 26 M/s | 5.9 M/s | Original (4.4x) |
| End-to-end P50 | 15.4 μs | **0.79 μs** | Fast (19x) |
| End-to-end P99 | 54 μs | **2.8 μs** | Fast (19x) |
| Multi-producer | ~26 M/s | **662 M/s** | Fast (25x) |
| Idle CPU | 0% | 0% | Tie |
| Delivery rate | 100% | 100% | Tie |

### Original MessageCenter (mutex-based)

| Metric | Value |
|--------|-------|
| Post throughput (1 producer) | 26 M/s |
| Post latency | 38 ns |
| End-to-end P50 | 15.4 μs |
| End-to-end P99 | 54 μs |

### Bottleneck Analysis

The original MessageCenter post path:

```
Post() {
    lock(mutex)           // ~20ns uncontended
    new Message()         // ~50ns heap alloc
    deque.push_back()     // ~10ns
    unlock(mutex)         // ~5ns
    cv.notify_one()       // ~20ns
}
```

Total: ~105ns theoretical, measured ~37ns (optimized by compiler/CPU)

### MessageCenterFast (lock-free + adaptive wait)

Uses lock-free MPMC queue with adaptive wait strategy:
- **Active**: Yields briefly for low latency
- **Idle**: Blocks on condition variable (zero CPU)

| Metric | Value |
|--------|-------|
| Post throughput | 5.9 M/s |
| End-to-end P50 | **0.79 μs** |
| End-to-end P99 | **2.8 μs** |
| Idle CPU | 0% |

### Trade-offs

**Original wins on single-producer throughput because:**
1. Uncontended mutex is very fast (~20ns on modern CPUs)
2. No object pool overhead
3. No payload memcpy (stores pointer directly)

**Fast wins on:**
1. **Latency**: 19x better P50/P99 (lock-free dispatch)
2. **Multi-producer**: 25x better scaling (no mutex contention)
3. **Predictability**: Consistent sub-microsecond latency

## Optimization Strategies

### 1. Reduce Allocation Overhead (High Impact)

**Current**: `new Message()` on every post

**Options:**
- **Object pooling**: Pre-allocate Message objects, reuse after dispatch
- **Arena allocator**: Bulk allocate, reset after drain
- **Inline messages**: Store small payloads directly in queue slots

Expected improvement: 2-3x for small payloads

### 2. Batch Notifications (Medium Impact)

**Current**: `cv.notify_one()` on every post

**Options:**
- Only notify when transitioning empty → non-empty
- Batch notifications every N posts
- Use atomic flag instead of condition variable

Expected improvement: 10-20% throughput

### 3. Reduce Mutex Scope (Medium Impact)

**Current**: Mutex held during entire post

**Options:**
- Use try_lock with fallback queue
- Separate queues per topic (reduce contention)
- Reader-writer lock for observer access

Expected improvement: 2-5x under contention

### 4. Optimize Payload Handling (Medium Impact)

**Current**: `MessagePayload*` requires heap allocation

**Options:**
- Inline small payloads (≤64 bytes)
- Use variant/union for common payload types
- Support zero-copy for large payloads (pointer + size)

Expected improvement: 2x for small payloads

### 5. Worker Thread Optimization (Low-Medium Impact)

**Current**: Single dispatcher thread, waits on CV

**Options:**
- Multiple dispatcher threads (partition by topic)
- Adaptive polling (spin briefly before blocking)
- Work-stealing between threads

Expected improvement: Linear scaling with threads

## Recommended Approach

### Choose based on your needs:

| Use Case | Recommendation |
|----------|----------------|
| General apps | Original MessageCenter |
| Latency-critical | MessageCenterFast |
| Multi-producer | MessageCenterFast |
| Maximum throughput | EventQueueBroadcastFast (manual dispatch) |

### Decision guide:

1. **Original MessageCenter** — Best for most applications
   - Simple API, automatic dispatch
   - 26M msg/sec single-producer
   - ~15μs latency is acceptable for most use cases

2. **MessageCenterFast** — Best for latency-critical code
   - Sub-microsecond P50 latency (0.79μs)
   - Excellent multi-producer scaling (662M/s)
   - Zero CPU when idle
   - Same fire-and-forget API

3. **EventQueueBroadcastFast** — Maximum control
   - 5.75M msg/sec with manual dispatch
   - Sub-2μs latency
   - No background thread overhead
   - Requires calling dispatchAll() manually

## Implementation Recommendations

### Quick Wins (Minimal Code Change)

1. **Cache topic IDs**: Resolve once, post by ID
   ```cpp
   // Instead of:
   mc.Post("player.moved", payload);
   
   // Use:
   static int topicId = mc.RegisterTopic("player.moved");
   mc.Post(topicId, payload);
   ```
   Saves: ~90ns per post

2. **Reuse payload objects**: Pool MessagePayload instances
   ```cpp
   // Instead of:
   mc.Post(topic, new PlayerEvent(...));
   
   // Use:
   PlayerEvent* evt = eventPool.acquire();
   evt->set(...);
   mc.Post(topic, evt, false);  // Don't auto-cleanup
   // In observer: eventPool.release(evt);
   ```
   Saves: ~50-100ns per post

### Larger Refactoring

1. **Replace deque with ring buffer** (bounded queue)
2. **Add message pooling** to EventQueue base class
3. **Implement batch dispatch** in worker thread
4. **Add inline payload support** for small messages

## Benchmark Commands

```bash
# Build benchmarks
cd build && cmake .. && cmake --build . --target message-center-benchmark

# Run MessageCenter benchmarks
./bin/message-center-benchmark --benchmark_filter="MessageCenter"

# Compare original vs fast
./bin/message-center-benchmark --benchmark_filter="Compare"

# Full benchmark with repetitions
./bin/message-center-benchmark --benchmark_repetitions=3
```
