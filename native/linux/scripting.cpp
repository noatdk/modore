// scripting.cpp — Linux-side façade for libmodore_script.so.

#include "scripting.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <algorithm>

extern "C" {
#include "modore_script.h"
}

#include "log.hpp"

namespace modore_script {
namespace {

// Single-writer-at-boot invariant: boot() runs once from main() before the
// IPC pickup thread starts; subsequent reads come from that thread. No lock
// because Linux's pthread create/join is a full memory barrier.
mdr_engine_t* g_engine = nullptr;

int log_trampoline(void* /*userdata*/, int level, const char* tag, const char* msg) {
  const char* prefix = "";
  switch (level) {
    case MDR_LOG_WARN:  prefix = "WARN ";  break;
    case MDR_LOG_ERROR: prefix = "ERROR "; break;
    default: break;
  }
  char composite_tag[64];
  std::snprintf(composite_tag, sizeof(composite_tag), "scripting:%s", tag ? tag : "lua");
  modore_log(composite_tag, "%s%s", prefix, msg ? msg : "");
  return 0;
}

std::size_t clamp_to_boundary(const char* s, std::size_t len, std::size_t pos) {
  pos = std::min(pos, len);
  while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0u) == 0x80u) --pos;
  return pos;
}

std::size_t next_char(const char* s, std::size_t len, std::size_t pos) {
  if (pos >= len) return len;
  ++pos;
  while (pos < len && (static_cast<unsigned char>(s[pos]) & 0xC0u) == 0x80u) ++pos;
  return pos;
}

std::size_t prev_char(const char* s, std::size_t pos) {
  if (pos == 0) return 0;
  --pos;
  while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0u) == 0x80u) --pos;
  return pos;
}

bool is_ws(const char* s, std::size_t pos) {
  const unsigned char c = static_cast<unsigned char>(s[pos]);
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_ascii(const char* s, std::size_t pos) {
  return static_cast<unsigned char>(s[pos]) < 0x80u;
}

int default_pickup(void* /*userdata*/, const mdr_pickup_ctx_t* ctx, mdr_span_t* out) {
  if (!ctx || !out || !ctx->full_text) return 0;
  const char* s = ctx->full_text;
  const std::size_t len = ctx->full_text_len;
  const std::size_t caret = clamp_to_boundary(s, len, ctx->caret_byte);

  std::size_t start = caret;
  while (start > 0) {
    const std::size_t prev = prev_char(s, start);
    if (is_ws(s, prev)) break;
    start = prev;
  }

  std::size_t end = caret;
  while (end < len) {
    if (is_ws(s, end)) break;
    end = next_char(s, len, end);
  }

  if (start == end) {
    if (caret < len) {
      start = caret;
      end = next_char(s, len, caret);
    } else if (caret > 0) {
      start = prev_char(s, caret);
      end = caret;
    }
  }

  out->span_start_byte = start;
  out->span_end_byte = end;
  out->romaji = nullptr;
  out->romaji_len = 0;
  return start < end ? 1 : 0;
}

int default_replacement(void* /*userdata*/,
                        const char* /*app_id*/,
                        const mdr_span_t* /*span*/,
                        const char* const* cands,
                        std::size_t n_cands,
                        char* out_buf,
                        std::size_t out_cap,
                        std::size_t* out_len) {
  if (!cands || n_cands == 0 || !cands[0] || !out_buf || !out_len || out_cap == 0) return 0;
  std::size_t n = std::strlen(cands[0]);
  if (n >= out_cap) n = out_cap - 1;
  std::memcpy(out_buf, cands[0], n);
  out_buf[n] = '\0';
  *out_len = n;
  return 1;
}

int default_route(void* /*userdata*/, const char* /*app_id*/, mdr_route_t* out_route) {
  if (!out_route) return 0;
  *out_route = MDR_ROUTE_DEFAULT;
  return 1;
}

}  // namespace

void boot(const std::string& script_dir) {
  if (g_engine) return;
  g_engine = mdr_init();
  if (!g_engine) {
    modore_log("scripting", "engine init failed");
    return;
  }
  mdr_set_log_callback(g_engine, log_trampoline, nullptr);
  mdr_set_defaults(g_engine, nullptr, default_pickup, default_replacement, default_route);
  mdr_load_dir(g_engine, script_dir.c_str());
  modore_log("scripting", "engine ABI v%d loaded (dir=%s)",
             mdr_abi_version(), script_dir.c_str());
}

void shutdown() {
  if (!g_engine) return;
  mdr_shutdown(g_engine);
  g_engine = nullptr;
}

bool is_loaded() { return g_engine != nullptr; }

std::optional<Route> route_for(const char* app_id) {
  if (!g_engine) return std::nullopt;
  mdr_route_t out = MDR_ROUTE_DEFAULT;
  int rc = mdr_route(g_engine, app_id, &out);
  if (rc != 1) return std::nullopt;
  switch (out) {
    case MDR_ROUTE_AX:        return Route::Ax;
    case MDR_ROUTE_KEYSTROKE: return Route::Keystroke;
    case MDR_ROUTE_CLIPBOARD: return Route::Clipboard;
    default:                  return std::nullopt;
  }
}

std::optional<std::pair<std::size_t, std::size_t>> pickup_span(
    const std::string& full_text,
    std::size_t caret_byte,
    const char* app_id,
    bool katakana) {
  if (!g_engine) return std::nullopt;
  mdr_pickup_ctx_t ctx{};
  ctx.full_text     = full_text.c_str();
  ctx.full_text_len = full_text.size();
  ctx.caret_byte    = caret_byte;
  ctx.app_id        = app_id;
  ctx.flags         = katakana ? 1u : 0u;
  mdr_span_t span{};
  int rc = mdr_pickup(g_engine, &ctx, &span);
  if (rc != 1) return std::nullopt;
  return std::make_pair<std::size_t, std::size_t>(
      static_cast<std::size_t>(span.span_start_byte),
      static_cast<std::size_t>(span.span_end_byte));
}

std::optional<std::string> replacement(
    const char* app_id,
    std::size_t span_start_byte,
    std::size_t span_end_byte,
    const std::vector<std::string>& candidates) {
  if (!g_engine) return std::nullopt;
  mdr_span_t span{};
  span.span_start_byte = span_start_byte;
  span.span_end_byte   = span_end_byte;
  span.romaji          = nullptr;
  span.romaji_len      = 0;

  std::vector<const char*> raw;
  raw.reserve(candidates.size());
  for (const auto& s : candidates) raw.push_back(s.c_str());

  char buf[2048];
  std::size_t out_len = 0;
  int rc = mdr_replacement(g_engine, app_id, &span,
                           raw.empty() ? nullptr : raw.data(), raw.size(),
                           buf, sizeof(buf), &out_len);
  if (rc != 1 || out_len == 0) return std::nullopt;
  // Defensive: engine clamps to out_cap - 1, but assert in case a future
  // change ever violates that contract.
  if (out_len >= sizeof(buf)) return std::nullopt;
  return std::string(buf, out_len);
}

std::optional<std::vector<std::string>> candidates(
    const char* app_id,
    const std::vector<std::string>& list,
    int current_index) {
  if (!g_engine) return std::nullopt;
  std::vector<const char*> raw;
  raw.reserve(list.size());
  for (const auto& s : list) raw.push_back(s.c_str());

  char buf[8192];
  std::size_t out_count = 0;
  int rc = mdr_candidates(g_engine, app_id,
                          raw.empty() ? nullptr : raw.data(), raw.size(),
                          current_index, buf, sizeof(buf), &out_count);
  if (rc != 1 || out_count == 0) return std::nullopt;
  // Sanity cap: even with one-byte candidates, out_count can't exceed the
  // buffer size in bytes. Reject obviously corrupt engine output.
  if (out_count > sizeof(buf)) return std::nullopt;

  std::vector<std::string> result;
  result.reserve(out_count);
  const char* p = buf;
  const char* end = buf + sizeof(buf);
  for (std::size_t i = 0; i < out_count; ++i) {
    if (p >= end) break;
    // Defensive: scan for NUL before constructing the string.
    const char* probe = p;
    while (probe < end && *probe != '\0') ++probe;
    if (probe >= end) break;
    result.emplace_back(p, static_cast<std::size_t>(probe - p));
    p = probe + 1;
  }
  return result.empty() ? std::nullopt : std::optional<std::vector<std::string>>(std::move(result));
}

}  // namespace modore_script
