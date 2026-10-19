#pragma once
#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "storage.h"

// Everything needed to continue a download later.
struct ResumeState {
    std::string url;
    long long size = 0;
    std::string etag;
    long long chunk_size = 0;
    std::vector<bool> done;   // done[i] = chunk i is fully written
};

// Reads "<name>.progress.json". Empty if the file is missing or unreadable.
std::optional<ResumeState> load_state(const std::string& path);

// Keeps the progress file up to date while workers download (thread-safe).
// Write-ahead rule: chunk data is fsync'ed BEFORE the file says "done",
// so after a crash the file never claims a chunk that is not on disk.
class StateFile {
public:
    StateFile(std::string path, ResumeState initial);

    // A worker finished chunk `index`. Saves if the last save is 2+ s old.
    void chunk_done(int index, Storage& out);

    // Save right now (pause, error).
    void save(Storage& out);

    // Download finished: the progress file is not needed any more.
    void remove();

    int done_count() const;
    const std::string& path() const { return path_; }

private:
    void save_locked(Storage& out);   // caller holds m_

    mutable std::mutex m_;
    std::string path_;
    ResumeState state_;
    std::chrono::steady_clock::time_point last_save_;
};