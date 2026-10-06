#pragma once
#include <cstddef>
#include <string>

// Owns one open file (RAII on a file descriptor).
// Opens the file once and writes bytes at any offset with pwrite.
class Storage {
public:
    Storage(const std::string& path, long long size);
    ~Storage();

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    void write_at(long long offset, const char* data, std::size_t len);
    void sync();
    const std::string& path() const { return path_; }

private:
    int fd_;
    std::string path_;
};