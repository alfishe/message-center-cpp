#include "eventqueue.h"

// Static pool initialization
ObjectPool<Message> EventQueue::MessagePool;

EventQueue::EventQueue() {
  // Pre-allocate COW vector
  m_cowObservers.resize(4096);
  m_initialized = false;
}

EventQueue::~EventQueue() { 
  WaitForDispatchesComplete(); 
}

void EventQueue::ResizeCOW(size_t size) {
  std::unique_lock<std::shared_mutex> lock(m_mutexCOW);
  if (m_cowObservers.size() < size) {
    m_cowObservers.resize(size);
  }
}

int EventQueue::RegisterTopic(const std::string &topic) {
  int result = -1;

  if (topic.length() > 0) {
    if (mc::key_exists(m_topicsResolveMap, topic)) {
      // Already registered. Returning its ID
      result = m_topicsResolveMap[topic];
    } else {
      if (m_topicMax < MAX_TOPICS) {
        // Registering new ID
        m_topicsResolveMap.insert({topic, m_topicMax});
        m_topics[m_topicMax] = topic;

        result = m_topicMax;
        m_topicMax++;

        // Resize COW vector if needed
        if (result >= (int)m_cowObservers.size()) {
          ResizeCOW(result + 1 + 1024);
        }
      } else {
        // Array for topic descriptors is full
        result = -2;
      }
    }
  }

  return result;
}

int EventQueue::RegisterTopic(const char *topic) {
  return RegisterTopic(std::string(topic));
}

void EventQueue::Post(int id, MessagePayload *obj, bool autoCleanupPayload) {
  Message *message = MessagePool.acquire();
  message->tid = id;
  message->obj = obj;
  message->cleanupPayload = autoCleanupPayload;

  std::lock_guard<std::mutex> lock(m_mutexMessages);
  m_messageQueue.push_back(message);
  m_cvEvents.notify_one();
}

void EventQueue::Post(std::string topic, MessagePayload *obj,
                      bool autoCleanupPayload) {
  int id = ResolveTopic(topic);
  if (id >= 0) {
    Post(id, obj, autoCleanupPayload);
  }
}

int EventQueue::AddObserver(const std::string &topic,
                            ObserverDescriptor *observer) {
  int id = RegisterTopic(topic);
  if (id < 0)
    return id;

  std::lock_guard<std::mutex> updateLock(m_mutexUpdates);

  if (id >= (int)m_cowObservers.size()) {
    ResizeCOW(id + 1 + 1024);
  }

  ObserversListPtr current = std::atomic_load(&m_cowObservers[id]);
  ObserversListPtr next =
      current ? std::make_shared<ObserversList>(*current)
              : std::make_shared<ObserversList>();

  next->push_back(observer);
  std::atomic_store(&m_cowObservers[id], next);

  return id;
}

void EventQueue::WaitForDispatchesComplete() {
  std::unique_lock<std::mutex> lock(m_mutexWait);
  m_cvNoneActive.wait(lock, [this] {
    return m_activeDispatches.load(std::memory_order_acquire) == 0;
  });
}

void EventQueue::RemoveObserverGeneric(
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

void EventQueue::RemoveObserver(const std::string &topic,
                                ObserverCallback callback) {
  RemoveObserverGeneric(topic, [callback](ObserverDescriptor *obs) {
    return obs->callback == callback;
  });
  WaitForDispatchesComplete();
}

void EventQueue::RemoveObserver(const std::string &topic, Observer *instance,
                                ObserverCallbackMethod callback) {
  RemoveObserverGeneric(topic, [instance, callback](ObserverDescriptor *obs) {
    return obs->observerInstance == instance && obs->callbackMethod == callback;
  });
  WaitForDispatchesComplete();
}

void EventQueue::RemoveObserver(const std::string &topic,
                                ObserverCallbackFunc callback) {
  auto callbackTargetAddr = mc_lambda_display::getTargetAddress(callback);
  RemoveObserverGeneric(topic, [callbackTargetAddr](ObserverDescriptor *obs) {
    auto curTargetAddr = mc_lambda_display::getTargetAddress(obs->callbackFunc);
    return curTargetAddr == callbackTargetAddr;
  });
  WaitForDispatchesComplete();
}

void EventQueue::RemoveObserver(const std::string &topic,
                                ObserverDescriptor *observer) {
  RemoveObserverGeneric(
      topic, [observer](ObserverDescriptor *obs) { return obs == observer; });
  WaitForDispatchesComplete();
}

Message *EventQueue::GetQueueMessage() {
  std::lock_guard<std::mutex> lock(m_mutexMessages);

  Message *result = nullptr;
  if (!m_messageQueue.empty()) {
    result = m_messageQueue.front();
    m_messageQueue.pop_front();
  }

  return result;
}

void EventQueue::Dispatch(int id, Message *message) {
  if (!message)
    return;

  // Increment active dispatch counter
  m_activeDispatches.fetch_add(1, std::memory_order_release);

  // Get COW snapshot
  std::shared_lock<std::shared_mutex> lock(m_mutexCOW);
  ObserversListPtr observers = std::atomic_load(&m_cowObservers[id]);
  lock.unlock();

  // Dispatch to all observers
  if (observers) {
    for (auto *obs : *observers) {
      if (obs->callback) {
        (*obs->callback)(id, message);
      } else if (obs->callbackMethod && obs->observerInstance) {
        (obs->observerInstance->*obs->callbackMethod)(id, message);
      } else if (obs->callbackFunc) {
        obs->callbackFunc(id, message);
      }
    }
  }

  // Cleanup payload if requested
  if (message->cleanupPayload && message->obj) {
    delete message->obj;
    message->obj = nullptr;
  }

  // Return message to pool (CRITICAL FIX!)
  MessagePool.release(message);

  // Decrement counter and notify if zero
  if (m_activeDispatches.fetch_sub(1, std::memory_order_release) == 1) {
    m_cvNoneActive.notify_all();
  }
}

// Topic resolution methods
int EventQueue::ResolveTopic(const char* topic) {
  std::string strTopic(topic);
  return ResolveTopic(strTopic);
}

int EventQueue::ResolveTopic(const std::string& topic) {
  int result = -1;

  if (topic.length() > 0) {
    if (mc::key_exists(m_topicsResolveMap, topic)) {
      result = m_topicsResolveMap[topic];
    }
  }

  return result;
}

std::string EventQueue::GetTopicByID(int id) {
  std::string result;

  if (id >= 0 && id < MAX_TOPICS) {
    result = m_topics[id];
  }

  return result;
}

void EventQueue::ClearTopics() {
  m_topicsResolveMap.clear();
  m_topicMax = 0;
}

// Initialization methods
bool EventQueue::init() {
  bool result = true;

  if (!m_initialized) {
    m_initialized = true;
  }

  return result;
}

void EventQueue::dispose() {
  if (m_initialized) {
    m_initialized = false;
  }
}

// AddObserver overloads - create descriptor and delegate
int EventQueue::AddObserver(const std::string& topic, ObserverCallback callback) {
  ObserverDescriptor* descriptor = new ObserverDescriptor();
  descriptor->callback = callback;
  descriptor->callbackMethod = nullptr;
  descriptor->callbackFunc = nullptr;
  descriptor->observerInstance = nullptr;
  
  return AddObserver(topic, descriptor);
}

int EventQueue::AddObserver(const std::string& topic, Observer* instance, ObserverCallbackMethod callback) {
  ObserverDescriptor* descriptor = new ObserverDescriptor();
  descriptor->callback = nullptr;
  descriptor->callbackMethod = callback;
  descriptor->callbackFunc = nullptr;
  descriptor->observerInstance = instance;
  
  return AddObserver(topic, descriptor);
}

int EventQueue::AddObserver(const std::string& topic, ObserverCallbackFunc callback) {
  ObserverDescriptor* descriptor = new ObserverDescriptor();
  descriptor->callback = nullptr;
  descriptor->callbackMethod = nullptr;
  descriptor->callbackFunc = callback;
  descriptor->observerInstance = nullptr;
  
  return AddObserver(topic, descriptor);
}

// GetObservers - for backward compatibility with tests
ObserverVectorPtr EventQueue::GetObservers(int id) {
  // Always sync from COW to ensure tests see current state
  if (id >= 0 && id < (int)m_cowObservers.size()) {
    std::shared_lock<std::shared_mutex> lock(m_mutexCOW);
    auto cowList = std::atomic_load(&m_cowObservers[id]);
    
    // Create or update the old map entry
    if (m_topicObservers.find(id) == m_topicObservers.end()) {
      m_topicObservers[id] = new ObserversVector();
    }
    
    // Always sync from COW
    m_topicObservers[id]->clear();
    if (cowList) {
      for (auto* obs : *cowList) {
        m_topicObservers[id]->push_back(obs);
      }
    }
    
    return m_topicObservers[id];
  }
  
  return nullptr;
}
