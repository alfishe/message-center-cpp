#include "eventqueue.h"


EventQueue::EventQueue()
{
    init();
}

EventQueue::~EventQueue()
{
    if (m_initialized)
    {
        dispose();
    }
}

bool EventQueue::init()
{
    bool result = false;

    m_initialized = true;

    return result;
}

void EventQueue::dispose()
{
    //top();
}

int EventQueue::AddObserver(std::string& topic)
{
    int result = RegisterTopic(topic);

    if (result >= 0)
    {

    }

    return result;
}

int EventQueue::ResolveTopic(std::string& topic)
{
    int result = -1;

    if (topic.length() > 0)
    {
        if (key_exists(m_topicsResolveMap, topic))
        {
            result = m_topicsResolveMap[topic];
        }
    }

    return result;
}

int EventQueue::RegisterTopic(std::string& topic)
{
    int result = -1;

    if (topic.length() > 0)
    {
        if (key_exists(m_topicsResolveMap, topic))
        {
            // Already registered. Returning it's ID
            result = m_topicsResolveMap[topic];
        }
        else
        {
            if (m_topicMax < MAX_TOPICS)
            {
                // Registering new ID
                m_topicsResolveMap.insert({ topic, m_topicMax });

                result = m_topicMax;
                m_topicMax++;
            }
            else
            {
                // Array for topic descriptors is full
                result = -2;
            }
        }
    }

    return result;
}

void EventQueue::ClearTopics()
{
    m_topicsResolveMap.clear();
    m_topicMax = 0;
}