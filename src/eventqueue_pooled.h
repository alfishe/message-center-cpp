#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_POOLED_H
#define MESSAGE_CENTER_EVENTQUEUE_POOLED_H

#include "eventqueue_lockfree.h"
#include "objectpool.h"

/// region <Pooled Lock-free EventQueue>

/// Lock-free EventQueue with object pooling for Message structs
/// Combines lock-free queue performance with reduced allocation overhead
///
/// Performance improvements over EventQueueLockFree:
/// - Eliminates heap allocation for Message wrappers
/// - Reduces allocator mutex contention
/// - Better cache locality through object reuse
///
/// Expected performance gain: 1.5-2x for high-throughput scenarios
class EventQueuePooled : public EventQueueLockFree {
protected:
  // Object pool for Message structs
  ObjectPool<Message> m_messagePool;

public:
  /// Constructor
  /// @param poolInitialSize - Number of Message objects to pre-allocate
  /// @param poolMaxSize - Maximum pool size (0 = unlimited)
  EventQueuePooled(size_t poolInitialSize = 128, size_t poolMaxSize = 2048);
  virtual ~EventQueuePooled();

  // Override Post methods to use pooled messages
  void Post(int id, MessagePayload *obj = nullptr,
            bool autoCleanupPayload = false);
  void Post(std::string topic, MessagePayload *obj = nullptr,
            bool autoCleanupPayload = false);

protected:
  // Override Dispatch to return messages to pool
  void Dispatch(int id, Message *message);

public:
  // Pool statistics
  size_t GetPoolSize() const { return m_messagePool.size(); }
  size_t GetPoolAllocated() const { return m_messagePool.allocated(); }
};

//
// Code Under Test (CUT) wrapper for pooled version
//
#ifdef _CODE_UNDER_TEST

class EventQueuePooledCUT : public EventQueuePooled {
public:
  EventQueuePooledCUT(size_t poolInitialSize = 128, size_t poolMaxSize = 2048)
      : EventQueuePooled(poolInitialSize, poolMaxSize){};

public:
  using EventQueuePooled::Dispatch;
  using EventQueuePooled::GetObservers;
  using EventQueuePooled::GetQueueMessage;
  using EventQueuePooled::m_lockfreeQueue;
  using EventQueuePooled::m_messagePool;
  using EventQueuePooled::m_topicMax;
  using EventQueuePooled::m_topicObservers;
  using EventQueuePooled::m_topicsResolveMap;
};

#endif // _CODE_UNDER_TEST

/// endregion </Pooled Lock-free EventQueue>

#endif // MESSAGE_CENTER_EVENTQUEUE_POOLED_H
