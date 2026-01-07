#include "eventqueue_cowsafe.h"

EventQueueCOWSafe::EventQueueCOWSafe() : EventQueueCOW() {}

EventQueueCOWSafe::~EventQueueCOWSafe() {
  // Wait for any remaining dispatches before destruction
  WaitForDispatchesComplete();
}

void EventQueueCOWSafe::Dispatch(int id, Message *message) {
  if (message == nullptr)
    return;

  // Increment active dispatch counter
  m_activeDispatches.fetch_add(1, std::memory_order_acquire);

  // Perform the actual dispatch (parent implementation)
  EventQueueCOW::Dispatch(id, message);

  // Decrement counter and notify if we were the last one
  int remaining = m_activeDispatches.fetch_sub(1, std::memory_order_release);
  if (remaining == 1) {
    // We were the last active dispatch
    std::lock_guard<std::mutex> lock(m_mutexWait);
    m_cvNoneActive.notify_all();
  }
}

void EventQueueCOWSafe::WaitForDispatchesComplete() {
  std::unique_lock<std::mutex> lock(m_mutexWait);
  m_cvNoneActive.wait(lock, [this] {
    return m_activeDispatches.load(std::memory_order_acquire) == 0;
  });
}

void EventQueueCOWSafe::RemoveObserver(const std::string &topic,
                                       ObserverCallback callback) {
  // First, remove from the observer list (COW)
  EventQueueCOW::RemoveObserver(topic, callback);

  // Then wait for all in-flight dispatches to complete
  // This guarantees the callback won't be called after this returns
  WaitForDispatchesComplete();
}

void EventQueueCOWSafe::RemoveObserver(const std::string &topic,
                                       Observer *instance,
                                       ObserverCallbackMethod callback) {
  EventQueueCOW::RemoveObserver(topic, instance, callback);
  WaitForDispatchesComplete();
}

void EventQueueCOWSafe::RemoveObserver(const std::string &topic,
                                       ObserverCallbackFunc callback) {
  EventQueueCOW::RemoveObserver(topic, callback);
  WaitForDispatchesComplete();
}

void EventQueueCOWSafe::RemoveObserver(const std::string &topic,
                                       ObserverDescriptor *observer) {
  EventQueueCOW::RemoveObserver(topic, observer);
  WaitForDispatchesComplete();
}
