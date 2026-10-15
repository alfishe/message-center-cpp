/**
 * @file message_center_fuzzer.cpp
 * @brief Standalone Multi-Flavor libFuzzer Harness for EventQueue, Broadcast, Fast, Batch, and Emulator.
 *
 * How to compile:
 *   clang++ -std=c++17 -fsanitize=fuzzer,address,undefined -g -O1 \
 *     -I../../src -I../../include ../../src/eventqueue.cpp message_center_fuzzer.cpp \
 *     -lpthread -o message_center_fuzzer
 *
 * How to run:
 *   ./message_center_fuzzer -detect_leaks=1 -max_total_time=60
 */

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define _CODE_UNDER_TEST 1
#include "eventqueue.h"
#include "eventqueue_broadcast.h"
#include "eventqueue_fast.h"
#include "eventqueue_batch.h"
#include "eventqueue_emulator.h"

// ============================================================================
// EventQueue Fuzzer Implementation
// ============================================================================

struct FuzzerDummyPayload : public MessagePayload {
    uint32_t val;
    explicit FuzzerDummyPayload(uint32_t v) : val(v) {}
    ~FuzzerDummyPayload() override = default;
};

class FuzzerTargetObserver : public Observer {
public:
    int count = 0;
    void OnMessage(int, Message*) {
        count++;
    }
};

static void FuzzEventQueue(const uint8_t *data, size_t size) {
    if (size < 4) return;

    EventQueueCUT queue;
    queue.init();

    std::vector<uint64_t> observerIds;
    std::vector<std::string> topicNames;
    std::vector<int> topicIds;
    FuzzerTargetObserver targetObserver;

    topicNames.push_back("seed_topic");
    topicIds.push_back(queue.RegisterTopic("seed_topic"));

    size_t offset = 0;
    while (offset < size) {
        uint8_t op = data[offset++] % 10;
        switch (op) {
            case 0: { // RegisterTopic
                if (offset >= size) break;
                uint8_t len = (data[offset++] % 16) + 1;
                std::string name;
                for (uint8_t i = 0; i < len && offset < size; ++i) {
                    name += static_cast<char>('a' + (data[offset++] % 26));
                }
                if (!name.empty()) {
                    int tid = queue.RegisterTopic(name);
                    topicNames.push_back(name);
                    topicIds.push_back(tid);
                }
                break;
            }
            case 1: { // AddObserver lambda
                if (topicNames.empty()) break;
                size_t tIdx = (offset < size) ? data[offset++] % topicNames.size() : 0;
                uint64_t obsId = queue.AddObserver(topicNames[tIdx], ObserverCallbackFunc([](int, Message*) {}));
                observerIds.push_back(obsId);
                break;
            }
            case 2: { // AddObserver class method
                if (topicNames.empty()) break;
                size_t tIdx = (offset < size) ? data[offset++] % topicNames.size() : 0;
                ObserverCallbackMethod m = static_cast<ObserverCallbackMethod>(&FuzzerTargetObserver::OnMessage);
                uint64_t obsId = queue.AddObserver(topicNames[tIdx], &targetObserver, m);
                observerIds.push_back(obsId);
                break;
            }
            case 3: { // RemoveObserverById
                if (observerIds.empty() || topicNames.empty()) break;
                size_t oIdx = (offset < size) ? data[offset++] % observerIds.size() : 0;
                size_t tIdx = (offset < size) ? data[offset++] % topicNames.size() : 0;
                queue.RemoveObserverById(topicNames[tIdx], observerIds[oIdx]);
                observerIds.erase(observerIds.begin() + oIdx);
                break;
            }
            case 4: { // Post with payload
                if (topicNames.empty()) break;
                size_t tIdx = (offset < size) ? data[offset++] % topicNames.size() : 0;
                queue.Post(topicNames[tIdx], new FuzzerDummyPayload(42), true);
                break;
            }
            case 5: { // Post without payload
                if (topicNames.empty()) break;
                size_t tIdx = (offset < size) ? data[offset++] % topicNames.size() : 0;
                queue.Post(topicNames[tIdx], nullptr, false);
                break;
            }
            case 6: { // GetQueueMessage + Dispatch
                Message* msg = queue.GetQueueMessage();
                if (msg != nullptr) {
                    queue.Dispatch(msg->tid, msg);
                }
                break;
            }
            case 7: { // ResolveTopic
                if (topicNames.empty()) break;
                size_t tIdx = (offset < size) ? data[offset++] % topicNames.size() : 0;
                queue.ResolveTopic(topicNames[tIdx]);
                break;
            }
            case 8: { // Re-entrant post from callback
                if (topicNames.empty()) break;
                size_t tIdx = (offset < size) ? data[offset++] % topicNames.size() : 0;
                std::string curTopic = topicNames[tIdx];
                uint64_t obsId = queue.AddObserver(curTopic, ObserverCallbackFunc([&queue, curTopic](int, Message*) {
                    queue.Post(curTopic, nullptr, false);
                }));
                observerIds.push_back(obsId);
                break;
            }
            case 9: { // Re-entrant remove self from callback
                if (topicNames.empty()) break;
                size_t tIdx = (offset < size) ? data[offset++] % topicNames.size() : 0;
                std::string curTopic = topicNames[tIdx];
                struct SelfRemover {
                    uint64_t id = 0;
                };
                auto remover = std::make_shared<SelfRemover>();
                remover->id = queue.AddObserver(curTopic, ObserverCallbackFunc([&queue, curTopic, remover](int, Message*) {
                    queue.RemoveObserverById(curTopic, remover->id);
                }));
                break;
            }
        }
    }

    int maxDrain = 50;
    while (maxDrain-- > 0) {
        Message* msg = queue.GetQueueMessage();
        if (!msg) break;
        queue.Dispatch(msg->tid, msg);
    }

    queue.dispose();
}

// ============================================================================
// EventQueueBroadcast Fuzzer Implementation
// ============================================================================

static void FuzzEventQueueBroadcast(const uint8_t *data, size_t size) {
    if (size < 4) return;

    EventQueueBroadcast<512> queue;
    std::vector<uint16_t> topicIds;
    for (int i = 0; i < 4; ++i) {
        topicIds.push_back(queue.registerTopic("bcast_fuzz_" + std::to_string(i)));
    }

    for (uint16_t tid : topicIds) {
        queue.addObserver(tid, [](uint16_t, const void*, size_t) {});
    }

    size_t offset = 0;
    while (offset < size) {
        // Single-threaded harness: post() blocks while the queue is full, so drain before saturation
        if (queue.queueSize() >= 512 - 64) queue.dispatchAll();
        uint8_t op = data[offset++] % 5;
        uint16_t tid = topicIds[offset % topicIds.size()];
        switch (op) {
            case 0: { // Inline payload <= 48
                uint8_t pSize = (offset < size) ? (data[offset++] % 48) + 1 : 16;
                char buf[48];
                std::memset(buf, 0x11, sizeof(buf));
                queue.post(tid, buf, pSize);
                break;
            }
            case 1: { // Managed payload > 48
                uint8_t pSize = (offset < size) ? (data[offset++] % 80) + 49 : 64;
                std::vector<char> buf(pSize, 0x22);
                queue.post(tid, buf.data(), pSize);
                break;
            }
            case 2: { // Signal only
                queue.post(tid);
                break;
            }
            case 3: { // Dispatch one
                queue.dispatchOne();
                break;
            }
            case 4: { // Dispatch all
                queue.dispatchAll();
                break;
            }
        }
    }

    queue.dispatchAll();
}

// ============================================================================
// EventQueueBroadcastFast Fuzzer Implementation
// ============================================================================

static void FuzzEventQueueBroadcastFast(const uint8_t *data, size_t size) {
    if (size < 4) return;

    EventQueueBroadcastFast<512> queue;
    std::vector<uint16_t> topicIds;
    for (int i = 0; i < 4; ++i) {
        topicIds.push_back(queue.registerTopic());
    }

    for (uint16_t tid : topicIds) {
        queue.addObserver(tid, [](uint16_t, const void*, size_t, void*) {}, nullptr);
    }

    size_t offset = 0;
    while (offset < size) {
        // Single-threaded harness: post() blocks while the queue is full, so drain before saturation
        if (queue.queueSize() >= 512 - 64) queue.dispatchAll();
        uint8_t op = data[offset++] % 4;
        uint16_t tid = topicIds[offset % topicIds.size()];
        switch (op) {
            case 0: { // Inline payload <= 48
                uint8_t pSize = (offset < size) ? (data[offset++] % 48) + 1 : 16;
                char buf[48];
                std::memset(buf, 0x33, sizeof(buf));
                queue.post(tid, buf, pSize);
                break;
            }
            case 1: { // Managed payload > 48
                uint8_t pSize = (offset < size) ? (data[offset++] % 80) + 49 : 64;
                std::vector<char> buf(pSize, 0x44);
                queue.post(tid, buf.data(), pSize);
                break;
            }
            case 2: { // Dispatch one
                queue.dispatchOne();
                break;
            }
            case 3: { // Dispatch all
                queue.dispatchAll();
                break;
            }
        }
    }

    queue.dispatchAll();
}

// ============================================================================
// EventQueueBroadcastFastBatch Fuzzer Implementation
// ============================================================================

static void FuzzEventQueueBroadcastFastBatch(const uint8_t *data, size_t size) {
    if (size < 4) return;

    EventQueueBroadcastFastBatch<512> queue;
    std::vector<uint16_t> topicIds;
    for (int i = 0; i < 4; ++i) {
        topicIds.push_back(queue.registerTopic());
    }

    for (uint16_t tid : topicIds) {
        queue.addObserver(tid, [](uint16_t, const void*, size_t, void*) {}, nullptr);
    }

    size_t offset = 0;
    while (offset < size) {
        // Single-threaded harness: post() blocks while the queue is full, so drain before saturation
        if (queue.queueSize() >= 512 - 64) { queue.flushBatch(); queue.dispatchAll(); }
        uint8_t op = data[offset++] % 5;
        uint16_t tid = topicIds[offset % topicIds.size()];
        switch (op) {
            case 0: { // Inline payload <= 48
                uint8_t pSize = (offset < size) ? (data[offset++] % 48) + 1 : 16;
                char buf[48];
                std::memset(buf, 0x55, sizeof(buf));
                queue.post(tid, buf, pSize);
                break;
            }
            case 1: { // Managed payload > 48
                uint8_t pSize = (offset < size) ? (data[offset++] % 80) + 49 : 64;
                std::vector<char> buf(pSize, 0x66);
                queue.post(tid, buf.data(), pSize);
                break;
            }
            case 2: { // Flush batch
                queue.flushBatch();
                break;
            }
            case 3: { // Dispatch batch
                queue.dispatchBatch(16);
                break;
            }
            case 4: { // Dispatch all
                queue.dispatchAll();
                break;
            }
        }
    }

    queue.flushBatch();
    queue.dispatchAll();
}

// ============================================================================
// EventQueueEmulator Fuzzer Implementation
// ============================================================================

static void FuzzEventQueueEmulator(const uint8_t *data, size_t size) {
    if (size < 4) return;

    EventQueueEmulator<256, 512> queue;
    uint16_t critTopic = queue.registerTopic(TopicPriority::Critical);
    uint16_t bulkTopic = queue.registerTopic(TopicPriority::Normal);

    queue.addObserver(critTopic, [](uint16_t, const void*, size_t, void*) {}, nullptr);
    queue.addObserver(bulkTopic, [](uint16_t, const void*, size_t, void*) {}, nullptr);

    size_t offset = 0;
    while (offset < size) {
        // Single-threaded harness: post() blocks while the queue is full, so drain before saturation
        if (queue.fastQueueSize() >= 256 - 16 || queue.bulkQueueSize() >= 512 - 16) queue.dispatchAll();
        uint8_t op = data[offset++] % 6;
        switch (op) {
            case 0: { // Critical fast inline <= 48
                uint8_t pSize = (offset < size) ? (data[offset++] % 48) + 1 : 16;
                char buf[48];
                std::memset(buf, 0x77, sizeof(buf));
                queue.postFast(critTopic, buf, pSize);
                break;
            }
            case 1: { // Bulk inline <= 48
                uint8_t pSize = (offset < size) ? (data[offset++] % 48) + 1 : 16;
                char buf[48];
                std::memset(buf, 0x88, sizeof(buf));
                queue.postBulk(bulkTopic, buf, pSize);
                break;
            }
            case 2: { // Bulk managed > 48
                uint8_t pSize = (offset < size) ? (data[offset++] % 80) + 49 : 64;
                std::vector<char> buf(pSize, 0x99);
                queue.postBulk(bulkTopic, buf.data(), pSize);
                break;
            }
            case 3: { // Dispatch fast one
                queue.dispatchFast();
                break;
            }
            case 4: { // Dispatch bulk one
                queue.dispatchBulk();
                break;
            }
            case 5: { // Dispatch priority
                queue.dispatchPriority();
                break;
            }
        }
    }

    queue.dispatchAll();
}

// ============================================================================
// Multi-Flavor libFuzzer Entry Point
// ============================================================================

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;

    uint8_t flavor = data[0] % 5;
    const uint8_t* fData = data + 1;
    size_t fSize = size - 1;

    switch (flavor) {
        case 0:
            FuzzEventQueue(fData, fSize);
            break;
        case 1:
            FuzzEventQueueBroadcast(fData, fSize);
            break;
        case 2:
            FuzzEventQueueBroadcastFast(fData, fSize);
            break;
        case 3:
            FuzzEventQueueBroadcastFastBatch(fData, fSize);
            break;
        case 4:
            FuzzEventQueueEmulator(fData, fSize);
            break;
    }

    return 0;
}
