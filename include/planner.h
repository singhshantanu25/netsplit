#pragma once
#include <string>
#include <vector>

// One piece of the file. Both start and end are inclusive, like HTTP Range.
struct Chunk {
    int index;
    long long start;
    long long end;

    long long size() const { return end - start + 1; }
    std::string range() const {
        return std::to_string(start) + "-" + std::to_string(end);
    }
};

// Splits a file of total_size bytes into chunks of chunk_size bytes.
// Promise: chunks cover the whole file with no gaps and no overlaps.
class Planner {
public:
    Planner(long long total_size, long long chunk_size);

    int count() const { return static_cast<int>(chunks_.size()); }
    const Chunk& chunk(int i) const;
    long long total_size() const { return total_size_; }
    long long chunk_size() const { return chunk_size_; }

private:
    long long total_size_;
    long long chunk_size_;
    std::vector<Chunk> chunks_;
};