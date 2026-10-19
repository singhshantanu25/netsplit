#include "stop.h"

#include <atomic>
#include <csignal>

namespace {

// A signal handler may only touch lock-free atomics (no mutex, no cout, no malloc).
std::atomic<bool> g_stop{false};
static_assert(std::atomic<bool>::is_always_lock_free, "need a lock-free flag");

void on_signal(int sig) {
    g_stop.store(true);
    std::signal(sig, SIG_DFL);   // a second Ctrl+C quits at once, like normal
}

}  // namespace

void install_stop_handler() {
    std::signal(SIGINT, on_signal);    // Ctrl+C
    std::signal(SIGTERM, on_signal);   // "kill <pid>"
}

bool stop_requested() {
    return g_stop.load();
}
