#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_BROADCAST_FAST_TL_H
#define MESSAGE_CENTER_EVENTQUEUE_BROADCAST_FAST_TL_H

#include "mpmc_queue.h"
#include "objectpool_threadlocal.h"
#include "payload_refcounted.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

// Thread-local pool variant of EventQueueBroadcastFast.
// Eliminates pool contention on the hot path by giving each thread
// its own message cache. Best for balanced producer/consumer workloads.

constexpr size_t TL_MAX_TOPICS = 4096;
constexpr size_t TL_INLINE_MAX = 48;
constexpr size_t TL_MAX_OBSERVERS = 16;

using TLCallback = void (*)(uint16_t topicId, const void *data, size_t size, void *userData);

struct TLObserver {
  TLCallback callback;
  void *userData;
};

struct TLMessage {
  uint16_t topicId;
  uint8_t observerCount;
  bool isInline;
  size_t payloadSize;
  TLObserver observers[TL_MAX_OBSERVERS];
  union {
    RefCountedPayload *managed;
    alignas(8) char inlineData[TL_INLINE_MAX];
  };
};

template <size_t QueueCapacity = 65536>
class EventQueueBroadcastFastTL {
  static_assert((QueueCapacity & (QueueCapacity - 1)) == 0,
                "QueueCapacity must be a power of 2");

  MPMCQueue<TLMessage *, QueueCapacity> m_queue;
  ObjectPoolThreadLocal<TLMessage> m_messagePool;

  struct TopicObservers {
    std::atomic<uint8_t> count{0};
    TLObserver observers[TL_MAX_OBSERVERS];
  };

  alignas(64) TopicObservers m_topics[TL_MAX_TOPICS];
  std::atomic<uint16_t> m_topicCount{0};

public:
  EventQueueBroadcastFastTL() = default;

  uint16_t registerTopic() {
    uint16_t id = m_topicCount.fetch_add(1, std::memory_order_relaxed);
    return (id < TL_MAX_TOPICS) ? id : UINT16_MAX;
  }

  bool addObserver(uint16_t topicId, TLCallback callback, void *userData = nullptr) {
    if (topicId >= TL_MAX_TOPICS)
      return false;

    TopicObservers &topic = m_topics[topicId];
    uint8_t idx = topic.count.load(std::memory_order_acquire);
    if (idx >= TL_MAX_OBSERVERS)
      return false;

    topic.observers[idx].callback = callback;
    topic.observers[idx].userData = userData;
    topic.count.store(idx + 1, std::memory_order_release);
    return true;
  }

  bool post(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= TL_MAX_TOPICS)
      return false;

    TopicObservers &topic = m_topics[topicId];
    uint8_t count = topic.count.load(std::memory_order_acquire);

    if (count == 0)
      return true;

    TLMessage *msg = m_messagePool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    std::memcpy(msg->observers, topic.observers, count * sizeof(TLObserver));

    if (size <= TL_INLINE_MAX) {
      msg->isInline = true;
      if (data && size > 0)
        std::memcpy(msg->inlineData, data, size);
    } else {
      msg->isInline = false;
      msg->managed = RefCountedPayload::create(data, size);
      if (count > 1)
        msg->managed->addRefs(count - 1);
    }

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

  bool dispatchOne() {
    TLMessage *msg = nullptr;
    if (!m_queue.try_pop(msg) || !msg)
      return false;

    const void *data = msg->isInline ? msg->inlineData
                                      : (msg->managed ? msg->managed->data() : nullptr);

    for (uint8_t i = 0; i < msg->observerCount; ++i) {
      msg->observers[i].callback(msg->topicId, data, msg->payloadSize,
                                  msg->observers[i].userData);

      if (!msg->isInline && msg->managed)
        msg->managed->release();
    }

    m_messagePool.release(msg);
    return true;
  }

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

#endif // MESSAGE_CENTER_EVENTQUEUE_BROADCAST_FAST_TL_H
