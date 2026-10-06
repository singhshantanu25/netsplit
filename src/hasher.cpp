#include "hasher.h"

#include <CommonCrypto/CommonDigest.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

std::string sha256_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        throw std::runtime_error("open " + path + ": " + std::strerror(errno));

    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);                       // 1. start

    std::vector<unsigned char> buf(1 << 20);    // read 1 MB at a time
    while (true) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            int err = errno;
            ::close(fd);
            throw std::runtime_error("read " + path + ": " + std::strerror(err));
        }
        if (n == 0) break;                      // end of file
        CC_SHA256_Update(&ctx, buf.data(), static_cast<CC_LONG>(n));   // 2. feed
    }
    ::close(fd);

    unsigned char digest[CC_SHA256_DIGEST_LENGTH];   // 32 bytes
    CC_SHA256_Final(digest, &ctx);                    // 3. finish

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char b : digest) {   // each byte -> 2 hex characters
        out += hex[b >> 4];
        out += hex[b & 0x0f];
    }
    return out;
}