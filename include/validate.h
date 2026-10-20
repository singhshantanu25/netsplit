#pragma once
#include <map>
#include <string>

#include "planner.h"

// What the probe told us about the file. Every chunk answer must agree with it.
struct Expect {
    long long total;    // full file size
    std::string etag;   // empty = server sent no ETag
};

// "chunk 3 (12582912-16777215)" for messages.
std::string chunk_name(const Chunk& c);

// Pure check (no network, no files): throws BadResponse unless this answer is
// exactly the chunk we asked for — ETag, status (206, or 200 only for a chunk
// that starts at 0), Content-Range "bytes START-END/TOTAL", Content-Length.
// headers: lowercase names, like HttpClient fills them.
void check_response(const Chunk& c, const Expect& expect, long status,
                    const std::map<std::string, std::string>& headers);
