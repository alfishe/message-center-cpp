#include "messagecenter.h"

// Definition for static field(s)
MessageCenter* MessageCenter::m_instance = nullptr;

#ifdef _CODE_UNDER_TEST
MessageCenterCUT* MessageCenterCUT::m_instanceCUT = nullptr;
#endif // _CODE_UNDER_TEST

MessageCenter::MessageCenter()
{
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

MessageCenter& MessageCenter::DefaultMessageCenter()
{
    if (m_instance == nullptr)
    {
        m_instance = new MessageCenter();
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

    m_requestStop = false;
    m_stopped = false;

    m_thread = new std::thread(&MessageCenter::ThreadWorker, this);
}

void MessageCenter::Stop()
{
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

#ifdef _DEBUG
        std::cout << "MessageCenter thread stopped..." << std::endl;
#endif // _DEBUG
    }
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
            // Thread is requested to stop
            break;
        }
    }

#ifdef _DEBUG
    std::cout << "MessageCenter thread finishing..." << std::endl;
#endif // _DEBUG

    m_stopped = true;
}