#include "storage.h"

#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

Storage::Storage(const std::string& path, long long size)
    : fd_(-1), path_(path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd_ < 0)
        throw std::runtime_error("open " + path + ": " + std::strerror(errno));

    // Make the file its final size up front, so any offset is valid.
    if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) {
        int err = errno;
        ::close(fd_);   // the destructor will NOT run if the constructor throws
        throw std::runtime_error("ftruncate " + path + ": " + std::strerror(err));
    }
}

Storage::~Storage() {
    if (fd_ >= 0) ::close(fd_);
}

void Storage::write_at(long long offset, const char* data, std::size_t len) {
    while (len > 0) {   // pwrite may write fewer bytes than asked, so loop
        ssize_t n = ::pwrite(fd_, data, len, static_cast<off_t>(offset));
        if (n < 0) {
            if (errno == EINTR) continue;   // interrupted by a signal, retry
            throw std::runtime_error(std::string("pwrite: ") + std::strerror(errno));
        }
        data += n;
        len -= static_cast<std::size_t>(n);
        offset += n;
    }
}
void Storage::sync() {
    if (::fsync(fd_) != 0)
        throw std::runtime_error(std::string("fsync ") + path_ + ": " +
                                 std::strerror(errno));
}