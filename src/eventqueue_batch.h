#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_BATCH_H
#define MESSAGE_CENTER_EVENTQUEUE_BATCH_H

// EventQueueBroadcastFastBatch - Batch dispatch variant for low tail latency
//
// Performance: P999=76μs (47% better than single dispatch)
//
// Same API as EventQueueBroadcastFast, but with batch dispatch that groups
// queue pops and pool returns to reduce atomic operation overhead.
//
// When to use:
// - P999/max latency matters more than average throughput
// - High message volume with multiple consumers
// - Bursty workloads
//
// Usage:
//   EventQueueBroadcastFastBatch<65536> queue;
//   // ... setup same as EventQueueBroadcastFast ...
//   while (running) {
//     queue.dispatchBatch(32);  // Process up to 32 messages at once
//   }

#include "mpmc_queue.h"
#include "objectpool_lockfree.h"
#include "payload_refcounted.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>
#include <vector>

constexpr size_t BATCH_MAX_TOPICS = 4096;
constexpr size_t BATCH_INLINE_MAX = 48;
constexpr size_t BATCH_MAX_OBSERVERS = 16;
constexpr size_t BATCH_SIZE = 32;

using BatchCallback = void (*)(uint16_t topicId, const void *data, size_t size, void *userData);

struct BatchObserver {
  BatchCallback callback;
  void *userData;
};

struct BatchMessage {
  uint16_t topicId;
  uint8_t observerCount;
  bool isInline;
  size_t payloadSize;
  BatchObserver observers[BATCH_MAX_OBSERVERS];
  union {
    RefCountedPayload *managed;
    alignas(alignof(std::max_align_t)) char inlineData[BATCH_INLINE_MAX];
  };
};

template <size_t QueueCapacity = 65536>
class EventQueueBroadcastFastBatch {
  static_assert((QueueCapacity & (QueueCapacity - 1)) == 0,
                "QueueCapacity must be a power of 2");

  MPMCQueue<BatchMessage *, QueueCapacity> m_queue;
  ObjectPoolLockFree<BatchMessage> m_messagePool;

  struct TopicObservers {
    std::atomic<uint8_t> count{0};
    BatchObserver observers[BATCH_MAX_OBSERVERS];
  };

  static constexpr size_t TOPIC_CHUNK_SHIFT = 6; // 64 topics per chunk
  static constexpr size_t TOPIC_CHUNK_SIZE = 1u << TOPIC_CHUNK_SHIFT;
  static constexpr size_t TOPIC_CHUNK_MASK = TOPIC_CHUNK_SIZE - 1;
  static constexpr size_t TOPIC_NUM_CHUNKS =
      BATCH_MAX_TOPICS / TOPIC_CHUNK_SIZE;

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

  // Thread-local batch buffer for accumulating posts, bound to its owning queue
  // instance so multi-instance usage never mixes pools or leaves dangling pointers.
  struct BatchBuffer {
    EventQueueBroadcastFastBatch *owner = nullptr;
    BatchMessage *msgs[BATCH_SIZE];
    size_t count = 0;

    ~BatchBuffer() {
      if (owner) {
        owner->detachThreadBuffer(this);
      }
    }
  };

  static thread_local BatchBuffer t_batch;
  std::vector<BatchBuffer *> m_threadBuffers; // guarded by m_topicGrowMutex

  void pushBufferMessages(BatchBuffer &buf) {
    for (size_t i = 0; i < buf.count; ++i) {
      while (!m_queue.try_push(buf.msgs[i])) {
        std::this_thread::yield();
      }
    }
    buf.count = 0;
  }

  void detachThreadBuffer(BatchBuffer *buf) {
    pushBufferMessages(*buf);
    std::lock_guard<std::mutex> lock(m_topicGrowMutex);
    for (auto it = m_threadBuffers.begin(); it != m_threadBuffers.end(); ++it) {
      if (*it == buf) {
        m_threadBuffers.erase(it);
        break;
      }
    }
    buf->owner = nullptr;
  }

  void bindThreadBuffer(BatchBuffer &buf) {
    if (buf.owner && buf.owner != this) {
      buf.owner->detachThreadBuffer(&buf);
    }
    std::lock_guard<std::mutex> lock(m_topicGrowMutex);
    buf.owner = this;
    m_threadBuffers.push_back(&buf);
  }

public:
  EventQueueBroadcastFastBatch()
      : m_messagePool(std::min<size_t>(QueueCapacity, 256),
                      QueueCapacity + BATCH_SIZE * 4),
        m_topicBase(new TopicObservers[TOPIC_CHUNK_SIZE]()) {
    m_topicChunks[0].store(m_topicBase, std::memory_order_relaxed);
    for (size_t i = 1; i < TOPIC_NUM_CHUNKS; ++i)
      m_topicChunks[i].store(nullptr, std::memory_order_relaxed);
  }

  ~EventQueueBroadcastFastBatch() {
    if (t_batch.owner == this) {
      pushBufferMessages(t_batch);
    }
    {
      std::lock_guard<std::mutex> lock(m_topicGrowMutex);
      for (BatchBuffer *buf : m_threadBuffers) {
        for (size_t i = 0; i < buf->count; ++i) {
          BatchMessage *m = buf->msgs[i];
          if (m) {
            if (!m->isInline && m->managed)
              m->managed->release();
            m_messagePool.release(m);
          }
        }
        buf->count = 0;
        buf->owner = nullptr;
      }
      m_threadBuffers.clear();
    }
    BatchMessage *msg = nullptr;
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
      if (id >= BATCH_MAX_TOPICS)
        return UINT16_MAX;
      if (m_topicCount.compare_exchange_weak(id, static_cast<uint16_t>(id + 1),
                                             std::memory_order_relaxed))
        break;
    }
    ensureTopic(id);
    return id;
  }

  bool addObserver(uint16_t topicId, BatchCallback callback, void *userData = nullptr) {
    if (topicId >= BATCH_MAX_TOPICS)
      return false;

    TopicObservers &topic = ensureTopic(topicId);
    std::lock_guard<std::mutex> lock(m_topicGrowMutex);
    uint8_t idx = topic.count.load(std::memory_order_relaxed);
    if (idx >= BATCH_MAX_OBSERVERS)
      return false;

    topic.observers[idx].callback = callback;
    topic.observers[idx].userData = userData;
    topic.count.store(static_cast<uint8_t>(idx + 1), std::memory_order_release);
    return true;
  }

  // Regular post - immediate queue push
  bool post(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= BATCH_MAX_TOPICS)
      return false;

    TopicObservers *topic = findTopic(topicId);
    if (!topic)
      return true;
    uint8_t count = topic->count.load(std::memory_order_acquire);

    if (count == 0)
      return true;

    BatchMessage *msg = m_messagePool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    if (MC_LIKELY(count == 1)) {
      msg->observers[0] = topic->observers[0];
    } else {
      std::memcpy(msg->observers, topic->observers, count * sizeof(BatchObserver));
    }

    if (MC_LIKELY(size <= BATCH_INLINE_MAX)) {
      msg->isInline = true;
      if (data && size > 0)
        std::memcpy(msg->inlineData, data, size);
    } else {
      msg->isInline = false;
      msg->managed = RefCountedPayload::create(data, size);
    }

    while (!m_queue.try_push(msg)) {
      std::this_thread::yield();
    }
    return true;
  }

  // Non-blocking variant: returns false immediately if the ring buffer is full
  bool tryPost(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= BATCH_MAX_TOPICS)
      return false;

    TopicObservers *topic = findTopic(topicId);
    if (!topic)
      return true;
    uint8_t count = topic->count.load(std::memory_order_acquire);

    if (count == 0)
      return true;

    if (m_queue.full())
      return false;

    BatchMessage *msg = m_messagePool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    if (MC_LIKELY(count == 1)) {
      msg->observers[0] = topic->observers[0];
    } else {
      std::memcpy(msg->observers, topic->observers, count * sizeof(BatchObserver));
    }

    if (MC_LIKELY(size <= BATCH_INLINE_MAX)) {
      msg->isInline = true;
      if (data && size > 0)
        std::memcpy(msg->inlineData, data, size);
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

  // Batched post - accumulates messages, flushes when batch is full
  bool postBatched(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= BATCH_MAX_TOPICS)
      return false;

    TopicObservers *topic = findTopic(topicId);
    if (!topic)
      return true;
    uint8_t count = topic->count.load(std::memory_order_acquire);

    if (count == 0)
      return true;

    BatchMessage *msg = m_messagePool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    if (MC_LIKELY(count == 1)) {
      msg->observers[0] = topic->observers[0];
    } else {
      std::memcpy(msg->observers, topic->observers, count * sizeof(BatchObserver));
    }

    if (MC_LIKELY(size <= BATCH_INLINE_MAX)) {
      msg->isInline = true;
      if (data && size > 0)
        std::memcpy(msg->inlineData, data, size);
    } else {
      msg->isInline = false;
      msg->managed = RefCountedPayload::create(data, size);
    }

    if (t_batch.owner != this) {
      bindThreadBuffer(t_batch);
    }

    t_batch.msgs[t_batch.count++] = msg;

    if (t_batch.count >= BATCH_SIZE) {
      pushBufferMessages(t_batch);
    }
    return true;
  }

  // Flush batched messages to queue
  void flushBatch() {
    if (t_batch.owner == this && t_batch.count > 0) {
      pushBufferMessages(t_batch);
    }
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
  bool postBatched(uint16_t topicId, const T &value) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "T must be trivially copyable");
    static_assert(!std::is_pointer<T>::value,
                  "Do not pass a pointer to typed postBatched(); pass the value by reference or use postBatched(topicId, ptr, size)");
    return postBatched(topicId, &value, sizeof(T));
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

  bool postBatched(uint16_t topicId) {
    return postBatched(topicId, nullptr, 0);
  }

  bool dispatchOne() {
    BatchMessage *msg = nullptr;
    if (!m_queue.try_pop(msg) || !msg)
      return false;

    const void *data = msg->isInline ? msg->inlineData
                                      : (msg->managed ? msg->managed->data() : nullptr);

    for (uint8_t i = 0; i < msg->observerCount; ++i) {
      msg->observers[i].callback(msg->topicId, data, msg->payloadSize,
                                  msg->observers[i].userData);
    }

    if (!msg->isInline && msg->managed)
      msg->managed->release();

    m_messagePool.release(msg);
    return true;
  }

  // Batch dispatch - process multiple messages with better cache locality
  size_t dispatchBatch(size_t maxCount) {
    size_t totalDispatched = 0;

    while (totalDispatched < maxCount) {
      // Pre-fetch messages into local buffer
      BatchMessage *batch[BATCH_SIZE];
      size_t batchSize = 0;
      size_t toPop = std::min(maxCount - totalDispatched, BATCH_SIZE);

      while (batchSize < toPop) {
        BatchMessage *msg = nullptr;
        if (!m_queue.try_pop(msg) || !msg)
          break;
        batch[batchSize++] = msg;
      }

      if (batchSize == 0)
        break;

      // Process batch with better cache locality
      for (size_t i = 0; i < batchSize; ++i) {
        BatchMessage *msg = batch[i];
        const void *data = msg->isInline ? msg->inlineData
                                          : (msg->managed ? msg->managed->data() : nullptr);

        for (uint8_t j = 0; j < msg->observerCount; ++j) {
          msg->observers[j].callback(msg->topicId, data, msg->payloadSize,
                                      msg->observers[j].userData);
        }

        if (!msg->isInline && msg->managed)
          msg->managed->release();
      }

      // Return messages to pool in batch
      for (size_t i = 0; i < batchSize; ++i) {
        m_messagePool.release(batch[i]);
      }

      totalDispatched += batchSize;
    }

    return totalDispatched;
  }

  size_t dispatchAll() {
    size_t count = 0;
    while (dispatchOne())
      ++count;
    return count;
  }

  bool empty() const { return m_queue.empty(); }
  size_t queueSize() const { return m_queue.size_approx(); }
};

template <size_t QueueCapacity>
thread_local typename EventQueueBroadcastFastBatch<QueueCapacity>::BatchBuffer
    EventQueueBroadcastFastBatch<QueueCapacity>::t_batch;

#endif // MESSAGE_CENTER_EVENTQUEUE_BATCH_H
