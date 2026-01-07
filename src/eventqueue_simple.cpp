#include "eventqueue_simple.h"

// Static pool initialization
ObjectPool<Message> EventQueueSimple::MessagePool;

EventQueueSimple::EventQueueSimple() : EventQueue() {
  // Pre-allocate COW vector
  m_cowObservers.resize(4096);
}

EventQueueSimple::~EventQueueSimple() { WaitForDispatchesComplete(); }

void EventQueueSimple::ResizeCOW(size_t size) {
  std::unique_lock<std::shared_mutex> lock(m_mutexCOW);
  if (m_cowObservers.size() < size) {
    m_cowObservers.resize(size);
  }
}

int EventQueueSimple::RegisterTopic(const std::string &topic) {
  int id = EventQueue::RegisterTopic(topic);

  if (id >= (int)m_cowObservers.size()) {
    ResizeCOW(id + 1 + 1024);
  }
  return id;
}

int EventQueueSimple::RegisterTopic(const char *topic) {
  return RegisterTopic(std::string(topic));
}

void EventQueueSimple::Post(int id, MessagePayload *obj,
                            bool autoCleanupPayload) {
  // Acquire message from pool
  Message *message = MessagePool.acquire();
  message->tid = id;
  message->obj = obj;
  message->cleanupPayload = autoCleanupPayload;

  // Use base class mutex-based queue
  std::lock_guard<std::mutex> lock(m_mutexMessages);
  m_messageQueue.push_back(message);
  m_cvEvents.notify_one();
}

void EventQueueSimple::Post(std::string topic, MessagePayload *obj,
                            bool autoCleanupPayload) {
  int id = ResolveTopic(topic);
  if (id >= 0) {
    Post(id, obj, autoCleanupPayload);
  }
}

Message *EventQueueSimple::GetQueueMessage() {
  // Use base class mutex-based dequeue
  Message *result = nullptr;

  std::lock_guard<std::mutex> lock(m_mutexMessages);
  if (!m_messageQueue.empty()) {
    result = m_messageQueue.front();
    m_messageQueue.pop_front();
  }

  return result;
}

void EventQueueSimple::Dispatch(int id, Message *message) {
  if (message == nullptr)
    return;

  // Track active dispatches
  m_activeDispatches.fetch_add(1, std::memory_order_acquire);

  ObserversListPtr observers = nullptr;

  // Lock-free read (pre-allocated vector)
  if (id >= 0 && id < (int)m_cowObservers.size()) {
    observers = std::atomic_load(&m_cowObservers[id]);
  }

  // Dispatch to observers
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

  // Cleanup message
  if (message->cleanupPayload && message->obj) {
    delete message->obj;
    message->obj = nullptr;
  }

  // Return to pool
  MessagePool.release(message);

  // Decrement and notify
  if (m_activeDispatches.fetch_sub(1, std::memory_order_release) == 1) {
    std::lock_guard<std::mutex> lock(m_mutexWait);
    m_cvNoneActive.notify_all();
  }
}

int EventQueueSimple::AddObserver(const std::string &topic,
                                  ObserverDescriptor *observer) {
  int id = RegisterTopic(topic);
  if (id < 0)
    return -1;

  std::lock_guard<std::mutex> updateLock(m_mutexUpdates);

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

void EventQueueSimple::WaitForDispatchesComplete() {
  std::unique_lock<std::mutex> lock(m_mutexWait);
  m_cvNoneActive.wait(lock, [this] {
    return m_activeDispatches.load(std::memory_order_acquire) == 0;
  });
}

void EventQueueSimple::RemoveObserverGeneric(
    const std::string &topic, std::function<bool(ObserverDescriptor *)> match) {
  int id = EventQueue::ResolveTopic(topic);
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

void EventQueueSimple::RemoveObserver(const std::string &topic,
                                      ObserverCallback callback) {
  RemoveObserverGeneric(topic, [callback](ObserverDescriptor *obs) {
    return obs->callback == callback;
  });
  WaitForDispatchesComplete();
}

void EventQueueSimple::RemoveObserver(const std::string &topic,
                                      Observer *instance,
                                      ObserverCallbackMethod callback) {
  RemoveObserverGeneric(topic, [instance, callback](ObserverDescriptor *obs) {
    return obs->observerInstance == instance && obs->callbackMethod == callback;
  });
  WaitForDispatchesComplete();
}

void EventQueueSimple::RemoveObserver(const std::string &topic,
                                      ObserverCallbackFunc callback) {
  RemoveObserverGeneric(topic, [](ObserverDescriptor *) { return false; });
  WaitForDispatchesComplete();
}

void EventQueueSimple::RemoveObserver(const std::string &topic,
                                      ObserverDescriptor *observer) {
  RemoveObserverGeneric(
      topic, [observer](ObserverDescriptor *obs) { return obs == observer; });
  WaitForDispatchesComplete();
}
