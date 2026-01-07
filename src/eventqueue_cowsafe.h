#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_COWSAFE_H
#define MESSAGE_CENTER_EVENTQUEUE_COWSAFE_H

#include "eventqueue_cow.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

/// region <Copy-On-Write Safe EventQueue>

/// EventQueue with Copy-On-Write observer lists AND safe unregistration
/// Guarantees that RemoveObserver blocks until all in-flight dispatches
/// complete
///
/// Design:
/// - Inherits all COW benefits (lock-free dispatch, thread-safe snapshots)
/// - Adds dispatch tracking to prevent dangling callback crashes
/// - RemoveObserver waits for active dispatches before returning
///
/// Performance:
/// - Zero overhead on Dispatch (just atomic increment/decrement)
/// - Blocking on RemoveObserver (acceptable for rare operation)
class EventQueueCOWSafe : public EventQueueCOW {
protected:
  // Track number of active dispatches
  std::atomic<int> m_activeDispatches{0};

  // Synchronization for waiting
  std::condition_variable m_cvNoneActive;
  mutable std::mutex m_mutexWait;

public:
  EventQueueCOWSafe();
  virtual ~EventQueueCOWSafe();

  // Override Dispatch to track active count
  void Dispatch(int id, Message *message) override;

  // Override RemoveObserver to wait for completion
  void RemoveObserver(const std::string &topic,
                      ObserverCallback callback) override;
  void RemoveObserver(const std::string &topic, Observer *instance,
                      ObserverCallbackMethod callback) override;
  void RemoveObserver(const std::string &topic,
                      ObserverCallbackFunc callback) override;
  void RemoveObserver(const std::string &topic,
                      ObserverDescriptor *observer) override;

  // Utility: Get current active dispatch count (for testing/monitoring)
  int GetActiveDispatchCount() const {
    return m_activeDispatches.load(std::memory_order_acquire);
  }

protected:
  // Helper to wait for all dispatches to complete
  void WaitForDispatchesComplete();
};

// CUT Wrapper
#ifdef _CODE_UNDER_TEST
class EventQueueCOWSafeCUT : public EventQueueCOWSafe {
public:
  EventQueueCOWSafeCUT() : EventQueueCOWSafe(){};

public:
  using EventQueueCOWSafe::Dispatch;
  using EventQueueCOWSafe::GetActiveDispatchCount;
  using EventQueueCOWSafe::GetQueueMessage;
  using EventQueueCOWSafe::m_cowObservers;
};
#endif

/// endregion </Copy-On-Write Safe EventQueue>

#endif // MESSAGE_CENTER_EVENTQUEUE_COWSAFE_H
