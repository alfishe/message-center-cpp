#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_H
#define MESSAGE_CENTER_EVENTQUEUE_H

#include "collectionhelper.h"

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

// MAX_TOPICS - By default we're allocating descriptor table for 1024 topics
// Override example:
// auto queue = new EventQueue<512>(); - That will force to allocate only 512 entries descriptor table
constexpr unsigned MAX_TOPICS = 1024;

struct Observer;
struct Message;
typedef void (ObserverCallback)(int id, Message* message);
typedef void (Observer::* ObserverCallbackMethod)(int id, Message* message);
typedef std::function<void(int id, Message* message)> ObserverCallbackFunc;
struct ObserverDescriptor
{
    ObserverCallback* callback;
    ObserverCallbackMethod callbackMethod;
    ObserverCallbackFunc callbackFunc;
};

// Topic types
typedef std::map<std::string, int> TopicResolveMap;
typedef std::pair<std::string, int> TopicResolveRecord;

// Observer types
typedef std::vector<ObserverDescriptor*> ObserversVector;
typedef ObserversVector* ObserverVectorPtr;
typedef std::map<int, ObserverVectorPtr> TopicObserversMap;

// Message types
struct Message
{
public:
    unsigned tid;
    void* obj;

    Message(unsigned tid, void* obj = nullptr)
    {
        this->tid = tid;
        this->obj = obj;
    }
};
typedef std::deque<Message*> MessageQueue;

class EventQueue
{
// Synchronization primitives
protected:
    std::atomic<bool> m_initialized;
    std::mutex m_mutexObservers;

    std::mutex m_mutexMessages;
    std::condition_variable m_cvEvents;

// Fields
protected:
    std::string m_topics[MAX_TOPICS];
    TopicResolveMap m_topicsResolveMap;
    int m_topicMax = 0;

    TopicObserversMap m_topicObservers;

    MessageQueue m_messageQueue;

// Class methods
public:
    EventQueue();
    virtual ~EventQueue();
    EventQueue(const EventQueue& that) = delete; 			// Disable copy constructor. C++11 feature
    EventQueue& operator =(EventQueue const&) = delete;		// Disable assignment operator. C++11 feature

// Initialization
public:
    bool init();
    void dispose();

// Public methods
public:
    int AddObserver(std::string& topic, ObserverCallback callback);
    int AddObserver(std::string& topic, ObserverCallbackMethod callback);
    int AddObserver(std::string& topic, ObserverCallbackFunc callback);
    int AddObserver(std::string& topic, ObserverDescriptor* observer);

    int ResolveTopic(std::string& topic);
    int RegisterTopic(std::string& topic);
    std::string GetTopicByID(int id);
    void ClearTopics();

    void Post(int id, void* obj = nullptr);
    void Post(std::string topic, void* obj = nullptr);

protected:
    void Get();
    void Dispatch(int id);

    ObserverVectorPtr GetObservers(int id);

#ifdef _DEBUG
    // Debug helpers
public:
    std::string DumpTopics();
    std::string DumpObservers();
    std::string DumpMessageQueue();
    std::string DumpMessageQueueNoLock();
#endif // _DEBUG
};

//
// Code Under Test (CUT) wrapper to allow access to protected and private properties and methods for unit testing / benchmark purposes
//
#ifdef _CODE_UNDER_TEST

class EventQueueCUT : public EventQueue
{
public:
    EventQueueCUT() : EventQueue() {};

public:
    using EventQueue::m_topicsResolveMap;
    using EventQueue::m_topicMax;

    using EventQueue::m_topicObservers;

    using EventQueue::m_messageQueue;

    using EventQueue::GetObservers;
};
#endif // _CODE_UNDER_TEST

#endif // MESSAGE_CENTER_EVENTQUEUE_H
