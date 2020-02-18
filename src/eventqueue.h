#pragma once

#ifndef MESSAGE_CENTER_EVENTQUEUE_H
#define MESSAGE_CENTER_EVENTQUEUE_H

#include "collectionhelper.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>

// MAX_TOPICS - By default we're allocating descriptor table for 1024 topics
// Override example:
// auto queue = new EventQueue<512>(); - That will force to allocate only 512 entries descriptor table
constexpr unsigned MAX_TOPICS = 1024;

struct TopicDescriptor
{
public:
    unsigned tid;
    std::string topic;
};

//typedef std::function<
typedef std::map<std::string, int> TopicResolveMap;
typedef std::pair<std::string, int> TopicResolveRecord;

class EventQueue
{
// Synchronization primitives
protected:
    std::atomic<bool> m_initialized;
    std::mutex m_mutexObservers;

    std::mutex m_mutexEvents;
    std::condition_variable m_cvEvents;

// Fields
protected:
    //TopicDescriptor m_topics[MAX_TOPICS];
    TopicResolveMap m_topicsResolveMap;
    int m_topicMax = 0;

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
    int AddObserver(std::string& topic);

    int ResolveTopic(std::string& topic);
    int RegisterTopic(std::string& topic);
    void ClearTopics();
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
    //using EventQueue::m_topics;
    using EventQueue::m_topicsResolveMap;
    using EventQueue::m_topicMax;
};
#endif // _CODE_UNDER_TEST

#endif // MESSAGE_CENTER_EVENTQUEUE_H
