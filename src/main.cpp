#include <curl/curl.h>
#include <iostream>
#include <string>

static size_t write_cb(char* data, size_t size, size_t nmemb, void* userp) {
    size_t n = size * nmemb;
    static_cast<std::string*>(userp)->append(data, n);
    return n;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: netsplit <url>\n";
        return 1;
    }

    curl_global_init(CURL_GLOBAL_DEFAULT);
    CURL* h = curl_easy_init();

    std::string body;
    curl_easy_setopt(h, CURLOPT_URL, argv[1]);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);

    CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        std::cerr << "request failed: " << curl_easy_strerror(rc) << "\n";
        curl_easy_cleanup(h);
        curl_global_cleanup();
        return 1;
    }

    long status = 0;
    curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
    std::cout << "HTTP status: " << status << "\n";
    std::cout << "Body size:   " << body.size() << " bytes\n";

    curl_easy_cleanup(h);
    curl_global_cleanup();
    return 0;
}