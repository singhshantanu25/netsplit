#pragma once
#include <exception>
#include <string>

#include "http_client.h"
#include "planner.h"
#include "progress.h"
#include "storage.h"
#include "work_queue.h"

// What the probe told us about the file. Every chunk answer must agree with it.
struct Expect {
    long long total;    // full file size
    std::string etag;   // empty = server sent no ETag
};

// Downloads one chunk and writes it at its offset.
// Throws ConnectionFailed (network) or BadResponse (wrong answer) from errors.h.
// On failure, the bytes it added to progress are taken back out.
long long download_chunk(HttpClient& client, const std::string& url,
                         const Chunk& c, Storage& out, Progress* progress,
                         const Expect& expect);

// One download worker. run() takes jobs from the queue until there are none.
// A failed chunk waits (backoff + jitter) and goes back to the queue.
class Worker {
public:
    Worker(int id, const std::string& url, const Expect& expect, WorkQueue& queue,
           Storage& out, Progress& progress, bool reuse);

    void run();   // call this on its own thread

    int id() const { return id_; }
    int chunks_done() const { return chunks_done_; }
    long long bytes_done() const { return bytes_done_; }
    int retries() const { return retries_; }
    long connections_opened() const { return client_.connections_opened(); }
    std::exception_ptr error() const { return error_; }

private:
    int id_;
    std::string url_;
    Expect expect_;
    WorkQueue& queue_;      // shared by all workers
    Storage& out_;          // shared by all workers (pwrite at different offsets)
    Progress& progress_;    // shared by all workers
    HttpClient client_;     // OWN curl handle, never shared between threads

    int chunks_done_ = 0;
    long long bytes_done_ = 0;
    int retries_ = 0;
    std::exception_ptr error_;
};