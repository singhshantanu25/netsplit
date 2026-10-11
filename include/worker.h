#pragma once
#include <exception>
#include <string>

#include "http_client.h"
#include "planner.h"
#include "progress.h"
#include "storage.h"
#include "work_queue.h"

// Downloads one chunk and writes it at its offset. Throws on any problem.
long long download_chunk(HttpClient& client, const std::string& url,
                         const Chunk& c, Storage& out, Progress* progress);

// One download worker. run() takes chunks from the queue until there are none.
// How a chunk is fetched (curl, Range, 206 check, pwrite) is hidden inside.
class Worker {
public:
   Worker(int id, const std::string& url, WorkQueue& queue, Storage& out,
           Progress& progress, bool reuse);

    void run();   // call this on its own thread

    int id() const { return id_; }
    int chunks_done() const { return chunks_done_; }
    long long bytes_done() const { return bytes_done_; }
    long connections_opened() const { return client_.connections_opened(); }
    std::exception_ptr error() const { return error_; }

private:
    int id_;
    std::string url_;
    WorkQueue& queue_;      // shared by all workers
    Storage& out_;          // shared by all workers (pwrite at different offsets)
    Progress& progress_;    // shared by all workers
    HttpClient client_;     // OWN curl handle, never shared between threads

    int chunks_done_ = 0;
    long long bytes_done_ = 0;
    std::exception_ptr error_;
};