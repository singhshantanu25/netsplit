#pragma once

// Ctrl+C handling. The signal handler only sets an atomic flag; workers and
// curl check the flag and stop at a safe point (never in the middle of a write).
void install_stop_handler();   // call once at program start
bool stop_requested();         // true after Ctrl+C (SIGINT) or SIGTERM
