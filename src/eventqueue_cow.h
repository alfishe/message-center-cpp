#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_COW_H
#define MESSAGE_CENTER_EVENTQUEUE_COW_H

#include "eventqueue_ultimate.h"
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

/// region <Copy-On-Write EventQueue>

/// EventQueue with Copy-On-Write (COW) observer lists
/// Solves the race condition and eliminates read-side mutex contention
///
/// Design:
/// - Readers (Dispatch) perform a cheap atomic copy of shared_ptr and iterate
/// safely.
/// - Writers (Add/Remove) Copy-Update-Swap the shared_ptr.
/// - Zero mutex contention during callback execution.
class EventQueueCOW : public EventQueueUltimate {
protected:
  // COW storage types
  using ObserversList = std::vector<ObserverDescriptor *>;
  using ObserversListPtr = std::shared_ptr<ObserversList>;

  // The master vector of observer lists
  // Access to this vector structure is protected by m_mutexCOW (shared_mutex)
  // The shared_ptrs inside are accessed atomically
  std::vector<ObserversListPtr> m_cowObservers;
  mutable std::shared_mutex m_mutexCOW;

  // Mutex to serialize updates (Add/Remove) to prevent write-write races on
  // same topic We use one global update mutex simplifies logic. Per-topic would
  // be better if high flux.
  std::mutex m_mutexUpdates;

public:
  EventQueueCOW();
  virtual ~EventQueueCOW();

  // Expose base class overloads hidden by our overrides
  using EventQueue::AddObserver;
  using EventQueue::RemoveObserver;

  // Override the core AddObserver used by other overloads
  // The base class overloads for (callback, instance, func) call this virtual
  // method
  int AddObserver(const std::string &topic,
                  ObserverDescriptor *observer) override;

  // Override all RemoveObserver variants as they have distinct logic in base
  void RemoveObserver(const std::string &topic,
                      ObserverCallback callback) override;
  void RemoveObserver(const std::string &topic, Observer *instance,
                      ObserverCallbackMethod callback) override;
  void RemoveObserver(const std::string &topic,
                      ObserverCallbackFunc callback) override;
  void RemoveObserver(const std::string &topic,
                      ObserverDescriptor *observer) override;

  // Override RegisterTopic to resize our COW vector
  int RegisterTopic(const std::string &topic) override;
  int RegisterTopic(const char *topic) override;

protected:
  // Override Dispatch to use safe COW iteration
  void Dispatch(int id, Message *message) override;

  // Helper to resize vector thread-safely
  void ResizeCOW(size_t size);

  // Helper for removing observers
  void RemoveObserverGeneric(const std::string &topic,
                             std::function<bool(ObserverDescriptor *)> match);
};

// CUT Wrapper
#ifdef _CODE_UNDER_TEST
class EventQueueCOWCUT : public EventQueueCOW {
public:
  EventQueueCOWCUT() : EventQueueCOW(){};

public:
  using EventQueueCOW::Dispatch;
  using EventQueueCOW::GetQueueMessage;
  using EventQueueCOW::m_cowObservers;
};
#endif

/// endregion </Copy-On-Write EventQueue>

#endif // MESSAGE_CENTER_EVENTQUEUE_COW_H
