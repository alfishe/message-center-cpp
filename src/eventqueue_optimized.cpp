#include "eventqueue_optimized.h"

EventQueueOptimized::EventQueueOptimized() : EventQueueUltimate() {
  // Shared mutex is initialized automatically
}

EventQueueOptimized::~EventQueueOptimized() {
  // Parent destructor handles cleanup
}

ObserverVectorPtr EventQueueOptimized::GetObservers(int id) {
  // Use shared lock - multiple readers can access simultaneously!
  std::shared_lock<std::shared_mutex> lock(m_sharedMutexObservers);

  if (id < 0 || id >= m_topicMax)
    return nullptr;

  return m_topicObservers[id];
}

void EventQueueOptimized::RegisterObserver(int id,
                                           ObserverDescriptor *observer) {
  if (observer == nullptr)
    return;

  // Use exclusive lock for writes
  std::unique_lock<std::shared_mutex> lock(m_sharedMutexObservers);

  if (id < 0 || id >= m_topicMax)
    return;

  ObserverVectorPtr observers = m_topicObservers[id];
  if (observers == nullptr) {
    observers = new ObserversVector();
    m_topicObservers[id] = observers;
  }

  observers->push_back(observer);
}

void EventQueueOptimized::UnregisterObserver(int id,
                                             ObserverDescriptor *observer) {
  if (observer == nullptr)
    return;

  // Use exclusive lock for writes
  std::unique_lock<std::shared_mutex> lock(m_sharedMutexObservers);

  if (id < 0 || id >= m_topicMax)
    return;

  ObserverVectorPtr observers = m_topicObservers[id];
  if (observers != nullptr) {
    for (auto it = observers->begin(); it != observers->end(); ++it) {
      if (*it == observer) {
        observers->erase(it);
        break;
      }
    }
  }
}
