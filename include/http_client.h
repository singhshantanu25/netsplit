#pragma once
#include <curl/curl.h>

#include <cstddef>
#include <limits>
#include <map>
#include <string>

struct Response {
    long status = 0;
    std::map<std::string, std::string> headers;  // names in lowercase
    std::string body;
    bool truncated = false;   // true if we stopped reading the body early
};

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    // RAII: one object owns one curl handle, so copying is not allowed.
    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // range example: "0-0". Empty string means no Range header.
    Response get(const std::string& url,
                 const std::string& range = "",
                 std::size_t max_body = std::numeric_limits<std::size_t>::max());

private:
    CURL* handle_;
};