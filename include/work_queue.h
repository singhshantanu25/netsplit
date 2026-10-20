#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <queue>

#include "planner.h"

// One piece of work: a chunk, plus how often it has failed so far.
struct Job {
    Chunk chunk;
    int failures = 0;        // all failed attempts (sets the backoff delay)
    int bad_responses = 0;   // failed attempts where the server answered wrongly
};

// Thread-safe queue of jobs (producer-consumer).
// Many workers call pop() at the same time; only one gets each job.
// A failed job can be put back with retry(), so the queue only says
// "finished" when it is empty AND no job is still being downloaded.
class WorkQueue {
public:
    void push(const Chunk& c) {
        {
            std::lock_guard<std::mutex> lock(m_);
            q_.push(Job{c});
            ++pending_;
        }
        cv_.notify_one();   // wake one sleeping worker
    }

    // No new chunks will be added (failed ones can still come back via retry).
    void close() {
        {
            std::lock_guard<std::mutex> lock(m_);
            closed_ = true;
        }
        cv_.notify_all();
    }

    // A job finished successfully.
    void done() {
        {
            std::lock_guard<std::mutex> lock(m_);
            --pending_;
        }
        cv_.notify_all();   // if that was the last one, waiting workers can exit
    }

    // A job failed: put it back so any worker can try it again.
    void retry(const Job& job) {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (cancelled_) return;
            q_.push(job);
        }
        cv_.notify_one();
    }

    // Something failed for good: drop all remaining work so every worker stops.
    void cancel() {
        {
            std::lock_guard<std::mutex> lock(m_);
            std::queue<Job>().swap(q_);   // empty the queue
            cancelled_ = true;
        }
        cv_.notify_all();
    }

    // True when every chunk is finished (closed, empty, nothing in flight).
    bool all_done() {
        std::lock_guard<std::mutex> lock(m_);
        return closed_ && pending_ == 0;
    }

    // Sleeps for d, but wakes up early if the download is cancelled.
    // Returns false if it was cancelled.
    bool wait_unless_cancelled(std::chrono::milliseconds d) {
        std::unique_lock<std::mutex> lock(m_);
        return !cv_.wait_for(lock, d, [&] { return cancelled_; });
    }

    // Waits until a job is available.
    // Returns an empty optional when there is nothing left to do
    // (closed, empty and nothing in flight) or when cancelled.
    std::optional<Job> pop() {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] {
            return !q_.empty() || cancelled_ || (closed_ && pending_ == 0);
        });
        if (cancelled_ || q_.empty()) return std::nullopt;
        Job job = q_.front();
        q_.pop();
        return job;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::queue<Job> q_;
    int pending_ = 0;   // pushed but not done yet (waiting OR being downloaded)
    bool closed_ = false;
    bool cancelled_ = false;
};