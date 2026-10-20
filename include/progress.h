#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// Live progress.
// Workers only call add(): one atomic operation, no lock, very cheap.
// A separate printer thread wakes up every second and prints MB done and speed,
// plus the speed of each network ("en0 0.5 MB/s  en5 10.7 MB/s").
class Progress {
public:
    // already = bytes finished in an earlier run (when resuming).
    explicit Progress(std::uint64_t total, std::uint64_t already = 0)
        : total_(total), done_(already) {}
    ~Progress() { stop(); }   // RAII: never leave the printer thread running

    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;

    void add(std::uint64_t bytes) { done_.fetch_add(bytes); }
    void sub(std::uint64_t bytes) { done_.fetch_sub(bytes); }   // undo a failed attempt
    std::uint64_t done() const { return done_.load(); }

    // Show this counter's speed on the progress line too. Call before start().
    // The counter only grows (bytes received on one network).
    // down (optional): when true, the line shows "DOWN" instead of a speed.
    void add_source(const std::string& name, const std::atomic<std::uint64_t>* counter,
                    const std::atomic<bool>* down = nullptr) {
        sources_.push_back(Source{name, counter, down, counter->load()});
    }

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
    struct Source {
        std::string name;
        const std::atomic<std::uint64_t>* counter;
        const std::atomic<bool>* down;
        std::uint64_t last;   // value at the previous tick
    };

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

            std::ostringstream extra;   // "  | en0 0.5 MB/s  en5 10.7 MB/s"
            extra << std::fixed << std::setprecision(1);
            for (std::size_t i = 0; i < sources_.size(); ++i) {
                Source& s = sources_[i];
                std::uint64_t now_bytes = s.counter->load();
                double v = secs > 0 ? (now_bytes - s.last) / secs / kMB : 0.0;
                s.last = now_bytes;
                extra << (i == 0 ? "  | " : "  ") << s.name << " ";
                if (s.down && s.down->load())
                    extra << "DOWN";
                else
                    extra << v << " MB/s";
            }

            print_line(bytes, speed, extra.str());
            last_time = now;
            last_bytes = bytes;
            if (stop_now) break;
        }
    }

    void print_line(std::uint64_t bytes, double speed, const std::string& extra) const {
        int percent = total_ ? static_cast<int>(bytes * 100 / total_) : 100;
        std::ostringstream line;
        line << std::fixed << std::setprecision(1)
             << "\r" << bytes / kMB << " / " << total_ / kMB << " MB ("
             << percent << "%)  " << speed << " MB/s" << extra << "     ";
        std::cout << line.str() << std::flush;
    }

    static constexpr double kMB = 1024.0 * 1024.0;

    const std::uint64_t total_;
    std::atomic<std::uint64_t> done_{0};   // shared by all workers
    std::vector<Source> sources_;          // only touched by the printer thread after start()
    std::thread printer_;
    std::mutex m_;                         // only for sleeping/waking the printer
    std::condition_variable cv_;
    bool stopping_ = false;
};
