#include "eventqueue.h"


EventQueue::EventQueue()
{
    init();
}

EventQueue::~EventQueue()
{
    dispose();
}

bool EventQueue::init()
{
    bool result = false;

    m_topicsResolveMap.reserve(MAX_TOPICS);
    for (unsigned i = 0; i < MAX_TOPICS; ++i)
    {
        m_topicObserversTable[i] = nullptr;
        m_topicSnapshots[i].reset();
    }
    m_workerSleeping.store(false, std::memory_order_relaxed);
    m_pendingMessages.store(false, std::memory_order_relaxed);
    m_maxQueueSize.store(0, std::memory_order_relaxed);

    m_initialized = true;

    return result;
}

void EventQueue::dispose()
{
    // Cleanup message queue
    {
        std::lock_guard<std::mutex> lock(m_mutexMessages);

        for (auto it : m_messageQueue)
        {
            if (it != nullptr)
            {
                if (it->cleanupPayload && it->obj != nullptr)
                {
                    delete it->obj;
                    it->obj = nullptr;
                }
                delete it;
            }
        }

        m_messageQueue.clear();
    }

    // Cleanup observers
    {
        std::lock_guard<std::mutex> lock(m_mutexObservers);

        for (auto it : m_topicObservers)
        {
            if (it.second != nullptr)
            {
                for (auto observerDescriptor : *it.second)
                {
                    if (observerDescriptor != nullptr)
                    {
                        if (observerDescriptor->active)
                        {
                            observerDescriptor->active->store(false, std::memory_order_release);
                        }
                        delete observerDescriptor;
                    }
                }

                it.second->clear();
                delete it.second;
            }
        }

        m_topicObservers.clear();
        for (unsigned i = 0; i < MAX_TOPICS; ++i)
        {
            m_topicObserversTable[i] = nullptr;
            m_topicSnapshots[i].reset();
        }
    }
}

uint64_t EventQueue::AddObserver(const std::string& topic, ObserverCallback callback)
{
    ObserverDescriptor* observer = new ObserverDescriptor();
    observer->callback = callback;
    return AddObserver(topic, observer);
}

// Add class method as observer
// 1. Class should be derived from Observer
// 2. Method signature should be void method(int id, Message* message);
// Example:
// class TestObservers_ClassMethod_class : public Observer
// {
// public:
//     void ObserverTestMethod(int id, Message* message) { str::cout << "It works!" << std::endl; }
// }
//
// EventQueue queue;
// // Register topics... other prep. actions
//
// TestObservers_ClassMethod_class observerDerivedInstance;
// Observer* observerInstance = static_cast<Observer*>(&observerDerivedInstance);
// ObserverCallbackMethod callback = static_cast<ObserverCallbackMethod>(&TestObservers_ClassMethod_class::ObserverTestMethod);
//
// queue.AddObserver(topic, observerInstance, callback);
uint64_t EventQueue::AddObserver(const std::string& topic, Observer* instance, ObserverCallbackMethod callback)
{
    ObserverDescriptor* observer = new ObserverDescriptor();
    observer->callbackMethod = callback;
    observer->observerInstance = instance;

    return AddObserver(topic, observer);
}

uint64_t EventQueue::AddObserver(const std::string& topic, ObserverCallbackFunc callback)
{
    ObserverDescriptor* observer = new ObserverDescriptor();
    observer->callbackFunc = callback;
    return AddObserver(topic, observer);
}

uint64_t EventQueue::AddObserver(const std::string& topic, ObserverDescriptor* observer)
{
    if (observer == nullptr)
        return 0;

    // Register Topic (or get TopicID if already registered)
    int topicId = RegisterTopic(topic);

    if (topicId < 0)
    {
        delete observer;
        return 0;  // Invalid topic
    }

    // Assign unique observer ID
    uint64_t observerId = m_nextObserverId.fetch_add(1, std::memory_order_relaxed);
    observer->observerId = observerId;

    {
        // Lock parallel threads to access (active till return from method and lock destruction)
        std::lock_guard<std::mutex> lock(m_mutexObservers);

        ObserverVectorPtr observers = GetObservers(topicId);
        if (observers == nullptr)
        {
            // Observers list not created yet - create vector and register it in topic-observers map collection
            observers = new ObserversVector();
            m_topicObservers.insert( {topicId, observers });
            if (topicId >= 0 && static_cast<unsigned>(topicId) < MAX_TOPICS)
            {
                m_topicObserversTable[topicId] = observers;
            }
        }

        if (observers != nullptr)
        {
            observers->push_back(observer);
        }

        if (topicId >= 0 && static_cast<unsigned>(topicId) < MAX_TOPICS)
        {
            m_topicSnapshots[topicId].reset();
        }
    }

    return observerId;
}

void EventQueue::RemoveObserver(const std::string& topic, ObserverCallback callback)
{
    int result = ResolveTopic(topic);

    {
        // Lock parallel threads to access (active till return from method and lock destruction)
        std::lock_guard<std::mutex> lock(m_mutexObservers);

        ObserverVectorPtr observers = GetObservers(result);
        if (observers != nullptr)
        {
            ObserversVector::const_iterator it;
            for (it = observers->begin(); it != observers->end(); )
            {
                ObserverDescriptor* observer = *it;

                if (observer->callback == callback)
                {
                    // Erase current element and get next iterator value
                    it = observers->erase(it);

                    // Deactivate and destroy descriptor object
                    if (observer->active)
                    {
                        observer->active->store(false, std::memory_order_release);
                    }
                    delete observer;
                }
                else
                {
                    it++;
                }
            }
        }

        if (result >= 0 && static_cast<unsigned>(result) < MAX_TOPICS)
        {
            m_topicSnapshots[result].reset();
        }
    }

    // In-flight dispatches may still be invoking this observer's handler from
    // a pre-removal snapshot - wait for them (see WaitForDispatchesToComplete)
    WaitForDispatchesToComplete();
}

void EventQueue::RemoveObserver(const std::string& topic, Observer* instance, ObserverCallbackMethod callback)
{
    int result = ResolveTopic(topic);

    {
        // Lock parallel threads to access (active till return from method and lock destruction)
        std::lock_guard<std::mutex> lock(m_mutexObservers);

        ObserverVectorPtr observers = GetObservers(result);
        if (observers != nullptr)
        {
            ObserversVector::const_iterator it;
            for (it = observers->begin(); it != observers->end(); )
            {
                ObserverDescriptor* observer = *it;

                if (observer->observerInstance == instance && observer->callbackMethod == callback)
                {
                    // Erase current element and get next iterator value
                    it = observers->erase(it);

                    // Deactivate and destroy descriptor object
                    if (observer->active)
                    {
                        observer->active->store(false, std::memory_order_release);
                    }
                    delete observer;
                }
                else
                {
                    it++;
                }
            }
        }

        if (result >= 0 && static_cast<unsigned>(result) < MAX_TOPICS)
        {
            m_topicSnapshots[result].reset();
        }
    }

    // In-flight dispatches may still be invoking this observer's handler from
    // a pre-removal snapshot - wait for them (see WaitForDispatchesToComplete)
    WaitForDispatchesToComplete();
}

void EventQueue::RemoveObserverById(const std::string& topic, uint64_t observerId)
{
    if (observerId == 0)
        return;  // Invalid ID

    int topicId = ResolveTopic(topic);

    {
        // Lock parallel threads to access (active till return from method and lock destruction)
        std::lock_guard<std::mutex> lock(m_mutexObservers);

        ObserverVectorPtr observers = GetObservers(topicId);
        if (observers != nullptr)
        {
            ObserversVector::const_iterator it;
            for (it = observers->begin(); it != observers->end(); )
            {
                ObserverDescriptor* observer = *it;

                if (observer->observerId == observerId)
                {
                    // Erase current element and get next iterator value
                    it = observers->erase(it);

                    // Deactivate and destroy descriptor object
                    if (observer->active)
                    {
                        observer->active->store(false, std::memory_order_release);
                    }
                    delete observer;
                    break;  // IDs are unique, no need to continue
                }
                else
                {
                    it++;
                }
            }
        }

        if (topicId >= 0 && static_cast<unsigned>(topicId) < MAX_TOPICS)
        {
            m_topicSnapshots[topicId].reset();
        }
    }

    // In-flight dispatches may still be invoking this observer's handler from
    // a pre-removal snapshot - wait for them (see WaitForDispatchesToComplete)
    WaitForDispatchesToComplete();
}

void EventQueue::RemoveObserver(const std::string& topic, ObserverDescriptor* observer)
{
    if (observer == nullptr)
        return;

    int topicId = ResolveTopic(topic);

    {
        std::lock_guard<std::mutex> lock(m_mutexObservers);

        ObserverVectorPtr observers = GetObservers(topicId);
        if (observers != nullptr)
        {
            ObserversVector::const_iterator it;
            for (it = observers->begin(); it != observers->end(); )
            {
                ObserverDescriptor* desc = *it;

                if (desc == observer || (desc != nullptr && desc->observerId == observer->observerId))
                {
                    it = observers->erase(it);
                    if (desc->active)
                    {
                        desc->active->store(false, std::memory_order_release);
                    }
                    delete desc;
                    break;
                }
                else
                {
                    it++;
                }
            }
        }

        if (topicId >= 0 && static_cast<unsigned>(topicId) < MAX_TOPICS)
        {
            m_topicSnapshots[topicId].reset();
        }
    }

    WaitForDispatchesToComplete();
}

int EventQueue::ResolveTopic(const char* topic)
{
    if (topic == nullptr || topic[0] == '\0')
        return -1;

    std::string strTopic(topic);
    return ResolveTopic(strTopic);
}


int EventQueue::ResolveTopic(const std::string& topic)
{
    if (topic.empty())
        return -1;

    // The registry is mutated by RegisterTopic() from any thread while
    // Post() resolves topics from the dispatching path - guard the access
    std::lock_guard<std::mutex> lock(m_mutexTopics);

    auto it = m_topicsResolveMap.find(topic);
    if (it != m_topicsResolveMap.end())
    {
        return it->second;
    }

    return -1;
}

int EventQueue::RegisterTopic(const char* topic)
{
    if (topic == nullptr || topic[0] == '\0')
        return -1;

    std::string strTopic(topic);
    return RegisterTopic(strTopic);
}

int EventQueue::RegisterTopic(const std::string& topic)
{
    if (topic.empty())
        return -1;

    // The registry is read by ResolveTopic() from any thread - guard the access
    std::lock_guard<std::mutex> lock(m_mutexTopics);

    auto it = m_topicsResolveMap.find(topic);
    if (it != m_topicsResolveMap.end())
    {
        // Already registered. Returning its ID
        return it->second;
    }

    if (static_cast<unsigned>(m_topicMax) < MAX_TOPICS)
    {
        // Registering new ID
        int result = m_topicMax;
        m_topicsResolveMap.emplace(topic, result);
        m_topics[result] = topic;
        m_topicMax++;
        return result;
    }
    else
    {
        // Array for topic descriptors is full
        return -2;
    }
}

std::string EventQueue::GetTopicByID(int id)
{
    std::string result;

    // The registry is mutated by RegisterTopic() from any thread - guard the access
    std::lock_guard<std::mutex> lock(m_mutexTopics);

    if (id > 0 && static_cast<unsigned>(id) < MAX_TOPICS)
    {
        result = m_topics[id];
    }

    return result;
}

void EventQueue::ClearTopics()
{
    {
        std::lock_guard<std::mutex> lock(m_mutexObservers);
        for (unsigned i = 0; i < MAX_TOPICS; ++i)
        {
            m_topicSnapshots[i].reset();
        }
    }

    std::lock_guard<std::mutex> lock(m_mutexTopics);

    m_topicsResolveMap.clear();
    for (auto& topic : m_topics)
    {
        topic.clear();
    }
    m_topicMax = 0;
}

void EventQueue::SetMaxQueueSize(size_t maxQueueSize)
{
    m_maxQueueSize.store(maxQueueSize, std::memory_order_release);
}

size_t EventQueue::GetMaxQueueSize() const
{
    return m_maxQueueSize.load(std::memory_order_acquire);
}

size_t EventQueue::GetQueueSize()
{
    std::lock_guard<std::mutex> lock(m_mutexMessages);
    return m_messageQueue.size();
}

bool EventQueue::Post(int id, MessagePayload* obj, bool autoCleanupPayload)
{
    if (id < 0)
    {
        if (autoCleanupPayload && obj != nullptr)
        {
            delete obj;
        }
        return false;
    }

    const size_t maxQueueSize = m_maxQueueSize.load(std::memory_order_relaxed);
    Message* message = new Message(id, obj, autoCleanupPayload);
    bool rejected = false;
    {
        std::lock_guard<std::mutex> lock(m_mutexMessages);
        if (maxQueueSize > 0 && m_messageQueue.size() >= maxQueueSize)
        {
            // Reject outside the lock to avoid holding m_mutexMessages during delete
            rejected = true;
        }
        else
        {
            m_messageQueue.push_back(message);
            m_pendingMessages.store(true, std::memory_order_release);
        }
    }

    if (rejected)
    {
        if (autoCleanupPayload && obj != nullptr)
        {
            delete obj;
        }
        delete message;
        return false;
    }

    // The worker sets m_workerSleeping under m_mutexMessages before blocking, so a push
    // made after it blocked is guaranteed to observe the flag (no lost wakeup)
    if (m_workerSleeping.load(std::memory_order_acquire))
    {
        m_cvEvents.notify_one();
    }
    return true;
}

bool EventQueue::Post(std::string topic, MessagePayload* obj, bool autoCleanupPayload)
{
    int id = ResolveTopic(topic);
    return Post(id, obj, autoCleanupPayload);
}

size_t EventQueue::PostBatch(int id, MessagePayload* const* objs, size_t count, bool autoCleanupPayload)
{
    if (count == 0)
    {
        return 0;
    }

    if (id < 0)
    {
        if (autoCleanupPayload && objs != nullptr)
        {
            for (size_t i = 0; i < count; ++i)
            {
                delete objs[i];
            }
        }
        return 0;
    }

    constexpr size_t CHUNK = 256;
    Message* local[CHUNK];
    const size_t maxQueueSize = m_maxQueueSize.load(std::memory_order_relaxed);
    size_t done = 0;
    size_t enqueued = 0;

    while (done < count)
    {
        const size_t n = std::min<size_t>(count - done, CHUNK);
        for (size_t i = 0; i < n; ++i)
        {
            MessagePayload* payload = (objs != nullptr) ? objs[done + i] : nullptr;
            local[i] = new Message(id, payload, autoCleanupPayload);
        }

        size_t toPush = n;
        {
            std::lock_guard<std::mutex> lock(m_mutexMessages);
            if (maxQueueSize > 0)
            {
                const size_t cur = m_messageQueue.size();
                const size_t avail = (cur < maxQueueSize) ? (maxQueueSize - cur) : 0;
                if (toPush > avail)
                {
                    toPush = avail;
                }
            }
            if (toPush > 0)
            {
                m_messageQueue.insert(m_messageQueue.end(), local, local + toPush);
                m_pendingMessages.store(true, std::memory_order_release);
            }
        }

        enqueued += toPush;

        if (toPush < n)
        {
            // Clean up unpushed messages from this chunk plus any remaining chunks
            for (size_t i = toPush; i < n; ++i)
            {
                if (autoCleanupPayload && local[i]->obj != nullptr)
                {
                    delete local[i]->obj;
                }
                delete local[i];
            }
            if (autoCleanupPayload && objs != nullptr)
            {
                for (size_t i = done + n; i < count; ++i)
                {
                    delete objs[i];
                }
            }
            break;
        }

        done += n;
    }

    if (enqueued > 0 && m_workerSleeping.load(std::memory_order_acquire))
    {
        m_cvEvents.notify_one();
    }
    return enqueued;
}

size_t EventQueue::PostBatch(const std::string& topic, MessagePayload* const* objs, size_t count, bool autoCleanupPayload)
{
    int id = ResolveTopic(topic);
    return PostBatch(id, objs, count, autoCleanupPayload);
}

// Lookup for observer list for topic with <id>
// Returns: ObserverVectorPtr aka vector<Observer*>
ObserverVectorPtr EventQueue::GetObservers(int id)
{
    if (id >= 0 && static_cast<unsigned>(id) < MAX_TOPICS)
    {
        return m_topicObserversTable[id];
    }

    return nullptr;
}

// Retrieve topmost message in Message Queue
Message* EventQueue::GetQueueMessage()
{
    Message* result = nullptr;

    std::lock_guard<std::mutex> lock(m_mutexMessages);
    if (!m_messageQueue.empty())
    {
        result = m_messageQueue.front();
        m_messageQueue.pop_front();
    }

    return result;
}

namespace
{
    // Tracks EventQueue instances currently dispatching on this thread;
    // removals issued from a handler on the same queue must not wait for their own dispatch to complete
    thread_local std::vector<const EventQueue*> tl_activeQueues;
}

// Dispatch message to all subscribers of the topic
void EventQueue::Dispatch(int id, Message* message)
{
    if (message == nullptr)
        return;

    std::shared_ptr<const std::vector<ObserverDescriptor>> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutexObservers);

        if (id >= 0 && static_cast<unsigned>(id) < MAX_TOPICS)
        {
            if (!m_topicSnapshots[id])
            {
                auto snap = std::make_shared<std::vector<ObserverDescriptor>>();
                ObserverVectorPtr observers = GetObservers(id);
                if (observers != nullptr)
                {
                    snap->reserve(observers->size());
                    for (const ObserverDescriptor* observer : *observers)
                    {
                        if (observer != nullptr)
                        {
                            snap->push_back(*observer);
                        }
                    }
                }
                m_topicSnapshots[id] = snap;
            }
            snapshot = m_topicSnapshots[id];
        }

        MarkDispatchActive();
    }

    if (snapshot)
    {
        for (const ObserverDescriptor& observer : *snapshot)
        {
            if (observer.active && !observer.active->load(std::memory_order_acquire))
            {
                continue;
            }

            if (observer.callback != nullptr)
            {
                (*observer.callback)(id, message);
            }
            else if (observer.callbackMethod != nullptr && observer.observerInstance != nullptr)
            {
                ObserverCallbackMethod callbackMethod = observer.callbackMethod;
                (observer.observerInstance->*callbackMethod)(id, message);
            }
            else if (observer.callbackFunc != nullptr)
            {
                (observer.callbackFunc)(id, message);
            }
        }
    }

    MarkDispatchComplete();

    // Cleanup message when delivered
    if (message)
    {
        if (message->cleanupPayload && message->obj)
        {
            delete message->obj;
        }

        delete message;
    }
}

void EventQueue::MarkDispatchActive()
{
    m_activeDispatches.fetch_add(1, std::memory_order_seq_cst);
    tl_activeQueues.push_back(this);
}

void EventQueue::MarkDispatchComplete()
{
    for (auto it = tl_activeQueues.rbegin(); it != tl_activeQueues.rend(); ++it)
    {
        if (*it == this)
        {
            tl_activeQueues.erase(std::next(it).base());
            break;
        }
    }

    if (m_activeDispatches.fetch_sub(1, std::memory_order_seq_cst) == 1 &&
        m_waitingRemovers.load(std::memory_order_seq_cst) > 0)
    {
        std::lock_guard<std::mutex> lock(m_mutexDispatches);
        m_cvDispatches.notify_all();
    }
}

void EventQueue::WaitForDispatchesToComplete()
{
    // RemoveObserver() contract: once it returns, no removed handler is
    // running and none will start later - Dispatch() takes its snapshot under
    // the observers mutex, so any dispatch that already snapshotted the
    // removed descriptor is still mid-invocation here. Callers rely on this
    // to destroy handler state (stack locals, observer instances) right
    // after removal.
    for (const EventQueue* q : tl_activeQueues)
    {
        if (q == this)
        {
            // A handler removing an observer from this queue must not wait for its own dispatch
            return;
        }
    }

    if (m_activeDispatches.load(std::memory_order_seq_cst) == 0)
    {
        return;
    }

    m_waitingRemovers.fetch_add(1, std::memory_order_seq_cst);
    {
        std::unique_lock<std::mutex> lock(m_mutexDispatches);
        m_cvDispatches.wait(lock, [this]() {
            return m_activeDispatches.load(std::memory_order_seq_cst) == 0;
        });
    }
    m_waitingRemovers.fetch_sub(1, std::memory_order_seq_cst);
}

#ifdef _DEBUG

#include <sstream>

std::string EventQueue::DumpTopics()
{
    std::string result;
    std::stringstream ss;

    ss << "Topics map contains: " << m_topicsResolveMap.size();
    if (m_topicsResolveMap.size() > 0)
        ss << std::endl;

    for (auto topic : m_topicsResolveMap)
    {
        ss << "  tid: " << topic.second << "; topic:'" << topic.first << '\'';
        ss << std::endl;
    }

    if (m_topicsResolveMap.size() > 0)
        ss << std::endl;

    result = ss.str();
    return result;
}
std::string EventQueue::DumpObservers()
{
    std::string result;
    std::stringstream ss;

    ss << "Observers registered for: " << m_topicObservers.size() << " topics";
    if (m_topicsResolveMap.size() > 0)
        ss << std::endl;

    for (auto topic : m_topicObservers)
    {
        ss << "tid:" << topic.first;
        if (topic.second != nullptr)
        {
            ss << " has " << topic.second->size() << " observers" << std::endl;

            int callbackCounter = 0;
            for (auto observer : *topic.second)
            {
                ss << "  [" << callbackCounter << "] ";

                if (observer->callback != nullptr)
                {
                    using namespace mc_function_display;
                    ss << "callback: " <<  observer->callback << std::endl;
                }
                else if (observer->callbackFunc != nullptr)
                {
                    using namespace mc_lambda_display;
                    ss << "callbackFunc: " << observer->callbackFunc << std::endl;
                }
                else if (observer->callbackMethod != nullptr)
                {
                    using namespace mc_class_method_display;
                    ss << "callbackMethod: " << observer->callbackMethod << std::endl;
                }
                else
                {
                    ss << "No callbacks registered" << std::endl;
                }

                callbackCounter++;
            }
        }
        else
        {
            ss << " has no observers" << std::endl;
        }
    }

    if (m_topicsResolveMap.size() > 0)
        ss << std::endl;

    result = ss.str();
    return result;
}

std::string EventQueue::DumpMessageQueue()
{
    // Lock parallel threads to access (unique_lock allows arbitrary lock/unlock)
    std::lock_guard<std::mutex> lock(m_mutexMessages);

    return DumpMessageQueueNoLock();
}

std::string EventQueue::DumpMessageQueueNoLock()
{
    std::string result;

    std::stringstream ss;

    ss << "Message queue contains: " << m_messageQueue.size() << " messages";
    if (m_messageQueue.size() > 0)
        ss << std::endl;

    for (auto message : m_messageQueue)
    {
        ss << "  tid: " << message->tid << "; obj*: " << message->obj;
        ss << std::endl;
    }

    if (m_messageQueue.size() > 0)
        ss << std::endl;

    result = ss.str();
    return result;
}

#endif // _DEBUG