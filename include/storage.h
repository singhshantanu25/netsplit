#pragma once
#include <cstddef>
#include <string>

// Owns the staging file "<final>.part" (RAII on a file descriptor).
// All data goes into the .part file; finalize() gives it the final name.
class Storage {
public:
    Storage(const std::string& final_path, long long size);
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    void write_at(long long offset, const char* data, std::size_t len);
    void sync();       // fsync: wait until the data is really on disk
    void finalize();   // fsync, atomic rename .part -> final name, fsync the folder

    const std::string& path() const { return part_path_; }   // the staging file

private:
    int fd_;
    std::string final_path_;
    std::string part_path_;
    bool finalized_ = false;
};