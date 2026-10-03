#include "probe.h"

#include <algorithm>
#include <stdexcept>

namespace {

long long parse_ll(const std::string& s) {
    try {
        return std::stoll(s);
    } catch (...) {
        return -1;
    }
}

std::string filename_from(const std::string& url,
                          const std::map<std::string, std::string>& headers) {
    auto it = headers.find("content-disposition");
    if (it != headers.end()) {
        size_t pos = it->second.find("filename=");
        if (pos != std::string::npos) {
            std::string name = it->second.substr(pos + 9);
            size_t semi = name.find(';');
            if (semi != std::string::npos) name = name.substr(0, semi);
            name.erase(std::remove(name.begin(), name.end(), '"'), name.end());
            size_t slash = name.find_last_of("/\\");   // never allow a path
            if (slash != std::string::npos) name = name.substr(slash + 1);
            if (!name.empty()) return name;
        }
    }
    std::string path = url.substr(0, url.find('?'));
    size_t slash = path.find_last_of('/');
    std::string name =
        (slash == std::string::npos) ? path : path.substr(slash + 1);
    return name.empty() ? "download.bin" : name;
}

}  // namespace

ProbeResult probe(HttpClient& client, const std::string& url) {
    Response r = client.get(url, "0-0", 1024);   // never read more than 1 KiB

    if (r.status != 200 && r.status != 206)
        throw std::runtime_error("server returned HTTP " +
                                 std::to_string(r.status));

    ProbeResult p;
    if (r.status == 206) {
        p.ranges_supported = true;
        auto it = r.headers.find("content-range");   // "bytes 0-0/TOTAL"
        if (it != r.headers.end()) {
            size_t slash = it->second.find('/');
            if (slash != std::string::npos)
                p.size = parse_ll(it->second.substr(slash + 1));
        }
    } else {   // 200: server ignored Range
        auto it = r.headers.find("content-length");
        if (it != r.headers.end()) p.size = parse_ll(it->second);
    }

    auto et = r.headers.find("etag");
    if (et != r.headers.end()) p.etag = et->second;

    p.filename = filename_from(url, r.headers);
    return p;
}
