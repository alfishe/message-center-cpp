#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_OPTIMIZED_H
#define MESSAGE_CENTER_EVENTQUEUE_OPTIMIZED_H

#include "eventqueue_ultimate.h"
#include <shared_mutex>

/// region <Optimized EventQueue>

/// EventQueue with optimized observer access using read-write locks
/// Combines all previous optimizations PLUS:
/// - Read-write lock for observer access (multiple readers, single writer)
/// - Allows parallel GetObservers() calls from multiple consumer threads
///
/// Expected performance:
/// - Single-threaded: Same as Ultimate (~7.5 M/s)
/// - Multi-threaded: 2-3x improvement over Ultimate
/// - Scales much better with thread count
///
class EventQueueOptimized : public EventQueueUltimate {
protected:
  // Replace observer mutex with shared_mutex for read-write access
  mutable std::shared_mutex m_sharedMutexObservers;

public:
  EventQueueOptimized();
  virtual ~EventQueueOptimized();

protected:
  // Override GetObservers to use shared lock (multiple readers allowed!)
  ObserverVectorPtr GetObservers(int id);

  // Override observer registration to use exclusive lock
  void RegisterObserver(int id, ObserverDescriptor *observer);
  void UnregisterObserver(int id, ObserverDescriptor *observer);
};

//
// Code Under Test (CUT) wrapper
//
#ifdef _CODE_UNDER_TEST

class EventQueueOptimizedCUT : public EventQueueOptimized {
public:
  EventQueueOptimizedCUT() : EventQueueOptimized(){};

public:
  using EventQueueOptimized::Dispatch;
  using EventQueueOptimized::GetObservers;
  using EventQueueOptimized::GetQueueMessage;
  using EventQueueOptimized::m_lockfreeQueue;
  using EventQueueOptimized::m_topicMax;
  using EventQueueOptimized::m_topicObservers;
  using EventQueueOptimized::m_topicsResolveMap;
};

#endif // _CODE_UNDER_TEST

/// endregion </Optimized EventQueue>

#endif // MESSAGE_CENTER_EVENTQUEUE_OPTIMIZED_H
