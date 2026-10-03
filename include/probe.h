#pragma once
#include <string>

#include "http_client.h"

struct ProbeResult {
    long long size = -1;            // -1 means unknown
    bool ranges_supported = false;
    std::string etag;
    std::string filename;
};

ProbeResult probe(HttpClient& client, const std::string& url);
