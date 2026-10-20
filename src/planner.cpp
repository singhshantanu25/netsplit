#include "planner.h"

#include <algorithm>
#include <stdexcept>

Planner::Planner(long long total_size, long long chunk_size)
    : total_size_(total_size), chunk_size_(chunk_size) {
    if (total_size <= 0)
        throw std::invalid_argument("total size must be positive");
    if (chunk_size <= 0)
        throw std::invalid_argument("chunk size must be positive");

    int index = 0;
    for (long long start = 0; start < total_size; start += chunk_size) {
        long long end = std::min(start + chunk_size, total_size) - 1;
        chunks_.push_back(Chunk{index++, start, end});
    }
}

const Chunk& Planner::chunk(int i) const {
    if (i < 0 || i >= count())
        throw std::out_of_range("chunk index " + std::to_string(i) +
                                " out of range (0.." +
                                std::to_string(count() - 1) + ")");
    return chunks_[static_cast<std::size_t>(i)];
}