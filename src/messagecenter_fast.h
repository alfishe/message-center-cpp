#pragma once

#ifndef MESSAGE_CENTER_MESSAGECENTER_FAST_H
#define MESSAGE_CENTER_MESSAGECENTER_FAST_H

#include "mpmc_queue.h"
#include "objectpool_lockfree.h"
#include "platform.h"

#include <atomic>
#include <cassert>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

constexpr size_t MC_FAST_QUEUE_SIZE = 65536;
constexpr size_t MC_FAST_MAX_TOPICS = 1024;
constexpr size_t MC_FAST_INLINE_SIZE = 64;

struct MessageFast {
    uint16_t topicId;
    uint16_t flags;
    uint32_t size;
    alignas(alignof(std::max_align_t)) char inlineData[MC_FAST_INLINE_SIZE];
    void* managedData;
    std::atomic<uint32_t> refCount;

    MessageFast() : topicId(0), flags(0), size(0), managedData(nullptr), refCount(0) {}

    ~MessageFast() {
        if (managedData != nullptr) {
            free(managedData);
            managedData = nullptr;
        }
    }

    void reset() {
        if (managedData != nullptr) {
            free(managedData);
            managedData = nullptr;
        }
        topicId = 0;
        flags = 0;
        size = 0;
        refCount.store(0, std::memory_order_relaxed);
    }

    const void* data() const {
        return (flags & 1) ? managedData : inlineData;
    }
};

using ObserverCallbackFast = std::function<void(uint16_t topicId, const void* data, size_t size)>;

struct ObserverEntryFast {
    ObserverCallbackFast callback;
    void* userData;
};

class MessageCenterFast {
private:
    static MessageCenterFast* s_instance;

    MPMCQueue<MessageFast*, MC_FAST_QUEUE_SIZE> m_queue;
    ObjectPoolLockFree<MessageFast> m_pool;

    using ObserverList = std::vector<ObserverEntryFast>;

    struct TopicSlot {
        std::atomic<const ObserverList*> list{nullptr};
    };

    static constexpr size_t TOPIC_CHUNK_SHIFT = 6; // 64 topics per chunk
    static constexpr size_t TOPIC_CHUNK_SIZE = 1u << TOPIC_CHUNK_SHIFT;
    static constexpr size_t TOPIC_CHUNK_MASK = TOPIC_CHUNK_SIZE - 1;
    static constexpr size_t TOPIC_NUM_CHUNKS =
        MC_FAST_MAX_TOPICS / TOPIC_CHUNK_SIZE;

    TopicSlot* m_topicBase{nullptr};
    std::unique_ptr<std::atomic<TopicSlot*>[]> m_topicChunks{
        new std::atomic<TopicSlot*>[TOPIC_NUM_CHUNKS]};
    std::unordered_map<std::string, uint16_t> m_topicMap;
    std::mutex m_topicMutex;
    std::atomic<uint16_t> m_nextTopicId{0};

    std::vector<const ObserverList*> m_allLists;
    std::mutex m_observerMutex;

    std::mutex m_lifecycleMutex;
    std::thread* m_dispatchThread{nullptr};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};

    std::mutex m_cvMutex;
    std::condition_variable m_cv;
    char m_padBeforeSleep[64];
    std::atomic<bool> m_sleeping{false};
    char m_padSleep[64 - sizeof(std::atomic<bool>)];

    std::atomic<uint64_t> m_posted{0};
    char m_padPosted[64 - sizeof(std::atomic<uint64_t>)];
    std::atomic<uint64_t> m_dispatched{0};
    char m_padDispatched[64 - sizeof(std::atomic<uint64_t>)];

    inline TopicSlot* findTopic(uint16_t topicId) const {
        if (topicId < TOPIC_CHUNK_SIZE)
            return &m_topicBase[topicId];
        TopicSlot* chunk = m_topicChunks[topicId >> TOPIC_CHUNK_SHIFT].load(
            std::memory_order_acquire);
        return chunk ? &chunk[topicId & TOPIC_CHUNK_MASK] : nullptr;
    }

    TopicSlot& ensureTopic(uint16_t topicId) {
        if (topicId < TOPIC_CHUNK_SIZE)
            return m_topicBase[topicId];
        size_t c = topicId >> TOPIC_CHUNK_SHIFT;
        TopicSlot* chunk = m_topicChunks[c].load(std::memory_order_acquire);
        if (!chunk) {
            std::lock_guard<std::mutex> lock(m_observerMutex);
            chunk = m_topicChunks[c].load(std::memory_order_relaxed);
            if (!chunk) {
                chunk = new TopicSlot[TOPIC_CHUNK_SIZE]();
                m_topicChunks[c].store(chunk, std::memory_order_release);
            }
        }
        return chunk[topicId & TOPIC_CHUNK_MASK];
    }

public:
    static MessageCenterFast& DefaultMessageCenter(bool autostart = true) {
        if (s_instance == nullptr) {
            s_instance = new MessageCenterFast();
            if (autostart) {
                s_instance->start();
            }
        }
        return *s_instance;
    }

    static void DisposeDefaultMessageCenter() {
        if (s_instance != nullptr) {
            s_instance->stop();
            delete s_instance;
            s_instance = nullptr;
        }
    }

    MessageCenterFast()
        : m_pool(256, MC_FAST_QUEUE_SIZE + 64),
          m_topicBase(new TopicSlot[TOPIC_CHUNK_SIZE]()) {
        (void)m_padBeforeSleep;
        (void)m_padSleep;
        (void)m_padPosted;
        (void)m_padDispatched;
        m_topicChunks[0].store(m_topicBase, std::memory_order_relaxed);
        for (size_t i = 1; i < TOPIC_NUM_CHUNKS; ++i)
            m_topicChunks[i].store(nullptr, std::memory_order_relaxed);
    }

    ~MessageCenterFast() {
        // Destruction from the dispatch thread (inside a callback) would leave the
        // worker running on a destroyed object - fail loudly instead of corrupting memory.
        assert((m_dispatchThread == nullptr || m_dispatchThread->get_id() != std::this_thread::get_id()) &&
               "MessageCenterFast must not be destroyed from its own observer callback");
        stop();
        MessageFast* msg = nullptr;
        while (m_queue.try_pop(msg)) {
            if (msg) {
                msg->reset();
                m_pool.release(msg);
            }
        }
        for (const ObserverList* list : m_allLists)
            delete list;
        for (size_t i = 1; i < TOPIC_NUM_CHUNKS; ++i)
            delete[] m_topicChunks[i].load(std::memory_order_relaxed);
        delete[] m_topicBase;
    }

    MessageCenterFast(const MessageCenterFast&) = delete;
    MessageCenterFast& operator=(const MessageCenterFast&) = delete;

    void start() {
        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        if (m_running.load(std::memory_order_acquire)) {
            if (!m_stopping.load(std::memory_order_acquire))
                return;
            // Clean up deferred stop before restarting
            if (m_dispatchThread != nullptr) {
                if (m_dispatchThread->get_id() == std::this_thread::get_id())
                    return;
                if (m_dispatchThread->joinable())
                    m_dispatchThread->join();
                delete m_dispatchThread;
                m_dispatchThread = nullptr;
            }
        }

        m_stopping.store(false, std::memory_order_release);
        m_running.store(true, std::memory_order_release);
        m_dispatchThread = new std::thread(&MessageCenterFast::dispatchLoop, this);
    }

    // Stops the dispatch thread and waits for it to drain.
    // When called from an observer callback (i.e. on the dispatch thread itself),
    // stop is only requested: the loop exits after the current callback returns and
    // the thread is joined by the next stop()/destructor call from another thread.
    // Destroying the MessageCenterFast from inside its own callback is not supported.
    void stop() {
        if (!m_running.load(std::memory_order_acquire)) return;

        m_stopping.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(m_cvMutex);
        }
        m_cv.notify_all();

        std::lock_guard<std::mutex> lock(m_lifecycleMutex);
        if (!m_running.load(std::memory_order_acquire)) return;

        if (m_dispatchThread != nullptr) {
            if (m_dispatchThread->get_id() == std::this_thread::get_id()) {
                return; // Deferred: cannot join ourselves
            }
            if (m_dispatchThread->joinable()) {
                m_dispatchThread->join();
            }
            delete m_dispatchThread;
            m_dispatchThread = nullptr;
        }

        m_running.store(false, std::memory_order_release);
    }

    uint16_t registerTopic(const std::string& name) {
        std::lock_guard<std::mutex> lock(m_topicMutex);

        auto it = m_topicMap.find(name);
        if (it != m_topicMap.end()) {
            return it->second;
        }

        uint16_t id = m_nextTopicId.load(std::memory_order_relaxed);
        if (id >= MC_FAST_MAX_TOPICS) {
            return UINT16_MAX;
        }
        m_nextTopicId.store(static_cast<uint16_t>(id + 1), std::memory_order_relaxed);
        ensureTopic(id);
        m_topicMap[name] = id;
        return id;
    }

    uint16_t resolveTopic(const std::string& name) {
        std::lock_guard<std::mutex> lock(m_topicMutex);
        auto it = m_topicMap.find(name);
        return (it != m_topicMap.end()) ? it->second : UINT16_MAX;
    }

    void addObserver(uint16_t topicId, ObserverCallbackFast callback, void* userData = nullptr) {
        if (topicId >= MC_FAST_MAX_TOPICS) return;

        TopicSlot& slot = ensureTopic(topicId);
        std::lock_guard<std::mutex> lock(m_observerMutex);
        const ObserverList* current = slot.list.load(std::memory_order_relaxed);
        ObserverList* next = current ? new ObserverList(*current) : new ObserverList();
        next->push_back({std::move(callback), userData});
        m_allLists.push_back(next);
        slot.list.store(next, std::memory_order_release);
    }

    void addObserver(const std::string& topic, ObserverCallbackFast callback, void* userData = nullptr) {
        uint16_t id = registerTopic(topic);
        if (id != UINT16_MAX) {
            addObserver(id, std::move(callback), userData);
        }
    }

    bool post(uint16_t topicId, const void* data, size_t size) {
        if (topicId >= MC_FAST_MAX_TOPICS) return false;
        if (size > MC_FAST_INLINE_SIZE && m_queue.full() &&
            (!m_running.load(std::memory_order_relaxed) ||
             m_stopping.load(std::memory_order_relaxed))) {
            return false;
        }

        MessageFast* msg = m_pool.acquire();
        if (msg == nullptr) return false;

        msg->topicId = topicId;
        msg->size = static_cast<uint32_t>(size);

        if (size <= MC_FAST_INLINE_SIZE) {
            msg->flags = 0;
            if (data && size > 0) {
                memcpy(msg->inlineData, data, size);
            }
        } else {
            msg->flags = 1;
            msg->managedData = malloc(size);
            if (msg->managedData && data) {
                memcpy(msg->managedData, data, size);
            }
        }

        if (!m_queue.try_push(msg)) {
            bool pushed = false;
            if (size > MC_FAST_INLINE_SIZE &&
                m_running.load(std::memory_order_relaxed) &&
                !m_stopping.load(std::memory_order_relaxed)) {
                m_cv.notify_one();
                for (int retry = 0; retry < 64; ++retry) {
                    std::this_thread::yield();
                    if (m_queue.try_push(msg)) {
                        pushed = true;
                        break;
                    }
                }
            }
            if (!pushed) {
                msg->reset();
                m_pool.release(msg);
                return false;
            }
        }

        m_posted.fetch_add(1, std::memory_order_seq_cst);
        if (m_sleeping.load(std::memory_order_seq_cst)) {
            // Rare path: the worker is (about to be) blocked on m_cv.
            // Taking m_cvMutex serializes with the worker's predicate check, so the
            // notification cannot fall between "queue is empty" and "block on m_cv"
            // (which would otherwise cost a full wait_for timeout).
            { std::lock_guard<std::mutex> lock(m_cvMutex); }
            m_cv.notify_one();
        }
        return true;
    }

    bool post(const std::string& topic, const void* data, size_t size) {
        uint16_t id = resolveTopic(topic);
        if (id == UINT16_MAX) {
            id = registerTopic(topic);
        }
        return post(id, data, size);
    }

    uint64_t pending() const {
        return m_posted.load(std::memory_order_relaxed) - m_dispatched.load(std::memory_order_relaxed);
    }

    void flush(uint32_t timeoutMs = 5000) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        int spinCount = 0;
        while (pending() > 0 && std::chrono::steady_clock::now() < deadline) {
            m_cv.notify_one();
            if (spinCount < 100) {
                spinCount++;
                std::this_thread::yield();
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }
    }

private:
    void dispatchLoop() {
        int emptyCount = 0;

        while (!m_stopping.load(std::memory_order_acquire)) {
            MessageFast* msg = nullptr;

            if (m_queue.try_pop(msg)) {
                dispatch(msg);
                msg->reset();
                m_pool.release(msg);
                m_dispatched.fetch_add(1, std::memory_order_relaxed);
                emptyCount = 0;
            } else {
                emptyCount++;
                if (emptyCount < 100) {
                    std::this_thread::yield();
                } else {
                    std::unique_lock<std::mutex> lock(m_cvMutex);
                    m_sleeping.store(true, std::memory_order_seq_cst);
                    m_cv.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                        return m_stopping.load(std::memory_order_relaxed) || !m_queue.empty();
                    });
                    m_sleeping.store(false, std::memory_order_relaxed);
                }
            }
        }

        MessageFast* msg = nullptr;
        while (m_queue.try_pop(msg)) {
            dispatch(msg);
            msg->reset();
            m_pool.release(msg);
            m_dispatched.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void dispatch(MessageFast* msg) {
        if (msg == nullptr || msg->topicId >= MC_FAST_MAX_TOPICS) return;

        const void* data = msg->data();
        size_t size = msg->size;
        uint16_t topicId = msg->topicId;

        TopicSlot* slot = findTopic(topicId);
        const ObserverList* snapshot =
            slot ? slot->list.load(std::memory_order_acquire) : nullptr;

        if (snapshot) {
            for (const auto& entry : *snapshot) {
                if (entry.callback) {
                    entry.callback(topicId, data, size);
                }
            }
        }
    }
};

#endif // MESSAGE_CENTER_MESSAGECENTER_FAST_H
