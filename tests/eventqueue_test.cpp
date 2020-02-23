#include "eventqueue_test.h"

#include <cstdio>

void EventQueue_Test::SetUp()
{

}

void EventQueue_Test::TearDown()
{

}

TEST_F(EventQueue_Test, TopicResolve_Capacity)
{
    EventQueueCUT queue;

    for (int i = 0; i < MAX_TOPICS + 2; i++)
    {
        std::stringstream ss;
        ss << "topic_" << i;
        std::string topic = ss.str();

        int id = queue.RegisterTopic(topic);
        if (id < 0 && i < MAX_TOPICS)
        {
            FAIL() << "ID was not assigned";
        }
        else if (id < 0 && i >= MAX_TOPICS)
        {
            EXPECT_EQ(id, -2);

            EXPECT_EQ(queue.m_topicsResolveMap.size(), MAX_TOPICS);
        }
    }
}

TEST_F(EventQueue_Test, TopicResolve_Lookup)
{
    EventQueueCUT queue;

    // Populate the map
    for (int i = 0; i < MAX_TOPICS + 2; i++)
    {
        std::stringstream ss;
        ss << "topic_" << i;
        std::string topic = ss.str();

        int id = queue.RegisterTopic(topic);
        if (id < 0 && i < MAX_TOPICS)
        {
            FAIL() << "ID was not assigned";
        }
        else if (id < 0 && i >= MAX_TOPICS)
        {
            EXPECT_EQ(id, -2);

            EXPECT_EQ(queue.m_topicsResolveMap.size(), MAX_TOPICS);
        }
    }

    // Test lookup
    for (int i = 0; i < MAX_TOPICS * 2; i++)
    {
        std::stringstream ss;
        ss << "topic_" << i;
        std::string topic = ss.str();

        int id = queue.ResolveTopic(topic);
        if (i < MAX_TOPICS)
        {
            // All valid IDs within resolve array capacity should be found
            EXPECT_EQ(id, i);
        }
        else
        {
            // All others - not
            EXPECT_EQ(id, -1);
        }
    }

    // Few special cases
    std::string topic = "";
    int id = queue.ResolveTopic(topic);
    EXPECT_EQ(id, -1);

    topic = "01234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789";
    id = queue.ResolveTopic(topic);
    EXPECT_EQ(id, -1);
}

TEST_F(EventQueue_Test, RegisterSingleObserver)
{
    ObserverCallbackFunc callback = [=](int id, Message* message)
    {
        std::cout << "It works!";
    };

    EventQueueCUT queue;
    std::string topic = "test";
    queue.AddObserver(topic, callback);
}

TEST_F(EventQueue_Test, Register_10k_ObserversForSingleTopic)
{
    ObserverCallbackFunc callback = [=](int id, Message* message)
    {
        std::cout << "It works!";
    };

    EventQueueCUT queue;
    std::string topic = "test";
    for (int i = 0; i < 10000; i++)
    {
        queue.AddObserver(topic, callback);
    }
}

TEST_F(EventQueue_Test, Register_1k_ObserversFor_100_Topics)
{
    static char buffer[200];
    static const int TOPIC_COUNT = 100;
    static const int OBSERVERS_COUNT = 1000;

    ObserverCallbackFunc callback = [=](int id, Message* message)
    {
        std::cout << "It works!";
    };

    EventQueueCUT queue;
    for (int topics = 0; topics < TOPIC_COUNT; topics ++)
    {
        snprintf(buffer, sizeof(buffer), "topic_%03d\0", topics);
        std::string topic(buffer);

        for (int i = 0; i < OBSERVERS_COUNT; i++)
        {
            queue.AddObserver(topic, callback);
        }

        int topicID = queue.ResolveTopic(topic);
        EXPECT_GE(topicID, 0);
        size_t uniqueObservers = queue.GetObservers(topicID)->size();
        EXPECT_EQ(uniqueObservers, OBSERVERS_COUNT);
    }

    size_t uniqueTopics = queue.m_topicObservers.size();
    EXPECT_EQ(uniqueTopics, TOPIC_COUNT);
}

void TestObservers_Callback_callback(int id, Message* message)
{
#ifdef _DEBUG
    std::cout << "  Callback observer for topic tid: " << id << " works" << std::endl;
#endif // _DEBUG
}

TEST_F(EventQueue_Test, TestObservers_Callback)
{
    static char buffer[200];
    static const int TOPIC_COUNT = 10;
    static const int OBSERVERS_COUNT = 5;

    EventQueueCUT queue;
    for (int topics = 0; topics < TOPIC_COUNT; topics ++)
    {
        snprintf(buffer, sizeof(buffer), "topic_%03d\0", topics);
        std::string topic(buffer);

        for (int i = 0; i < OBSERVERS_COUNT; i++)
        {
            queue.AddObserver(topic, TestObservers_Callback_callback);
        }

        int topicID = queue.ResolveTopic(topic);
        EXPECT_GE(topicID, 0);
        size_t uniqueObservers = queue.GetObservers(topicID)->size();
        EXPECT_EQ(uniqueObservers, OBSERVERS_COUNT);
    }

    size_t uniqueTopics = queue.m_topicObservers.size();
    EXPECT_EQ(uniqueTopics, TOPIC_COUNT);

    for (int i = 0; i < TOPIC_COUNT; i++)
    {
        queue.Post(i, nullptr);
    }
    int messageNumber = queue.m_messageQueue.size();
    EXPECT_EQ(messageNumber, TOPIC_COUNT);

#ifdef _DEBUG
    std::cout << queue.DumpTopics();
    std::cout << queue.DumpObservers();
    std::cout << queue.DumpMessageQueue();
#endif // _DEBUG

    for (int i = 0; i < queue.m_messageQueue.size(); i++)
    {
        int messagesBeforeGet = queue.m_messageQueue.size();
        Message* message = queue.GetMessage();
        int messagesAfterGet = queue.m_messageQueue.size();

        EXPECT_NE(message, nullptr);
        EXPECT_EQ(messagesAfterGet + 1, messagesBeforeGet);

#ifdef _DEBUG
        std::cout << "[" << i << "] " << "Starting dispatch message for tid:" << message->tid << std::endl;
#endif // _DEBUG

        queue.Dispatch(message->tid, message);

#ifdef _DEBUG
        std::cout << "End of dispatch message for tid:" << message->tid << std::endl;
#endif // _DEBUG
    }
}


class TestObservers_ClassMethod_class : public Observer
{
public:
    void ObserverTestMethod(int id, Message* message)
    {
#ifdef _DEBUG
        std::cout << "  Class method observer for topic tid: " << id << " works" << std::endl;
#endif // _DEBUG
    }
};
TEST_F(EventQueue_Test, TestObservers_ClassMethod)
{
    static char buffer[200];
    static const int TOPIC_COUNT = 10;
    static const int OBSERVERS_COUNT = 5;

    TestObservers_ClassMethod_class observerDerivedInstance;

    EventQueueCUT queue;
    for (int topics = 0; topics < TOPIC_COUNT; topics ++)
    {
        snprintf(buffer, sizeof(buffer), "topic_%03d\0", topics);
        std::string topic(buffer);

        for (int i = 0; i < OBSERVERS_COUNT; i++)
        {
            Observer* observerInstance = static_cast<Observer*>(&observerDerivedInstance);
            ObserverCallbackMethod callback = static_cast<ObserverCallbackMethod>(&TestObservers_ClassMethod_class::ObserverTestMethod);
            queue.AddObserver(topic, observerInstance, callback);
        }

        int topicID = queue.ResolveTopic(topic);
        EXPECT_GE(topicID, 0);
        size_t uniqueObservers = queue.GetObservers(topicID)->size();
        EXPECT_EQ(uniqueObservers, OBSERVERS_COUNT);
    }

    size_t uniqueTopics = queue.m_topicObservers.size();
    EXPECT_EQ(uniqueTopics, TOPIC_COUNT);

    for (int i = 0; i < TOPIC_COUNT; i++)
    {
        queue.Post(i, nullptr);
    }
    int messageNumber = queue.m_messageQueue.size();
    EXPECT_EQ(messageNumber, TOPIC_COUNT);

#ifdef _DEBUG
    std::cout << queue.DumpTopics();
    std::cout << queue.DumpObservers();
    std::cout << queue.DumpMessageQueue();
#endif // _DEBUG

    for (int i = 0; i < queue.m_messageQueue.size(); i++)
    {
        Message* message = queue.GetMessage();
        EXPECT_NE(message, nullptr);

#ifdef _DEBUG
        std::cout << "[" << i << "] " << "Starting dispatch message for tid:" << message->tid << std::endl;
#endif // _DEBUG

        queue.Dispatch(message->tid, message);

#ifdef _DEBUG
        std::cout << "End of dispatch message for tid:" << message->tid << std::endl;
#endif // _DEBUG
    }
}

TEST_F(EventQueue_Test, TestObservers_Lambda)
{
    static char buffer[200];
    static const int TOPIC_COUNT = 10;
    static const int OBSERVERS_COUNT = 5;

    EventQueueCUT queue;
    for (int topics = 0; topics < TOPIC_COUNT; topics ++)
    {
        snprintf(buffer, sizeof(buffer), "topic_%03d\0", topics);
        std::string topic(buffer);

        for (int i = 0; i < OBSERVERS_COUNT; i++)
        {
            ObserverCallbackFunc callback = [=](int id, Message* message)
            {
#ifdef _DEBUG
                std::cout << "  Observer: " << i << " for topic: " << topics << " works" << std::endl;
#endif // _DEBUG

            };

            queue.AddObserver(topic, callback);
        }

        int topicID = queue.ResolveTopic(topic);
        EXPECT_GE(topicID, 0);
        size_t uniqueObservers = queue.GetObservers(topicID)->size();
        EXPECT_EQ(uniqueObservers, OBSERVERS_COUNT);
    }

    size_t uniqueTopics = queue.m_topicObservers.size();
    EXPECT_EQ(uniqueTopics, TOPIC_COUNT);

    for (int i = 0; i < TOPIC_COUNT; i++)
    {
        queue.Post(i, nullptr);
    }
    int messageNumber = queue.m_messageQueue.size();
    EXPECT_EQ(messageNumber, TOPIC_COUNT);

#ifdef _DEBUG
    std::cout << queue.DumpTopics();
    std::cout << queue.DumpObservers();
    std::cout << queue.DumpMessageQueue();
#endif // _DEBUG

    for (int i = 0; i < queue.m_messageQueue.size(); i++)
    {
        Message* message = queue.GetMessage();
        EXPECT_NE(message, nullptr);

#ifdef _DEBUG
        std::cout << "[" << i << "] " << "Starting dispatch message for tid:" << message->tid << std::endl;
#endif // _DEBUG

        queue.Dispatch(message->tid, message);

#ifdef _DEBUG
        std::cout << "End of dispatch message for tid:" << message->tid << std::endl;
#endif // _DEBUG
    }
}

