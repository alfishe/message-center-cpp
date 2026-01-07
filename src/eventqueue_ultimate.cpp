#include "eventqueue_ultimate.h"

EventQueueUltimate::EventQueueUltimate() : EventQueueLockFree() {
  // Thread-local pools are initialized automatically
}

EventQueueUltimate::~EventQueueUltimate() {
  // Clean up remaining messages in queue
  Message *message = nullptr;
  while (m_lockfreeQueue.try_dequeue(message)) {
    if (message != nullptr) {
      // Cleanup payload if requested
      if (message->cleanupPayload && message->obj) {
        delete message->obj;
      }

      // Return message to thread-local pool
      MessagePool::release(message);
    }
  }

  // Thread-local pools will be cleaned up automatically when threads exit
}

void EventQueueUltimate::Post(int id, MessagePayload *obj,
                              bool autoCleanupPayload) {
  if (id >= 0) {
    // Acquire message from thread-local pool (no mutex!)
    Message *message = MessagePool::acquire();

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

void EventQueueUltimate::Post(std::string topic, MessagePayload *obj,
                              bool autoCleanupPayload) {
  int id = ResolveTopic(topic);
  Post(id, obj, autoCleanupPayload);
}

void EventQueueUltimate::Dispatch(int id, Message *message) {
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

  // Return message to thread-local pool (no mutex!)
  MessagePool::release(message);
}
