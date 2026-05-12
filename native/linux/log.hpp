#pragma once

// Writes the same line to stderr and ~/.config/modore/modore.log (see implementation
// for XDG_CONFIG_HOME). Thread-safe.
//
// New code should call `modore_log(tag, fmt, ...)` so the log line carries a
// subsystem tag (`[boot]`, `[config]`, `[ipc]`, etc.). The legacy
// `modore_logf(fmt, ...)` keeps working but tags lines `[host]`; migrate
// clusters opportunistically as you touch them — see `native/macos/Log.swift`
// for the parallel tag vocabulary.
//
// The line format (`<timestamp> [<tag>] <message>`) lives in log.cpp. Change
// it there, not at call sites.

void modore_log(const char* tag, const char* fmt, ...);
void modore_logf(const char* fmt, ...);

[[nodiscard]] bool modore_e2e_trace_enabled();

// Verbose step trace for E2E / Puppeteer runs. Enable with MODORE_E2E_TRACE=1.
#define MODORE_E2E_LOGF(...)                                                           \
  do {                                                                                 \
    if (modore_e2e_trace_enabled()) {                                                  \
      modore_log("e2e", __VA_ARGS__);                                                  \
    }                                                                                  \
  } while (0)
