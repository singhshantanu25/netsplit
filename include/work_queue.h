#pragma once
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

#include "planner.h"

// Thread-safe queue of chunks (producer-consumer).
// Many workers call pop() at the same time; only one gets each chunk.
class WorkQueue {
public:
    void push(const Chunk& c) {
        {
            std::lock_guard<std::mutex> lock(m_);
            q_.push(c);
        }
        cv_.notify_one();   // wake one sleeping worker
    }

    // No more chunks will be added. Workers exit once the queue is empty.
    void close() {
        {
            std::lock_guard<std::mutex> lock(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    // Something failed: drop all remaining work so every worker stops.
    void cancel() {
        {
            std::lock_guard<std::mutex> lock(m_);
            std::queue<Chunk>().swap(q_);   // empty the queue
            closed_ = true;
        }
        cv_.notify_all();
    }

    // Waits until a chunk is available.
    // Returns an empty optional when the queue is closed AND empty (time to stop).
    std::optional<Chunk> pop() {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return !q_.empty() || closed_; });
        if (q_.empty()) return std::nullopt;
        Chunk c = q_.front();
        q_.pop();
        return c;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<Chunk> q_;
    bool closed_ = false;
};