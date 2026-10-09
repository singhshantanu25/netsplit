#pragma once
#include <iostream>
#include <mutex>

// Thread-safe progress line. Several workers call add() at the same time.
class Progress {
public:
    explicit Progress(long long total) : total_(total) {}

    void add(long long bytes) {
        std::lock_guard<std::mutex> lock(m_);   // done_ is shared
        done_ += bytes;
        int percent = static_cast<int>(done_ * 100 / total_);
        if (percent != last_percent_) {
            last_percent_ = percent;
            std::cout << "\r" << done_ << " / " << total_ << " bytes ("
                      << percent << "%)" << std::flush;
        }
    }

    void finish() {
        std::lock_guard<std::mutex> lock(m_);
        std::cout << "\n";
    }

private:
    std::mutex m_;
    long long total_;
    long long done_ = 0;
    int last_percent_ = -1;
};