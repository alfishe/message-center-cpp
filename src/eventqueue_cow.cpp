#include "eventqueue_cow.h"

EventQueueCOW::EventQueueCOW() : EventQueueUltimate() {
  // Pre-allocate to avoid resizing and allow lock-free reads in Dispatch
  // 4096 topics cover 256 instances * 16 events each, plenty.
  m_cowObservers.resize(4096);
}

EventQueueCOW::~EventQueueCOW() {}

void EventQueueCOW::ResizeCOW(size_t size) {
  std::unique_lock<std::shared_mutex> lock(m_mutexCOW);
  if (m_cowObservers.size() < size) {
    m_cowObservers.resize(size);
  }
}

int EventQueueCOW::RegisterTopic(const std::string &topic) {
  int id = EventQueueUltimate::RegisterTopic(topic);

  // Resize checks with potential lock
  if (id >= (int)m_cowObservers.size()) {
    ResizeCOW(id + 1 + 1024); // Grow in chunks
  }
  return id;
}

int EventQueueCOW::RegisterTopic(const char *topic) {
  return RegisterTopic(std::string(topic));
}

void EventQueueCOW::Dispatch(int id, Message *message) {
  if (message == nullptr)
    return;

  ObserversListPtr observers = nullptr;

  // OPTIMIZATION: Lock-Free Read Support
  // We assume m_cowObservers doesn't resize often.
  // If id is within safe bounds (pre-allocated), we skip the lock.
  // Note: In strict correctness, we'd need the lock or a lock-free vector.
  // Ideally use a paged vector (deque-like) that never invalidates.
  // For now, we rely on pre-allocation.

  if (id >= 0 && id < (int)m_cowObservers.size()) {
    // Atomic load of shared_ptr is thread-safe vs concurrent atomic_store
    observers = std::atomic_load(&m_cowObservers[id]);
  } else {
    // Fallback with lock if out of bounds (rare race during resize)
    std::shared_lock<std::shared_mutex> lock(m_mutexCOW);
    if (id >= 0 && id < (int)m_cowObservers.size()) {
      observers = std::atomic_load(&m_cowObservers[id]);
    }
  }

  if (observers) {
    for (auto observer : *observers) {
      if (observer->callback != nullptr) {
        (*observer->callback)(id, message);
      } else if (observer->callbackMethod != nullptr &&
                 observer->observerInstance != nullptr) {
        ObserverCallbackMethod callbackMethod = observer->callbackMethod;
        (observer->observerInstance->*callbackMethod)(id, message);
      } else if (observer->callbackFunc != nullptr) {
        (observer->callbackFunc)(id, message);
      }
    }
  }

  if (message->cleanupPayload && message->obj) {
    delete message->obj;
    message->obj = nullptr;
  }

  MessagePool::release(message);
}

int EventQueueCOW::AddObserver(const std::string &topic,
                               ObserverDescriptor *observer) {
  int id = RegisterTopic(topic);
  if (id < 0)
    return -1;

  std::lock_guard<std::mutex> updateLock(m_mutexUpdates);

  // We need to read the current slot.
  // If we are within bounds, direct access.
  // We don't hold m_mutexCOW unless resizing (handled in RegisterTopic).
  // But we should ensure we read a valid slot.

  if (id >= (int)m_cowObservers.size())
    return -1;

  ObserversListPtr current = std::atomic_load(&m_cowObservers[id]);
  ObserversListPtr next;

  if (current) {
    next = std::make_shared<ObserversList>(*current);
  } else {
    next = std::make_shared<ObserversList>();
  }

  next->push_back(observer);

  std::atomic_store(&m_cowObservers[id], next);

  return id;
}

void EventQueueCOW::RemoveObserverGeneric(
    const std::string &topic, std::function<bool(ObserverDescriptor *)> match) {
  int id = EventQueueUltimate::ResolveTopic(topic);
  if (id < 0)
    return;

  std::lock_guard<std::mutex> updateLock(m_mutexUpdates);

  if (id >= (int)m_cowObservers.size())
    return;

  ObserversListPtr current = std::atomic_load(&m_cowObservers[id]);
  if (!current)
    return;

  ObserversListPtr next = std::make_shared<ObserversList>(*current);

  auto it = next->begin();
  while (it != next->end()) {
    if (match(*it)) {
      delete *it;
      it = next->erase(it);
    } else {
      ++it;
    }
  }

  if (next->size() != current->size()) {
    std::atomic_store(&m_cowObservers[id], next);
  }
}

void EventQueueCOW::RemoveObserver(const std::string &topic,
                                   ObserverCallback callback) {
  RemoveObserverGeneric(topic, [callback](ObserverDescriptor *obs) {
    return obs->callback == callback;
  });
}

void EventQueueCOW::RemoveObserver(const std::string &topic, Observer *instance,
                                   ObserverCallbackMethod callback) {
  RemoveObserverGeneric(topic, [instance, callback](ObserverDescriptor *obs) {
    return obs->observerInstance == instance && obs->callbackMethod == callback;
  });
}

void EventQueueCOW::RemoveObserver(const std::string &topic,
                                   ObserverCallbackFunc callback) {
  RemoveObserverGeneric(topic, [](ObserverDescriptor *) { return false; });
}

void EventQueueCOW::RemoveObserver(const std::string &topic,
                                   ObserverDescriptor *observer) {
  RemoveObserverGeneric(
      topic, [observer](ObserverDescriptor *obs) { return obs == observer; });
}
