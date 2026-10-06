#pragma once
#include <string>

// Reads the whole file in blocks and returns its SHA-256 as 64 hex characters.
std::string sha256_file(const std::string& path);