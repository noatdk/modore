// scripting.hpp — Linux-side façade for libmodore_script.so.
//
// Mirrors the shape of native/macos/Scripting/ModoreScript.swift. One
// engine instance lives for the process. Hook callers return std::optional
// — empty means "no script weighed in, use host default."
//
// All strings are UTF-8 (no AX-style UTF-16 boundary). Span offsets are
// UTF-8 BYTE offsets across the C ABI; the AT-SPI flow uses CHAR offsets,
// so the call sites convert with g_utf8_offset_to_pointer at the boundary.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace modore_script {

enum class Route {
  Ax,
  Keystroke,
  Clipboard,
};

// One-shot boot. Safe to call when the dylib is absent — function logs and
// leaves the engine disabled (every hook caller returns nullopt).
void boot(const std::string &script_dir);

// Process-exit teardown. Optional — engine state is released on exit anyway.
void shutdown();

bool is_loaded();

// Hook callers. Return std::nullopt when no script weighed in (undefined
// hook, returned nil, or raised); the host should fall back to its default.

std::optional<Route> route_for(const char *app_id);

// `full_text` UTF-8, `caret_byte` UTF-8 byte offset into full_text.
// Returns a (start_byte, end_byte) UTF-8 span on success.
std::optional<std::pair<std::size_t, std::size_t>>
pickup_span(const std::string &full_text, std::size_t caret_byte,
            const char *app_id, bool katakana);

// `span` describes a substring of the pickup full_text in byte offsets;
// `candidates[0]` is the host's current top choice (Mozc's). Returns the
// script-chosen replacement string, or nullopt for "use the host default."
std::optional<std::string>
replacement(const char *app_id, std::size_t span_start_byte,
            std::size_t span_end_byte,
            const std::vector<std::string> &candidates);

// Optional reorder/filter of the candidate list. Not yet wired into the
// Linux pickup pipeline (no structured cycle path exposes it today); kept
// here so a future Cycle integration can plug in without changing the API.
std::optional<std::vector<std::string>>
candidates(const char *app_id, const std::vector<std::string> &list,
           int current_index);

} // namespace modore_script
