#pragma once
#include <curl/curl.h>

#include <cstddef>
#include <functional>
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
    // Called for every piece of body data as it arrives.
    // Return true to continue, false to stop the download.
    using DataHandler = std::function<bool(const char* data, std::size_t len)>;

    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    // Collects the body in memory (used by probe).
    Response get(const std::string& url,
                 const std::string& range = "",
                 std::size_t max_body = std::numeric_limits<std::size_t>::max());

    // Streams the body to on_data instead of memory (used for chunks).
    Response get_stream(const std::string& url,
                        const std::string& range,
                        const DataHandler& on_data);
                            // false = open a fresh connection for every request (for comparison tests).
    void set_reuse(bool reuse) { reuse_ = reuse; }

    // Total number of NEW connections this client has opened so far.
    long connections_opened() const { return connections_opened_; }

private:
    void prepare(const std::string& url, const std::string& range, Response& r);

    CURL* handle_;
        bool reuse_ = true;
    long connections_opened_ = 0;
};
