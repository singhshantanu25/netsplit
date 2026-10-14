#include "worker.h"

#include <algorithm>
#include <map>
#include <stdexcept>

namespace {

using Headers = std::map<std::string, std::string>;

// Whole string must be a number, otherwise -1.
long long to_ll(const std::string& s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return -1;
    try {
        return std::stoll(s);
    } catch (...) {
        return -1;
    }
}

std::string chunk_name(const Chunk& c) {
    return "chunk " + std::to_string(c.index) + " (" + c.range() + ")";
}

// Throws if this answer is not exactly the chunk we asked for.
void check_response(const Chunk& c, const Expect& expect, long status,
                    const Headers& h) {
    const std::string who = chunk_name(c);

    // 1. Same file as when we probed?
    if (!expect.etag.empty()) {
        auto et = h.find("etag");
        std::string got = (et == h.end()) ? "(none)" : et->second;
        if (got != expect.etag)
            throw std::runtime_error(who + ": file changed on the server (ETag " +
                                     expect.etag + " -> " + got + ")");
    }

    // 2. Status. 200 = Range ignored, the body starts at byte 0.
    //    That is only usable for a chunk that starts at 0.
    if (status == 200) {
        if (c.start != 0)
            throw std::runtime_error(who + ": server ignored Range (HTTP 200)");
        return;   // we keep only the first c.size() bytes and stop
    }
    if (status != 206)
        throw std::runtime_error(who + ": expected HTTP 206, got " +
                                 std::to_string(status));

    // 3. Content-Range must be exactly "bytes START-END/TOTAL".
    auto cr = h.find("content-range");
    if (cr == h.end())
        throw std::runtime_error(who + ": 206 without Content-Range");
    const std::string& v = cr->second;
    size_t dash = v.find('-');
    size_t slash = v.find('/');
    if (v.rfind("bytes ", 0) != 0 || dash == std::string::npos ||
        slash == std::string::npos || dash > slash)
        throw std::runtime_error(who + ": bad Content-Range '" + v + "'");
    long long s = to_ll(v.substr(6, dash - 6));
    long long e = to_ll(v.substr(dash + 1, slash - dash - 1));
    std::string total = v.substr(slash + 1);
    if (s != c.start || e != c.end)
        throw std::runtime_error(who + ": server sent '" + v + "'");
    if (total != "*" && to_ll(total) != expect.total)
        throw std::runtime_error(who + ": file size changed ('" + v + "')");

    // 4. Content-Length, if sent, must be the chunk size.
    auto cl = h.find("content-length");
    if (cl != h.end() && to_ll(cl->second) != c.size())
        throw std::runtime_error(who + ": Content-Length " + cl->second +
                                 ", expected " + std::to_string(c.size()));
}

}  // namespace

long long download_chunk(HttpClient& client, const std::string& url,
                         const Chunk& c, Storage& out, Progress* progress,
                         const Expect& expect) {
    long long received = 0;

    auto on_headers = [&](long status, const Headers& h) {
        check_response(c, expect, status, h);   // runs before any byte is written
    };

    auto on_data = [&](const char* data, std::size_t len) {
        long long left = c.size() - received;
        if (left <= 0) return false;   // already full: anything more is extra
        long long take = std::min(static_cast<long long>(len), left);
        out.write_at(c.start + received, data, static_cast<std::size_t>(take));
        received += take;
        if (progress) progress->add(static_cast<std::uint64_t>(take));
        return take == static_cast<long long>(len);   // false = more than asked
    };

    Response r = client.get_stream(url, c.range(), on_data, on_headers);

    // Again after the transfer: covers answers with an empty body,
    // where the header check never ran.
    check_response(c, expect, r.status, r.headers);

    if (r.truncated && r.status == 206)
        throw std::runtime_error(chunk_name(c) + ": server sent more bytes than asked");
    if (received != c.size())
        throw std::runtime_error(chunk_name(c) + ": got " + std::to_string(received) +
                                 " bytes, expected " + std::to_string(c.size()));
    return received;
}

Worker::Worker(int id, const std::string& url, const Expect& expect, WorkQueue& queue,
               Storage& out, Progress& progress, bool reuse)
    : id_(id), url_(url), expect_(expect), queue_(queue), out_(out),
      progress_(progress) {
    client_.set_reuse(reuse);
}

void Worker::run() {
    try {
        while (auto c = queue_.pop()) {   // stops when pop() returns nothing
            bytes_done_ += download_chunk(client_, url_, *c, out_, &progress_, expect_);
            ++chunks_done_;
        }
    } catch (...) {
        // An exception must never escape a thread (that calls std::terminate).
        // Save it for main, and stop the other workers.
        error_ = std::current_exception();
        queue_.cancel();
    }
}