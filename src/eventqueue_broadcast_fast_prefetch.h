#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_BROADCAST_FAST_PREFETCH_H
#define MESSAGE_CENTER_EVENTQUEUE_BROADCAST_FAST_PREFETCH_H

#include "mpmc_queue.h"
#include "objectpool_lockfree.h"
#include "payload_refcounted.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

// Prefetch-optimized variant of EventQueueBroadcastFast.
// Uses software prefetch hints to hide memory latency during dispatch.

constexpr size_t PF_MAX_TOPICS = 4096;
constexpr size_t PF_INLINE_MAX = 48;
constexpr size_t PF_MAX_OBSERVERS = 16;

using PFCallback = void (*)(uint16_t topicId, const void *data, size_t size, void *userData);

struct PFObserver {
  PFCallback callback;
  void *userData;
};

struct PFMessage {
  uint16_t topicId;
  uint8_t observerCount;
  bool isInline;
  size_t payloadSize;
  PFObserver observers[PF_MAX_OBSERVERS];
  union {
    RefCountedPayload *managed;
    alignas(8) char inlineData[PF_INLINE_MAX];
  };
};

// Prefetch macros - use compiler builtins when available
#if defined(__GNUC__) || defined(__clang__)
#define PF_PREFETCH_READ(addr) __builtin_prefetch((addr), 0, 3)
#define PF_PREFETCH_WRITE(addr) __builtin_prefetch((addr), 1, 3)
#else
#define PF_PREFETCH_READ(addr) ((void)0)
#define PF_PREFETCH_WRITE(addr) ((void)0)
#endif

template <size_t QueueCapacity = 65536>
class EventQueueBroadcastFastPrefetch {
  static_assert((QueueCapacity & (QueueCapacity - 1)) == 0,
                "QueueCapacity must be a power of 2");

  MPMCQueue<PFMessage *, QueueCapacity> m_queue;
  ObjectPoolLockFree<PFMessage> m_messagePool;

  struct TopicObservers {
    std::atomic<uint8_t> count{0};
    PFObserver observers[PF_MAX_OBSERVERS];
  };

  alignas(64) TopicObservers m_topics[PF_MAX_TOPICS];
  std::atomic<uint16_t> m_topicCount{0};

public:
  EventQueueBroadcastFastPrefetch() : m_messagePool(512, 16384) {}

  uint16_t registerTopic() {
    uint16_t id = m_topicCount.fetch_add(1, std::memory_order_relaxed);
    return (id < PF_MAX_TOPICS) ? id : UINT16_MAX;
  }

  bool addObserver(uint16_t topicId, PFCallback callback, void *userData = nullptr) {
    if (topicId >= PF_MAX_TOPICS)
      return false;

    TopicObservers &topic = m_topics[topicId];
    uint8_t idx = topic.count.load(std::memory_order_acquire);
    if (idx >= PF_MAX_OBSERVERS)
      return false;

    topic.observers[idx].callback = callback;
    topic.observers[idx].userData = userData;
    topic.count.store(idx + 1, std::memory_order_release);
    return true;
  }

  bool post(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= PF_MAX_TOPICS)
      return false;

    TopicObservers &topic = m_topics[topicId];
    uint8_t count = topic.count.load(std::memory_order_acquire);

    if (count == 0)
      return true;

    PFMessage *msg = m_messagePool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    std::memcpy(msg->observers, topic.observers, count * sizeof(PFObserver));

    if (size <= PF_INLINE_MAX) {
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
    PFMessage *msg = nullptr;
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

  // Prefetching batch dispatch - prefetch next message while processing current
  size_t dispatchBatchPrefetch(size_t maxCount) {
    constexpr size_t BATCH_SIZE = 8;
    PFMessage *batch[BATCH_SIZE];
    size_t batchSize = 0;

    // Fill batch
    while (batchSize < std::min(maxCount, BATCH_SIZE)) {
      PFMessage *msg = nullptr;
      if (!m_queue.try_pop(msg) || !msg)
        break;
      batch[batchSize++] = msg;
    }

    if (batchSize == 0)
      return 0;

    // Prefetch first message's data
    if (batch[0]->isInline) {
      PF_PREFETCH_READ(batch[0]->inlineData);
    } else if (batch[0]->managed) {
      PF_PREFETCH_READ(batch[0]->managed->data());
    }

    for (size_t i = 0; i < batchSize; ++i) {
      PFMessage *msg = batch[i];

      // Prefetch next message while processing current
      if (i + 1 < batchSize) {
        PF_PREFETCH_READ(batch[i + 1]);
        if (batch[i + 1]->isInline) {
          PF_PREFETCH_READ(batch[i + 1]->inlineData);
        } else if (batch[i + 1]->managed) {
          PF_PREFETCH_READ(batch[i + 1]->managed);
        }
      }

      const void *data = msg->isInline ? msg->inlineData
                                        : (msg->managed ? msg->managed->data() : nullptr);

      for (uint8_t j = 0; j < msg->observerCount; ++j) {
        msg->observers[j].callback(msg->topicId, data, msg->payloadSize,
                                    msg->observers[j].userData);

        if (!msg->isInline && msg->managed)
          msg->managed->release();
      }

      m_messagePool.release(msg);
    }

    return batchSize;
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

#endif // MESSAGE_CENTER_EVENTQUEUE_BROADCAST_FAST_PREFETCH_H
