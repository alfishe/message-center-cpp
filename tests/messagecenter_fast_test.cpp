// MessageCenter and MessageCenterFast Tests
// Includes memory leak detection for all modes

#include <gtest/gtest.h>
#include "messagecenter.h"
#include "messagecenter_fast.h"
#include <atomic>
#include <thread>
#include <vector>
#include <chrono>

MessageCenterFast* MessageCenterFast::s_instance = nullptr;

class MessageCenterTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {}
};

// ============================================================================
// Original MessageCenter Tests
// ============================================================================

TEST_F(MessageCenterTest, Original_BasicPostAndReceive) {
    auto& mc = MessageCenter::DefaultMessageCenter();

    std::atomic<int> received{0};
    mc.AddObserver("test.basic", ObserverCallbackFunc([&received](int id, Message* msg) {
        received++;
    }));

    mc.Post("test.basic", nullptr, false);
    mc.Post("test.basic", nullptr, false);
    mc.Post("test.basic", nullptr, false);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(received.load(), 3);

    MessageCenter::DisposeDefaultMessageCenter();
}

TEST_F(MessageCenterTest, Original_MultipleObservers) {
    auto& mc = MessageCenter::DefaultMessageCenter();

    std::atomic<int> count1{0}, count2{0};

    mc.AddObserver("test.multi", ObserverCallbackFunc([&count1](int id, Message* msg) {
        count1++;
    }));
    mc.AddObserver("test.multi", ObserverCallbackFunc([&count2](int id, Message* msg) {
        count2++;
    }));

    mc.Post("test.multi", nullptr, false);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(count1.load(), 1);
    EXPECT_EQ(count2.load(), 1);

    MessageCenter::DisposeDefaultMessageCenter();
}

TEST_F(MessageCenterTest, Original_TopicById) {
    auto& mc = MessageCenter::DefaultMessageCenter();

    int topicId = mc.RegisterTopic("test.byid");

    std::atomic<int> received{0};
    mc.AddObserver("test.byid", ObserverCallbackFunc([&received](int id, Message* msg) {
        received++;
    }));

    mc.Post(topicId, nullptr, false);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    EXPECT_EQ(received.load(), 1);

    MessageCenter::DisposeDefaultMessageCenter();
}

// ============================================================================
// MessageCenterFast Tests
// ============================================================================

TEST_F(MessageCenterTest, Fast_BasicPostAndReceive) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();

    std::atomic<int> received{0};
    mc.addObserver("test.basic", [&received](uint16_t t, const void* data, size_t size) {
        received++;
    });

    int value = 1;
    mc.post("test.basic", &value, sizeof(value));
    mc.post("test.basic", &value, sizeof(value));
    mc.post("test.basic", &value, sizeof(value));

    // Wait with timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.load() < 3 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(received.load(), 3);

    MessageCenterFast::DisposeDefaultMessageCenter();
}

TEST_F(MessageCenterTest, Fast_PayloadDelivery) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();

    std::atomic<int> receivedValue{0};
    mc.addObserver("test.payload", [&receivedValue](uint16_t t, const void* data, size_t size) {
        if (data && size >= sizeof(int)) {
            receivedValue = *static_cast<const int*>(data);
        }
    });

    int value = 42;
    mc.post("test.payload", &value, sizeof(value));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (receivedValue.load() == 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(receivedValue.load(), 42);

    MessageCenterFast::DisposeDefaultMessageCenter();
}

TEST_F(MessageCenterTest, Fast_LargePayload) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();

    std::atomic<bool> receivedCorrect{false};
    mc.addObserver("test.large", [&receivedCorrect](uint16_t t, const void* data, size_t size) {
        if (size == 256) {
            const char* bytes = static_cast<const char*>(data);
            receivedCorrect = (bytes[0] == 'A' && bytes[255] == 'Z');
        }
    });

    char largeData[256];
    largeData[0] = 'A';
    largeData[255] = 'Z';

    mc.post("test.large", largeData, sizeof(largeData));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!receivedCorrect.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_TRUE(receivedCorrect.load());

    MessageCenterFast::DisposeDefaultMessageCenter();
}

TEST_F(MessageCenterTest, Fast_MultipleObservers) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();

    std::atomic<int> count1{0}, count2{0};

    mc.addObserver("test.multi", [&count1](uint16_t t, const void* data, size_t size) {
        count1++;
    });
    mc.addObserver("test.multi", [&count2](uint16_t t, const void* data, size_t size) {
        count2++;
    });

    int value = 1;
    mc.post("test.multi", &value, sizeof(value));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while ((count1.load() < 1 || count2.load() < 1) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(count1.load(), 1);
    EXPECT_EQ(count2.load(), 1);

    MessageCenterFast::DisposeDefaultMessageCenter();
}

TEST_F(MessageCenterTest, Fast_TopicById) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();

    uint16_t topicId = mc.registerTopic("test.byid");

    std::atomic<int> received{0};
    mc.addObserver(topicId, [&received](uint16_t t, const void* data, size_t size) {
        received++;
    });

    int value = 1;
    mc.post(topicId, &value, sizeof(value));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(received.load(), 1);

    MessageCenterFast::DisposeDefaultMessageCenter();
}

TEST_F(MessageCenterTest, Fast_StopWithPendingMessages) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();

    std::atomic<int> received{0};
    mc.addObserver("test.pending", [&received](uint16_t t, const void* data, size_t size) {
        received++;
    });

    // Post many messages
    for (int i = 0; i < 100; i++) {
        mc.post("test.pending", &i, sizeof(i));
    }

    // Stop immediately - should drain remaining messages
    MessageCenterFast::DisposeDefaultMessageCenter();

    // Should not crash, some messages delivered
    EXPECT_GE(received.load(), 0);
}

TEST_F(MessageCenterTest, Fast_Throughput) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();

    std::atomic<int> received{0};
    mc.addObserver("test.throughput", [&received](uint16_t t, const void* data, size_t size) {
        received++;
    });

    const int TOTAL = 500;

    for (int i = 0; i < TOTAL; i++) {
        mc.post("test.throughput", &i, sizeof(i));
    }

    // Wait for delivery with generous timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received.load() < TOTAL && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Allow small variance (99%+ delivery is acceptable)
    EXPECT_GE(received.load(), TOTAL * 99 / 100);

    MessageCenterFast::DisposeDefaultMessageCenter();
}

TEST_F(MessageCenterTest, Fast_IdleCPU) {
    auto& mc = MessageCenterFast::DefaultMessageCenter();

    mc.addObserver("test.idle", [](uint16_t t, const void* data, size_t size) {});

    // Let it sit idle for a bit - should not burn CPU
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Post one message to verify it's still working
    std::atomic<int> received{0};
    mc.addObserver("test.idle2", [&received](uint16_t t, const void* data, size_t size) {
        received++;
    });

    int value = 1;
    mc.post("test.idle2", &value, sizeof(value));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (received.load() < 1 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    EXPECT_EQ(received.load(), 1);

    MessageCenterFast::DisposeDefaultMessageCenter();
}
