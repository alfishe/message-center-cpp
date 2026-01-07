#include "eventqueue_pooled.h"

EventQueuePooled::EventQueuePooled(size_t poolInitialSize, size_t poolMaxSize)
    : EventQueueLockFree(), m_messagePool(poolInitialSize, poolMaxSize) {
  // Object pool is initialized in member initializer list
}

EventQueuePooled::~EventQueuePooled() {
  // Clean up remaining messages in queue
  Message *message = nullptr;
  while (m_lockfreeQueue.try_dequeue(message)) {
    if (message != nullptr) {
      // Cleanup payload if requested
      if (message->cleanupPayload && message->obj) {
        delete message->obj;
      }

      // Return message to pool instead of deleting
      m_messagePool.release(message);
    }
  }

  // Pool will be cleaned up by its destructor
}

void EventQueuePooled::Post(int id, MessagePayload *obj,
                            bool autoCleanupPayload) {
  if (id >= 0) {
    // Acquire message from pool instead of 'new'
    Message *message = m_messagePool.acquire();

    // Initialize message fields
    message->tid = id;
    message->obj = obj;
    message->cleanupPayload = autoCleanupPayload;

    // Lock-free enqueue
    m_lockfreeQueue.enqueue(message);

    // Increment atomic counter
    m_queueSize.fetch_add(1, std::memory_order_relaxed);

    // Notify waiting threads
    m_cvEvents.notify_one();
  }
}

void EventQueuePooled::Post(std::string topic, MessagePayload *obj,
                            bool autoCleanupPayload) {
  int id = ResolveTopic(topic);
  Post(id, obj, autoCleanupPayload);
}

void EventQueuePooled::Dispatch(int id, Message *message) {
  if (message == nullptr)
    return;

  // Get observers for this topic
  ObserverVectorPtr observers = GetObservers(id);

  // Dispatch to all observers
  if (observers != nullptr) {
    for (auto it : *observers) {
      if (it->callback != nullptr) {
        (*it->callback)(id, message);
      } else if (it->callbackMethod != nullptr &&
                 it->observerInstance != nullptr) {
        ObserverCallbackMethod callbackMethod = it->callbackMethod;
        (it->observerInstance->*callbackMethod)(id, message);
      } else if (it->callbackFunc != nullptr) {
        (it->callbackFunc)(id, message);
      }
    }
  }

  // Cleanup payload if requested (user's responsibility)
  if (message->cleanupPayload && message->obj) {
    delete message->obj;
    message->obj = nullptr; // Prevent double-delete
  }

  // Return message to pool instead of deleting
  m_messagePool.release(message);
}
