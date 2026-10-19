#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

// Live progress.
// Workers only call add(): one atomic operation, no lock, very cheap.
// A separate printer thread wakes up every second and prints MB done and speed.
class Progress {
public:
    // already = bytes finished in an earlier run (when resuming).
    explicit Progress(std::uint64_t total, std::uint64_t already = 0)
        : total_(total), done_(already) {}
    ~Progress() { stop(); }   // RAII: never leave the printer thread running

    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;

    void add(std::uint64_t bytes) { done_.fetch_add(bytes); }
    void sub(std::uint64_t bytes) { done_.fetch_sub(bytes); }  
    std::uint64_t done() const { return done_.load(); }

    void start() {
        printer_ = std::thread([this] { print_loop(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_);
            stopping_ = true;
        }
        cv_.notify_all();   // wake the printer right away
        if (printer_.joinable()) {
            printer_.join();
            std::cout << "\n";
        }
    }

private:
    void print_loop() {
        using clock = std::chrono::steady_clock;
        auto last_time = clock::now();
        std::uint64_t last_bytes = done_.load();   // speed counts only new bytes

        std::unique_lock<std::mutex> lock(m_);
        while (true) {
            // Sleep up to 1 second, but wake immediately if stop() is called.
            bool stop_now = cv_.wait_for(lock, std::chrono::seconds(1),
                                         [&] { return stopping_; });
            auto now = clock::now();
            std::uint64_t bytes = done_.load();
            double secs = std::chrono::duration<double>(now - last_time).count();
            // bytes can go DOWN (a failed or paused chunk is taken back out), and
            // unsigned subtraction would then wrap around to a huge number.
            double speed = (secs > 0 && bytes >= last_bytes)
                               ? (bytes - last_bytes) / secs / kMB : 0.0;
            print_line(bytes, speed);
            last_time = now;
            last_bytes = bytes;
            if (stop_now) break;
        }
    }

    void print_line(std::uint64_t bytes, double speed) const {
        int percent = total_ ? static_cast<int>(bytes * 100 / total_) : 100;
        std::ostringstream line;
        line << std::fixed << std::setprecision(1)
             << "\r" << bytes / kMB << " / " << total_ / kMB << " MB ("
             << percent << "%)  " << speed << " MB/s     ";
        std::cout << line.str() << std::flush;
    }

    static constexpr double kMB = 1024.0 * 1024.0;

    const std::uint64_t total_;
    std::atomic<std::uint64_t> done_{0};   // shared by all workers
    std::thread printer_;
    std::mutex m_;                         // only for sleeping/waking the printer
    std::condition_variable cv_;
    bool stopping_ = false;
};