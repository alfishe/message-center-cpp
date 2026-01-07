#include "eventqueue_lockfree.h"

EventQueueLockFree::EventQueueLockFree() : EventQueue() {
  // Lock-free queue is initialized in member initializer list
  // Pre-allocate some capacity to reduce allocations
  // Note: ConcurrentQueue handles its own memory management
}

EventQueueLockFree::~EventQueueLockFree() {
  // Clean up remaining messages in lock-free queue
  Message *message = nullptr;
  while (m_lockfreeQueue.try_dequeue(message)) {
    if (message != nullptr) {
      if (message->cleanupPayload && message->obj) {
        delete message->obj;
      }
      delete message;
    }
  }
}

void EventQueueLockFree::Post(int id, MessagePayload *obj,
                              bool autoCleanupPayload) {
  if (id >= 0) {
    // Create message (still allocated on heap - we'll optimize this with object
    // pooling later)
    Message *message = new Message(id, obj, autoCleanupPayload);

    // Lock-free enqueue - no mutex required!
    m_lockfreeQueue.enqueue(message);

    // Increment atomic counter
    m_queueSize.fetch_add(1, std::memory_order_relaxed);

    // Notify waiting threads (if using condition variable for blocking
    // operations)
    m_cvEvents.notify_one();
  }
}

void EventQueueLockFree::Post(std::string topic, MessagePayload *obj,
                              bool autoCleanupPayload) {
  int id = ResolveTopic(topic);
  Post(id, obj, autoCleanupPayload);
}

Message *EventQueueLockFree::GetQueueMessage() {
  Message *result = nullptr;

  // Lock-free dequeue - no mutex required!
  if (m_lockfreeQueue.try_dequeue(result)) {
    // Successfully dequeued
    m_queueSize.fetch_sub(1, std::memory_order_relaxed);
  }

  return result;
}
