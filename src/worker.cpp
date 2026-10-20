#include "worker.h"

#include "errors.h"
#include "stop.h"
#include "validate.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <stdexcept>

namespace {

constexpr int kMaxBadResponses = 5;   // wrong server answers before we give up

using Headers = std::map<std::string, std::string>;

// Wait time before the next try. failures = 1, 2, 3, ...
// Base: 1 s, 2 s, 4 s, 8 s, then 15 s max. Plus random jitter of up to half,
// so workers that failed together do not all retry at the same moment.
std::chrono::milliseconds backoff(int failures) {
    int shift = std::min(failures - 1, 4);              // never shift too far
    long long base = std::min(1000LL << shift, 15000LL);
    thread_local std::mt19937 rng{std::random_device{}()};   // one per thread, no lock
    std::uniform_int_distribution<long long> jitter(0, base / 2);
    return std::chrono::milliseconds(base + jitter(rng));
}

// Several workers print at once, so one line at a time.
std::mutex log_mutex;
void log_line(const std::string& line) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cerr << "\n" << line << "\n";
}

}  // namespace

long long download_chunk(HttpClient& client, const std::string& url,
                         const Chunk& c, Storage& out, Progress* progress,
                         const Expect& expect, Network* net, Reservation* mobile) {
    long long received = 0;

    auto on_headers = [&](long status, const Headers& h) {
        check_response(c, expect, status, h);   // runs before any byte is written
    };

    auto on_data = [&](const char* data, std::size_t len) {
        // Count EVERY byte that came over the phone, before anything else:
        // even if this attempt fails later, the phone has already used this data.
        if (mobile) mobile->consume(static_cast<std::uint64_t>(len));
        if (stop_requested()) return false;   // Ctrl+C: the last write is complete, stop here
        long long left = c.size() - received;
        if (left <= 0) return false;   // already full: anything more is extra
        long long take = std::min(static_cast<long long>(len), left);
        out.write_at(c.start + received, data, static_cast<std::size_t>(take));
        received += take;
        if (progress) progress->add(static_cast<std::uint64_t>(take));
        if (net) net->add_received(static_cast<std::uint64_t>(take));
        return take == static_cast<long long>(len);   // false = more than asked
    };

    try {
        Response r = client.get_stream(url, c.range(), on_data, on_headers);

        // Again after the transfer: covers answers with an empty body,
        // where the header check never ran.
        check_response(c, expect, r.status, r.headers);

        if (r.truncated && r.status == 206)
            throw BadResponse(chunk_name(c) + ": server sent more bytes than asked");
        if (received != c.size())
            throw BadResponse(chunk_name(c) + ": got " + std::to_string(received) +
                              " bytes, expected " + std::to_string(c.size()));
        return received;
    } catch (const ConnectionFailed& e) {
        // The retry will download this chunk again, so do not count it twice.
        if (progress) progress->sub(static_cast<std::uint64_t>(received));
        throw ConnectionFailed(chunk_name(c) + ": " + e.what());   // add the chunk name
    } catch (...) {
        if (progress) progress->sub(static_cast<std::uint64_t>(received));
        throw;
    }
}

Worker::Worker(int id, const std::string& url, const Expect& expect, WorkQueue& queue,
               Storage& out, Progress& progress, StateFile& state, bool reuse,
               Network& net, DataBudget* budget, PhoneGate* gate)
    : id_(id), url_(url), expect_(expect), queue_(queue), out_(out),
      progress_(progress), state_(state), net_(net), budget_(budget), gate_(gate) {
    client_.set_reuse(reuse);
    client_.set_interface(net.iface());   // bind the curl handle to this network
}

void Worker::run() {
    try {
        if (gate_) gate_->wait();   // smart mode: sleep until the phone is needed
        while (!stop_requested()) {   // Ctrl+C: take no new chunk
            auto job = queue_.pop();
            if (!job) break;          // nothing left (or cancelled)

            // Phone worker: may this chunk still fit in the mobile data limit?
            // try_reserve checks AND reserves under one lock.
            const std::uint64_t size = static_cast<std::uint64_t>(job->chunk.size());
            if (budget_ && !budget_->try_reserve(size)) {
                queue_.retry(*job);   // give it back: another network finishes it
                stopped_by_limit_ = true;
                log_line("  [worker " + std::to_string(id_) + " " + net_.label() +
                         "] mobile data limit reached (" +
                         std::to_string(budget_->used() / (1024 * 1024)) + " / " +
                         std::to_string(budget_->limit() / (1024 * 1024)) +
                         " MB): this worker stops, other networks finish the rest");
                break;
            }

            std::string why;
            try {
                long long got = 0;
                {
                    // Holds this chunk's reservation. Its destructor gives back the
                    // unused part when the block ends, also when an exception leaves it.
                    Reservation mobile(budget_, size);
                    got = download_chunk(client_, url_, job->chunk, out_, &progress_,
                                         expect_, &net_, &mobile);
                }
                bytes_done_ += got;
                ++chunks_done_;
                net_.add_kept(static_cast<std::uint64_t>(got));
                net_.add_chunk();
                state_.chunk_done(job->chunk.index, out_);   // record it (saved every 2 s)
                queue_.done();
                net_failures_ = 0;
                if (net_.set_down(false))   // it was down and works again
                    log_line("  " + net_.label() + " is back up");
                continue;
            } catch (const ConnectionFailed& e) {
                if (stop_requested()) break;   // we stopped it ourselves: no retry
                // The problem is THIS worker's network, not the chunk: give the chunk
                // back right now so a worker on another network can download it.
                queue_.retry(*job);
                ++retries_;
                ++net_failures_;
                if (!net_.set_down(true))     // first worker to notice reports it
                    log_line("  " + net_.label() + " is down (" + e.what() +
                             "): its chunks go back to the queue");
                auto delay = backoff(net_failures_);
                if (!queue_.wait_unless_cancelled(delay)) break;   // cancelled or Ctrl+C
                continue;   // try the network again with the next chunk
            } catch (const BadResponse& e) {
                if (stop_requested()) break;
                if (++job->bad_responses >= kMaxBadResponses)
                    throw BadResponse(std::string(e.what()) + " (gave up after " +
                                      std::to_string(kMaxBadResponses) + " tries)");
                why = e.what();
            }
            // Anything else (disk full, ...) is not caught here: it ends the download.

            // Wrong answer from the server: the chunk waits, then goes back to the queue.
            ++job->failures;
            ++retries_;
            auto delay = backoff(job->failures);
            log_line("  [worker " + std::to_string(id_) + " " + net_.label() + "] " + why +
                     " -> retry " + std::to_string(job->failures) + " in " +
                     std::to_string(delay.count() / 1000.0).substr(0, 4) + " s");

            if (!queue_.wait_unless_cancelled(delay)) break;   // cancelled or Ctrl+C
            queue_.retry(*job);   // back in the queue: any worker can take it
        }
    } catch (...) {
        // An exception must never escape a thread (that calls std::terminate).
        // Save it for main, and stop the other workers.
        error_ = std::current_exception();
        queue_.cancel();
    }
}
