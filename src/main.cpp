#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
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
              << "  netsplit get <url> [--workers N] [--sha256 <expected>]\n";
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
    Storage out(p.filename + ".part", p.size);
    long long got = download_chunk(client, url, c, out, nullptr);
    std::cout << "chunk " << c.index << " (" << c.range() << ") -> " << out.path()
              << " at offset " << c.start << ", " << got << " bytes\n";
    return 0;
}

int cmd_get(HttpClient& client, const std::string& url, std::string expected,
            int num_workers) {
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });

    ProbeResult p = probe_for_download(client, url);
    Planner plan(p.size, kChunkSize);
    const std::string final_path = p.filename;
    const std::string part_path = final_path + ".part";
    const int n = std::min(num_workers, plan.count());   // no idle workers

    std::cout << "Downloading " << final_path << " (" << p.size << " bytes, "
              << plan.count() << " chunks, " << n << " workers)\n";

    Storage out(part_path, p.size);
    Progress progress(p.size);

    // Producer: put every chunk in the queue, then close it.
    WorkQueue queue;
    for (int i = 0; i < plan.count(); ++i) queue.push(plan.chunk(i));
    queue.close();

    // Each worker owns its own HttpClient (curl handle).
    // unique_ptr because a Worker can't be copied or moved.
    std::vector<std::unique_ptr<Worker>> workers;
    for (int i = 0; i < n; ++i)
        workers.push_back(std::make_unique<Worker>(i, url, queue, out, progress));

    // Consumers: one thread per worker.
    auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    for (auto& w : workers)
        pool.emplace_back([&w] { w->run(); });
    for (auto& t : pool) t.join();   // wait for every thread
    progress.finish();
    double secs = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - start).count();

    // If any worker failed, report it (the file stays as .part).
    for (auto& w : workers)
        if (w->error()) std::rethrow_exception(w->error());

    for (auto& w : workers)
        std::cout << "  worker " << w->id() << ": " << w->chunks_done()
                  << " chunks, " << w->bytes_done() << " bytes\n";
    std::cout << "Time: " << secs << " s, average "
              << (p.size / (1024.0 * 1024.0)) / secs << " MB/s\n";

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

    if (std::rename(part_path.c_str(), final_path.c_str()) != 0)
        throw std::runtime_error("rename " + part_path + " -> " + final_path +
                                 ": " + std::strerror(errno));

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
            for (int i = 3; i + 1 < argc; i += 2) {
                std::string flag = argv[i];
                if (flag == "--sha256")
                    expected = argv[i + 1];
                else if (flag == "--workers")
                    num_workers = std::stoi(argv[i + 1]);
                else
                    throw std::runtime_error("unknown option " + flag);
            }
            if (num_workers < 1)
                throw std::runtime_error("--workers must be at least 1");
            code = cmd_get(client, url, expected, num_workers);
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