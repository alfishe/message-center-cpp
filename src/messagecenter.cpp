#include "messagecenter.h"

#include <cassert>

#ifdef _WIN32
    // windows.h cannot be included from function => C2958 linking and many other errors
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN 1
    #endif
    #define _CRT_SECURE_NO_WARNINGS 1
    #include <windows.h>
    #include <stdlib.h>
#endif

#if defined(__APPLE__) || defined(__linux__)
    #include <pthread.h>
#endif

// Definition for static field(s)
MessageCenter* MessageCenter::m_instance = nullptr;

#ifdef _CODE_UNDER_TEST
MessageCenterCUT* MessageCenterCUT::m_instanceCUT = nullptr;
#endif // _CODE_UNDER_TEST

MessageCenter::MessageCenter()
{
    m_thread = nullptr;
    m_started = false;
    m_requestStop = false;
    m_stopped = true;
}

MessageCenter::~MessageCenter()
{
    if (m_thread != nullptr)
    {
        // Destruction from the worker thread (inside an observer callback) would leave
        // the worker running on a destroyed object - fail loudly instead of corrupting memory.
        assert(m_thread->get_id() != std::this_thread::get_id() &&
               "MessageCenter must not be destroyed from its own observer callback");

        if (m_thread->joinable())
        {
            m_requestStop = true;
            {
                std::lock_guard<std::mutex> lock(m_mutexMessages);
            }
            m_cvEvents.notify_all();
            m_thread->join();
        }

        delete m_thread;
    }

    // No 'delete m_instance;' here! Singleton should be disposed from static method
}

MessageCenter& MessageCenter::DefaultMessageCenter(bool autostart)
{
    if (m_instance == nullptr)
    {
        m_instance = new MessageCenter();

        if (autostart)
        {
            m_instance->Start();
        }
    }

    return *m_instance;
}

void MessageCenter::DisposeDefaultMessageCenter()
{
    if (m_instance != nullptr)
    {
        m_instance->Stop();

        delete m_instance;
        m_instance = nullptr;
    }
}

void MessageCenter::Start()
{
    // Lock mutex until leaving the scope
    std::lock_guard<std::mutex> lock(m_mutexThreads);

    if (m_started.load())
    {
        if (!m_requestStop.load() && !m_stopped.load())
        {
            // Already started and running
            return;
        }
        // Clean up deferred self-stop before starting a new worker thread
        if (m_thread != nullptr)
        {
            if (m_thread->get_id() == std::this_thread::get_id())
            {
                return;
            }
            if (m_thread->joinable())
            {
                m_thread->join();
            }
            delete m_thread;
            m_thread = nullptr;
        }
    }

    m_requestStop = false;
    m_stopped = false;

    m_thread = new std::thread(&MessageCenter::ThreadWorker, this);

    m_started = true;
}

void MessageCenter::Stop()
{
    if (m_stopped.load() && !m_started.load())
    {
        // Thread already stopped and joined
        return;
    }

    // Lock mutex until leaving the scope
    std::lock_guard<std::mutex> lock(m_mutexThreads);

    if (m_stopped.load() && !m_started.load())
    {
        return;
    }

#ifdef _DEBUG
    std::cout << "MessageCenter::Stop - requesting thread stop..." << std::endl;
#endif // _DEBUG

    m_requestStop = true;
    {
        // Serialize with the worker's predicate check to avoid a lost wakeup
        std::lock_guard<std::mutex> lockMessages(m_mutexMessages);
    }
    m_cvEvents.notify_all();

    if (m_thread != nullptr)
    {
        if (m_thread->get_id() == std::this_thread::get_id())
        {
            // Called from an observer callback on the worker thread: stop is only requested.
            // The worker exits after the current callback; the thread is joined by the next
            // Stop()/destructor call from another thread.
            return;
        }

        if (m_thread->joinable())
        {
            m_thread->join();
        }

        delete m_thread;
        m_thread = nullptr;

#ifdef _DEBUG
        std::cout << "MessageCenter thread stopped..." << std::endl;
#endif // _DEBUG
    }

    m_started = false;
}

// Thread worker method
void MessageCenter::ThreadWorker()
{
    const char* threadName = "message_center_worker";

#ifdef __APPLE__
    pthread_setname_np(threadName);
#endif
#ifdef __linux__
    pthread_setname_np(pthread_self(), threadName);
#endif
#ifdef _WIN32
    using SetThreadDescriptionFunc = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static auto setThreadDescription = reinterpret_cast<SetThreadDescriptionFunc>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandle("kernelbase.dll"), "SetThreadDescription")));
    if (setThreadDescription != nullptr)
    {
        wchar_t wname[32];
        size_t retval;
        mbstowcs_s(&retval, wname, 32, threadName, 31);
        setThreadDescription(GetCurrentThread(), wname);
    }
#endif

#ifdef _DEBUG
    std::cout << "MessageCenter thread started" << std::endl;
#endif // _DEBUG

    MessageQueue localBatch;
    int emptyCount = 0;

    while (true)
    {
        if (m_requestStop.load())
        {
            // Thread is being requested to stop
            break;
        }

        // Optional spin phase (MESSAGECENTER_WORKER_SPIN, default 0 = disabled).
        // Polls a lock-free hint, so the worker never contends on m_mutexMessages with producers.
        // Spinning lowers request/response latency only when the next message arrives within the
        // spin window, but it makes the worker pick messages up one by one during bursts, which
        // roughly doubled per-post producer cost in burst benchmarks - hence opt-in.
        if (emptyCount < MESSAGECENTER_WORKER_SPIN && !m_pendingMessages.load(std::memory_order_acquire))
        {
            emptyCount++;
            std::this_thread::yield();
            continue;
        }

        {
            std::unique_lock<std::mutex> lock(m_mutexMessages);
            if (m_messageQueue.empty())
            {
                m_workerSleeping.store(true, std::memory_order_seq_cst);
                m_cvEvents.wait_for(lock, std::chrono::milliseconds(50), [this]() {
                    return m_requestStop.load() || !m_messageQueue.empty();
                });
                m_workerSleeping.store(false, std::memory_order_relaxed);
            }

            if (!m_messageQueue.empty())
            {
                localBatch.swap(m_messageQueue);
            }
            m_pendingMessages.store(false, std::memory_order_relaxed);
            emptyCount = 0;
        }

        while (!localBatch.empty())
        {
            Message* message = localBatch.front();
            localBatch.pop_front();
            if (message != nullptr)
            {
                Dispatch(message->tid, message);
            }

            if (m_requestStop.load())
            {
                break;
            }
        }
    }

    if (!localBatch.empty())
    {
        std::lock_guard<std::mutex> lock(m_mutexMessages);
        m_messageQueue.insert(m_messageQueue.begin(), localBatch.begin(), localBatch.end());
        localBatch.clear();
    }

#ifdef _DEBUG
    std::cout << "MessageCenter thread finishing..." << std::endl;
#endif // _DEBUG

    m_stopped = true;
}