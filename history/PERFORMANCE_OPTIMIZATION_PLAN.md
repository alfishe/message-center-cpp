# Message Center Performance Optimization Plan

## Executive Summary
Based on benchmark analysis and code review, several optimizations can significantly improve message queue performance, particularly for high-throughput scenarios.

## Current Performance Baseline
- **Post (1 byte, 4 threads)**: 29.65 M/s
- **Post (32KB, 1 thread)**: 941 /s
- **Get (1 byte, 1 thread)**: 1.47 M/s
- **Get (32KB, 1 thread)**: 3.29 k/s

## Identified Bottlenecks

### 1. Memory Allocation in Hot Path ⚠️ **HIGH IMPACT**
**Problem**: Every `Post()` allocates a new `Message*` and `ObserverDescriptor*` on the heap.

**Location**: 
- `eventqueue.cpp:321` - `Message* message = new Message(...)`
- `eventqueue.cpp:68, 93, 102` - Observer allocations

**Impact**: 
- Heap fragmentation
- Cache misses
- Allocator overhead (mutex contention in malloc/free)

**Solution**: Object pooling
```cpp
template<typename T>
class ObjectPool {
    std::vector<T*> pool;
    std::mutex poolMutex;
    
public:
    T* acquire() {
        std::lock_guard<std::mutex> lock(poolMutex);
        if (pool.empty()) {
            return new T();
        }
        T* obj = pool.back();
        pool.pop_back();
        return obj;
    }
    
    void release(T* obj) {
        std::lock_guard<std::mutex> lock(poolMutex);
        pool.push_back(obj);
    }
};
```

**Expected Improvement**: 2-5x for small messages

---

### 2. Mutex Contention ⚠️ **HIGH IMPACT**
**Problem**: Single `m_mutexMessages` serializes all Post/Get operations.

**Location**: `eventqueue.cpp:319, 354`

**Impact**: Limited scalability with multiple threads

**Solution A**: Lock-free queue (best performance)
```cpp
// Use a lock-free SPSC or MPSC queue like:
// - Boost.Lockfree
// - moodycamel::ConcurrentQueue
// - folly::MPMCQueue

#include <boost/lockfree/queue.hpp>
boost::lockfree::queue<Message*> m_messageQueue{1024};
```

**Solution B**: Multiple queues with sharding
```cpp
// Hash topic ID to queue index
static constexpr size_t NUM_QUEUES = 8;
struct QueueShard {
    std::deque<Message*> queue;
    std::mutex mutex;
};
std::array<QueueShard, NUM_QUEUES> m_queueShards;
```

**Expected Improvement**: 3-10x for multi-threaded scenarios

---

### 3. std::deque Not Optimized for Queues ⚠️ **MEDIUM IMPACT**
**Problem**: `std::deque` is a general-purpose container, not optimized for producer-consumer.

**Location**: `eventqueue.h:83`

**Solution**: Use ring buffer or lock-free queue
```cpp
// Ring buffer with power-of-2 size for fast modulo
template<typename T, size_t N>
class RingBuffer {
    static_assert((N & (N - 1)) == 0, "N must be power of 2");
    std::array<T, N> buffer;
    std::atomic<size_t> head{0};
    std::atomic<size_t> tail{0};
    
public:
    bool push(const T& item) {
        size_t current_tail = tail.load(std::memory_order_relaxed);
        size_t next_tail = (current_tail + 1) & (N - 1);
        if (next_tail == head.load(std::memory_order_acquire))
            return false; // Full
        buffer[current_tail] = item;
        tail.store(next_tail, std::memory_order_release);
        return true;
    }
    
    bool pop(T& item) {
        size_t current_head = head.load(std::memory_order_relaxed);
        if (current_head == tail.load(std::memory_order_acquire))
            return false; // Empty
        item = buffer[current_head];
        head.store((current_head + 1) & (N - 1), std::memory_order_release);
        return true;
    }
};
```

**Expected Improvement**: 1.5-2x

---

### 4. String-based Topic Lookups ⚠️ **MEDIUM IMPACT**
**Problem**: `Post(std::string topic, ...)` requires map lookup every time.

**Location**: `eventqueue.cpp:329-332`

**Impact**: String comparison overhead, map traversal

**Solution**: Encourage integer-based API, cache topic IDs
```cpp
// User caches topic ID
class MessagePublisher {
    int cachedTopicId;
public:
    void init(EventQueue& queue) {
        cachedTopicId = queue.RegisterTopic("my.topic");
    }
    
    void sendMessage(EventQueue& queue) {
        queue.Post(cachedTopicId, payload);  // Fast path
    }
};
```

**Alternative**: Use `std::unordered_map` instead of `std::map`
```cpp
std::unordered_map<std::string, int> m_topicsResolveMap;
```

**Expected Improvement**: 2-3x for string-based Post()

---

### 5. Condition Variable Overhead (if used for blocking) ⚠️ **LOW IMPACT**
**Problem**: `m_cvEvents` (line 112) may cause spurious wakeups and context switches.

**Solution**: Use semaphore (C++20) or futex-based primitives
```cpp
#include <semaphore>
std::counting_semaphore<> m_semaphore{0};

// In Post()
m_semaphore.release();

// In blocking GetQueueMessage()
m_semaphore.acquire();
```

**Expected Improvement**: 1.2-1.5x for blocking scenarios

---

## Implementation Priority

### Phase 1: Quick Wins (1-2 days)
1. ✅ Replace `std::map` with `std::unordered_map` for topic resolution
2. ✅ Add `Post(int id, ...)` fast-path documentation
3. ✅ Pre-allocate deque capacity if known

### Phase 2: Object Pooling (2-3 days)
1. ✅ Implement `ObjectPool<Message>`
2. ✅ Integrate into `Post()` and `GetQueueMessage()`
3. ✅ Benchmark improvements

### Phase 3: Lock-Free Queue (3-5 days)
1. ✅ Evaluate libraries (Boost.Lockfree, moodycamel, folly)
2. ✅ Integrate lock-free queue
3. ✅ Remove mutex where possible
4. ✅ Benchmark improvements

### Phase 4: Advanced Optimizations (1 week)
1. ✅ Implement queue sharding
2. ✅ Add cache-line padding to avoid false sharing
3. ✅ Profile and optimize observer dispatch

---

## Expected Overall Improvement

| Scenario | Current | Expected |
|----------|---------|----------|
| Small messages, 1 thread | 1.47 M/s | **5-8 M/s** |
| Small messages, 4 threads | 29.65 M/s | **80-150 M/s** |
| Large messages, 1 thread | 941 /s | **2-3 k/s** |

---

## Additional Recommendations

### Memory Efficiency
- Use small object optimization for `MessagePayload`
- Consider arena allocator for short-lived messages

### Cache Optimization
```cpp
// Add cache-line padding to avoid false sharing
struct alignas(64) CachePaddedMutex {
    std::mutex mtx;
};
```

### Batch Processing
```cpp
// Process multiple messages at once
std::vector<Message*> GetQueueMessageBatch(size_t max_count);
```

### Profiling Tools
- Google Benchmark (already in use ✓)
- `perf` (Linux) or Instruments (macOS)
- Valgrind/Cachegrind for cache analysis

---

## References
- [Lock-Free Programming](https://preshing.com/20120612/an-introduction-to-lock-free-programming/)
- [moodycamel::ConcurrentQueue](https://github.com/cameron314/concurrentqueue)
- [Boost.Lockfree](https://www.boost.org/doc/libs/1_84_0/doc/html/lockfree.html)
