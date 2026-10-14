#include "storage.h"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {

// "downloads/a.bin" -> "downloads", "a.bin" -> "."
std::string folder_of(const std::string& path) {
    std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return path.substr(0, slash);
}

}  // namespace

Storage::Storage(const std::string& final_path, long long size)
    : fd_(-1), final_path_(final_path), part_path_(final_path + ".part") {
    fd_ = ::open(part_path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0)
        throw std::runtime_error("open " + part_path_ + ": " + std::strerror(errno));

    // Make the file its final size up front, so any offset is valid.
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        int err = errno;
        ::close(fd_);   // the destructor will NOT run if the constructor throws
        throw std::runtime_error("ftruncate " + part_path_ + ": " + std::strerror(err));
    }
}

Storage::~Storage() {
    if (fd_ >= 0) ::close(fd_);
}

void Storage::write_at(long long offset, const char* data, std::size_t len) {
    while (len > 0) {   // pwrite may write fewer bytes than asked, so loop
        ssize_t n = ::pwrite(fd_, data, len, static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(std::string("pwrite: ") + std::strerror(errno));
        }
        data += n;
        len -= static_cast<std::size_t>(n);
        offset += n;
    }
}

void Storage::sync() {
    if (::fsync(fd_) != 0)
        throw std::runtime_error("fsync " + part_path_ + ": " + std::strerror(errno));
}

void Storage::finalize() {
    if (finalized_) return;
    sync();   // data on disk BEFORE it gets the final name

    // Atomic on the same filesystem: anyone sees either the .part or the
    // complete final file, never a half-written file with the final name.
    if (std::rename(part_path_.c_str(), final_path_.c_str()) != 0)
        throw std::runtime_error("rename " + part_path_ + " -> " + final_path_ +
                                 ": " + std::strerror(errno));
    finalized_ = true;

    // The rename changed the folder, so fsync the folder too.
    // Otherwise a power cut could undo the rename.
    int dir = ::open(folder_of(final_path_).c_str(), O_RDONLY);
    if (dir >= 0) {
        ::fsync(dir);
        ::close(dir);
    }
}