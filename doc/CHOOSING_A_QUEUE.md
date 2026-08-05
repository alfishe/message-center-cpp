# Choosing the Right Queue

This guide helps you select the appropriate queue variant for your use case.

## Queue Variants Overview

| Variant | Architecture | Best For |
|---------|--------------|----------|
| `MessageCenter` | Mutex + background thread | General apps, simplest API |
| `EventQueueBroadcastFast` | Lock-free MPMC | High throughput, low latency |
| `EventQueueBroadcastFastBatch` | Lock-free + batching | Lowest tail latency (P999) |
| `EventQueueEmulator` | Dual lock-free queues | Games, emulators, mixed workloads |
| `EventQueueBroadcast` | Lock-free + std::function | Flexible callbacks, lambdas |

## Performance Comparison

### Throughput & Latency

| Variant | Post Latency | End-to-End P50 | Throughput |
|---------|--------------|----------------|------------|
| MessageCenter | ~110 ns | ~33 μs | 9.1 M/s |
| EventQueueFast | ~175 ns | 1.2 μs | 5.75 M/s |
| EventQueueBatch | ~180 ns | 1.2 μs | 5.44 M/s |
| EventQueueEmulator | ~170 ns | 0.17 μs | 128 M/s |

**Note:** MessageCenter post latency appears lower because dispatch happens asynchronously on a background thread. The end-to-end latency (post → callback invocation) is significantly higher.

### Tail Latency

| Variant | P99 | P999 | Max |
|---------|-----|------|-----|
| MessageCenter | — | — | — |
| EventQueueFast | 108 μs | 144 μs | 234 μs |
| EventQueueBatch | 17 μs | 76 μs | 159 μs |
| EventQueueEmulator | 99 μs | 130 μs | 135 μs |

## Architecture Differences

### MessageCenter

```
Producer → [Mutex Lock] → Queue → [Condition Variable] → Background Thread → Callback
```

**Characteristics:**
- Uses `std::mutex` and `std::condition_variable`
- Background dispatcher thread (auto-started)
- String-based topic names with `std::map` lookup
- Heap allocation per `MessagePayload`
- Fire-and-forget: post returns immediately

**Overhead sources:**
1. Mutex acquisition on every post (~50-100 ns)
2. Condition variable signal (~20-50 ns)
3. Thread wake-up latency (~10-30 μs)
4. String topic resolution (~90 ns per lookup)
5. Heap allocation per message (~50-200 ns)

### Lock-Free Queues (Fast/Batch/Emulator)

```
Producer → [Atomic CAS] → Ring Buffer → [Manual Dispatch] → Callback
```

**Characteristics:**
- Lock-free MPMC ring buffer with sequence numbers
- Object pooling (Treiber stack) for message reuse
- Integer topic IDs (no string lookup)
- Inline payloads (≤48B) or ref-counted (>48B)
- Manual dispatch required (`dispatchAll()`)

**Why faster:**
1. No mutex contention — atomic compare-and-swap only
2. No thread wake-up — dispatch on caller's thread
3. No heap allocation for small payloads
4. Cache-aligned structures prevent false sharing

## Decision Guide

### Use MessageCenter when:

- Building a typical application (GUI, server, tool)
- Simplicity matters more than microseconds
- You want automatic background dispatch
- String topic names improve code readability
- ~30-50 μs latency is acceptable

```cpp
auto& mc = MessageCenter::DefaultMessageCenter();
mc.AddObserver("player.moved", myCallback);
mc.Post("player.moved", new PlayerEvent(...));
// Dispatch happens automatically on background thread
```

### Use EventQueueBroadcastFast when:

- Building performance-critical systems
- Need <5 μs end-to-end latency
- High message throughput (>1M msg/sec)
- Can call `dispatchAll()` at appropriate points
- Memory efficiency matters (no heap per message)

```cpp
EventQueueBroadcastFast<4096> queue;
uint16_t topic = queue.registerTopic();
queue.addObserver(topic, callback, userData);
queue.post(topic, &data, sizeof(data));
queue.dispatchAll();  // Must call manually
```

### Use EventQueueBroadcastFastBatch when:

- Tail latency (P999) is critical
- Running in latency-sensitive loops
- Willing to trade ~5% throughput for 47% better P999

```cpp
EventQueueBroadcastFastBatch<4096> queue;
// Same API as Fast variant
// Batches atomic operations internally
while (running) {
    queue.dispatchBatch(32);  // Process up to 32 at once
}
```

### Use EventQueueEmulator when:

- Building games or emulators
- Have mixed-criticality events (audio vs debug)
- Need to guarantee critical events dispatch first
- Running at high frame rates (1000+ fps)

```cpp
EventQueueEmulator<4096, 16384> queue;  // Critical, Bulk capacities

uint16_t vblank = queue.registerTopic(TopicPriority::Critical);
uint16_t trace = queue.registerTopic(TopicPriority::Normal);

queue.postFast(vblank, &frameNum, 4);   // Goes to critical queue
queue.postBulk(trace, traceData, 1024); // Goes to bulk queue

queue.dispatchAll();  // Drains critical first, then bulk
```

### Use EventQueueBroadcast when:

- Need `std::function` callbacks (capturing lambdas)
- Flexibility matters more than raw speed
- Still want lock-free performance
- Moderate throughput requirements (3-4 M/s)

```cpp
EventQueueBroadcast<4096> queue;
int capturedValue = 42;
queue.addObserver(topic, [capturedValue](uint16_t t, const void* data, size_t sz) {
    // Can capture external state
});
```

## Migration Path

If starting with MessageCenter and need more performance later:

1. **Profile first** — MessageCenter may be fast enough
2. **Identify hot paths** — only optimize where needed
3. **Switch incrementally** — use Fast queues for critical paths
4. **Keep MessageCenter** — for non-critical subsystems

Mixed usage is fine:
```cpp
// Critical path: lock-free
EventQueueBroadcastFast<4096> audioQueue;

// Non-critical: convenience
auto& mc = MessageCenter::DefaultMessageCenter();
mc.AddObserver("ui.update", ...);
```

## Memory Characteristics

| Variant | Allocation Pattern | Payload Handling |
|---------|-------------------|------------------|
| MessageCenter | Heap per message | `MessagePayload*` derived class |
| EventQueueFast | Pool + inline | ≤48B inline, >48B ref-counted |
| EventQueueBatch | Pool + inline | ≤48B inline, >48B ref-counted |
| EventQueueEmulator | Pool + inline | Critical: inline only, Bulk: ref-counted OK |
| EventQueueBroadcast | Pool + inline | ≤48B inline, >48B ref-counted |

### Payload Size Recommendations

| Size | Recommendation |
|------|----------------|
| ≤48 bytes | Use inline (automatic) |
| 49-256 bytes | Ref-counted OK, slight overhead |
| 256B - 4KB | Consider pointer + metadata pattern |
| >4KB | Always use pointer + metadata |

Pointer + metadata pattern:
```cpp
struct FrameRef {
    void* pixels;      // Pointer to external buffer
    uint16_t width;
    uint16_t height;
    uint32_t frameNum;
};  // 16 bytes, inline storage

queue.post(topic, &FrameRef{buffer, 320, 240, frame}, sizeof(FrameRef));
// Buffer ownership stays with caller
```

## Thread Safety

All variants are thread-safe for:
- Multiple producers posting concurrently
- Single consumer dispatching

For multiple consumers, use external synchronization or partition topics.
