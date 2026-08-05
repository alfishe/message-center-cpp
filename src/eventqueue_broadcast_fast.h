#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_BROADCAST_FAST_H
#define MESSAGE_CENTER_EVENTQUEUE_BROADCAST_FAST_H

#include "mpmc_queue.h"
#include "objectpool_lockfree.h"
#include "payload_refcounted.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

// Maximum performance broadcast queue.
// Optimizations over EventQueueBroadcast:
// 1. Function pointer instead of std::function (no virtual call, no heap)
// 2. Raw pointer to observer array instead of shared_ptr (no atomic refcount)
// 3. Observer snapshot cached in message (no lookup on dispatch)
// 4. Busy-spin with pause instead of yield (no syscall)
// 5. Batch dispatch support

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
    alignas(8) char inlineData[FAST_INLINE_MAX];
  };
};

template <size_t QueueCapacity = 65536>
class EventQueueBroadcastFast {
  static_assert((QueueCapacity & (QueueCapacity - 1)) == 0,
                "QueueCapacity must be a power of 2");

  MPMCQueue<FastMessage *, QueueCapacity> m_queue;
  ObjectPoolLockFree<FastMessage> m_messagePool;

  // Simple array of observer lists - no COW, just raw pointers
  // Registration is rare, dispatch is hot path
  struct TopicObservers {
    std::atomic<uint8_t> count{0};
    FastObserver observers[FAST_MAX_OBSERVERS];
  };

  alignas(64) TopicObservers m_topics[FAST_MAX_TOPICS];
  std::atomic<uint16_t> m_topicCount{0};

public:
  EventQueueBroadcastFast() : m_messagePool(512, 16384) {}

  uint16_t registerTopic() {
    uint16_t id = m_topicCount.fetch_add(1, std::memory_order_relaxed);
    return (id < FAST_MAX_TOPICS) ? id : UINT16_MAX;
  }

  bool addObserver(uint16_t topicId, FastCallback callback, void *userData = nullptr) {
    if (topicId >= FAST_MAX_TOPICS)
      return false;

    TopicObservers &topic = m_topics[topicId];
    uint8_t idx = topic.count.load(std::memory_order_acquire);
    if (idx >= FAST_MAX_OBSERVERS)
      return false;

    topic.observers[idx].callback = callback;
    topic.observers[idx].userData = userData;
    topic.count.store(idx + 1, std::memory_order_release);
    return true;
  }

  // Hot path - minimal overhead post
  bool post(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= FAST_MAX_TOPICS)
      return false;

    TopicObservers &topic = m_topics[topicId];
    uint8_t count = topic.count.load(std::memory_order_acquire);

    if (count == 0)
      return true;

    FastMessage *msg = m_messagePool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    // Copy observer snapshot into message
    std::memcpy(msg->observers, topic.observers, count * sizeof(FastObserver));

    if (size <= FAST_INLINE_MAX) {
      msg->isInline = true;
      if (data && size > 0)
        std::memcpy(msg->inlineData, data, size);
    } else {
      msg->isInline = false;
      msg->managed = RefCountedPayload::create(data, size);
      if (count > 1)
        msg->managed->addRefs(count - 1);
    }

    // Yield on contention - better than busy-spin for multi-consumer scenarios
    while (!m_queue.try_push(msg)) {
      std::this_thread::yield();
    }
    return true;
  }

  template <typename T>
  bool postValue(uint16_t topicId, const T &value) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be POD");
    return post(topicId, &value, sizeof(T));
  }

  bool post(uint16_t topicId) {
    return post(topicId, nullptr, 0);
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

      if (!msg->isInline && msg->managed)
        msg->managed->release();
    }

    m_messagePool.release(msg);
    return true;
  }

  // Batch dispatch - process multiple messages, better cache utilization
  size_t dispatchBatch(size_t maxCount) {
    size_t dispatched = 0;
    while (dispatched < maxCount && dispatchOne())
      ++dispatched;
    return dispatched;
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

#endif // MESSAGE_CENTER_EVENTQUEUE_BROADCAST_FAST_H
