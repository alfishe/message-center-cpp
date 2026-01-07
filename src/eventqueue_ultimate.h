#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_ULTIMATE_H
#define MESSAGE_CENTER_EVENTQUEUE_ULTIMATE_H

#include "eventqueue_lockfree.h"
#include "threadlocal_pool.h"

/// region <Ultimate EventQueue>

/// The ultimate EventQueue combining all optimizations:
/// 1. Lock-free queue (moodycamel::ConcurrentQueue)
/// 2. Object pooling (eliminates allocations)
/// 3. Per-thread pools (eliminates pool mutex contention)
///
/// Expected performance:
/// - Single-threaded: 2x improvement (from pooling)
/// - Multi-threaded: 3-5x improvement (from per-thread pools)
/// - Scales linearly with thread count
///
class EventQueueUltimate : public EventQueueLockFree {
protected:
  // Thread-local object pool for Message structs
  // Each thread has its own pool - no mutex contention!
  using MessagePool = ThreadLocalObjectPool<Message>;

public:
  EventQueueUltimate();
  virtual ~EventQueueUltimate();

  // Override Post methods to use thread-local pooled messages
  void Post(int id, MessagePayload *obj = nullptr,
            bool autoCleanupPayload = false);
  void Post(std::string topic, MessagePayload *obj = nullptr,
            bool autoCleanupPayload = false);

protected:
  // Override Dispatch to return messages to thread-local pool
  void Dispatch(int id, Message *message);

public:
  // Pool statistics
  size_t GetLocalPoolSize() const { return MessagePool::getLocalPoolSize(); }
  size_t GetLocalAllocated() const { return MessagePool::getLocalAllocated(); }
  size_t GetGlobalPoolSize() const { return MessagePool::getGlobalPoolSize(); }
  size_t GetGlobalAllocated() const {
    return MessagePool::getGlobalAllocated();
  }
};

//
// Code Under Test (CUT) wrapper for ultimate version
//
#ifdef _CODE_UNDER_TEST

class EventQueueUltimateCUT : public EventQueueUltimate {
public:
  EventQueueUltimateCUT() : EventQueueUltimate(){};

public:
  using EventQueueUltimate::Dispatch;
  using EventQueueUltimate::GetObservers;
  using EventQueueUltimate::GetQueueMessage;
  using EventQueueUltimate::m_lockfreeQueue;
  using EventQueueUltimate::m_topicMax;
  using EventQueueUltimate::m_topicObservers;
  using EventQueueUltimate::m_topicsResolveMap;
};

#endif // _CODE_UNDER_TEST

/// endregion </Ultimate EventQueue>

#endif // MESSAGE_CENTER_EVENTQUEUE_ULTIMATE_H
