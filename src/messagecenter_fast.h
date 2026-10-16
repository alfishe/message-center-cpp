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
#include <type_traits>
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

    MessageFast() : topicId(0), flags(0), size(0), managedData(nullptr) {}

    ~MessageFast() {
        if (managedData != nullptr) {
            free(managedData);
            managedData = nullptr;
        }
    }

    MC_FORCE_INLINE void reset() {
        if (MC_UNLIKELY(managedData != nullptr)) {
            free(managedData);
            managedData = nullptr;
        }
        flags = 0;
    }

    MC_FORCE_INLINE const void* data() const {
        return MC_UNLIKELY(flags & 1) ? managedData : inlineData;
    }
};

using ObserverCallbackFast = std::function<void(uint16_t topicId, const void* data, size_t size)>;

struct ObserverEntryFast {
    ObserverCallbackFast callback;
    void* userData;
    uint64_t id;
};

class MessageCenterFast {
private:
    static MessageCenterFast* s_instance;

    MPMCQueue<MessageFast*, MC_FAST_QUEUE_SIZE> m_queue;
    ObjectPoolLockFree<MessageFast> m_pool;

    using ObserverList = std::vector<ObserverEntryFast>;

    struct TopicSlot {
        std::atomic<uint8_t> count{0};
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
    uint64_t m_nextObserverId{1};
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

    MC_FORCE_INLINE TopicSlot* findTopic(uint16_t topicId) const {
        if (MC_LIKELY(topicId < TOPIC_CHUNK_SIZE))
            return &m_topicBase[topicId];
        TopicSlot* chunk = m_topicChunks[topicId >> TOPIC_CHUNK_SHIFT].load(
            std::memory_order_acquire);
        return chunk ? &chunk[topicId & TOPIC_CHUNK_MASK] : nullptr;
    }

    static std::mutex& singletonMutex() {
        static std::mutex s_mutex;
        return s_mutex;
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
        std::lock_guard<std::mutex> lock(singletonMutex());
        if (s_instance == nullptr) {
            s_instance = new MessageCenterFast();
            if (autostart) {
                s_instance->start();
            }
        }
        return *s_instance;
    }

    static void DisposeDefaultMessageCenter() {
        std::lock_guard<std::mutex> lock(singletonMutex());
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

    uint64_t addObserver(uint16_t topicId, ObserverCallbackFast callback, void* userData = nullptr) {
        if (topicId >= MC_FAST_MAX_TOPICS) return 0;

        TopicSlot& slot = ensureTopic(topicId);
        std::lock_guard<std::mutex> lock(m_observerMutex);
        uint64_t obsId = m_nextObserverId++;
        const ObserverList* current = slot.list.load(std::memory_order_relaxed);
        ObserverList* next = current ? new ObserverList(*current) : new ObserverList();
        next->push_back({std::move(callback), userData, obsId});
        m_allLists.push_back(next);
        slot.list.store(next, std::memory_order_release);
        uint8_t newCount = static_cast<uint8_t>(std::min(next->size(), size_t(255)));
        slot.count.store(newCount, std::memory_order_release);
        return obsId;
    }

    uint64_t addObserver(const std::string& topic, ObserverCallbackFast callback, void* userData = nullptr) {
        uint16_t id = registerTopic(topic);
        return (id != UINT16_MAX) ? addObserver(id, std::move(callback), userData) : 0;
    }

    bool removeObserver(uint16_t topicId, uint64_t observerId) {
        if (topicId >= MC_FAST_MAX_TOPICS || observerId == 0) return false;
        TopicSlot* slot = findTopic(topicId);
        if (!slot) return false;
        std::lock_guard<std::mutex> lock(m_observerMutex);
        const ObserverList* current = slot->list.load(std::memory_order_relaxed);
        if (!current) return false;
        for (size_t i = 0; i < current->size(); ++i) {
            if ((*current)[i].id == observerId) {
                ObserverList* next = new ObserverList();
                next->reserve(current->size() - 1);
                for (size_t j = 0; j < current->size(); ++j) {
                    if (j != i) next->push_back((*current)[j]);
                }
                m_allLists.push_back(next);
                slot->list.store(next, std::memory_order_release);
                uint8_t newCount = static_cast<uint8_t>(std::min(next->size(), size_t(255)));
                slot->count.store(newCount, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    bool removeObserver(const std::string& topic, uint64_t observerId) {
        uint16_t id = resolveTopic(topic);
        return (id != UINT16_MAX) ? removeObserver(id, observerId) : false;
    }

    void clearObservers(uint16_t topicId) {
        if (topicId >= MC_FAST_MAX_TOPICS) return;
        TopicSlot* slot = findTopic(topicId);
        if (!slot) return;
        std::lock_guard<std::mutex> lock(m_observerMutex);
        slot->count.store(0, std::memory_order_release);
        slot->list.store(nullptr, std::memory_order_release);
    }

    void clearObservers(const std::string& topic) {
        uint16_t id = resolveTopic(topic);
        if (id != UINT16_MAX) clearObservers(id);
    }

    bool post(uint16_t topicId, const void* data, size_t size) {
        if (MC_UNLIKELY(topicId >= MC_FAST_MAX_TOPICS)) return false;

        TopicSlot* slot = findTopic(topicId);
        if (!slot || slot->count.load(std::memory_order_acquire) == 0)
            return true;

        if (MC_UNLIKELY(size > MC_FAST_INLINE_SIZE) && m_queue.full() &&
            (!m_running.load(std::memory_order_relaxed) ||
             m_stopping.load(std::memory_order_relaxed))) {
            return false;
        }

        MessageFast* msg = m_pool.acquire();
        if (MC_UNLIKELY(msg == nullptr)) return false;

        msg->topicId = topicId;
        msg->size = static_cast<uint32_t>(size);

        if (MC_LIKELY(size <= MC_FAST_INLINE_SIZE)) {
            msg->flags = 0;
            if (MC_LIKELY(data != nullptr && size > 0)) {
                memcpy(msg->inlineData, data, size);
            } else if (MC_UNLIKELY(size > 0)) {
                mc_detail::zeroMemoryCold(msg->inlineData, size);
            }
        } else {
            msg->flags = 1;
            msg->managedData = malloc(size);
            if (MC_UNLIKELY(!msg->managedData)) {
                msg->flags = 0;
                m_pool.release(msg);
                return false;
            }
            if (MC_LIKELY(data != nullptr)) {
                memcpy(msg->managedData, data, size);
            } else {
                mc_detail::zeroMemoryCold(msg->managedData, size);
            }
        }

        if (MC_UNLIKELY(!m_queue.try_push(msg))) {
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
        if (MC_UNLIKELY(m_sleeping.load(std::memory_order_seq_cst))) {
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

    // Non-blocking variant: returns false immediately if the ring buffer is full
    bool tryPost(uint16_t topicId, const void* data, size_t size) {
        if (MC_UNLIKELY(topicId >= MC_FAST_MAX_TOPICS)) return false;

        TopicSlot* slot = findTopic(topicId);
        if (!slot || slot->count.load(std::memory_order_acquire) == 0)
            return true;

        if (MC_UNLIKELY(m_queue.full())) return false;

        MessageFast* msg = m_pool.acquire();
        if (MC_UNLIKELY(msg == nullptr)) return false;

        msg->topicId = topicId;
        msg->size = static_cast<uint32_t>(size);

        if (MC_LIKELY(size <= MC_FAST_INLINE_SIZE)) {
            msg->flags = 0;
            if (MC_LIKELY(data != nullptr && size > 0)) {
                memcpy(msg->inlineData, data, size);
            } else if (MC_UNLIKELY(size > 0)) {
                mc_detail::zeroMemoryCold(msg->inlineData, size);
            }
        } else {
            msg->flags = 1;
            msg->managedData = malloc(size);
            if (MC_UNLIKELY(!msg->managedData)) {
                msg->flags = 0;
                m_pool.release(msg);
                return false;
            }
            if (MC_LIKELY(data != nullptr)) {
                memcpy(msg->managedData, data, size);
            } else {
                mc_detail::zeroMemoryCold(msg->managedData, size);
            }
        }

        if (MC_UNLIKELY(!m_queue.try_push(msg))) {
            msg->reset();
            m_pool.release(msg);
            return false;
        }

        m_posted.fetch_add(1, std::memory_order_seq_cst);
        if (MC_UNLIKELY(m_sleeping.load(std::memory_order_seq_cst))) {
            { std::lock_guard<std::mutex> lock(m_cvMutex); }
            m_cv.notify_one();
        }
        return true;
    }

    bool tryPost(const std::string& topic, const void* data, size_t size) {
        uint16_t id = resolveTopic(topic);
        if (id == UINT16_MAX) {
            id = registerTopic(topic);
        }
        return tryPost(id, data, size);
    }

    template <typename T>
    bool post(uint16_t topicId, const T& value) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "T must be trivially copyable");
        static_assert(!std::is_pointer<T>::value,
                      "Do not pass a pointer to typed post(); pass the value by reference or use post(topicId, ptr, size)");
        return post(topicId, &value, sizeof(T));
    }

    template <typename T>
    bool post(const std::string& topic, const T& value) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "T must be trivially copyable");
        static_assert(!std::is_pointer<T>::value,
                      "Do not pass a pointer to typed post(); pass the value by reference or use post(topic, ptr, size)");
        return post(topic, &value, sizeof(T));
    }

    template <typename T>
    bool tryPost(uint16_t topicId, const T& value) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "T must be trivially copyable");
        static_assert(!std::is_pointer<T>::value,
                      "Do not pass a pointer to typed tryPost(); pass the value by reference or use tryPost(topicId, ptr, size)");
        return tryPost(topicId, &value, sizeof(T));
    }

    template <typename T>
    bool tryPost(const std::string& topic, const T& value) {
        static_assert(std::is_trivially_copyable<T>::value,
                      "T must be trivially copyable");
        static_assert(!std::is_pointer<T>::value,
                      "Do not pass a pointer to typed tryPost(); pass the value by reference or use tryPost(topic, ptr, size)");
        return tryPost(topic, &value, sizeof(T));
    }

    bool post(uint16_t topicId) {
        return post(topicId, nullptr, 0);
    }

    bool tryPost(uint16_t topicId) {
        return tryPost(topicId, nullptr, 0);
    }

    uint64_t pending() const {
        return m_posted.load(std::memory_order_relaxed) - m_dispatched.load(std::memory_order_relaxed);
    }

    uint16_t topicCount() const {
        return m_nextTopicId.load(std::memory_order_relaxed);
    }

    size_t observerCount(uint16_t topicId) const {
        if (topicId >= MC_FAST_MAX_TOPICS) return 0;
        TopicSlot* slot = findTopic(topicId);
        if (!slot) return 0;
        const ObserverList* obs = slot->list.load(std::memory_order_acquire);
        return obs ? obs->size() : 0;
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
    MC_FORCE_INLINE void dispatchBatchMessages(MessageFast** batch, size_t batchSize) {
        uint16_t cachedTopicId = UINT16_MAX;
        const ObserverList* cachedSnapshot = nullptr;
        for (size_t i = 0; i < batchSize; ++i) {
            MessageFast* msg = batch[i];
            if (MC_LIKELY(msg != nullptr)) {
                uint16_t tid = msg->topicId;
                if (MC_UNLIKELY(tid != cachedTopicId)) {
                    cachedTopicId = tid;
                    TopicSlot* slot = (tid < MC_FAST_MAX_TOPICS) ? findTopic(tid) : nullptr;
                    cachedSnapshot = slot ? slot->list.load(std::memory_order_acquire) : nullptr;
                }
                if (cachedSnapshot) {
                    const void* data = msg->data();
                    size_t size = msg->size;
                    for (const auto& entry : *cachedSnapshot) {
                        if (entry.callback) {
                            entry.callback(tid, data, size);
                        }
                    }
                }
                msg->reset();
            }
        }
    }

    void dispatchLoop() {
        static constexpr size_t WORKER_BATCH_SIZE = 32;
        static constexpr size_t RECYCLE_BATCH_SIZE = 32;
        MessageFast* batch[WORKER_BATCH_SIZE];
        MessageFast* recycleBuf[RECYCLE_BATCH_SIZE];
        size_t recycleCount = 0;
        int emptyCount = 0;

        while (!m_stopping.load(std::memory_order_acquire)) {
            size_t batchSize = m_queue.try_pop_batch(batch, WORKER_BATCH_SIZE);

            if (batchSize > 0) {
                dispatchBatchMessages(batch, batchSize);
                m_dispatched.fetch_add(batchSize, std::memory_order_relaxed);
                for (size_t i = 0; i < batchSize; ++i) {
                    recycleBuf[recycleCount++] = batch[i];
                    if (MC_UNLIKELY(recycleCount == RECYCLE_BATCH_SIZE)) {
                        m_pool.releaseBatch(recycleBuf, recycleCount);
                        recycleCount = 0;
                    }
                }
                emptyCount = 0;
            } else {
                emptyCount++;
                if (emptyCount < 100) {
                    std::this_thread::yield();
                } else {
                    if (recycleCount > 0) {
                        m_pool.releaseBatch(recycleBuf, recycleCount);
                        recycleCount = 0;
                    }
                    std::unique_lock<std::mutex> lock(m_cvMutex);
                    m_sleeping.store(true, std::memory_order_seq_cst);
                    m_cv.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                        return m_stopping.load(std::memory_order_relaxed) || !m_queue.empty();
                    });
                    m_sleeping.store(false, std::memory_order_relaxed);
                }
            }
        }

        if (recycleCount > 0) {
            m_pool.releaseBatch(recycleBuf, recycleCount);
            recycleCount = 0;
        }

        for (;;) {
            size_t batchSize = m_queue.try_pop_batch(batch, WORKER_BATCH_SIZE);
            if (batchSize == 0)
                break;
            dispatchBatchMessages(batch, batchSize);
            m_pool.releaseBatch(batch, batchSize);
            m_dispatched.fetch_add(batchSize, std::memory_order_relaxed);
        }
    }

    MC_FORCE_INLINE void dispatch(MessageFast* msg) {
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
