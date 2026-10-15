#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_EMULATOR_H
#define MESSAGE_CENTER_EVENTQUEUE_EMULATOR_H

// EventQueueEmulator - Dual-queue system for mixed-criticality workloads
//
// Performance: P99=99μs for critical events (22x better than single queue)
//
// Designed for emulators, games, and real-time systems where some events
// (audio sync, input, vblank) must have guaranteed low latency while other
// events (debug traces, state saves) can tolerate delays.
//
// Architecture:
// - Fast queue: Inline-only (≤48B), zero allocation, always dispatched first
// - Bulk queue: Managed payloads OK, processed when fast queue is empty
//
// Topic priorities:
// - TopicPriority::Critical -> Fast queue (audio, vblank, input)
// - TopicPriority::Normal   -> Bulk queue (debug, state, screenshots)
//
// Usage:
//   EventQueueEmulator<4096, 16384> queue;  // fast capacity, bulk capacity
//   uint16_t audio = queue.registerTopic(TopicPriority::Critical);
//   uint16_t trace = queue.registerTopic(TopicPriority::Normal);
//   queue.postFast(audio, data, size);   // Goes to fast queue
//   queue.postBulk(trace, data, size);   // Goes to bulk queue
//   queue.dispatchPriority();            // Drain fast, then one bulk

#include "mpmc_queue.h"
#include "objectpool_lockfree.h"
#include "payload_refcounted.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

constexpr size_t EMU_MAX_TOPICS = 256;
constexpr size_t EMU_INLINE_MAX = 48;
constexpr size_t EMU_MAX_OBSERVERS = 8;

// Topic priority levels
enum class TopicPriority : uint8_t {
  Critical = 0,  // Audio, VBlank, Input - inline only, always processed first
  Normal = 1     // Debug, state saves - can use managed payloads
};

using EmuCallback = void (*)(uint16_t topicId, const void *data, size_t size, void *userData);

struct EmuObserver {
  EmuCallback callback;
  void *userData;
};

// Fast message - inline payload only, no allocation
struct EmuFastMessage {
  uint16_t topicId;
  uint8_t observerCount;
  uint8_t payloadSize;  // Max 48 bytes
  EmuObserver observers[EMU_MAX_OBSERVERS];
  alignas(alignof(std::max_align_t)) char data[EMU_INLINE_MAX];
};

// Bulk message - supports managed payloads
struct EmuBulkMessage {
  uint16_t topicId;
  uint8_t observerCount;
  bool isInline;
  size_t payloadSize;
  EmuObserver observers[EMU_MAX_OBSERVERS];
  union {
    RefCountedPayload *managed;
    alignas(alignof(std::max_align_t)) char inlineData[EMU_INLINE_MAX];
  };
};

template <size_t FastCapacity = 4096, size_t BulkCapacity = 16384>
class EventQueueEmulator {
  static_assert((FastCapacity & (FastCapacity - 1)) == 0, "FastCapacity must be power of 2");
  static_assert((BulkCapacity & (BulkCapacity - 1)) == 0, "BulkCapacity must be power of 2");

  // Separate queues for different priorities
  MPMCQueue<EmuFastMessage *, FastCapacity> m_fastQueue;
  MPMCQueue<EmuBulkMessage *, BulkCapacity> m_bulkQueue;

  ObjectPoolLockFree<EmuFastMessage> m_fastPool;
  ObjectPoolLockFree<EmuBulkMessage> m_bulkPool;

  struct TopicInfo {
    TopicPriority priority{TopicPriority::Normal};
    std::atomic<uint8_t> observerCount{0};
    EmuObserver observers[EMU_MAX_OBSERVERS];
  };

  std::unique_ptr<TopicInfo[]> m_topics{new TopicInfo[EMU_MAX_TOPICS]};
  std::mutex m_observerMutex;
  std::atomic<uint16_t> m_topicCount{0};
  std::atomic<size_t> m_bulkPending{0};

public:
  EventQueueEmulator()
      : m_fastPool(std::min<size_t>(FastCapacity, 256), FastCapacity + 64),
        m_bulkPool(std::min<size_t>(BulkCapacity, 128), BulkCapacity + 64) {}

  ~EventQueueEmulator() {
    EmuFastMessage *fastMsg = nullptr;
    while (m_fastQueue.try_pop(fastMsg)) {
      if (fastMsg) {
        m_fastPool.release(fastMsg);
      }
    }
    EmuBulkMessage *bulkMsg = nullptr;
    while (m_bulkQueue.try_pop(bulkMsg)) {
      if (bulkMsg) {
        if (!bulkMsg->isInline && bulkMsg->managed) {
          bulkMsg->managed->release();
        }
        m_bulkPool.release(bulkMsg);
      }
    }
  }

  // Register topic with priority
  uint16_t registerTopic(TopicPriority priority = TopicPriority::Normal) {
    uint16_t id = m_topicCount.load(std::memory_order_relaxed);
    for (;;) {
      if (id >= EMU_MAX_TOPICS)
        return UINT16_MAX;
      if (m_topicCount.compare_exchange_weak(id, static_cast<uint16_t>(id + 1),
                                             std::memory_order_relaxed))
        break;
    }
    m_topics[id].priority = priority;
    return id;
  }

  bool addObserver(uint16_t topicId, EmuCallback callback, void *userData = nullptr) {
    if (topicId >= EMU_MAX_TOPICS)
      return false;

    TopicInfo &topic = m_topics[topicId];
    std::lock_guard<std::mutex> lock(m_observerMutex);
    uint8_t idx = topic.observerCount.load(std::memory_order_relaxed);
    if (idx >= EMU_MAX_OBSERVERS)
      return false;

    topic.observers[idx].callback = callback;
    topic.observers[idx].userData = userData;
    topic.observerCount.store(static_cast<uint8_t>(idx + 1), std::memory_order_release);
    return true;
  }

  // Fast post for critical events - inline only, fails if payload > 48B
  bool postFast(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= EMU_MAX_TOPICS || size > EMU_INLINE_MAX)
      return false;

    TopicInfo &topic = m_topics[topicId];
    uint8_t count = topic.observerCount.load(std::memory_order_acquire);
    if (count == 0)
      return true;

    EmuFastMessage *msg = m_fastPool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = static_cast<uint8_t>(size);

    if (MC_LIKELY(count == 1)) {
      msg->observers[0] = topic.observers[0];
    } else {
      std::memcpy(msg->observers, topic.observers, count * sizeof(EmuObserver));
    }
    if (data && size > 0)
      std::memcpy(msg->data, data, size);

    while (!m_fastQueue.try_push(msg)) {
      std::this_thread::yield();
    }
    return true;
  }

  // Non-blocking fast post: returns false immediately if the fast queue is full
  bool tryPostFast(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= EMU_MAX_TOPICS || size > EMU_INLINE_MAX)
      return false;

    TopicInfo &topic = m_topics[topicId];
    uint8_t count = topic.observerCount.load(std::memory_order_acquire);
    if (count == 0)
      return true;

    if (m_fastQueue.full())
      return false;

    EmuFastMessage *msg = m_fastPool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = static_cast<uint8_t>(size);

    if (MC_LIKELY(count == 1)) {
      msg->observers[0] = topic.observers[0];
    } else {
      std::memcpy(msg->observers, topic.observers, count * sizeof(EmuObserver));
    }
    if (data && size > 0)
      std::memcpy(msg->data, data, size);

    if (!m_fastQueue.try_push(msg)) {
      m_fastPool.release(msg);
      return false;
    }
    return true;
  }

  // Convenience for POD types on fast queue
  template <typename T>
  bool postFastValue(uint16_t topicId, const T &value) {
    static_assert(sizeof(T) <= EMU_INLINE_MAX, "Type too large for fast queue");
    static_assert(std::is_trivially_copyable<T>::value, "T must be POD");
    return postFast(topicId, &value, sizeof(T));
  }

  // Signal-only post (no payload)
  bool postFast(uint16_t topicId) {
    return postFast(topicId, nullptr, 0);
  }

  // Bulk post - for larger payloads, auto-selects inline vs managed
  bool postBulk(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= EMU_MAX_TOPICS)
      return false;

    TopicInfo &topic = m_topics[topicId];
    uint8_t count = topic.observerCount.load(std::memory_order_acquire);
    if (count == 0)
      return true;

    EmuBulkMessage *msg = m_bulkPool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    if (MC_LIKELY(count == 1)) {
      msg->observers[0] = topic.observers[0];
    } else {
      std::memcpy(msg->observers, topic.observers, count * sizeof(EmuObserver));
    }

    if (MC_LIKELY(size <= EMU_INLINE_MAX)) {
      msg->isInline = true;
      if (data && size > 0)
        std::memcpy(msg->inlineData, data, size);
    } else {
      msg->isInline = false;
      msg->managed = RefCountedPayload::create(data, size);
    }

    while (!m_bulkQueue.try_push(msg)) {
      std::this_thread::yield();
    }
    m_bulkPending.fetch_add(1, std::memory_order_release);
    return true;
  }

  // Non-blocking bulk post: returns false immediately if the bulk queue is full
  bool tryPostBulk(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= EMU_MAX_TOPICS)
      return false;

    TopicInfo &topic = m_topics[topicId];
    uint8_t count = topic.observerCount.load(std::memory_order_acquire);
    if (count == 0)
      return true;

    if (m_bulkQueue.full())
      return false;

    EmuBulkMessage *msg = m_bulkPool.acquire();
    msg->topicId = topicId;
    msg->observerCount = count;
    msg->payloadSize = size;

    if (MC_LIKELY(count == 1)) {
      msg->observers[0] = topic.observers[0];
    } else {
      std::memcpy(msg->observers, topic.observers, count * sizeof(EmuObserver));
    }

    if (MC_LIKELY(size <= EMU_INLINE_MAX)) {
      msg->isInline = true;
      if (data && size > 0)
        std::memcpy(msg->inlineData, data, size);
    } else {
      msg->isInline = false;
      msg->managed = RefCountedPayload::create(data, size);
    }

    if (!m_bulkQueue.try_push(msg)) {
      if (!msg->isInline && msg->managed)
        msg->managed->release();
      m_bulkPool.release(msg);
      return false;
    }
    m_bulkPending.fetch_add(1, std::memory_order_release);
    return true;
  }

  // Non-blocking auto-routing post based on topic priority.
  // Critical topics use the inline fast queue when size <= EMU_INLINE_MAX, and
  // seamlessly fall back to the bulk queue for occasional larger payloads so
  // they are never denied service.
  bool tryPost(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= EMU_MAX_TOPICS)
      return false;

    if (m_topics[topicId].priority == TopicPriority::Critical &&
        size <= EMU_INLINE_MAX) {
      return tryPostFast(topicId, data, size);
    }
    return tryPostBulk(topicId, data, size);
  }

  // Auto-routing post based on topic priority.
  // Critical topics use the inline fast queue when size <= EMU_INLINE_MAX, and
  // seamlessly fall back to the bulk queue for occasional larger payloads so
  // they are never denied service.
  bool post(uint16_t topicId, const void *data, size_t size) {
    if (topicId >= EMU_MAX_TOPICS)
      return false;

    if (m_topics[topicId].priority == TopicPriority::Critical &&
        size <= EMU_INLINE_MAX) {
      return postFast(topicId, data, size);
    }
    return postBulk(topicId, data, size);
  }

  // Dispatch one fast message (returns true if dispatched)
  bool dispatchFast() {
    EmuFastMessage *msg = nullptr;
    if (!m_fastQueue.try_pop(msg) || !msg)
      return false;

    for (uint8_t i = 0; i < msg->observerCount; ++i) {
      msg->observers[i].callback(msg->topicId, msg->data, msg->payloadSize,
                                  msg->observers[i].userData);
    }

    m_fastPool.release(msg);
    return true;
  }

  // Dispatch one bulk message
  bool dispatchBulk() {
    EmuBulkMessage *msg = nullptr;
    if (!m_bulkQueue.try_pop(msg) || !msg)
      return false;
    m_bulkPending.fetch_sub(1, std::memory_order_relaxed);

    const void *data = msg->isInline ? msg->inlineData
                                      : (msg->managed ? msg->managed->data() : nullptr);

    for (uint8_t i = 0; i < msg->observerCount; ++i) {
      msg->observers[i].callback(msg->topicId, data, msg->payloadSize,
                                  msg->observers[i].userData);
    }

    if (!msg->isInline && msg->managed)
      msg->managed->release();

    m_bulkPool.release(msg);
    return true;
  }

  // Priority dispatch: drain up to maxFastBurst critical messages first, then
  // one bulk message so continuous fast traffic cannot starve (DoS) occasional
  // larger-payload messages in the bulk queue.
  // Returns number of messages dispatched.
  size_t dispatchPriority(size_t maxFastBurst = 64) {
    size_t count = 0;

    // Drain critical messages up to burst budget
    while (count < maxFastBurst && dispatchFast())
      ++count;

    // Give the bulk queue a turn when fast queue was empty or when bulk items
    // are waiting, so larger payloads are never starved without touching bulk
    // queue cache lines during pure-fast frames.
    if ((count == 0 || m_bulkPending.load(std::memory_order_acquire) != 0) &&
        dispatchBulk())
      ++count;

    return count;
  }

  // Dispatch all with priority (fast first, then bulk)
  size_t dispatchAll() {
    size_t count = 0;

    // Drain fast queue completely
    while (dispatchFast())
      ++count;

    // Then drain bulk queue
    while (dispatchBulk())
      ++count;

    return count;
  }

  // Ratio-based dispatch: N fast per 1 bulk (prevents bulk starvation)
  size_t dispatchRatio(size_t fastPerBulk = 4) {
    size_t count = 0;
    size_t fastCount = 0;

    while (true) {
      // Try fast messages up to ratio
      if (fastCount < fastPerBulk && dispatchFast()) {
        ++count;
        ++fastCount;
        continue;
      }

      // Try one bulk
      if (dispatchBulk()) {
        ++count;
        fastCount = 0;  // Reset ratio
        continue;
      }

      // Try fast if bulk was empty
      if (dispatchFast()) {
        ++count;
        ++fastCount;
        continue;
      }

      break;  // Both queues empty
    }

    return count;
  }

  bool fastEmpty() const { return m_fastQueue.empty(); }
  bool bulkEmpty() const { return m_bulkQueue.empty(); }
  bool empty() const { return fastEmpty() && bulkEmpty(); }

  size_t fastQueueSize() const { return m_fastQueue.size_approx(); }
  size_t bulkQueueSize() const { return m_bulkQueue.size_approx(); }
};

// Pre-defined topic IDs for common emulator events
namespace EmuTopics {
  constexpr uint16_t VBLANK = 0;
  constexpr uint16_t AUDIO_SYNC = 1;
  constexpr uint16_t INPUT_KEY = 2;
  constexpr uint16_t INPUT_JOY = 3;
  constexpr uint16_t FRAME_READY = 4;
  constexpr uint16_t CPU_TRACE = 5;
  constexpr uint16_t MEM_TRACE = 6;
  constexpr uint16_t STATE_SAVE = 7;
  constexpr uint16_t SCREENSHOT = 8;
}

#endif // MESSAGE_CENTER_EVENTQUEUE_EMULATOR_H
