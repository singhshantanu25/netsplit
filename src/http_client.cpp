#include "http_client.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <stdexcept>

namespace {

std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// ---- body into memory (get) ----
struct WriteCtx {
    std::string* body;
    size_t max_body;
    bool truncated = false;
};

size_t write_cb(char* data, size_t size, size_t nmemb, void* userp) {
    size_t n = size * nmemb;
    auto* ctx = static_cast<WriteCtx*>(userp);
    if (ctx->body->size() + n > ctx->max_body) {
        ctx->truncated = true;
        return 0;
    }
    ctx->body->append(data, n);
    return n;
}

// ---- body streamed to a handler (get_stream) ----
struct StreamCtx {
    const HttpClient::DataHandler* handler;
    bool stopped = false;
    std::exception_ptr error;
};

size_t stream_cb(char* data, size_t size, size_t nmemb, void* userp) {
    size_t n = size * nmemb;
    auto* ctx = static_cast<StreamCtx*>(userp);
    try {
        if ((*ctx->handler)(data, n)) return n;
        ctx->stopped = true;
    } catch (...) {
        // Never let a C++ exception escape into libcurl (C code).
        // Save it and rethrow after curl_easy_perform returns.
        ctx->error = std::current_exception();
    }
    return 0;   // tells libcurl to abort
}

// ---- headers ----
size_t header_cb(char* data, size_t size, size_t nmemb, void* userp) {
    size_t n = size * nmemb;
    auto* headers = static_cast<std::map<std::string, std::string>*>(userp);
    std::string line(data, n);
    if (line.rfind("HTTP/", 0) == 0) {
        headers->clear();
        return n;
    }
    size_t colon = line.find(':');
    if (colon == std::string::npos) return n;
    (*headers)[to_lower(trim(line.substr(0, colon)))] =
        trim(line.substr(colon + 1));
    return n;
}

}  // namespace

HttpClient::HttpClient() : handle_(curl_easy_init()) {
    if (!handle_) throw std::runtime_error("curl_easy_init failed");
}

HttpClient::~HttpClient() {
    curl_easy_cleanup(handle_);
}

// Options shared by get() and get_stream().
void HttpClient::prepare(const std::string& url, const std::string& range,
                         Response& r) {
    curl_easy_reset(handle_);   // clears old options, keeps the connection
    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle_, CURLOPT_USERAGENT, "netsplit/0.1");
    curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(handle_, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(handle_, CURLOPT_HEADERDATA, &r.headers);
    if (!range.empty())
        curl_easy_setopt(handle_, CURLOPT_RANGE, range.c_str());
}

Response HttpClient::get(const std::string& url, const std::string& range,
                         std::size_t max_body) {
    Response r;
    WriteCtx ctx{&r.body, max_body};
    prepare(url, range, r);
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &ctx);

    CURLcode rc = curl_easy_perform(handle_);
    bool stopped_by_us = (rc == CURLE_WRITE_ERROR && ctx.truncated);
    if (rc != CURLE_OK && !stopped_by_us)
        throw std::runtime_error(std::string("request failed: ") +
                                 curl_easy_strerror(rc));

    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &r.status);
    r.truncated = ctx.truncated;
    return r;
}

Response HttpClient::get_stream(const std::string& url,
                                const std::string& range,
                                const DataHandler& on_data) {
    Response r;
    StreamCtx ctx{&on_data};
    prepare(url, range, r);
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, stream_cb);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &ctx);   // the "user pointer"

    CURLcode rc = curl_easy_perform(handle_);
    if (ctx.error) std::rethrow_exception(ctx.error);
    if (ctx.stopped)
        throw std::runtime_error("download stopped: received more data than expected");
    if (rc != CURLE_OK)
        throw std::runtime_error(std::string("request failed: ") +
                                 curl_easy_strerror(rc));

    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &r.status);
    return r;
}