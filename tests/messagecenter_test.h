#ifndef MESSAGE_CENTER_MESSAGECENTER_TEST_H
#define MESSAGE_CENTER_MESSAGECENTER_TEST_H

#include "gtest/gtest.h"
#include "eventqueue.h"
#include <chrono>
#include <ctime>
#include <thread>
#include <messagecenter.h>


class MessageCenter_test : public ::testing::Test
{
protected:
    void SetUp() override;
    void TearDown() override;

    void FillQueue(MessageCenterCUT& queue)
    {
        static char buffer[200];
        static const int TOPIC_COUNT = 10;
        static const int OBSERVERS_COUNT = 5;

        for (int topics = 0; topics < TOPIC_COUNT; topics++)
        {
            snprintf(buffer, sizeof(buffer), "topic_%03d", topics);
            std::string topic(buffer);

            for (int i = 0; i < OBSERVERS_COUNT; i++)
            {
                ObserverCallbackFunc callback = [=](int id, Message *message)
                {
#ifdef _DEBUG
                    std::cout << "  Observer: " << i << " for topic: " << topics << " works" << std::endl;
#endif // _DEBUG

                };

                queue.AddObserver(topic, callback);
            }

            int topicID = queue.ResolveTopic(topic);
            size_t uniqueObservers = queue.GetObservers(topicID)->size();
        }

        size_t uniqueTopics = queue.m_topicObservers.size();

        for (int i = 0; i < TOPIC_COUNT; i++)
        {
            queue.Post(i, nullptr);
        }
        int messageNumber = queue.m_messageQueue.size();

#ifdef _DEBUG
        std::cout << queue.DumpTopics();
        std::cout << queue.DumpObservers();
        std::cout << queue.DumpMessageQueue();
#endif // _DEBUG
    }

    void sleep_ms(uint32_t ms)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }

    void sleep_us(uint32_t us)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(us));
    }
};


#endif //MESSAGE_CENTER_MESSAGECENTER_TEST_H
