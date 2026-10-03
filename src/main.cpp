#include <curl/curl.h>

#include <iostream>
#include <string>

#include "http_client.h"
#include "probe.h"

int main(int argc, char** argv) {
    if (argc < 3 || std::string(argv[1]) != "probe") {
        std::cerr << "usage: netsplit probe <url>\n";
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    int exit_code = 0;
    try {
        HttpClient client;   // destroyed at the end of this try block
        ProbeResult p = probe(client, argv[2]);

        std::cout << "File name:     " << p.filename << "\n";
        std::cout << "Size:          "
                  << (p.size >= 0 ? std::to_string(p.size) + " bytes" : "unknown")
                  << "\n";
        std::cout << "Range support: " << (p.ranges_supported ? "yes" : "no") << "\n";
        std::cout << "ETag:          " << (p.etag.empty() ? "(none)" : p.etag) << "\n";
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        exit_code = 1;
    }
    curl_global_cleanup();
    return exit_code;
}