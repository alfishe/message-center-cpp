#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_FAST_H
#define MESSAGE_CENTER_EVENTQUEUE_FAST_H

// EventQueueBroadcastFast - High-performance lock-free broadcast queue
//
// Performance: P50=1.2μs, 5.5M msg/sec (15x faster than mutex-based)
//
// Features:
// - Lock-free MPMC queue with object pooling
// - Fire-and-forget API (caller can reuse buffer immediately)
// - Small payloads (≤48B) stored inline, larger ones ref-counted
// - Raw function pointers for minimal callback overhead
// - Observer snapshot cached in message (no lookup on dispatch)
//
// Thread safety:
// - post(): Thread-safe, multiple producers supported
// - dispatchOne()/dispatchAll(): Thread-safe, multiple consumers supported
// - addObserver(): NOT thread-safe with post() - register before posting
//
// Usage:
//   EventQueueBroadcastFast<65536> queue;
//   uint16_t topic = queue.registerTopic();
//   queue.addObserver(topic, myCallback, userData);
//   queue.post(topic, data, size);
//   queue.dispatchAll();

#include "mpmc_queue.h"
#include "objectpool_lockfree.h"
#include "payload_refcounted.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

constexpr size_t FAST_MAX_TOPICS = 4096;
constexpr size_t FAST_INLINE_MAX = 48;
constexpr size_t FAST_MAX_OBSERVERS = 16;

// Raw function pointer callback - no std::function overhead
using FastCallback = void (*)(uint16_t topicId, const void *data, size_t size, void *userData);

struct FastObserver {
  FastCallback callback;
  void *userData;
};

struct FastMessage {
  uint16_t topicId;
  uint8_t observerCount;
  bool isInline;
  size_t payloadSize;

  // Snapshot of observers at post time - no lookup needed on dispatch
  FastObserver observers[FAST_MAX_OBSERVERS];

  union {
    RefCountedPayload *managed;
    alignas(alignof(std::max_align_t)) char inlineData[FAST_INLINE_MAX];
  };
};

template <size_t QueueCapacity = 65536>
class EventQueueBroadcastFast {
  static_assert((QueueCapacity & (QueueCapacity - 1)) == 0,
                "QueueCapacity must be a power of 2");

  MPMCQueue<FastMessage *, QueueCapacity> m_queue;
  ObjectPoolLockFree<FastMessage> m_messagePool;

  // Simple array of observer lists - no COW, just raw pointers
  // Registration is rare, dispatch is hot path.
  // Chunked in 64-topic blocks so normal usage (< 64 topics) only allocates 16.5 KB
  // instead of 1.05 MB up front, while scaling lazily up to FAST_MAX_TOPICS (4096).
  struct TopicObservers {
    std::atomic<uint8_t> count{0};
    FastObserver observers[FAST_MAX_OBSERVERS];
  };

  static constexpr size_t TOPIC_CHUNK_SHIFT = 6; // 64 topics per chunk
  static constexpr size_t TOPIC_CHUNK_SIZE = 1u << TOPIC_CHUNK_SHIFT;
  static constexpr size_t TOPIC_CHUNK_MASK = TOPIC_CHUNK_SIZE - 1;
  static constexpr size_t TOPIC_NUM_CHUNKS =
      FAST_MAX_TOPICS / TOPIC_CHUNK_SIZE;

  TopicObservers *m_topicBase{nullptr};
  std::unique_ptr<std::atomic<TopicObservers *>[]> m_topicChunks{
      new std::atomic<TopicObservers *>[TOPIC_NUM_CHUNKS]};
  std::mutex m_topicGrowMutex;
  std::atomic<uint16_t> m_topicCount{0};

  MC_FORCE_INLINE TopicObservers *findTopic(uint16_t topicId) const {
    if (MC_LIKELY(topicId < TOPIC_CHUNK_SIZE))
      return &m_topicBase[topicId];
    TopicObservers *chunk = m_topicChunks[topicId >> TOPIC_CHUNK_SHIFT].load(
        std::memory_order_acquire);
    return chunk ? &chunk[topicId & TOPIC_CHUNK_MASK] : nullptr;
  }

  TopicObservers &ensureTopic(uint16_t topicId) {
    if (topicId < TOPIC_CHUNK_SIZE)
      return m_topicBase[topicId];
    size_t c = topicId >> TOPIC_CHUNK_SHIFT;
    TopicObservers *chunk = m_topicChunks[c].load(std::memory_order_acquire);
    if (!chunk) {
      std::lock_guard<std::mutex> lock(m_topicGrowMutex);
      chunk = m_topicChunks[c].load(std::memory_order_relaxed);
      if (!chunk) {
        chunk = new TopicObservers[TOPIC_CHUNK_SIZE]();
        m_topicChunks[c].store(chunk, std::memory_order_release);
      }
    }
    return chunk[topicId & TOPIC_CHUNK_MASK];
  }

public:
  EventQueueBroadcastFast()
      : m_messagePool(std::min<size_t>(QueueCapacity, 256),
                      QueueCapacity + 64),
        m_topicBase(new TopicObservers[TOPIC_CHUNK_SIZE]()) {
    m_topicChunks[0].store(m_topicBase, std::memory_order_relaxed);
    for (size_t i = 1; i < TOPIC_NUM_CHUNKS; ++i)
      m_topicChunks[i].store(nullptr, std::memory_order_relaxed);
  }

  ~EventQueueBroadcastFast() {
    FastMessage *msg = nullptr;
    while (m_queue.try_pop(msg)) {
      if (msg) {
        if (!msg->isInline && msg->managed) {
          msg->managed->release();
        }
        m_messagePool.release(msg);
      }
    }
    for (size_t i = 1; i < TOPIC_NUM_CHUNKS; ++i)
      delete[] m_topicChunks[i].load(std::memory_order_relaxed);
    delete[] m_topicBase;
  }

  uint16_t registerTopic() {
    uint16_t id = m_topicCount.load(std::memory_order_relaxed);
    for (;;) {
      if (id >= FAST_MAX_TOPICS)
        return UINT16_MAX;
      if (m_topicCount.compare_exchange_weak(id, static_cast<uint16_t>(id + 1),
                                             std::memory_order_relaxed))
        break;
    }
    ensureTopic(id);
    return id;
  }

  bool addObserver(uint16_t topicId, FastCallback callback, void *userData = nullptr) {
    if (topicId >= FAST_MAX_TOPICS)
      return false;

    TopicObservers &topic = ensureTopic(topicId);
    std::lock_guard<std::mutex> lock(m_topicGrowMutex);
    uint8_t idx = topic.count.load(std::memory_order_relaxed);
    if (idx >= FAST_MAX_OBSERVERS)
      return false;

    topic.observers[idx].callback = callback;
    topic.observers[idx].userData = userData;
    topic.count.store(static_cast<uint8_t>(idx + 1), std::memory_order_release);
    return true;
  }

  // Hot path - minimal overhead post
  bool post(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= FAST_MAX_TOPICS)
      return false;

    TopicObservers *topic = findTopic(topicId);
    if (!topic)
      return true;
    uint8_t count = topic->count.load(std::memory_order_acquire);

    if (count == 0)
      return true;

    FastMessage *msg = m_messagePool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    // Copy observer snapshot into message (single-observer fast path avoids variable memcpy call)
    if (MC_LIKELY(count == 1)) {
      msg->observers[0] = topic->observers[0];
    } else {
      std::memcpy(msg->observers, topic->observers, count * sizeof(FastObserver));
    }

    if (MC_LIKELY(size <= FAST_INLINE_MAX)) {
      msg->isInline = true;
      if (MC_LIKELY(data != nullptr && size > 0))
        std::memcpy(msg->inlineData, data, size);
      else if (MC_UNLIKELY(size > 0))
        mc_detail::zeroMemoryCold(msg->inlineData, size);
    } else {
      msg->isInline = false;
      msg->managed = RefCountedPayload::create(data, size);
    }

    // Yield on contention - better than busy-spin for multi-consumer scenarios
    while (!m_queue.try_push(msg)) {
      std::this_thread::yield();
    }
    return true;
  }

  // Non-blocking variant: returns false immediately if the ring buffer is full
  bool tryPost(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= FAST_MAX_TOPICS)
      return false;

    TopicObservers *topic = findTopic(topicId);
    if (!topic)
      return true;
    uint8_t count = topic->count.load(std::memory_order_acquire);

    if (count == 0)
      return true;

    if (m_queue.full())
      return false;

    FastMessage *msg = m_messagePool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    if (MC_LIKELY(count == 1)) {
      msg->observers[0] = topic->observers[0];
    } else {
      std::memcpy(msg->observers, topic->observers, count * sizeof(FastObserver));
    }

    if (MC_LIKELY(size <= FAST_INLINE_MAX)) {
      msg->isInline = true;
      if (MC_LIKELY(data != nullptr && size > 0))
        std::memcpy(msg->inlineData, data, size);
      else if (MC_UNLIKELY(size > 0))
        mc_detail::zeroMemoryCold(msg->inlineData, size);
    } else {
      msg->isInline = false;
      msg->managed = RefCountedPayload::create(data, size);
    }

    if (!m_queue.try_push(msg)) {
      if (!msg->isInline && msg->managed)
        msg->managed->release();
      m_messagePool.release(msg);
      return false;
    }
    return true;
  }

  template <typename T>
  bool post(uint16_t topicId, const T &value) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "T must be trivially copyable");
    static_assert(!std::is_pointer<T>::value,
                  "Do not pass a pointer to typed post(); pass the value by reference or use post(topicId, ptr, size)");
    return post(topicId, &value, sizeof(T));
  }

  template <typename T>
  bool tryPost(uint16_t topicId, const T &value) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "T must be trivially copyable");
    static_assert(!std::is_pointer<T>::value,
                  "Do not pass a pointer to typed tryPost(); pass the value by reference or use tryPost(topicId, ptr, size)");
    return tryPost(topicId, &value, sizeof(T));
  }

  template <typename T>
  bool postValue(uint16_t topicId, const T &value) {
    return post(topicId, value);
  }

  bool post(uint16_t topicId) {
    return post(topicId, nullptr, 0);
  }

  bool tryPost(uint16_t topicId) {
    return tryPost(topicId, nullptr, 0);
  }

  // Hot path - minimal overhead dispatch
  bool dispatchOne() {
    FastMessage *msg = nullptr;
    if (!m_queue.try_pop(msg) || !msg)
      return false;

    const void *data = msg->isInline ? msg->inlineData
                                      : (msg->managed ? msg->managed->data() : nullptr);

    // Direct function pointer calls - no virtual dispatch
    for (uint8_t i = 0; i < msg->observerCount; ++i) {
      msg->observers[i].callback(msg->topicId, data, msg->payloadSize,
                                  msg->observers[i].userData);
    }

    if (!msg->isInline && msg->managed)
      msg->managed->release();

    m_messagePool.release(msg);
    return true;
  }

  // Batch dispatch - process multiple messages, better cache utilization
  size_t dispatchBatch(size_t maxCount) {
    static constexpr size_t DRAIN_CHUNK = 32;
    FastMessage *batch[DRAIN_CHUNK];
    size_t dispatched = 0;

    while (dispatched < maxCount) {
      size_t limit = std::min(maxCount - dispatched, DRAIN_CHUNK);
      size_t batchSize = 0;
      while (batchSize < limit) {
        FastMessage *msg = nullptr;
        if (!m_queue.try_pop(msg) || !msg)
          break;
        batch[batchSize++] = msg;
      }
      if (batchSize == 0)
        break;

      for (size_t i = 0; i < batchSize; ++i) {
        FastMessage *msg = batch[i];
        const void *data =
            msg->isInline ? msg->inlineData
                          : (msg->managed ? msg->managed->data() : nullptr);
        for (uint8_t j = 0; j < msg->observerCount; ++j) {
          msg->observers[j].callback(msg->topicId, data, msg->payloadSize,
                                      msg->observers[j].userData);
        }
        if (!msg->isInline && msg->managed)
          msg->managed->release();
      }
      if (batchSize == 1)
        m_messagePool.release(batch[0]);
      else
        m_messagePool.releaseBatch(batch, batchSize);

      dispatched += batchSize;
      if (batchSize < limit)
        break;
    }
    return dispatched;
  }

  size_t dispatchAll() {
    size_t count = 0;
    for (;;) {
      size_t n = dispatchBatch(32);
      if (n == 0)
        break;
      count += n;
    }
    return count;
  }

  bool empty() const { return m_queue.empty(); }
  size_t queueSize() const { return m_queue.size_approx(); }
};

#endif // MESSAGE_CENTER_EVENTQUEUE_FAST_H
