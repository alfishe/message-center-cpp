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
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
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
    alignas(alignof(std::max_align_t)) char inlineData[INLINE_PAYLOAD_MAX];
  };
};

using BroadcastCallback = std::function<void(uint16_t, const void *, size_t)>;

template <size_t QueueCapacity = 65536>
class EventQueueBroadcast {
  static_assert((QueueCapacity & (QueueCapacity - 1)) == 0,
                "QueueCapacity must be a power of 2");

  MPMCQueue<BroadcastMessage *, QueueCapacity> m_queue;
  ObjectPoolLockFree<BroadcastMessage> m_messagePool;

  using ObserverList = std::vector<BroadcastCallback>;

  struct TopicSlot {
    std::atomic<uint8_t> count{0};
    std::atomic<const ObserverList *> list{nullptr};
  };

  static constexpr size_t TOPIC_CHUNK_SHIFT = 6; // 64 topics per chunk
  static constexpr size_t TOPIC_CHUNK_SIZE = 1u << TOPIC_CHUNK_SHIFT;
  static constexpr size_t TOPIC_CHUNK_MASK = TOPIC_CHUNK_SIZE - 1;
  static constexpr size_t TOPIC_NUM_CHUNKS =
      BROADCAST_MAX_TOPICS / TOPIC_CHUNK_SIZE;

  TopicSlot *m_topicBase{nullptr};
  std::unique_ptr<std::atomic<TopicSlot *>[]> m_topicChunks{
      new std::atomic<TopicSlot *>[TOPIC_NUM_CHUNKS]};
  std::unordered_map<std::string, uint16_t> m_topicMap;
  std::atomic<uint16_t> m_topicCount{0};
  mutable std::mutex m_topicMutex;

  std::vector<const ObserverList *> m_allLists; // owned snapshots, freed in dtor
  mutable std::mutex m_observerMutex;

  inline TopicSlot *findTopic(uint16_t topicId) const {
    if (topicId < TOPIC_CHUNK_SIZE)
      return &m_topicBase[topicId];
    TopicSlot *chunk = m_topicChunks[topicId >> TOPIC_CHUNK_SHIFT].load(
        std::memory_order_acquire);
    return chunk ? &chunk[topicId & TOPIC_CHUNK_MASK] : nullptr;
  }

  TopicSlot &ensureTopic(uint16_t topicId) {
    if (topicId < TOPIC_CHUNK_SIZE)
      return m_topicBase[topicId];
    size_t c = topicId >> TOPIC_CHUNK_SHIFT;
    TopicSlot *chunk = m_topicChunks[c].load(std::memory_order_acquire);
    if (!chunk) {
      std::lock_guard<std::mutex> lock(m_observerMutex);
      chunk = m_topicChunks[c].load(std::memory_order_relaxed);
      if (!chunk) {
        chunk = new TopicSlot[TOPIC_CHUNK_SIZE]();
        m_topicChunks[c].store(chunk, std::memory_order_release);
      }
    }
    return chunk[topicId & TOPIC_CHUNK_MASK];
  }

public:
  EventQueueBroadcast()
      : m_messagePool(std::min<size_t>(QueueCapacity, 256),
                      QueueCapacity + 64),
        m_topicBase(new TopicSlot[TOPIC_CHUNK_SIZE]()) {
    m_topicChunks[0].store(m_topicBase, std::memory_order_relaxed);
    for (size_t i = 1; i < TOPIC_NUM_CHUNKS; ++i)
      m_topicChunks[i].store(nullptr, std::memory_order_relaxed);
  }

  ~EventQueueBroadcast() {
    BroadcastMessage *msg = nullptr;
    while (m_queue.try_pop(msg)) {
      if (msg) {
        if (!msg->isInline && msg->managed) {
          msg->managed->release();
        }
        m_messagePool.release(msg);
      }
    }
    for (const ObserverList *list : m_allLists)
      delete list;
    for (size_t i = 1; i < TOPIC_NUM_CHUNKS; ++i)
      delete[] m_topicChunks[i].load(std::memory_order_relaxed);
    delete[] m_topicBase;
  }

  uint16_t registerTopic(const std::string &name) {
    std::lock_guard<std::mutex> lock(m_topicMutex);
    auto it = m_topicMap.find(name);
    if (it != m_topicMap.end())
      return it->second;

    uint16_t id = m_topicCount.load(std::memory_order_relaxed);
    if (id >= BROADCAST_MAX_TOPICS)
      return UINT16_MAX;
    m_topicCount.store(static_cast<uint16_t>(id + 1), std::memory_order_relaxed);
    ensureTopic(id);
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
    TopicSlot &slot = ensureTopic(topicId);
    std::lock_guard<std::mutex> lock(m_observerMutex);
    const ObserverList *current = slot.list.load(std::memory_order_relaxed);
    ObserverList *next =
        current ? new ObserverList(*current) : new ObserverList();
    next->push_back(std::move(callback));
    m_allLists.push_back(next);
    slot.list.store(next, std::memory_order_release);
    uint8_t newCount =
        static_cast<uint8_t>(std::min(next->size(), size_t(255)));
    slot.count.store(newCount, std::memory_order_release);
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

    TopicSlot *slot = findTopic(topicId);
    uint8_t count = slot ? slot->count.load(std::memory_order_acquire) : 0;

    if (count == 0)
      return true; // No observers, nothing to do

    BroadcastMessage *msg = m_messagePool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    if (size <= INLINE_PAYLOAD_MAX) {
      // Small payload: copy inline (or zero-init if data is nullptr)
      msg->isInline = true;
      if (MC_LIKELY(data != nullptr && size > 0))
        std::memcpy(msg->inlineData, data, size);
      else if (MC_UNLIKELY(size > 0))
        mc_detail::zeroMemoryCold(msg->inlineData, size);
    } else {
      // Large payload: allocate managed (owned by message, refcount=1)
      msg->isInline = false;
      msg->managed = RefCountedPayload::create(data, size);
    }

    while (!m_queue.try_push(msg))
      std::this_thread::yield();
    return true;
  }

  // Non-blocking variant: returns false immediately if the ring buffer is full
  bool tryPost(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= BROADCAST_MAX_TOPICS)
      return false;

    TopicSlot *slot = findTopic(topicId);
    uint8_t count = slot ? slot->count.load(std::memory_order_acquire) : 0;

    if (count == 0)
      return true;

    if (m_queue.full())
      return false;

    BroadcastMessage *msg = m_messagePool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    if (size <= INLINE_PAYLOAD_MAX) {
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

  bool post(const std::string &topic, const void *data, size_t size) {
    uint16_t id = resolveTopic(topic);
    return (id != UINT16_MAX) ? post(id, data, size) : false;
  }

  bool tryPost(const std::string &topic, const void *data, size_t size) {
    uint16_t id = resolveTopic(topic);
    return (id != UINT16_MAX) ? tryPost(id, data, size) : false;
  }

  // Convenience for POD / trivially copyable types
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
  bool post(const std::string &topic, const T &value) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "T must be trivially copyable");
    static_assert(!std::is_pointer<T>::value,
                  "Do not pass a pointer to typed post(); pass the value by reference or use post(topic, ptr, size)");
    return post(topic, &value, sizeof(T));
  }

  template <typename T>
  bool tryPost(const std::string &topic, const T &value) {
    static_assert(std::is_trivially_copyable<T>::value,
                  "T must be trivially copyable");
    static_assert(!std::is_pointer<T>::value,
                  "Do not pass a pointer to typed tryPost(); pass the value by reference or use tryPost(topic, ptr, size)");
    return tryPost(topic, &value, sizeof(T));
  }

  template <typename T>
  bool postValue(uint16_t topicId, const T &value) {
    return post(topicId, value);
  }

  // Post without payload (signal/event only)
  bool post(uint16_t topicId) {
    return post(topicId, nullptr, 0);
  }

  bool tryPost(uint16_t topicId) {
    return tryPost(topicId, nullptr, 0);
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

    TopicSlot *slot = findTopic(msg->topicId);
    const ObserverList *observers =
        slot ? slot->list.load(std::memory_order_acquire) : nullptr;

    const void *data = nullptr;
    if (msg->isInline) {
      data = msg->inlineData;
    } else if (msg->managed) {
      data = msg->managed->data();
    }

    if (observers) {
      for (const auto &cb : *observers) {
        cb(msg->topicId, data, msg->payloadSize);
      }
    }

    // For managed payloads, release the message's reference once
    if (!msg->isInline && msg->managed) {
      msg->managed->release();
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
    TopicSlot *slot = findTopic(topicId);
    if (!slot)
      return 0;
    const ObserverList *obs = slot->list.load(std::memory_order_acquire);
    return obs ? obs->size() : 0;
  }
};

#endif // MESSAGE_CENTER_EVENTQUEUE_BROADCAST_H
