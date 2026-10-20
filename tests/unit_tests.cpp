// Unit tests for NetSplit's pure logic: no network, no files, no curl.
// Build: cmake --build build     Run: ./build/unit_tests   (or: cd build && ctest)
//
// We use our own CHECK instead of assert(): assert() is removed when the code is
// built with -DNDEBUG (release builds), and then a test would silently check nothing.

#include <atomic>
#include <cstdint>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "data_budget.h"
#include "errors.h"
#include "planner.h"
#include "validate.h"

namespace {

int g_checks = 0;
int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            std::cerr << "  FAILED: " #cond "  (" << __FILE__ << ":" << __LINE__ \
                      << ")\n";                                                  \
        }                                                                        \
    } while (0)

// CHECK_THROWS(expr, Type): expr must throw exactly this kind of exception.
#define CHECK_THROWS(expr, Type)                                                 \
    do {                                                                         \
        bool thrown = false;                                                     \
        try {                                                                    \
            expr;                                                                \
        } catch (const Type&) {                                                  \
            thrown = true;                                                       \
        } catch (...) {                                                          \
        }                                                                        \
        CHECK(thrown && "expected " #Type " from: " #expr);                      \
    } while (0)

constexpr std::uint64_t MB = 1024 * 1024;

// ---------------------------------------------------------------- Planner

// Chunks must cover [0, total) exactly: no gap, no overlap, none too big.
void check_plan(long long total, long long chunk_size) {
    Planner plan(total, chunk_size);
    long long expected_count = (total + chunk_size - 1) / chunk_size;   // ceil
    CHECK(plan.count() == expected_count);
    long long next = 0;
    for (int i = 0; i < plan.count(); ++i) {
        const Chunk& c = plan.chunk(i);
        CHECK(c.index == i);
        CHECK(c.start == next);                 // no gap, no overlap
        CHECK(c.size() >= 1 && c.size() <= chunk_size);
        next = c.end + 1;
    }
    CHECK(next == total);                       // ends exactly at the last byte
}

void test_planner() {
    check_plan(1, 4 * MB);                    // tiny file: one 1-byte chunk
    check_plan(4 * MB, 4 * MB);               // exactly one chunk
    check_plan(4 * MB + 1, 4 * MB);           // one extra byte -> 2 chunks
    check_plan(100 * MB, 4 * MB);             // the Linode file: 25 chunks
    check_plan(10485760, 4 * MB);             // sample.bin: 3 chunks (4, 4, 2 MB)
    check_plan(123456789, 1000);              // odd sizes

    Planner p(10485760, 4 * MB);
    CHECK(p.chunk(2).range() == "8388608-10485759");   // HTTP Range is inclusive
    CHECK(p.chunk(2).size() == 2 * static_cast<long long>(MB));

    CHECK_THROWS(Planner(0, 4 * MB), std::invalid_argument);
    CHECK_THROWS(Planner(100, 0), std::invalid_argument);
    CHECK_THROWS(p.chunk(3), std::out_of_range);
    CHECK_THROWS(p.chunk(-1), std::out_of_range);
}

// ---------------------------------------------------- Content-Range checks

using Headers = std::map<std::string, std::string>;

const Chunk kChunk1{1, 4194304, 8388607};   // second 4 MB chunk of a 10 MB file
const Expect kExpect{10485760, "\"v1\""};

Headers good_206() {
    return {{"etag", "\"v1\""},
            {"content-range", "bytes 4194304-8388607/10485760"},
            {"content-length", "4194304"}};
}

void test_validation() {
    // The correct answer passes.
    bool ok = true;
    try {
        check_response(kChunk1, kExpect, 206, good_206());
    } catch (...) {
        ok = false;
    }
    CHECK(ok);

    // Unknown total ("*") is allowed.
    Headers h = good_206();
    h["content-range"] = "bytes 4194304-8388607/*";
    try {
        check_response(kChunk1, kExpect, 206, h);
    } catch (...) {
        ok = false;
    }
    CHECK(ok);

    // 200 = Range ignored: fine for a chunk at byte 0, wrong for any other chunk.
    Headers whole = {{"etag", "\"v1\""}, {"content-length", "10485760"}};
    const Chunk chunk0{0, 0, 4194303};
    try {
        check_response(chunk0, kExpect, 200, whole);
    } catch (...) {
        ok = false;
    }
    CHECK(ok);
    CHECK_THROWS(check_response(kChunk1, kExpect, 200, whole), BadResponse);

    // Every wrong answer the fault server can send must be rejected.
    CHECK_THROWS(check_response(kChunk1, kExpect, 404, good_206()), BadResponse);
    CHECK_THROWS(check_response(kChunk1, kExpect, 429, good_206()), BadResponse);

    h = good_206(); h["content-range"] = "bytes 0-4194303/10485760";        // wrongstart
    CHECK_THROWS(check_response(kChunk1, kExpect, 206, h), BadResponse);
    h = good_206(); h["content-range"] = "bytes 4194305-8388607/10485760";  // start only
    CHECK_THROWS(check_response(kChunk1, kExpect, 206, h), BadResponse);
    h = good_206(); h["content-range"] = "bytes 4194304-8388606/10485760";  // end only
    CHECK_THROWS(check_response(kChunk1, kExpect, 206, h), BadResponse);
    h = good_206(); h["content-range"] = "bytes 4194304-5242879/10485760";  // cap (1 MB)
    CHECK_THROWS(check_response(kChunk1, kExpect, 206, h), BadResponse);
    h = good_206(); h["content-range"] = "bytes 4194304-8388607/99999999";  // size changed
    CHECK_THROWS(check_response(kChunk1, kExpect, 206, h), BadResponse);
    h = good_206(); h["etag"] = "\"v2\"";                                    // changed
    CHECK_THROWS(check_response(kChunk1, kExpect, 206, h), BadResponse);
    h = good_206(); h.erase("etag");                                         // no ETag
    CHECK_THROWS(check_response(kChunk1, kExpect, 206, h), BadResponse);
    h = good_206(); h.erase("content-range");                                // missing
    CHECK_THROWS(check_response(kChunk1, kExpect, 206, h), BadResponse);
    h = good_206(); h["content-length"] = "100";                             // length
    CHECK_THROWS(check_response(kChunk1, kExpect, 206, h), BadResponse);

    // Garbage must be rejected, never crash.
    for (const char* bad : {"bytes abc-def/10485760", "4194304-8388607/10485760",
                            "bytes 4194304/10485760", "bytes -/", "", "bytes 9-1/x"}) {
        h = good_206();
        h["content-range"] = bad;
        CHECK_THROWS(check_response(kChunk1, kExpect, 206, h), BadResponse);
    }

    CHECK(chunk_name(kChunk1) == "chunk 1 (4194304-8388607)");
}

// ------------------------------------------------------------ DataBudget

void test_parsing() {
    CHECK(parse_size("500MB") == 500 * MB);
    CHECK(parse_size("1.5GB") == 1536 * MB);
    CHECK(parse_size("800m") == 800 * MB);
    CHECK(parse_size("2048") == 2048);
    CHECK_THROWS(parse_size("5XB"), std::runtime_error);
    CHECK_THROWS(parse_size("-3MB"), std::runtime_error);
    CHECK_THROWS(parse_size("MB"), std::runtime_error);

    CHECK(parse_percent("2%") == 0.02);
    CHECK(parse_percent("0.05") == 0.05);
    CHECK_THROWS(parse_percent("70%"), std::runtime_error);
    CHECK_THROWS(parse_percent("abc"), std::runtime_error);
}

void test_budget_single_thread() {
    DataBudget b(20 * MB, 0.05);               // usable = 19 MB
    CHECK(b.usable() == 20 * MB - 1 * MB);

    CHECK(b.try_reserve(4 * MB));               // 4 reserved
    CHECK(b.try_reserve(4 * MB));               // 8
    CHECK(b.try_reserve(4 * MB));               // 12
    CHECK(b.try_reserve(4 * MB));               // 16
    CHECK(!b.try_reserve(4 * MB));              // 20 > 19: refused
    CHECK(b.reserved() == 16 * MB);
    CHECK(b.used() == 0);

    b.use(4 * MB, 4 * MB);                      // one chunk arrived fully
    CHECK(b.used() == 4 * MB && b.reserved() == 12 * MB);
    b.release(4 * MB);                          // one chunk failed before any byte
    CHECK(b.reserved() == 8 * MB);
    CHECK(b.try_reserve(3 * MB));               // 4 + 8 + 3 = 15 <= 19
    CHECK(!b.try_reserve(5 * MB));              // 15 + 5 = 20 > 19

    CHECK_THROWS(DataBudget(0), std::invalid_argument);
}

void test_reservation_raii() {
    DataBudget b(10 * MB, 0.0);
    CHECK(b.try_reserve(4 * MB));
    {
        Reservation r(&b, 4 * MB);
        r.consume(1 * MB);                      // 1 MB arrived, then the chunk failed
    }                                           // destructor gives back the other 3 MB
    CHECK(b.used() == 1 * MB);                  // the 1 MB stays counted: it was paid for
    CHECK(b.reserved() == 0);

    // Also when an exception leaves the scope.
    CHECK(b.try_reserve(4 * MB));
    try {
        Reservation r(&b, 4 * MB);
        r.consume(2 * MB);
        throw std::runtime_error("connection reset");
    } catch (const std::runtime_error&) {
    }
    CHECK(b.used() == 3 * MB);
    CHECK(b.reserved() == 0);

    // A Wi-Fi worker has no budget: Reservation(nullptr) does nothing.
    Reservation none(nullptr, 4 * MB);
    none.consume(4 * MB);
    CHECK(b.used() == 3 * MB);
}

// Many threads race for the same budget, like phone workers do.
// The limit must hold at every moment, and exactly as many chunks as fit must win.
void test_budget_threads() {
    const std::uint64_t chunk = 1 * MB;
    DataBudget b(100 * MB, 0.05);               // usable = 95 MB -> 95 chunks fit
    std::atomic<int> granted{0};
    std::atomic<bool> broken{false};
    std::atomic<bool> done{false};

    // Watcher: checks the invariant over and over while the workers run.
    std::thread watcher([&] {
        while (!done.load())
            if (b.used() + b.reserved() > b.usable()) broken = true;
    });

    std::vector<std::thread> workers;
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&] {
            // The cap (1000) only matters if the budget is broken: then the test
            // must FAIL (granted != 95), not loop forever.
            while (granted.load() < 1000 && b.try_reserve(chunk)) {
                ++granted;
                Reservation r(&b, chunk);
                for (int k = 0; k < 16; ++k) r.consume(chunk / 16);   // data arrives in pieces
            }
        });
    }
    for (auto& w : workers) w.join();
    done = true;
    watcher.join();

    CHECK(!broken.load());                      // never over the limit, not even for a moment
    CHECK(granted.load() == 95);                // exactly what fits, no more, no less
    CHECK(b.used() == 95 * MB);
    CHECK(b.reserved() == 0);
    CHECK(b.used() <= b.usable());
}

// Same race, but every second chunk "fails" halfway: failed bytes still count.
void test_budget_threads_with_failures() {
    const std::uint64_t chunk = 1 * MB;
    DataBudget b(50 * MB, 0.0);
    std::vector<std::thread> workers;
    std::atomic<int> attempt{0};
    for (int t = 0; t < 8; ++t) {
        workers.emplace_back([&] {
            while (attempt.load() < 1000 && b.try_reserve(chunk)) {   // cap: see above
                Reservation r(&b, chunk);
                bool fails = (attempt.fetch_add(1) % 2) == 0;
                r.consume(fails ? chunk / 2 : chunk);
            }
        });
    }
    for (auto& w : workers) w.join();
    CHECK(b.used() <= b.usable());              // counted bytes never pass the limit
    CHECK(b.reserved() == 0);
    CHECK(b.used() + chunk > b.usable());       // and the budget was really used up
}

}  // namespace

int main() {
    struct Test {
        const char* name;
        void (*fn)();
    };
    const Test tests[] = {
        {"planner covers the file exactly", test_planner},
        {"Content-Range / status / ETag validation", test_validation},
        {"size and percent parsing", test_parsing},
        {"DataBudget, one thread", test_budget_single_thread},
        {"Reservation gives back unused bytes (RAII)", test_reservation_raii},
        {"DataBudget, 8 threads racing", test_budget_threads},
        {"DataBudget, 8 threads with failed chunks", test_budget_threads_with_failures},
    };
    for (const auto& t : tests) {
        int before = g_failures;
        t.fn();
        std::cout << (g_failures == before ? "  ok    " : "  FAIL  ") << t.name << "\n";
    }
    std::cout << g_checks << " checks, " << g_failures << " failed\n";
    return g_failures == 0 ? 0 : 1;
}
