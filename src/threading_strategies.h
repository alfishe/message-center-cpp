#pragma once

#ifndef MESSAGE_CENTER_THREADING_STRATEGIES_H
#define MESSAGE_CENTER_THREADING_STRATEGIES_H

#include "eventqueue_pooled.h"
#include "queuestats.h"
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

/// region <Threading Strategies>

/// Base class for threading strategies
class ThreadingStrategy {
protected:
  EventQueuePooled &m_queue;
  std::atomic<bool> m_running{false};
  std::vector<std::thread> m_workers;
  QueueStats m_stats;

public:
  explicit ThreadingStrategy(EventQueuePooled &queue, int statsWindowMs = 1000)
      : m_queue(queue), m_stats(statsWindowMs) {}

  virtual ~ThreadingStrategy() { stop(); }

  virtual void start() = 0;
  virtual void stop() {
    m_running = false;
    for (auto &worker : m_workers) {
      if (worker.joinable())
        worker.join();
    }
    m_workers.clear();
  }

  virtual std::string getName() const = 0;
  virtual int getThreadCount() const {
    return static_cast<int>(m_workers.size());
  }

  const QueueStats &getStats() const { return m_stats; }
  QueueStats &getStats() { return m_stats; }
};

/// Strategy 1: Single-threaded event loop
class SingleThreadStrategy : public ThreadingStrategy {
public:
  explicit SingleThreadStrategy(EventQueuePooled &queue,
                                int statsWindowMs = 1000)
      : ThreadingStrategy(queue, statsWindowMs) {}

  void start() override {
    m_running = true;
    m_workers.emplace_back([this]() {
      while (m_running) {
        Message *msg = m_queue.GetQueueMessage();
        if (msg) {
          m_stats.recordDequeue();
          m_queue.Dispatch(msg->tid, msg);
        } else {
          // No message - yield to avoid busy-wait
          std::this_thread::yield();
        }
      }
    });
  }

  std::string getName() const override { return "SingleThread"; }
};

/// Strategy 2: Fixed thread pool
class FixedThreadPoolStrategy : public ThreadingStrategy {
private:
  int m_numThreads;

public:
  FixedThreadPoolStrategy(EventQueuePooled &queue, int numThreads,
                          int statsWindowMs = 1000)
      : ThreadingStrategy(queue, statsWindowMs), m_numThreads(numThreads) {}

  void start() override {
    m_running = true;
    for (int i = 0; i < m_numThreads; i++) {
      m_workers.emplace_back([this]() {
        while (m_running) {
          Message *msg = m_queue.GetQueueMessage();
          if (msg) {
            m_stats.recordDequeue();
            m_queue.Dispatch(msg->tid, msg);
          } else {
            std::this_thread::yield();
          }
        }
      });
    }
  }

  std::string getName() const override {
    return "FixedPool_" + std::to_string(m_numThreads);
  }
};

/// Strategy 3: Dynamic thread pool (adaptive)
class DynamicThreadPoolStrategy : public ThreadingStrategy {
private:
  int m_minThreads;
  int m_maxThreads;
  int m_scaleUpThreshold;
  int m_scaleDownThreshold;
  std::atomic<int> m_activeThreads{0};
  std::thread m_monitorThread;

public:
  DynamicThreadPoolStrategy(EventQueuePooled &queue, int minThreads = 1,
                            int maxThreads = 8, int scaleUpThreshold = 100,
                            int scaleDownThreshold = 10,
                            int statsWindowMs = 1000)
      : ThreadingStrategy(queue, statsWindowMs), m_minThreads(minThreads),
        m_maxThreads(maxThreads), m_scaleUpThreshold(scaleUpThreshold),
        m_scaleDownThreshold(scaleDownThreshold) {}

  void start() override {
    m_running = true;

    // Start minimum threads
    for (int i = 0; i < m_minThreads; i++) {
      addWorker();
    }

    // Start monitoring thread
    m_monitorThread = std::thread([this]() {
      while (m_running) {
        size_t queueDepth = m_queue.GetQueueSize();
        int currentThreads = m_activeThreads.load();

        // Scale up if queue is growing
        if (queueDepth > static_cast<size_t>(m_scaleUpThreshold) &&
            currentThreads < m_maxThreads) {
          addWorker();
        }

        // Scale down if queue is idle (simplified - just don't add more)
        // Full implementation would gracefully stop threads

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    });
  }

  void stop() override {
    m_running = false;
    if (m_monitorThread.joinable())
      m_monitorThread.join();
    ThreadingStrategy::stop();
  }

  std::string getName() const override {
    return "Dynamic_" + std::to_string(m_minThreads) + "-" +
           std::to_string(m_maxThreads);
  }

  int getThreadCount() const override { return m_activeThreads.load(); }

private:
  void addWorker() {
    m_workers.emplace_back([this]() {
      m_activeThreads++;

      while (m_running) {
        Message *msg = m_queue.GetQueueMessage();
        if (msg) {
          m_stats.recordDequeue();
          m_queue.Dispatch(msg->tid, msg);
        } else {
          std::this_thread::yield();
        }
      }

      m_activeThreads--;
    });
  }
};

/// endregion </Threading Strategies>

#endif // MESSAGE_CENTER_THREADING_STRATEGIES_H
