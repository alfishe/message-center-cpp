#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_LOCKFREE_H
#define MESSAGE_CENTER_EVENTQUEUE_LOCKFREE_H

#include "concurrentqueue.h"
#include "eventqueue.h"

#include <atomic>

/// region <Lock-free EventQueue>

/// Lock-free EventQueue implementation using moodycamel::ConcurrentQueue
/// This version removes mutex locks for Post() and GetQueueMessage() operations
/// providing better scalability for multi-threaded scenarios
class EventQueueLockFree : public EventQueue {
protected:
  // Lock-free message queue (replaces std::deque + mutex)
  moodycamel::ConcurrentQueue<Message *> m_lockfreeQueue;

  // Atomic counter for queue size (optional, for diagnostics)
  std::atomic<size_t> m_queueSize{0};

public:
  EventQueueLockFree();
  virtual ~EventQueueLockFree();

  // Override Post methods to use lock-free queue
  void Post(int id, MessagePayload *obj = nullptr,
            bool autoCleanupPayload = false);
  void Post(std::string topic, MessagePayload *obj = nullptr,
            bool autoCleanupPayload = false);

protected:
  // Override GetQueueMessage to use lock-free queue
  Message *GetQueueMessage();

public:
  // Queue size accessor
  size_t GetQueueSize() const {
    return m_queueSize.load(std::memory_order_relaxed);
  }
};

//
// Code Under Test (CUT) wrapper for lock-free version
//
#ifdef _CODE_UNDER_TEST

class EventQueueLockFreeCUT : public EventQueueLockFree {
public:
  EventQueueLockFreeCUT() : EventQueueLockFree(){};

public:
  using EventQueueLockFree::Dispatch;
  using EventQueueLockFree::GetObservers;
  using EventQueueLockFree::GetQueueMessage;
  using EventQueueLockFree::m_lockfreeQueue;
  using EventQueueLockFree::m_topicMax;
  using EventQueueLockFree::m_topicObservers;
  using EventQueueLockFree::m_topicsResolveMap;
};

#endif // _CODE_UNDER_TEST

/// endregion </Lock-free EventQueue>

#endif // MESSAGE_CENTER_EVENTQUEUE_LOCKFREE_H
