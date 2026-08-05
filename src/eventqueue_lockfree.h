#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_LOCKFREE_H
#define MESSAGE_CENTER_EVENTQUEUE_LOCKFREE_H

#include "eventqueue.h"
#include "mpmc_queue.h"
#include "objectpool_lockfree.h"
#include <thread>

// Lock-free variant of EventQueue using bounded MPMC ring buffer
// and lock-free object pool. Zero mutex contention in the hot path.

template <size_t QueueCapacity = 65536>
class EventQueueLockFree : public EventQueue {
  static_assert((QueueCapacity & (QueueCapacity - 1)) == 0,
                "QueueCapacity must be a power of 2");

  MPMCQueue<Message *, QueueCapacity> m_lockFreeQueue;
  ObjectPoolLockFree<Message> m_lockFreePool;

public:
  EventQueueLockFree() : EventQueue(), m_lockFreePool(256, 8192) {}

  void Post(int id, MessagePayload *obj = nullptr,
            bool autoCleanupPayload = false) {
    Message *message = m_lockFreePool.acquire();
    message->tid = id;
    message->obj = obj;
    message->cleanupPayload = autoCleanupPayload;

    // Spin until we can push (queue full is extremely rare with 64K capacity)
    while (!m_lockFreeQueue.try_push(message)) {
      std::this_thread::yield();
    }

    m_cvEvents.notify_one();
  }

  void Post(std::string topic, MessagePayload *obj = nullptr,
            bool autoCleanupPayload = false) {
    int id = ResolveTopic(topic);
    if (id >= 0) {
      Post(id, obj, autoCleanupPayload);
    }
  }

  void Dispatch(int id, Message *message) {
    if (!message)
      return;

    m_activeDispatches.fetch_add(1, std::memory_order_release);

    // Lock-free COW snapshot - atomic_load on shared_ptr is safe
    // COW vector is pre-sized to 4096 in constructor, no bounds check needed
    ObserversListPtr observers =
        (id >= 0 && id < 4096) ? std::atomic_load(&m_cowObservers[id]) : nullptr;

    if (observers) {
      for (auto *obs : *observers) {
        if (obs->callback) {
          (*obs->callback)(id, message);
        } else if (obs->callbackMethod && obs->observerInstance) {
          (obs->observerInstance->*obs->callbackMethod)(id, message);
        } else if (obs->callbackFunc) {
          obs->callbackFunc(id, message);
        }
      }
    }

    if (message->cleanupPayload && message->obj) {
      delete message->obj;
      message->obj = nullptr;
    }

    // Return to lock-free pool
    m_lockFreePool.release(message);

    if (m_activeDispatches.fetch_sub(1, std::memory_order_release) == 1) {
      m_cvNoneActive.notify_all();
    }
  }

protected:
  Message *GetQueueMessage() {
    Message *result = nullptr;
    m_lockFreeQueue.try_pop(result);
    return result;
  }
};

// CUT wrapper for benchmarking
#ifdef _CODE_UNDER_TEST
template <size_t QueueCapacity = 65536>
class EventQueueLockFreeCUT : public EventQueueLockFree<QueueCapacity> {
public:
  using EventQueueLockFree<QueueCapacity>::Dispatch;
  using EventQueueLockFree<QueueCapacity>::GetQueueMessage;
};
#endif

#endif // MESSAGE_CENTER_EVENTQUEUE_LOCKFREE_H
