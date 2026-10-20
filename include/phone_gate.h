#pragma once
#include <condition_variable>
#include <mutex>

// Smart mode (--smart): phone workers wait at this gate until main decides the
// phone is really needed (Wi-Fi alone would be too slow). Main also opens it when
// the download is finished or Ctrl+C is pressed, so waiting workers can exit.
class PhoneGate {
public:
    // Returns true only for the call that actually opened it.
    bool open() {
        {
            std::lock_guard<std::mutex> lock(m_);
            if (open_) return false;
            open_ = true;
        }
        cv_.notify_all();   // wake every waiting phone worker
        return true;
    }

    void wait() {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return open_; });
    }

    bool is_open() {
        std::lock_guard<std::mutex> lock(m_);
        return open_;
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    bool open_ = false;
};
