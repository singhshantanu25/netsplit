#pragma once
#include <cstdint>
#include <mutex>
#include <string>

// How much mobile data the phone may use in this run (single responsibility:
// it only counts and decides; workers ask it before each chunk).
//
//   used     = bytes that really arrived over the phone
//   reserved = bytes promised to chunks that are still downloading
//
// A phone worker may take a chunk only if
//   used + reserved + chunk_size <= limit - margin
// The check and the reservation happen under ONE lock, so two phone workers
// can never both squeeze in when only one chunk still fits.
class DataBudget {
public:
    // margin = part of the limit kept free for TCP/TLS overhead (the phone
    // counts those bytes too, we only see the file data).
    explicit DataBudget(std::uint64_t limit, double margin_fraction = 0.05);

    DataBudget(const DataBudget&) = delete;
    DataBudget& operator=(const DataBudget&) = delete;

    // Check and reserve together. False = this chunk would break the limit.
    bool try_reserve(std::uint64_t bytes);

    // `bytes` arrived over the phone; `from_reserved` of them were promised
    // to a chunk earlier, so they move from reserved to used.
    void use(std::uint64_t bytes, std::uint64_t from_reserved);

    // A chunk did not need all its reservation (finished early or failed).
    void release(std::uint64_t bytes);

    std::uint64_t used() const;
    std::uint64_t reserved() const;
    std::uint64_t limit() const { return limit_; }
    std::uint64_t usable() const { return limit_ - margin_; }   // limit - margin

private:
    mutable std::mutex m_;
    const std::uint64_t limit_;
    const std::uint64_t margin_;
    std::uint64_t used_ = 0;       // guarded by m_
    std::uint64_t reserved_ = 0;   // guarded by m_
};

// One chunk's share of the budget (RAII). Created right after try_reserve()
// succeeded. Every byte that arrives is counted with consume() — also bytes of
// an attempt that later fails, because the phone already paid for them.
// The destructor gives back whatever the chunk did not use, even when an
// exception ends the download early.
class Reservation {
public:
    Reservation(DataBudget* budget, std::uint64_t reserved)
        : budget_(budget), left_(budget ? reserved : 0) {}
    ~Reservation() {
        if (budget_ && left_ > 0) budget_->release(left_);
    }
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

    // Called from the curl write callback for every piece of data.
    void consume(std::uint64_t bytes) {
        if (!budget_) return;
        std::uint64_t from_reserved = bytes < left_ ? bytes : left_;
        left_ -= from_reserved;
        budget_->use(bytes, from_reserved);   // extra bytes still count as used
    }

private:
    DataBudget* budget_;   // nullptr = no limit (Wi-Fi worker)
    std::uint64_t left_;   // reserved but not received yet
};

// "500MB" -> 524288000, "1.5GB", "800M", "1048576" (bytes). MB = 1024*1024 bytes,
// like every "MB" NetSplit prints. Throws on bad input.
std::uint64_t parse_size(const std::string& text);
