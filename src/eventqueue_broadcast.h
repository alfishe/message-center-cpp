#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_BROADCAST_H
#define MESSAGE_CENTER_EVENTQUEUE_BROADCAST_H

#include "mpmc_queue.h"
#include "objectpool_lockfree.h"
#include "payload_refcounted.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

// Fire-and-forget broadcast EventQueue with automatic memory management.
//
// Usage:
//   queue.post(topicId, data, size);  // Producer posts and forgets
//   queue.dispatchOne();              // Consumer dispatches, memory auto-freed
//
// Internal modes (auto-selected):
//   - Inline (≤48 bytes): copied into message, freed with message pool
//   - Managed (>48 bytes): ref-counted, freed after last observer completes

constexpr size_t BROADCAST_MAX_TOPICS = 4096;
constexpr size_t INLINE_PAYLOAD_MAX = 48;

struct BroadcastMessage {
  uint16_t topicId;
  uint8_t observerCount;
  bool isInline;
  size_t payloadSize;
  union {
    RefCountedPayload *managed;
    alignas(8) char inlineData[INLINE_PAYLOAD_MAX];
  };
};

using BroadcastCallback = std::function<void(uint16_t, const void *, size_t)>;

template <size_t QueueCapacity = 65536>
class EventQueueBroadcast {
  static_assert((QueueCapacity & (QueueCapacity - 1)) == 0,
                "QueueCapacity must be a power of 2");

  MPMCQueue<BroadcastMessage *, QueueCapacity> m_queue;
  ObjectPoolLockFree<BroadcastMessage> m_messagePool;

  std::string m_topicNames[BROADCAST_MAX_TOPICS];
  std::map<std::string, uint16_t> m_topicMap;
  std::atomic<uint16_t> m_topicCount{0};
  mutable std::mutex m_topicMutex;

  using ObserverList = std::vector<BroadcastCallback>;
  using ObserverListPtr = std::shared_ptr<ObserverList>;
  std::vector<ObserverListPtr> m_observers;
  mutable std::mutex m_observerMutex;

public:
  EventQueueBroadcast() : m_messagePool(256, 8192) {
    m_observers.resize(BROADCAST_MAX_TOPICS);
  }

  uint16_t registerTopic(const std::string &name) {
    std::lock_guard<std::mutex> lock(m_topicMutex);
    auto it = m_topicMap.find(name);
    if (it != m_topicMap.end())
      return it->second;

    uint16_t id = m_topicCount.fetch_add(1, std::memory_order_relaxed);
    if (id >= BROADCAST_MAX_TOPICS) {
      m_topicCount.fetch_sub(1, std::memory_order_relaxed);
      return UINT16_MAX;
    }
    m_topicNames[id] = name;
    m_topicMap[name] = id;
    return id;
  }

  uint16_t resolveTopic(const std::string &name) const {
    std::lock_guard<std::mutex> lock(m_topicMutex);
    auto it = m_topicMap.find(name);
    return (it != m_topicMap.end()) ? it->second : UINT16_MAX;
  }

  void addObserver(uint16_t topicId, BroadcastCallback callback) {
    if (topicId >= BROADCAST_MAX_TOPICS)
      return;
    std::lock_guard<std::mutex> lock(m_observerMutex);
    ObserverListPtr current = std::atomic_load(&m_observers[topicId]);
    ObserverListPtr next = current ? std::make_shared<ObserverList>(*current)
                                   : std::make_shared<ObserverList>();
    next->push_back(std::move(callback));
    std::atomic_store(&m_observers[topicId], next);
  }

  void addObserver(const std::string &topic, BroadcastCallback callback) {
    uint16_t id = registerTopic(topic);
    if (id != UINT16_MAX)
      addObserver(id, std::move(callback));
  }

  // === FIRE AND FORGET API ===
  // Post data, queue handles memory. Producer can immediately reuse/free buffer.
  bool post(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= BROADCAST_MAX_TOPICS)
      return false;

    ObserverListPtr observers = std::atomic_load(&m_observers[topicId]);
    uint8_t count = observers ? static_cast<uint8_t>(std::min(observers->size(), size_t(255))) : 0;

    if (count == 0)
      return true; // No observers, nothing to do

    BroadcastMessage *msg = m_messagePool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    if (size <= INLINE_PAYLOAD_MAX) {
      // Small payload: copy inline
      msg->isInline = true;
      if (data && size > 0)
        std::memcpy(msg->inlineData, data, size);
    } else {
      // Large payload: allocate managed
      msg->isInline = false;
      msg->managed = RefCountedPayload::create(data, size);
      // Add refs for all observers (payload starts with refcount=1)
      if (count > 1)
        msg->managed->addRefs(count - 1);
    }

    while (!m_queue.try_push(msg))
      std::this_thread::yield();
    return true;
  }

  bool post(const std::string &topic, const void *data, size_t size) {
    uint16_t id = resolveTopic(topic);
    return (id != UINT16_MAX) ? post(id, data, size) : false;
  }

  // Convenience for POD types
  template <typename T>
  bool postValue(uint16_t topicId, const T &value) {
    static_assert(std::is_trivially_copyable<T>::value, "T must be trivially copyable");
    return post(topicId, &value, sizeof(T));
  }

  // Post without payload (signal/event only)
  bool post(uint16_t topicId) {
    return post(topicId, nullptr, 0);
  }

  // === POINTER + METADATA MODE ===
  // For emulator scenarios where caller owns the buffer (frame buffers, audio buffers).
  // Passes pointer directly - no copy, no allocation.
  // CALLER MUST ensure buffer is valid until all observers complete.
  // Use with synchronous dispatch (dispatchAll after post) or pooled buffers.
  //
  // Metadata struct can contain: pointer, size, stride, format, frame number, etc.
  template <typename MetaT>
  bool postRef(uint16_t topicId, MetaT meta) {
    static_assert(sizeof(MetaT) <= INLINE_PAYLOAD_MAX, "Metadata too large for inline");
    static_assert(std::is_trivially_copyable<MetaT>::value, "Metadata must be trivially copyable");
    return post(topicId, &meta, sizeof(MetaT));
  }

  // Dispatch one message. Returns false if queue empty.
  // Memory is automatically freed after all observers complete.
  bool dispatchOne() {
    BroadcastMessage *msg = nullptr;
    if (!m_queue.try_pop(msg) || !msg)
      return false;

    ObserverListPtr observers = std::atomic_load(&m_observers[msg->topicId]);

    const void *data = nullptr;
    if (msg->isInline) {
      data = msg->inlineData;
    } else if (msg->managed) {
      data = msg->managed->data();
    }

    if (observers) {
      for (const auto &cb : *observers) {
        cb(msg->topicId, data, msg->payloadSize);

        // For managed payloads, release one ref per observer
        if (!msg->isInline && msg->managed) {
          msg->managed->release();
        }
      }
    } else if (!msg->isInline && msg->managed) {
      // No observers but payload allocated - release all refs
      for (int i = 0; i < msg->observerCount; ++i) {
        msg->managed->release();
      }
    }

    m_messagePool.release(msg);
    return true;
  }

  size_t dispatchAll() {
    size_t count = 0;
    while (dispatchOne())
      ++count;
    return count;
  }

  bool empty() const { return m_queue.empty(); }
  size_t queueSize() const { return m_queue.size_approx(); }
  uint16_t topicCount() const { return m_topicCount.load(std::memory_order_relaxed); }

  size_t observerCount(uint16_t topicId) const {
    if (topicId >= BROADCAST_MAX_TOPICS)
      return 0;
    ObserverListPtr obs = std::atomic_load(&m_observers[topicId]);
    return obs ? obs->size() : 0;
  }
};

#endif // MESSAGE_CENTER_EVENTQUEUE_BROADCAST_H
