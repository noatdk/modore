#pragma once

// Writes the same line to stderr and ~/.config/modore/modore.log (see implementation
// for XDG_CONFIG_HOME). Thread-safe.

void modore_logf(const char* fmt, ...);

[[nodiscard]] bool modore_e2e_trace_enabled();

// Verbose step trace for E2E / Puppeteer runs. Enable with MODORE_E2E_TRACE=1.
#define MODORE_E2E_LOGF(...)                                                           \
  do {                                                                                 \
    if (modore_e2e_trace_enabled()) {                                                  \
      modore_logf("[e2e] " __VA_ARGS__);                                               \
    }                                                                                  \
  } while (0)
