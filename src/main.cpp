#include <curl/curl.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "hasher.h"
#include "http_client.h"
#include "network.h"
#include "planner.h"
#include "probe.h"
#include "progress.h"
#include "resume_state.h"
#include "stop.h"
#include "storage.h"
#include "work_queue.h"
#include "worker.h"

namespace {

constexpr long long kChunkSize = 4LL * 1024 * 1024;   // 4 MB

// True if the file exists and has exactly this size.
bool file_has_size(const std::string& path, long long size) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && st.st_size == size;
}

void print_usage() {
    std::cerr << "usage:\n"
              << "  netsplit interfaces\n"
              << "  netsplit probe <url>\n"
              << "  netsplit plan <url>\n"
              << "  netsplit chunk <url> <index>\n"
              << "  netsplit get <url> [--workers N] [--sha256 <expected>] [--no-reuse]\n"
              << "                     [--interfaces en0,en5 | en0:3,en5:2]   (see: netsplit interfaces)\n";
}

ProbeResult probe_for_download(HttpClient& client, const std::string& url) {
    ProbeResult p = probe(client, url);
    if (!p.ranges_supported || p.size <= 0)
        throw std::runtime_error("server does not support ranges, cannot split");
    return p;
}

// One entry of --interfaces: "en5" or "en5:2" (2 workers on en5).
struct NetSpec {
    std::string name;
    int workers = 0;   // 0 = not given
};

// "en0:3,en5:2" -> {{"en0",3}, {"en5",2}}
std::vector<NetSpec> parse_net_specs(const std::string& text) {
    std::vector<NetSpec> specs;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t comma = text.find(',', start);
        if (comma == std::string::npos) comma = text.size();
        std::string part = text.substr(start, comma - start);
        start = comma + 1;
        if (part.empty()) continue;

        NetSpec spec;
        std::size_t colon = part.find(':');
        spec.name = part.substr(0, colon);
        if (colon != std::string::npos) {
            spec.workers = std::stoi(part.substr(colon + 1));
            if (spec.workers < 1)
                throw std::runtime_error("need at least 1 worker on " + spec.name);
        }
        specs.push_back(spec);
    }
    return specs;
}

// One Network object per --interfaces entry. Every name must be an interface
// that is up and has a real IPv4 address. No entries = the OS default route.
std::vector<std::unique_ptr<Network>> build_networks(const std::vector<NetSpec>& specs) {
    std::vector<std::unique_ptr<Network>> nets;
    if (specs.empty()) {
        nets.push_back(std::make_unique<DefaultNetwork>());
        return nets;
    }
    std::vector<NetInterface> all = list_interfaces();
    for (const auto& spec : specs) {
        const NetInterface* n = find_interface(all, spec.name);
        if (n == nullptr)
            throw std::runtime_error("interface '" + spec.name + "' has no IPv4 address "
                                     "(run: netsplit interfaces)");
        if (!n->up)
            throw std::runtime_error("interface '" + spec.name + "' (" + n->ipv4 + ", " +
                                     n->kind + ") is not usable");
        nets.push_back(make_network(*n));   // WifiNetwork or PhoneNetwork
    }
    return nets;
}

// Which network each worker uses.
// With counts ("en0:3,en5:2"): 3 + 2 workers, mixed en0, en5, en0, en5, en0.
// Without counts ("en0,en5"): --workers N, round-robin over the networks.
std::vector<Network*> assign_workers(const std::vector<std::unique_ptr<Network>>& nets,
                                     const std::vector<NetSpec>& specs, int num_workers) {
    std::vector<Network*> plan;
    bool counts = false;
    for (const auto& spec : specs) counts = counts || spec.workers > 0;

    if (!counts) {
        for (int i = 0; i < num_workers; ++i)
            plan.push_back(nets[static_cast<std::size_t>(i) % nets.size()].get());
        return plan;
    }
    std::vector<int> left;
    for (const auto& spec : specs) left.push_back(spec.workers > 0 ? spec.workers : 1);
    bool added = true;
    while (added) {
        added = false;
        for (std::size_t k = 0; k < nets.size(); ++k) {
            if (left[k] > 0) {
                plan.push_back(nets[k].get());
                --left[k];
                added = true;
            }
        }
    }
    return plan;
}

int cmd_interfaces() {
    std::vector<NetInterface> all = list_interfaces();
    if (all.empty()) {
        std::cout << "No network interface with an IPv4 address.\n";
        return 1;
    }
    std::cout << "Interfaces with an IPv4 address:\n";
    for (const auto& n : all) {
        std::printf("  %-8s %-16s %-4s %s\n", n.name.c_str(), n.ipv4.c_str(),
                    n.up ? "up" : "DOWN", n.kind.c_str());
    }
    std::cout << "Use a name with --interfaces, e.g. --interfaces " << all.front().name << "\n";
    return 0;
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
            int num_workers, bool reuse, const std::vector<NetSpec>& specs) {
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](unsigned char ch) { return std::tolower(ch); });

    ProbeResult p = probe_for_download(client, url);
    Planner plan(p.size, kChunkSize);
    const std::string final_path = p.filename;
    const std::string part_path = final_path + ".part";
    const std::string state_path = final_path + ".progress.json";

    // Start fresh, or continue an earlier run if it is for the SAME file.
    ResumeState start_state{url, p.size, p.etag, kChunkSize,
                            std::vector<bool>(static_cast<std::size_t>(plan.count()), false)};
    if (std::optional<ResumeState> old = load_state(state_path)) {
        bool same = old->url == url && old->size == p.size && old->etag == p.etag &&
                    old->chunk_size == kChunkSize &&
                    static_cast<int>(old->done.size()) == plan.count() &&
                    file_has_size(part_path, p.size);
        if (same)
            start_state = *old;
        else
            std::cout << "Found " << state_path << ", but the file on the server "
                      << "changed (or the .part is missing): starting over.\n";
    }

    int already_chunks = 0;
    long long already_bytes = 0;
    for (int i = 0; i < plan.count(); ++i) {
        if (start_state.done[static_cast<std::size_t>(i)]) {
            ++already_chunks;
            already_bytes += plan.chunk(i).size();
        }
    }
    const int remaining = plan.count() - already_chunks;

    // The networks (Wi-Fi, phone, ...) and which one each worker uses.
    std::vector<std::unique_ptr<Network>> nets = build_networks(specs);
    std::vector<Network*> worker_net = assign_workers(nets, specs, num_workers);
    if (static_cast<int>(worker_net.size()) > remaining)
        worker_net.resize(static_cast<std::size_t>(remaining));   // no idle workers
    const int n = static_cast<int>(worker_net.size());

    if (already_chunks > 0)
        std::cout << "Resuming: " << already_chunks << " of " << plan.count()
                  << " chunks already done (" << already_bytes << " bytes)\n";
    std::cout << "Downloading " << final_path << " (" << p.size << " bytes, "
              << plan.count() << " chunks, " << n << " workers, connection reuse "
              << (reuse ? "on" : "OFF") << ")\n";

    if (!specs.empty()) {
        std::cout << "Networks:";
        for (const auto& net : nets) {
            int count = 0;
            for (Network* w : worker_net) count += (w == net.get()) ? 1 : 0;
            std::cout << "  " << net->label() << " " << net->ipv4() << " x" << count
                      << (net->metered() ? " [mobile data]" : "");
        }
        std::cout << "\n";
    }

    Storage out(final_path, p.size);   // keeps the existing .part data
    StateFile state(state_path, start_state);

    Progress progress(static_cast<std::uint64_t>(p.size),
                      static_cast<std::uint64_t>(already_bytes));
    if (!specs.empty())
        for (const auto& net : nets)   // live speed per network on the progress line
            progress.add_source(net->iface(), &net->received_counter(), &net->down_flag());
    progress.start();   // printer thread: one line per second

    // Producer: only the chunks that are not done yet.
    WorkQueue queue;
    for (int i = 0; i < plan.count(); ++i)
        if (!start_state.done[static_cast<std::size_t>(i)]) queue.push(plan.chunk(i));
    queue.close();

    // Each worker owns its own HttpClient (curl handle).
    std::vector<std::unique_ptr<Worker>> workers;
    for (int i = 0; i < n; ++i)
        workers.push_back(std::make_unique<Worker>(i, url, Expect{p.size, p.etag}, queue,
                                                   out, progress, state, reuse,
                                                   *worker_net[static_cast<std::size_t>(i)]));

    // Consumers: one thread per worker.
    auto start = std::chrono::steady_clock::now();
    std::vector<std::thread> pool;
    std::atomic<int> finished{0};
    for (auto& w : workers)
        pool.emplace_back([&w, &finished] { w->run(); ++finished; });

    // Wait for the workers. On Ctrl+C, wake any worker that is sleeping in a
    // backoff or waiting for the queue, so everyone exits quickly.
    while (finished.load() < n) {
        if (stop_requested()) queue.cancel();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    for (auto& t : pool) t.join();
    progress.stop();
    double secs = std::chrono::duration<double>(
                      std::chrono::steady_clock::now() - start).count();

    if (stop_requested()) {
        state.save(out);   // fsync the data, then record the finished chunks
        std::cout << "Paused: " << state.done_count() << " of " << plan.count()
                  << " chunks saved in " << state.path() << "\n"
                  << "Run the same command again to resume.\n";
        return 130;   // the usual exit code after Ctrl+C
    }

    for (auto& w : workers) {
        if (w->error()) {
            state.save(out);   // keep what we have, so the next run can continue
            std::rethrow_exception(w->error());
        }
    }

    long total_conns = 0;
    int total_retries = 0;
    long long new_bytes = 0;   // downloaded in THIS run
    for (auto& w : workers) {
        std::cout << "  worker " << w->id() << " [" << w->network().label() << "]: "
                  << w->chunks_done()
                  << " chunks, " << w->bytes_done() << " bytes, "
                  << w->connections_opened() << " connection(s) opened, "
                  << w->retries() << " retries\n";
        total_conns += w->connections_opened();
        total_retries += w->retries();
        new_bytes += w->bytes_done();
    }
    std::cout << "Time: " << secs << " s, average "
              << (new_bytes / (1024.0 * 1024.0)) / secs << " MB/s, "
              << total_conns << " connections for " << remaining
              << " chunks, " << total_retries << " retries\n";
    if (!specs.empty()) {
        // One line per network: workers, chunks, MB, speed, share of this run.
        std::cout << "Per network:\n";
        for (const auto& net : nets) {
            int count = 0;
            for (Network* w : worker_net) count += (w == net.get()) ? 1 : 0;
            double mb = net->kept() / (1024.0 * 1024.0);
            double speed = secs > 0 ? net->received() / (1024.0 * 1024.0) / secs : 0.0;
            double share = new_bytes > 0 ? 100.0 * net->kept() / new_bytes : 0.0;
            std::printf("  %-18s %d workers  %3d chunks  %6.1f MB  %5.2f MB/s  %3.0f%%\n",
                        net->label().c_str(), count, net->chunks(), mb, speed, share);
            if (net->drops() > 0)
                std::printf("  %-18s went down %d time(s); its chunks were taken over\n", "",
                            net->drops());
            if (net->metered())
                std::printf("  %-18s mobile data used: %.1f MB (incl. failed tries)\n", "",
                            net->received() / (1024.0 * 1024.0));
        }
    }

    state.save(out);   // all chunks done (also fsyncs the data)

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
    state.remove();   // finished: no progress file needed

    std::cout << "Saved " << final_path << "\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "interfaces") {
        try {
            return cmd_interfaces();   // needs no URL
        } catch (const std::exception& e) {
            std::cerr << "error: " << e.what() << "\n";
            return 1;
        }
    }
    if (argc < 3) {
        print_usage();
        return 1;
    }
    std::string cmd = argv[1];
    std::string url = argv[2];

    // Must happen once, before any thread starts using curl.
    curl_global_init(CURL_GLOBAL_DEFAULT);
    install_stop_handler();   // Ctrl+C now pauses instead of killing
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
            std::vector<NetSpec> specs;   // default: OS chooses
            for (int i = 3; i < argc; ++i) {
                std::string flag = argv[i];
                if (flag == "--no-reuse") {
                    reuse = false;                          // flag with no value
                } else if ((flag == "--sha256" || flag == "--workers" ||
                            flag == "--interfaces") && i + 1 < argc) {
                    std::string value = argv[++i];          // flag with a value
                    if (flag == "--sha256")
                        expected = value;
                    else if (flag == "--workers")
                        num_workers = std::stoi(value);
                    else
                        specs = parse_net_specs(value);
                } else {
                    throw std::runtime_error("unknown or incomplete option " + flag);
                }
            }
            if (num_workers < 1)
                throw std::runtime_error("--workers must be at least 1");
            if (!specs.empty()) build_networks(specs);   // fail early, before the probe
            code = cmd_get(client, url, expected, num_workers, reuse, specs);
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