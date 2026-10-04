#include <curl/curl.h>

#include <iostream>
#include <string>

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
              << "  netsplit chunk <url> <index>\n";
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
    ProbeResult p = probe(client, url);
    if (!p.ranges_supported || p.size <= 0) {
        std::cerr << "error: server does not support ranges, cannot split\n";
        return 1;
    }
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
    ProbeResult p = probe(client, url);
    if (!p.ranges_supported || p.size <= 0) {
        std::cerr << "error: server does not support ranges, cannot split\n";
        return 1;
    }
    Planner plan(p.size, kChunkSize);
    const Chunk& c = plan.chunk(index);
    Storage out(p.filename + ".part", p.size);

    long long received = 0;
    Response r = client.get_stream(url, c.range(),
        [&](const char* data, std::size_t len) {
            // Never accept more bytes than this chunk should have.
            if (received + static_cast<long long>(len) > c.size()) return false;
            out.write_at(c.start + received, data, len);
            received += static_cast<long long>(len);
            return true;
        });

    if (r.status != 206)
        throw std::runtime_error("expected HTTP 206, got " + std::to_string(r.status));
    if (received != c.size())
        throw std::runtime_error("chunk " + std::to_string(c.index) + ": got " +
                                 std::to_string(received) + " bytes, expected " +
                                 std::to_string(c.size()));

    std::cout << "chunk " << c.index << " (" << c.range() << ") -> " << out.path()
              << " at offset " << c.start << ", " << received << " bytes\n";
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
        } else {
            print_usage();
            code = 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        code = 1;
    }
    curl_global_cleanup();
    return code;
}