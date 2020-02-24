#ifndef MESSAGE_CENTER_MESSAGECENTER_TEST_H
#define MESSAGE_CENTER_MESSAGECENTER_TEST_H

#include "gtest/gtest.h"
#include "eventqueue.h"
#include <chrono>
#include <ctime>
#include <thread>


class MessageCenter_test : public ::testing::Test
{
protected:
    void SetUp() override;
    void TearDown() override;

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
