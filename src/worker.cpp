#include "worker.h"

#include <stdexcept>

long long download_chunk(HttpClient& client, const std::string& url,
                         const Chunk& c, Storage& out, Progress* progress) {
    long long received = 0;
    Response r = client.get_stream(url, c.range(),
        [&](const char* data, std::size_t len) {
            if (received + static_cast<long long>(len) > c.size()) return false;
            out.write_at(c.start + received, data, len);
            received += static_cast<long long>(len);
            if (progress) progress->add(static_cast<long long>(len));
            return true;
        });

    if (r.status != 206)
        throw std::runtime_error("chunk " + std::to_string(c.index) +
                                 ": expected HTTP 206, got " + std::to_string(r.status));
    if (received != c.size())
        throw std::runtime_error("chunk " + std::to_string(c.index) + ": got " +
                                 std::to_string(received) + " bytes, expected " +
                                 std::to_string(c.size()));
    return received;
}

Worker::Worker(int id, const std::string& url, WorkQueue& queue, Storage& out,
               Progress& progress, bool reuse)
    : id_(id), url_(url), queue_(queue), out_(out), progress_(progress) {
    client_.set_reuse(reuse);
}

void Worker::run() {
    try {
        while (auto c = queue_.pop()) {   // stops when pop() returns nothing
            bytes_done_ += download_chunk(client_, url_, *c, out_, &progress_);
            ++chunks_done_;
        }
    } catch (...) {
        // An exception must never escape a thread (that calls std::terminate).
        // Save it for main, and stop the other workers.
        error_ = std::current_exception();
        queue_.cancel();
    }
}