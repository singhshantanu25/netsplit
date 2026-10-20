#include "validate.h"

#include <stdexcept>

#include "errors.h"

namespace {

// Whole string must be a number, otherwise -1.
long long to_ll(const std::string& s) {
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) return -1;
    try {
        return std::stoll(s);
    } catch (...) {
        return -1;
    }
}

}  // namespace

std::string chunk_name(const Chunk& c) {
    return "chunk " + std::to_string(c.index) + " (" + c.range() + ")";
}

void check_response(const Chunk& c, const Expect& expect, long status,
                    const std::map<std::string, std::string>& h) {
    const std::string who = chunk_name(c);

    // 1. Same file as when we probed?
    if (!expect.etag.empty()) {
        auto et = h.find("etag");
        std::string got = (et == h.end()) ? "(none)" : et->second;
        if (got != expect.etag)
            throw BadResponse(who + ": file changed on the server (ETag " +
                              expect.etag + " -> " + got + ")");
    }

    // 2. Status. 200 = Range ignored, the body starts at byte 0.
    //    That is only usable for a chunk that starts at 0.
    if (status == 200) {
        if (c.start != 0)
            throw BadResponse(who + ": server ignored Range (HTTP 200)");
        return;   // we keep only the first c.size() bytes and stop
    }
    if (status != 206)
        throw BadResponse(who + ": expected HTTP 206, got " +
                          std::to_string(status));

    // 3. Content-Range must be exactly "bytes START-END/TOTAL".
    auto cr = h.find("content-range");
    if (cr == h.end())
        throw BadResponse(who + ": 206 without Content-Range");
    const std::string& v = cr->second;
    size_t dash = v.find('-');
    size_t slash = v.find('/');
    if (v.rfind("bytes ", 0) != 0 || dash == std::string::npos ||
        slash == std::string::npos || dash > slash)
        throw BadResponse(who + ": bad Content-Range '" + v + "'");
    long long s = to_ll(v.substr(6, dash - 6));
    long long e = to_ll(v.substr(dash + 1, slash - dash - 1));
    std::string total = v.substr(slash + 1);
    if (s != c.start || e != c.end)
        throw BadResponse(who + ": server sent '" + v + "'");
    if (total != "*" && to_ll(total) != expect.total)
        throw BadResponse(who + ": file size changed ('" + v + "')");

    // 4. Content-Length, if sent, must be the chunk size.
    auto cl = h.find("content-length");
    if (cl != h.end() && to_ll(cl->second) != c.size())
        throw BadResponse(who + ": Content-Length " + cl->second +
                          ", expected " + std::to_string(c.size()));
}
