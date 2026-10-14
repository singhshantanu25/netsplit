#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "hasher.h"
#include "http_client.h"
#include "planner.h"
#include "probe.h"
#include "progress.h"
#include "storage.h"
#include "work_queue.h"
#include "worker.h"

namespace {

constexpr long long kChunkSize = 4LL * 1024 * 1024;   // 4 MB

void print_usage() {
    std::cerr << "usage:\n"
              << "  netsplit probe <url>\n"
              << "  netsplit plan <url>\n"
              << "  netsplit chunk <url> <index>\n"
              << "  netsplit get <url> [--workers N] [--sha256 <expected>] [--no-reuse]\n";
}

ProbeResult probe_for_download(HttpClient& client, const std::string& url) {
    ProbeResult p = probe(client, url);
    if (!p.ranges_supported || p.size <= 0)
        throw std::runtime_error("server does not support ranges, cannot split");
    return p;
}

int cmd_probe(HttpClient& client, const std::string& url) {
    ProbeResult p = probe(client, url);
    std::cout << "File name:     " << p.filename << "\n";
    std::cout << "Size:          "
              << (p.size >= 0 ? std::to_string(p.size) + " bytes" : "unknown")
              << "\n";
    std::cout << "Range support: " << (p.ranges_supported ? "yes" : "no") << "\n";
    std::cout << "ETag:          " << (p.etag.empty() ? "(none)" : p.etag) << "\n";
    return 0;
}

int cmd_plan(HttpClient& client, const std::string& url) {
    ProbeResult p = probe_for_download(client, url);
    Planner plan(p.size, kChunkSize);
    std::cout << "File size: " << p.size << " bytes, " << plan.count()
              << " chunks of up to " << kChunkSize << " bytes\n";
    for (int i = 0; i < plan.count(); ++i) {
        const Chunk& c = plan.chunk(i);
        std::cout << "  chunk " << c.index << ": " << c.range() << "  ("
                  << c.size() << " bytes)\n";
    }
    return 0;
}

int cmd_chunk(HttpClient& client, const std::string& url, int index) {
    ProbeResult p = probe_for_download(client, url);
    Planner plan(p.size, kChunkSize);
    const Chunk& c = plan.chunk(index);
        Storage out(p.filename, p.size);
        long long got = download_chunk(client, url, c, out, nullptr, Expect{p.size, p.etag});
    std::cout << "chunk " << c.index << " (" << c.range() << ") -> " << out.path()
              << " at offset " << c.start << ", " << got << " bytes\n";
    return 0;
}

int cmd_get(HttpClient& client, const std::string& url, std::string expected,
            int num_workers, bool reuse) {
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });

    ProbeResult p = probe_for_download(client, url);
    Planner plan(p.size, kChunkSize);
    const std::string final_path = p.filename;
    const std::string part_path = final_path + ".part";
    const int n = std::min(num_workers, plan.count());   // no idle workers

    std::cout << "Downloading " << final_path << " (" << p.size << " bytes, "
              << plan.count() << " chunks, " << n << " workers, connection reuse "
              << (reuse ? "on" : "OFF") << ")\n";
  
        Storage out(final_path, p.size);
    Progress progress(static_cast<std::uint64_t>(p.size));
    progress.start();   // printer thread: one line per second

    // Producer: put every chunk in the queue, then close it.
    WorkQueue queue;
    for (int i = 0; i < plan.count(); ++i) queue.push(plan.chunk(i));
    queue.close();

    // Each worker owns its own HttpClient (curl handle).
    std::vector<std::unique_ptr<Worker>> workers;
    for (int i = 0; i < n; ++i)
        workers.push_back(
                        std::make_unique<Worker>(i, url, Expect{p.size, p.etag}, queue, out, progress, reuse));

    // Consumers: one thread per worker.
    auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    for (auto& w : workers)
        pool.emplace_back([&w] { w->run(); });
    for (auto& t : pool) t.join();
    progress.stop();
    double secs = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - start).count();

    for (auto& w : workers)
        if (w->error()) std::rethrow_exception(w->error());

    long total_conns = 0;
    for (auto& w : workers) {
        std::cout << "  worker " << w->id() << ": " << w->chunks_done()
                  << " chunks, " << w->bytes_done() << " bytes, "
                  << w->connections_opened() << " connection(s) opened\n";
        total_conns += w->connections_opened();
    }
    std::cout << "Time: " << secs << " s, average "
              << (p.size / (1024.0 * 1024.0)) / secs << " MB/s, "
              << total_conns << " connections for " << plan.count()
              << " chunks\n";

    out.sync();

    std::cout << "Verifying SHA-256...\n";
    std::string actual = sha256_file(part_path);
    std::cout << "SHA-256: " << actual << "\n";
    if (!expected.empty()) {
        if (actual != expected)
            throw std::runtime_error("SHA-256 mismatch! expected " + expected +
                                     ". The file was kept as " + part_path);
        std::cout << "SHA-256 matches the expected value.\n";
    }

        out.finalize();   // fsync + atomic rename + fsync the folder

    std::cout << "Saved " << final_path << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        print_usage();
        return 1;
    }
    std::string cmd = argv[1];
    std::string url = argv[2];

    // Must happen once, before any thread starts using curl.
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int code = 0;
    try {
        HttpClient client;   // used by main for probe / plan / chunk
        if (cmd == "probe") {
            code = cmd_probe(client, url);
        } else if (cmd == "plan") {
            code = cmd_plan(client, url);
        } else if (cmd == "chunk" && argc >= 4) {
            code = cmd_chunk(client, url, std::stoi(argv[3]));
        } else if (cmd == "get") {
            std::string expected;
            int num_workers = 4;   // default
            bool reuse = true;     // default
            for (int i = 3; i < argc; ++i) {
                std::string flag = argv[i];
                if (flag == "--no-reuse") {
                    reuse = false;                          // flag with no value
                } else if ((flag == "--sha256" || flag == "--workers") &&
                           i + 1 < argc) {
                    std::string value = argv[++i];          // flag with a value
                    if (flag == "--sha256")
                        expected = value;
                    else
                        num_workers = std::stoi(value);
                } else {
                    throw std::runtime_error("unknown or incomplete option " + flag);
                }
            }
            if (num_workers < 1)
                throw std::runtime_error("--workers must be at least 1");
            code = cmd_get(client, url, expected, num_workers, reuse);
        } else {
            print_usage();
            code = 1;
        }
    } catch (const std::exception& e) {
        std::cout << "\n";
        std::cerr << "error: " << e.what() << "\n";
        code = 1;
    }
    curl_global_cleanup();
    return code;
}