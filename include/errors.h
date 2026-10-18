#pragma once
#include <stdexcept>

// Every problem while downloading a chunk. Catch this to catch them all.
class DownloadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;   // inherit the constructor
};

// The network failed: could not connect, connection dropped, stalled.
// Usually temporary (Wi-Fi comes back), so keep retrying.
class ConnectionFailed : public DownloadError {
public:
    using DownloadError::DownloadError;
};

// The server answered, but wrongly: 404, 429, wrong Content-Range, wrong
// length, file changed. Retrying rarely helps, so give up after a few tries.
class BadResponse : public DownloadError {
public:
    using DownloadError::DownloadError;
};