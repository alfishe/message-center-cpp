#include "eventqueue_test.h"

#include <cstdio>
#include <array>

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
        snprintf(buffer, sizeof(buffer), "topic_%03d", topics);
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
        snprintf(buffer, sizeof(buffer), "topic_%03d", topics);
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
        Message* message = queue.GetQueueMessage();
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

    // Test observers removal
    for (int topics = 0; topics < TOPIC_COUNT; topics ++)
    {
        snprintf(buffer, sizeof(buffer), "topic_%03d", topics);
        std::string topic(buffer);
        int topicID = queue.ResolveTopic(topic);

        size_t observersBeforeDeletion = queue.GetObservers(topicID)->size();
        EXPECT_GE(observersBeforeDeletion, 0);

        queue.RemoveObserver(topic, TestObservers_Callback_callback);

        EXPECT_GE(topicID, 0);
        size_t uniqueObservers = queue.GetObservers(topicID)->size();
        EXPECT_EQ(uniqueObservers, 0);
    }
}

// Test that capturing lambdas (which exceed libc++'s small buffer optimization)
// can be reliably removed using observer IDs. This was broken before the fix
// because the getTargetAddress trick reads the first word of std::function,
// which points to different heap addresses for each lambda copy.
TEST_F(EventQueue_Test, TestObservers_CapturingLambda_IdBasedRemoval)
{
    EventQueueCUT queue;
    std::string topic = "test_capturing_lambda";

    // Create a capturing lambda that exceeds the small buffer optimization
    // (libc++ inline buffer is ~24 bytes, this capture is larger)
    std::array<uint64_t, 8> largeCapture = {1, 2, 3, 4, 5, 6, 7, 8};
    int callCount = 0;

    auto capturingLambda = [largeCapture, &callCount](int id, Message* message) {
        callCount++;
        // Use the capture to prevent optimization
        (void)largeCapture[0];
    };

    // Add the observer and save its ID
    uint64_t observerId = queue.AddObserver(topic, capturingLambda);
    EXPECT_NE(observerId, 0u);

    int topicID = queue.ResolveTopic(topic);
    EXPECT_GE(topicID, 0);
    EXPECT_EQ(queue.GetObservers(topicID)->size(), 1u);

    // Post a message and verify the observer was called
    queue.Post(topic);
    Message* message = queue.GetQueueMessage();
    ASSERT_NE(message, nullptr);
    queue.Dispatch(message->tid, message);
    EXPECT_EQ(callCount, 1);

    // Remove by ID (the only reliable way for capturing lambdas)
    queue.RemoveObserverById(topic, observerId);
    EXPECT_EQ(queue.GetObservers(topicID)->size(), 0u);

    // Verify removal worked - posting should not call the lambda
    queue.Post(topic);
    message = queue.GetQueueMessage();
    ASSERT_NE(message, nullptr);
    queue.Dispatch(message->tid, message);
    EXPECT_EQ(callCount, 1);  // Still 1, not incremented
}

// Test that multiple capturing lambdas can be added and removed independently
TEST_F(EventQueue_Test, TestObservers_MultipleCapturingLambdas)
{
    EventQueueCUT queue;
    std::string topic = "test_multiple_capturing";

    std::array<uint64_t, 8> capture1 = {1, 1, 1, 1, 1, 1, 1, 1};
    std::array<uint64_t, 8> capture2 = {2, 2, 2, 2, 2, 2, 2, 2};
    std::array<uint64_t, 8> capture3 = {3, 3, 3, 3, 3, 3, 3, 3};

    int count1 = 0, count2 = 0, count3 = 0;

    uint64_t id1 = queue.AddObserver(topic, [capture1, &count1](int, Message*) {
        count1++;
        (void)capture1[0];
    });

    uint64_t id2 = queue.AddObserver(topic, [capture2, &count2](int, Message*) {
        count2++;
        (void)capture2[0];
    });

    uint64_t id3 = queue.AddObserver(topic, [capture3, &count3](int, Message*) {
        count3++;
        (void)capture3[0];
    });

    EXPECT_NE(id1, id2);
    EXPECT_NE(id2, id3);
    EXPECT_NE(id1, id3);

    int topicID = queue.ResolveTopic(topic);
    EXPECT_EQ(queue.GetObservers(topicID)->size(), 3u);

    // Dispatch and verify all three called
    queue.Post(topic);
    Message* message = queue.GetQueueMessage();
    queue.Dispatch(message->tid, message);
    EXPECT_EQ(count1, 1);
    EXPECT_EQ(count2, 1);
    EXPECT_EQ(count3, 1);

    // Remove the middle one
    queue.RemoveObserverById(topic, id2);
    EXPECT_EQ(queue.GetObservers(topicID)->size(), 2u);

    // Dispatch again - only 1 and 3 should be called
    queue.Post(topic);
    message = queue.GetQueueMessage();
    queue.Dispatch(message->tid, message);
    EXPECT_EQ(count1, 2);
    EXPECT_EQ(count2, 1);  // Not called
    EXPECT_EQ(count3, 2);

    // Remove remaining observers
    queue.RemoveObserverById(topic, id1);
    queue.RemoveObserverById(topic, id3);
    EXPECT_EQ(queue.GetObservers(topicID)->size(), 0u);
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
        snprintf(buffer, sizeof(buffer), "topic_%03d", topics);
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
        Message* message = queue.GetQueueMessage();
        EXPECT_NE(message, nullptr);

#ifdef _DEBUG
        std::cout << "[" << i << "] " << "Starting dispatch message for tid:" << message->tid << std::endl;
#endif // _DEBUG

        queue.Dispatch(message->tid, message);

#ifdef _DEBUG
        std::cout << "End of dispatch message for tid:" << message->tid << std::endl;
#endif // _DEBUG
    }

    // Test observers removal
    for (int topics = 0; topics < TOPIC_COUNT; topics ++)
    {
        snprintf(buffer, sizeof(buffer), "topic_%03d", topics);
        std::string topic(buffer);
        int topicID = queue.ResolveTopic(topic);

        size_t observersBeforeDeletion = queue.GetObservers(topicID)->size();
        EXPECT_GE(observersBeforeDeletion, 0);


        Observer* observerInstance = static_cast<Observer*>(&observerDerivedInstance);
        ObserverCallbackMethod callback = static_cast<ObserverCallbackMethod>(&TestObservers_ClassMethod_class::ObserverTestMethod);
        queue.RemoveObserver(topic, observerInstance, callback);

        EXPECT_GE(topicID, 0);
        size_t uniqueObservers = queue.GetObservers(topicID)->size();
        EXPECT_EQ(uniqueObservers, 0);
    }
}

TEST_F(EventQueue_Test, TestObservers_Lambda)
{
    static char buffer[200];
    static const int TOPIC_COUNT = 10;
    static const int OBSERVERS_COUNT = 5;
    uint64_t observerIds[TOPIC_COUNT][OBSERVERS_COUNT];

    EventQueueCUT queue;
    for (int topics = 0; topics < TOPIC_COUNT; topics ++)
    {
        snprintf(buffer, sizeof(buffer), "topic_%03d", topics);
        std::string topic(buffer);

        for (int i = 0; i < OBSERVERS_COUNT; i++)
        {
            ObserverCallbackFunc callback = [=](int id, Message* message)
            {
#ifdef _DEBUG
                std::cout << "  Observer: " << i << " for topic: " << topics << " works" << std::endl;
#endif // _DEBUG

            };

            // Store observer ID for removal (lambdas can't be matched by address)
            observerIds[topics][i] = queue.AddObserver(topic, callback);
            EXPECT_NE(observerIds[topics][i], 0u);
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
        Message* message = queue.GetQueueMessage();
        EXPECT_NE(message, nullptr);

#ifdef _DEBUG
        std::cout << "[" << i << "] " << "Starting dispatch message for tid:" << message->tid << std::endl;
#endif // _DEBUG

        queue.Dispatch(message->tid, message);

#ifdef _DEBUG
        std::cout << "End of dispatch message for tid:" << message->tid << std::endl;
#endif // _DEBUG
    }

    // Test observers removal using IDs
    for (int topics = 0; topics < TOPIC_COUNT; topics ++)
    {
        snprintf(buffer, sizeof(buffer), "topic_%03d", topics);
        std::string topic(buffer);
        int topicID = queue.ResolveTopic(topic);

        for (int i = 0; i < OBSERVERS_COUNT; i++)
        {
            size_t observersBeforeDeletion = queue.GetObservers(topicID)->size();
            EXPECT_EQ(observersBeforeDeletion, static_cast<size_t>(OBSERVERS_COUNT - i));

            queue.RemoveObserverById(topic, observerIds[topics][i]);

            size_t observersAfterDeletion = queue.GetObservers(topicID)->size();
            EXPECT_EQ(observersAfterDeletion, static_cast<size_t>(OBSERVERS_COUNT - i - 1));
        }

        // Everything was cleared
        size_t uniqueObservers = queue.GetObservers(topicID)->size();
        EXPECT_EQ(uniqueObservers, 0);
    }
}

