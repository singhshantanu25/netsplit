#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include "hasher.h"
#include "http_client.h"
#include "planner.h"
#include "probe.h"
#include "storage.h"

namespace {

constexpr long long kChunkSize = 4LL * 1024 * 1024;   // 4 MB

void print_usage() {
    std::cerr << "usage:\n"
              << "  netsplit probe <url>\n"
              << "  netsplit plan <url>\n"
              << "  netsplit chunk <url> <index>\n"
              << "  netsplit get <url> [--sha256 <expected>]\n";
}

// One progress line that rewrites itself. "\r" moves back to the line start.
class Progress {
public:
    explicit Progress(long long total) : total_(total) {}

    void add(long long bytes) {
        done_ += bytes;
        int percent = static_cast<int>(done_ * 100 / total_);
        if (percent != last_percent_) {
            last_percent_ = percent;
            std::cout << "\r" << done_ << " / " << total_ << " bytes ("
                      << percent << "%)" << std::flush;
        }
    }
    void finish() { std::cout << "\n"; }

private:
    long long total_;
    long long done_ = 0;
    int last_percent_ = -1;
};

ProbeResult probe_for_download(HttpClient& client, const std::string& url) {
    ProbeResult p = probe(client, url);
    if (!p.ranges_supported || p.size <= 0)
        throw std::runtime_error("server does not support ranges, cannot split");
    return p;
}

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

int cmd_get(HttpClient& client, const std::string& url, std::string expected) {
    // People paste hashes in upper or lower case; compare in lower case.
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });

    ProbeResult p = probe_for_download(client, url);
    Planner plan(p.size, kChunkSize);
    const std::string final_path = p.filename;
    const std::string part_path = final_path + ".part";

    std::cout << "Downloading " << final_path << " (" << p.size << " bytes, "
              << plan.count() << " chunks)\n";

    Storage out(part_path, p.size);
    Progress progress(p.size);
    for (int i = 0; i < plan.count(); ++i)
        download_chunk(client, url, plan.chunk(i), out, &progress);
    progress.finish();

    out.sync();   // data really on disk BEFORE hashing and renaming

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

    curl_global_init(CURL_GLOBAL_DEFAULT);
    int code = 0;
    try {
        HttpClient client;
        if (cmd == "probe") {
            code = cmd_probe(client, url);
        } else if (cmd == "plan") {
            code = cmd_plan(client, url);
        } else if (cmd == "chunk" && argc >= 4) {
            code = cmd_chunk(client, url, std::stoi(argv[3]));
        } else if (cmd == "get") {
            std::string expected;
            if (argc >= 5 && std::string(argv[3]) == "--sha256")
                expected = argv[4];
            code = cmd_get(client, url, expected);
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