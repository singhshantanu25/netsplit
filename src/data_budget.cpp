#include "data_budget.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

DataBudget::DataBudget(std::uint64_t limit, double margin_fraction)
    : limit_(limit), margin_(static_cast<std::uint64_t>(limit * margin_fraction)) {
    if (limit == 0) throw std::invalid_argument("mobile limit must be more than 0");
}

bool DataBudget::try_reserve(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(m_);   // check + reserve = one step
    if (used_ + reserved_ + bytes > limit_ - margin_) return false;
    reserved_ += bytes;
    return true;
}

void DataBudget::use(std::uint64_t bytes, std::uint64_t from_reserved) {
    std::lock_guard<std::mutex> lock(m_);
    used_ += bytes;
    reserved_ -= std::min(reserved_, from_reserved);
}

void DataBudget::release(std::uint64_t bytes) {
    std::lock_guard<std::mutex> lock(m_);
    reserved_ -= std::min(reserved_, bytes);
}

std::uint64_t DataBudget::used() const {
    std::lock_guard<std::mutex> lock(m_);
    return used_;
}

std::uint64_t DataBudget::reserved() const {
    std::lock_guard<std::mutex> lock(m_);
    return reserved_;
}

std::uint64_t parse_size(const std::string& text) {
    std::size_t pos = 0;
    double number = 0;
    try {
        number = std::stod(text, &pos);
    } catch (...) {
        throw std::runtime_error("bad size '" + text + "' (example: 500MB)");
    }
    std::string unit = text.substr(pos);
    std::transform(unit.begin(), unit.end(), unit.begin(),
                   [](unsigned char c) { return std::toupper(c); });

    double factor = 0;
    if (unit.empty() || unit == "B") factor = 1;
    else if (unit == "K" || unit == "KB") factor = 1024.0;
    else if (unit == "M" || unit == "MB") factor = 1024.0 * 1024;
    else if (unit == "G" || unit == "GB") factor = 1024.0 * 1024 * 1024;
    else throw std::runtime_error("bad size unit in '" + text + "' (use KB, MB or GB)");

    if (number <= 0) throw std::runtime_error("size must be more than 0: '" + text + "'");
    return static_cast<std::uint64_t>(number * factor);
}
