#pragma once

#ifndef MESSAGE_CENTER_QUEUE_STATS_H
#define MESSAGE_CENTER_QUEUE_STATS_H

#include <algorithm>
#include <atomic>
#include <chrono>

/// region <Queue Statistics>

/// Tracks queue depth and performance metrics over time windows
/// Resets statistics periodically to show current behavior, not cumulative
class QueueStats {
private:
  // Current depth
  std::atomic<size_t> m_currentDepth{0};

  // Statistics for current window
  std::atomic<size_t> m_maxDepth{0};
  std::atomic<size_t> m_totalEnqueues{0};
  std::atomic<size_t> m_totalDequeues{0};

  // Timing
  std::chrono::steady_clock::time_point m_windowStart;
  std::chrono::milliseconds m_windowDuration;

public:
  /// Constructor
  /// @param windowDurationMs - Statistics window duration in milliseconds
  /// (default: 1000ms)
  explicit QueueStats(int windowDurationMs = 1000)
      : m_windowDuration(windowDurationMs),
        m_windowStart(std::chrono::steady_clock::now()) {}

  /// Record an enqueue operation
  void recordEnqueue() {
    size_t newDepth =
        m_currentDepth.fetch_add(1, std::memory_order_relaxed) + 1;
    m_totalEnqueues.fetch_add(1, std::memory_order_relaxed);

    // Update max depth
    size_t currentMax = m_maxDepth.load(std::memory_order_relaxed);
    while (newDepth > currentMax &&
           !m_maxDepth.compare_exchange_weak(currentMax, newDepth,
                                             std::memory_order_relaxed)) {
      // Retry if another thread updated max
    }
  }

  /// Record a dequeue operation
  void recordDequeue() {
    m_currentDepth.fetch_sub(1, std::memory_order_relaxed);
    m_totalDequeues.fetch_add(1, std::memory_order_relaxed);
  }

  /// Get current queue depth
  size_t getCurrentDepth() const {
    return m_currentDepth.load(std::memory_order_relaxed);
  }

  /// Get maximum depth in current window
  size_t getMaxDepth() const {
    return m_maxDepth.load(std::memory_order_relaxed);
  }

  /// Get total enqueues in current window
  size_t getTotalEnqueues() const {
    return m_totalEnqueues.load(std::memory_order_relaxed);
  }

  /// Get total dequeues in current window
  size_t getTotalDequeues() const {
    return m_totalDequeues.load(std::memory_order_relaxed);
  }

  /// Get average depth in current window
  double getAverageDepth() const {
    size_t enqueues = m_totalEnqueues.load(std::memory_order_relaxed);
    size_t dequeues = m_totalDequeues.load(std::memory_order_relaxed);

    if (enqueues == 0)
      return 0.0;

    // Approximate average as (total enqueued - total dequeued) / 2
    // This is a rough estimate; for precise average, we'd need to sample
    // periodically
    return static_cast<double>(enqueues + dequeues) / 2.0;
  }

  /// Check if window has expired and should be reset
  bool shouldReset() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_windowStart);
    return elapsed >= m_windowDuration;
  }

  /// Reset statistics for new window
  void reset() {
    m_maxDepth.store(m_currentDepth.load(std::memory_order_relaxed),
                     std::memory_order_relaxed);
    m_totalEnqueues.store(0, std::memory_order_relaxed);
    m_totalDequeues.store(0, std::memory_order_relaxed);
    m_windowStart = std::chrono::steady_clock::now();
  }

  /// Get window duration in milliseconds
  int getWindowDuration() const {
    return static_cast<int>(m_windowDuration.count());
  }

  /// Get elapsed time in current window (milliseconds)
  int getElapsedMs() const {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_windowStart);
    return static_cast<int>(elapsed.count());
  }

  /// Get throughput (messages/second) for current window
  double getThroughput() const {
    int elapsedMs = getElapsedMs();
    if (elapsedMs == 0)
      return 0.0;

    size_t total = m_totalEnqueues.load(std::memory_order_relaxed);
    return (total * 1000.0) / elapsedMs;
  }
};

/// endregion </Queue Statistics>

#endif // MESSAGE_CENTER_QUEUE_STATS_H
