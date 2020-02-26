#ifndef MESSAGE_CENTER_EVENTQUEUE_BENCHMARK_H
#define MESSAGE_CENTER_EVENTQUEUE_BENCHMARK_H

#include "eventqueue.h"
#include <thread>

void FillQueue(EventQueue& queue)
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
    }

    for (int i = 0; i < TOPIC_COUNT; i++)
    {
        queue.Post(i, nullptr);
    }
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

#endif //MESSAGE_CENTER_EVENTQUEUE_BENCHMARK_H
