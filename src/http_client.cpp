#include "http_client.h"

#include "errors.h"
#include "stop.h"

#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

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
    CURL* handle;
    const HttpClient::DataHandler* handler;
    const HttpClient::HeaderCheck* check;
    const std::map<std::string, std::string>* headers;
    bool checked = false;   // header check already done?
    bool stopped = false;   // handler asked us to stop
    std::exception_ptr error;
};

size_t stream_cb(char* data, size_t size, size_t nmemb, void* userp) {
    size_t n = size * nmemb;
    auto* ctx = static_cast<StreamCtx*>(userp);
    try {
        // First body bytes: all headers are in, so check them before writing.
        if (!ctx->checked) {
            ctx->checked = true;
            if (*ctx->check) {
                long status = 0;
                curl_easy_getinfo(ctx->handle, CURLINFO_RESPONSE_CODE, &status);
                (*ctx->check)(status, *ctx->headers);
            }
        }
        if ((*ctx->handler)(data, n)) return n;
        ctx->stopped = true;
    } catch (...) {
        // Never let a C++ exception escape into libcurl (C code).
        // Save it and rethrow after curl_easy_perform returns.
        ctx->error = std::current_exception();
    }
    return 0;   // tells libcurl to abort
}

// ---- Ctrl+C ----
// curl calls this often while transferring, and about once a second even when
// no data arrives, so a stalled or sleeping transfer still stops quickly.
int xferinfo_cb(void*, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return stop_requested() ? 1 : 0;   // non-zero = abort the transfer
}

// ---- interface binding ----
// curl calls this right after it creates each socket, before connect().
// CURLOPT_INTERFACE alone only sets the SOURCE address; on macOS the packets
// could still leave through the default route (Wi-Fi). IP_BOUND_IF scopes the
// socket to the interface, so it really uses that network.
int sockopt_cb(void* clientp, curl_socket_t fd, curlsocktype purpose) {
    if (purpose != CURLSOCKTYPE_IPCXN) return CURL_SOCKOPT_OK;
    unsigned int index = *static_cast<unsigned int*>(clientp);
    if (index == 0) return CURL_SOCKOPT_OK;
#ifdef IP_BOUND_IF
    if (setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index)) != 0)
        return CURL_SOCKOPT_ERROR;
#else
    (void)fd;   // Linux: CURLOPT_INTERFACE uses SO_BINDTODEVICE when allowed
#endif
    return CURL_SOCKOPT_OK;
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

void HttpClient::set_interface(const std::string& name) {
    iface_ = name;
    iface_index_ = 0;
    if (!name.empty()) {
        iface_index_ = if_nametoindex(name.c_str());
        if (iface_index_ == 0)
            throw std::runtime_error("unknown network interface '" + name + "'");
    }
}

// Options shared by get() and get_stream().
void HttpClient::prepare(const std::string& url, const std::string& range,
                         Response& r) {
    curl_easy_reset(handle_);   // clears old options, keeps the connection
    curl_easy_setopt(handle_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle_, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle_, CURLOPT_USERAGENT, "netsplit/0.1");
    curl_easy_setopt(handle_, CURLOPT_CONNECTTIMEOUT, 15L);
    // Stall detection: under 1 KB/s for 20 s in a row -> curl aborts with
    // CURLE_OPERATION_TIMEDOUT, which we throw as ConnectionFailed (retried).
    // No total time limit: a slow but moving download is fine.
    curl_easy_setopt(handle_, CURLOPT_LOW_SPEED_LIMIT, 1024L);   // bytes per second
    curl_easy_setopt(handle_, CURLOPT_LOW_SPEED_TIME, 20L);      // seconds
    curl_easy_setopt(handle_, CURLOPT_NOPROGRESS, 0L);   // turn the callback on
    curl_easy_setopt(handle_, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
    curl_easy_setopt(handle_, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(handle_, CURLOPT_HEADERDATA, &r.headers);
    if (!range.empty())
        curl_easy_setopt(handle_, CURLOPT_RANGE, range.c_str());
    if (!iface_.empty()) {
        // "if!" = use exactly this interface, never fall back to another one.
        const std::string spec = "if!" + iface_;
        curl_easy_setopt(handle_, CURLOPT_INTERFACE, spec.c_str());   // curl copies it
        curl_easy_setopt(handle_, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);   // we bind IPv4
        curl_easy_setopt(handle_, CURLOPT_SOCKOPTFUNCTION, sockopt_cb);
        curl_easy_setopt(handle_, CURLOPT_SOCKOPTDATA, &iface_index_);
    }
    if (!reuse_) {
        curl_easy_setopt(handle_, CURLOPT_FRESH_CONNECT, 1L);   // don't reuse an old one
        curl_easy_setopt(handle_, CURLOPT_FORBID_REUSE, 1L);    // close after this request
    }
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
    if (rc == CURLE_ABORTED_BY_CALLBACK)
        throw ConnectionFailed("stopped (Ctrl+C)");
    if (rc != CURLE_OK && !stopped_by_us)
        throw ConnectionFailed(std::string("request failed: ") +
                               curl_easy_strerror(rc));

    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &r.status);
    long new_conns = 0;
    curl_easy_getinfo(handle_, CURLINFO_NUM_CONNECTS, &new_conns);
    connections_opened_ += new_conns;
    r.truncated = ctx.truncated;
    return r;
}

Response HttpClient::get_stream(const std::string& url,
                                const std::string& range,
                                const DataHandler& on_data,
                                const HeaderCheck& on_headers) {
    Response r;
    StreamCtx ctx{handle_, &on_data, &on_headers, &r.headers};
    prepare(url, range, r);
    curl_easy_setopt(handle_, CURLOPT_WRITEFUNCTION, stream_cb);
    curl_easy_setopt(handle_, CURLOPT_WRITEDATA, &ctx);   // the "user pointer"

    CURLcode rc = curl_easy_perform(handle_);

    long new_conns = 0;
    curl_easy_getinfo(handle_, CURLINFO_NUM_CONNECTS, &new_conns);
    connections_opened_ += new_conns;

    if (ctx.error) std::rethrow_exception(ctx.error);   // header check or write failed
    bool stopped_by_us = (rc == CURLE_WRITE_ERROR && ctx.stopped);
    if (rc == CURLE_ABORTED_BY_CALLBACK)
        throw ConnectionFailed("stopped (Ctrl+C)");
    if (rc != CURLE_OK && !stopped_by_us)
        throw ConnectionFailed(std::string("request failed: ") +
                               curl_easy_strerror(rc));

    curl_easy_getinfo(handle_, CURLINFO_RESPONSE_CODE, &r.status);
    r.truncated = ctx.stopped;   // the caller decides if that is an error
    return r;
}