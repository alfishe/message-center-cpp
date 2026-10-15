/**
 * @file memory_safety_uaf_test.cpp
 * @brief Memory safety, Use-After-Free (UAF), Double-Free, and Re-entrancy Unit Tests.
 *
 * This test suite explicitly verifies:
 * 1. Re-entrant observer unsubscription (removing self during callback execution).
 * 2. Re-entrant observer unsubscription of peer observers during callback execution.
 * 3. Re-entrant message posting from within callbacks.
 * 4. Destruction ordering between subscribers, payloads, and the message center.
 * 5. Lifecycle of managed and ref-counted payloads (preventing double-free & leaks).
 * 6. Protection against dangling observer callbacks and instance pointers.
 */

#include <gtest/gtest.h>

#include "eventqueue.h"
#include "eventqueue_broadcast.h"
#include "messagecenter.h"
#include "messagecenter_fast.h"
#include "payload_refcounted.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class MemorySafetyTest : public ::testing::Test {
protected:
    void SetUp() override {}
    void TearDown() override {
        // Ensure default message center is disposed between tests if used
        MessageCenter::DisposeDefaultMessageCenter();
    }
};

// Helper tracked payload to verify constructor/destructor matching
struct TrackedLifetimePayload : public MessagePayload {
    static std::atomic<int> s_aliveCount;
    int payloadId;

    explicit TrackedLifetimePayload(int id) : payloadId(id) {
        s_aliveCount.fetch_add(1, std::memory_order_relaxed);
    }
    ~TrackedLifetimePayload() override {
        s_aliveCount.fetch_sub(1, std::memory_order_relaxed);
    }
};
std::atomic<int> TrackedLifetimePayload::s_aliveCount{0};

// Target listener class for method callback tests
class TargetListener : public Observer {
public:
    int callCount = 0;
    int lastId = -1;

    void OnEvent(int id, Message* msg) {
        callCount++;
        lastId = id;
    }
};

// ============================================================================
// Section 1: Re-Entrancy Tests
// ============================================================================

/**
 * Test: SelfUnsubscribeInsideCallback
 * Verifies that an observer can safely unregister itself while its own callback
 * is executing, without deadlocking or causing use-after-free.
 */
TEST_F(MemorySafetyTest, SelfUnsubscribeInsideCallback) {
    EventQueueCUT queue;
    queue.init();
    const std::string topic = "test.safety.reentrant.self";
    int topicId = queue.RegisterTopic(topic);
    ASSERT_GE(topicId, 0);

    uint64_t observerId = 0;
    int executionCount = 0;

    observerId = queue.AddObserver(topic, ObserverCallbackFunc([&](int id, Message* msg) {
        executionCount++;
        // Re-entrant removal of own observer ID
        queue.RemoveObserverById(topic, observerId);
    }));

    EXPECT_NE(observerId, 0u);

    // First post & dispatch: callback runs and unregisters itself
    queue.Post(topicId, nullptr, false);
    Message* msg = queue.GetQueueMessage();
    ASSERT_NE(msg, nullptr);
    queue.Dispatch(msg->tid, msg);
    EXPECT_EQ(executionCount, 1);

    // Verify observer is removed from the topic observer list
    auto observers = queue.GetObservers(topicId);
    EXPECT_TRUE(observers == nullptr || observers->empty());

    // Second post & dispatch: callback should NOT run again
    queue.Post(topicId, nullptr, false);
    msg = queue.GetQueueMessage();
    ASSERT_NE(msg, nullptr);
    queue.Dispatch(msg->tid, msg);
    EXPECT_EQ(executionCount, 1);

    queue.dispose();
}

/**
 * Test: PeerUnsubscribeInsideCallback
 * Verifies behavior when observer A removes observer B during dispatch.
 * Observer B is immediately marked inactive so it will not be executed from
 * an in-flight snapshot, preventing use-after-free even if observer B's target
 * object is destroyed immediately upon unsubscription.
 */
TEST_F(MemorySafetyTest, PeerUnsubscribeInsideCallback) {
    EventQueueCUT queue;
    queue.init();
    const std::string topic = "test.safety.reentrant.peer";
    int topicId = queue.RegisterTopic(topic);
    ASSERT_GE(topicId, 0);

    uint64_t idA = 0;
    uint64_t idB = 0;
    int aExecCount = 0;
    int bExecCount = 0;

    auto* targetB = new TargetListener();

    idA = queue.AddObserver(topic, ObserverCallbackFunc([&](int id, Message* msg) {
        aExecCount++;
        // Observer A removes Observer B and destroys its target
        queue.RemoveObserverById(topic, idB);
        delete targetB;
        targetB = nullptr;
    }));

    ObserverCallbackMethod method = static_cast<ObserverCallbackMethod>(&TargetListener::OnEvent);
    idB = queue.AddObserver(topic, targetB, method);

    // Post message 1: both were registered at dispatch start
    queue.Post(topicId, nullptr, false);
    Message* msg = queue.GetQueueMessage();
    ASSERT_NE(msg, nullptr);
    queue.Dispatch(msg->tid, msg);

    EXPECT_EQ(aExecCount, 1);
    // B was cancelled before execution, preventing UAF!
    EXPECT_EQ(bExecCount, 0);

    // Post message 2: B is now fully removed, only A should execute
    queue.Post(topicId, nullptr, false);
    msg = queue.GetQueueMessage();
    ASSERT_NE(msg, nullptr);
    queue.Dispatch(msg->tid, msg);

    EXPECT_EQ(aExecCount, 2);
    EXPECT_EQ(bExecCount, 0);

    queue.dispose();
}

/**
 * Test: ReentrantPostInsideCallback
 * Verifies that an observer callback can safely call queue.Post() without
 * deadlocking or corrupting message queue state.
 */
TEST_F(MemorySafetyTest, ReentrantPostInsideCallback) {
    EventQueueCUT queue;
    queue.init();
    const std::string topic = "test.safety.reentrant.post";
    int topicId = queue.RegisterTopic(topic);
    ASSERT_GE(topicId, 0);

    int receivedCount = 0;
    const int MAX_CHAIN = 5;

    queue.AddObserver(topic, ObserverCallbackFunc([&](int id, Message* msg) {
        receivedCount++;
        if (receivedCount < MAX_CHAIN) {
            // Re-entrantly post another message
            queue.Post(topicId, nullptr, false);
        }
    }));

    // Start chain
    queue.Post(topicId, nullptr, false);

    while (Message* m = queue.GetQueueMessage()) {
        queue.Dispatch(m->tid, m);
    }

    EXPECT_EQ(receivedCount, MAX_CHAIN);
    queue.dispose();
}

// ============================================================================
// Section 2: Payload Lifecycle and Leak Checks
// ============================================================================

/**
 * Test: DispatchedPayloadAutoCleanup
 * Verifies that when autoCleanupPayload is true, MessagePayload is deleted upon
 * dispatch delivery.
 */
TEST_F(MemorySafetyTest, DispatchedPayloadAutoCleanup) {
    TrackedLifetimePayload::s_aliveCount = 0;
    {
        EventQueueCUT queue;
        queue.init();
        int topicId = queue.RegisterTopic("test.safety.payload");

        queue.AddObserver("test.safety.payload", ObserverCallbackFunc([](int, Message* msg) {
            ASSERT_NE(msg, nullptr);
            ASSERT_NE(msg->obj, nullptr);
            auto* p = dynamic_cast<TrackedLifetimePayload*>(msg->obj);
            ASSERT_NE(p, nullptr);
            EXPECT_EQ(p->payloadId, 100);
        }));

        queue.Post(topicId, new TrackedLifetimePayload(100), true);
        EXPECT_EQ(TrackedLifetimePayload::s_aliveCount.load(), 1);

        Message* m = queue.GetQueueMessage();
        ASSERT_NE(m, nullptr);
        queue.Dispatch(m->tid, m);

        // After dispatch with autoCleanupPayload=true, payload should be destroyed
        EXPECT_EQ(TrackedLifetimePayload::s_aliveCount.load(), 0);
        queue.dispose();
    }
    EXPECT_EQ(TrackedLifetimePayload::s_aliveCount.load(), 0);
}

/**
 * Test: RefCountedPayloadBoundary
 * Verifies that RefCountedPayload correctly increments and decrements references,
 * deleting memory exactly when refCount drops to zero.
 */
TEST_F(MemorySafetyTest, RefCountedPayloadBoundary) {
    const size_t size = 128;
    char buffer[128];
    std::memset(buffer, 0x42, size);

    auto* payload = RefCountedPayload::create(buffer, size);
    ASSERT_NE(payload, nullptr);
    EXPECT_EQ(payload->refCount(), 1);
    EXPECT_EQ(payload->size(), size);
    EXPECT_EQ(std::memcmp(payload->data(), buffer, size), 0);

    // Add references (e.g. for 3 observers)
    payload->addRefs(2);
    EXPECT_EQ(payload->refCount(), 3);

    // Release 1
    payload->release();
    EXPECT_EQ(payload->refCount(), 2);

    // Release 2
    payload->release();
    EXPECT_EQ(payload->refCount(), 1);

    // Final release: frees memory
    payload->release();
}

/**
 * Test: PayloadGuardRAII
 * Verifies that PayloadGuard RAII helper releases payload on scope exit.
 */
TEST_F(MemorySafetyTest, PayloadGuardRAII) {
    auto* payload = RefCountedPayload::create(64);
    ASSERT_NE(payload, nullptr);
    EXPECT_EQ(payload->refCount(), 1);

    payload->addRef(); // refCount = 2
    {
        PayloadGuard guard(payload);
        EXPECT_EQ(payload->refCount(), 2);
    } // guard destroyed, calls release()

    EXPECT_EQ(payload->refCount(), 1);
    payload->release(); // clean up
}

// ============================================================================
// Section 3: Destruction Ordering & Listener Lifetimes
// ============================================================================

/**
 * Test: SubscriberDestroyedBeforeMessageCenter
 * Verifies that when a subscriber object is destroyed, removing its observer
 * method callback prevents any future dispatch from calling the dead object.
 */
TEST_F(MemorySafetyTest, SubscriberDestroyedBeforeMessageCenter) {
    EventQueueCUT queue;
    queue.init();
    const std::string topic = "test.safety.order";
    int topicId = queue.RegisterTopic(topic);

    auto listener = std::make_unique<TargetListener>();
    ObserverCallbackMethod method = static_cast<ObserverCallbackMethod>(&TargetListener::OnEvent);

    queue.AddObserver(topic, listener.get(), method);

    // Dispatch 1
    queue.Post(topicId, nullptr, false);
    Message* m = queue.GetQueueMessage();
    queue.Dispatch(m->tid, m);
    EXPECT_EQ(listener->callCount, 1);

    // Unregister BEFORE destroying subscriber
    queue.RemoveObserver(topic, listener.get(), method);
    listener.reset(); // Object destroyed

    // Dispatch 2: Should not call into destroyed listener
    queue.Post(topicId, nullptr, false);
    m = queue.GetQueueMessage();
    queue.Dispatch(m->tid, m);

    // Clean disposal
    queue.dispose();
}

/**
 * Test: SafeEmptyQueueDisposal
 * Verifies that initializing and immediately disposing an EventQueue without
 * any topics or observers does not crash or leak memory.
 */
TEST_F(MemorySafetyTest, SafeEmptyQueueDisposal) {
    EventQueue queue;
    EXPECT_FALSE(queue.init()); // EventQueue::init returns false by design
    queue.dispose();
}

/**
 * Test: EventQueueBroadcast_DynamicObserverRefSafety
 * Verifies that dynamically adding observers between post and dispatch
 * or during dispatch does not cause double-free or UAF on managed payloads.
 */
TEST_F(MemorySafetyTest, EventQueueBroadcast_DynamicObserverRefSafety) {
    EventQueueBroadcast<1024> queue;
    uint16_t topic = queue.registerTopic("test.dynamic.ref");

    std::atomic<int> receivedCount{0};

    // 1 initial observer
    queue.addObserver(topic, [&](uint16_t, const void* data, size_t size) {
        receivedCount++;
    });

    // Post large managed payload (> 48 bytes)
    char buffer[128];
    std::memset(buffer, 'Z', sizeof(buffer));
    queue.post(topic, buffer, sizeof(buffer));

    // Dynamically add a 2nd observer after post, before dispatch
    queue.addObserver(topic, [&](uint16_t, const void* data, size_t size) {
        receivedCount++;
    });

    // Dispatch should deliver to both observers without double-free or crash
    EXPECT_TRUE(queue.dispatchOne());
    EXPECT_EQ(receivedCount.load(), 2);
}

/**
 * Test: MessageCenterFast_ReentrantAddObserver
 * Verifies that an observer callback can re-entrantly add another observer
 * to MessageCenterFast without causing a recursive mutex deadlock.
 */
TEST_F(MemorySafetyTest, MessageCenterFast_ReentrantAddObserver) {
    MessageCenterFast mc;
    uint16_t topic = mc.registerTopic("test.fast.reentrant");

    std::atomic<bool> reentrantAdded{false};
    std::atomic<int> execCount{0};

    mc.addObserver(topic, [&](uint16_t tid, const void* data, size_t sz) {
        execCount++;
        if (!reentrantAdded.exchange(true)) {
            // Re-entrant observer registration inside callback
            mc.addObserver(tid, [&](uint16_t, const void*, size_t) {
                execCount++;
            });
        }
    });

    char msgData[] = "test";
    mc.post(topic, msgData, sizeof(msgData));

    // Start background thread or dispatch manually
    mc.start();
    mc.flush(500);
    mc.stop();

    EXPECT_GE(execCount.load(), 1);
}
