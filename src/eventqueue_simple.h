#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_SIMPLE_H
#define MESSAGE_CENTER_EVENTQUEUE_SIMPLE_H

#include "eventqueue.h"
#include "objectpool.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

/// region <Simple COW EventQueue>

/// Simplified EventQueue with:
/// - Original mutex-based message queue (std::deque + mutex)
/// - Object pooling for Message allocation
/// - COW for observer lists
/// - Safe unregistration (dispatch tracking)
///
/// This is the SIMPLEST production-ready implementation.
/// No lock-free queue complexity, just solid fundamentals.
class EventQueueSimple : public EventQueue {
protected:
  // Object pool for messages
  static ObjectPool<Message> MessagePool;

  // COW storage for observers
  using ObserversList = std::vector<ObserverDescriptor *>;
  using ObserversListPtr = std::shared_ptr<ObserversList>;

  std::vector<ObserversListPtr> m_cowObservers;
  mutable std::shared_mutex m_mutexCOW;
  std::mutex m_mutexUpdates;

  // Dispatch tracking for safe unregister
  std::atomic<int> m_activeDispatches{0};
  std::condition_variable m_cvNoneActive;
  mutable std::mutex m_mutexWait;

public:
  EventQueueSimple();
  virtual ~EventQueueSimple();

  // Expose base class overloads
  using EventQueue::AddObserver;
  using EventQueue::RemoveObserver;

  // Utility for benchmarking
  size_t GetQueueSize() {
    std::lock_guard<std::mutex> lock(m_mutexMessages);
    return m_messageQueue.size();
  }

  // Override Post to use object pool
  void Post(int id, MessagePayload *obj = nullptr,
            bool autoCleanupPayload = false) override;
  void Post(std::string topic, MessagePayload *obj = nullptr,
            bool autoCleanupPayload = false) override;

  // Override AddObserver to use COW
  int AddObserver(const std::string &topic,
                  ObserverDescriptor *observer) override;

  // Override RemoveObserver to use COW + wait for safety
  void RemoveObserver(const std::string &topic,
                      ObserverCallback callback) override;
  void RemoveObserver(const std::string &topic, Observer *instance,
                      ObserverCallbackMethod callback) override;
  void RemoveObserver(const std::string &topic,
                      ObserverCallbackFunc callback) override;
  void RemoveObserver(const std::string &topic,
                      ObserverDescriptor *observer) override;

  // Override RegisterTopic to resize COW vector
  int RegisterTopic(const std::string &topic) override;
  int RegisterTopic(const char *topic) override;

protected:
  // Override Dispatch to use COW + tracking
  void Dispatch(int id, Message *message) override;

  // Override GetQueueMessage to use pool
  Message *GetQueueMessage() override;

  void ResizeCOW(size_t size);
  void WaitForDispatchesComplete();
  void RemoveObserverGeneric(const std::string &topic,
                             std::function<bool(ObserverDescriptor *)> match);
};

// CUT Wrapper
#ifdef _CODE_UNDER_TEST
class EventQueueSimpleCUT : public EventQueueSimple {
public:
  EventQueueSimpleCUT() : EventQueueSimple(){};

public:
  using EventQueueSimple::Dispatch;
  using EventQueueSimple::GetQueueMessage;
  using EventQueueSimple::m_cowObservers;
};
#endif

/// endregion </Simple COW EventQueue>

#endif // MESSAGE_CENTER_EVENTQUEUE_SIMPLE_H
