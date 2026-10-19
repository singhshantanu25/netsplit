#include "resume_state.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace {

constexpr int kVersion = 1;
constexpr auto kSaveEvery = std::chrono::seconds(2);

// "downloads/a.json" -> "downloads", "a.json" -> "."
std::string folder_of(const std::string& path) {
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

// Crash-safe replace: write "<path>.tmp", fsync it, rename over <path>,
// fsync the folder. Readers see the old file or the new one, never half.
void write_file_atomic(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        throw std::runtime_error("open " + tmp + ": " + std::strerror(errno));

    const char* p = text.data();
    std::size_t left = text.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            int err = errno;
            ::close(fd);
            throw std::runtime_error("write " + tmp + ": " + std::strerror(err));
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) {
        int err = errno;
        ::close(fd);
        throw std::runtime_error("fsync " + tmp + ": " + std::strerror(err));
    }
    ::close(fd);

    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        throw std::runtime_error("rename " + tmp + ": " + std::strerror(errno));

    int dir = ::open(folder_of(path).c_str(), O_RDONLY);
    if (dir >= 0) {
        ::fsync(dir);
        ::close(dir);
    }
}

}  // namespace

std::optional<ResumeState> load_state(const std::string& path) {
    std::ifstream in(path);
    if (!in) return std::nullopt;   // no progress file
    try {
        json j = json::parse(in);
        if (j.at("version").get<int>() != kVersion) return std::nullopt;

        ResumeState s;
        s.url = j.at("url").get<std::string>();
        s.size = j.at("size").get<long long>();
        s.etag = j.at("etag").get<std::string>();
        s.chunk_size = j.at("chunk_size").get<long long>();
        int chunks = j.at("chunks").get<int>();
        if (s.size <= 0 || s.chunk_size <= 0 || chunks <= 0) return std::nullopt;
        s.done.assign(static_cast<std::size_t>(chunks), false);
        for (int i : j.at("done").get<std::vector<int>>()) {
            if (i < 0 || i >= chunks) return std::nullopt;
            s.done[static_cast<std::size_t>(i)] = true;
        }
        return s;
    } catch (const json::exception&) {
        return std::nullopt;   // damaged or old format: start over
    }
}

StateFile::StateFile(std::string path, ResumeState initial)
    : path_(std::move(path)), state_(std::move(initial)),
      last_save_(std::chrono::steady_clock::now()) {}

void StateFile::chunk_done(int index, Storage& out) {
    std::lock_guard<std::mutex> lock(m_);
    state_.done.at(static_cast<std::size_t>(index)) = true;
    if (std::chrono::steady_clock::now() - last_save_ >= kSaveEvery)
        save_locked(out);
}

void StateFile::save(Storage& out) {
    std::lock_guard<std::mutex> lock(m_);
    save_locked(out);
}

void StateFile::save_locked(Storage& out) {
    // 1. Data first: every chunk marked done has been pwrite'n, now make it durable.
    out.sync();

    // 2. Then the record that says so (the "log" in write-ahead logging).
    std::vector<int> done;
    for (std::size_t i = 0; i < state_.done.size(); ++i)
        if (state_.done[i]) done.push_back(static_cast<int>(i));

    json j = {
        {"version", kVersion},
        {"url", state_.url},
        {"size", state_.size},
        {"etag", state_.etag},
        {"chunk_size", state_.chunk_size},
        {"chunks", state_.done.size()},
        {"done", done},
    };
    write_file_atomic(path_, j.dump() + "\n");
    last_save_ = std::chrono::steady_clock::now();
}

void StateFile::remove() {
    std::lock_guard<std::mutex> lock(m_);
    std::remove(path_.c_str());
}

int StateFile::done_count() const {
    std::lock_guard<std::mutex> lock(m_);
    int n = 0;
    for (bool d : state_.done) n += d ? 1 : 0;
    return n;
}