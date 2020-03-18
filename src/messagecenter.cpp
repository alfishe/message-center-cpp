#include "messagecenter.h"

// Definition for static field(s)
MessageCenter* MessageCenter::m_instance = nullptr;

#ifdef _CODE_UNDER_TEST
MessageCenterCUT* MessageCenterCUT::m_instanceCUT = nullptr;
#endif // _CODE_UNDER_TEST

MessageCenter::MessageCenter()
{
    m_started = false;
    m_requestStop = false;
    m_stopped = true;
}

MessageCenter::~MessageCenter()
{
    if (m_thread != nullptr)
    {
        if (m_thread->joinable())
            m_thread->join();

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
    if (m_started.load())
    {
        // Already started
        return;
    }

    // Lock mutex until leaving the scope
    std::lock_guard<std::mutex> lock(m_mutexThreads);

    m_requestStop = false;
    m_stopped = false;

    m_thread = new std::thread(&MessageCenter::ThreadWorker, this);

    m_started = true;
}

void MessageCenter::Stop()
{
    if (m_stopped.load())
    {
        // Thread already stopped
        return;
    }

    // Lock mutex until leaving the scope
    std::lock_guard<std::mutex> lock(m_mutexThreads);

#ifdef _DEBUG
    std::cout << "MessageCenter::Stop - requesting thread stop..." << std::endl;
#endif // _DEBUG

    m_requestStop = true;

    if (m_thread != nullptr)
    {
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
#ifdef _DEBUG
    std::cout << "MessageCenter thread started" << std::endl;
#endif // _DEBUG

    while (true)
    {
        if (m_requestStop.load())
        {
            // Thread is being requested to stop
            break;
        }

        Message* message = GetMessage();
        if (message != nullptr)
        {
            Dispatch(message->tid, message);
        }
        else
        {
            // Wait for new messages if queue is empty, but no more than 50ms
            std::unique_lock<std::mutex> lock(m_mutexMessages);
            m_cvEvents.wait_for(lock, std::chrono::milliseconds(50));
            lock.unlock();
        }
    }

#ifdef _DEBUG
    std::cout << "MessageCenter thread finishing..." << std::endl;
#endif // _DEBUG

    m_stopped = true;
}