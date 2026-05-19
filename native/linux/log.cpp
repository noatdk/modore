#include "log.hpp"

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <mutex>
#include <string>

#include <ctime>

#include <sys/stat.h>
#include <unistd.h>

namespace {

std::mutex g_log_mu;
FILE* g_log_file = nullptr;

// Returns "YYYY-mm-dd HH:MM:SS.mmm " (fixed width for grep/sort).
void format_timestamp(char* out, size_t cap) {
  if (!out || cap < 28) {
    return;
  }
  const auto now = std::chrono::system_clock::now();
  const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch());
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm_local{};
  if (localtime_r(&t, &tm_local) == nullptr) {
    std::snprintf(out, cap, "(no-time) ");
    return;
  }
  const auto ms = static_cast<int>(now_ms.count() % 1000);
  std::strftime(out, cap, "%Y-%m-%d %H:%M:%S", &tm_local);
  std::snprintf(out + std::strlen(out), cap - std::strlen(out), ".%03d ", ms);
}

std::string modore_log_file_path() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  if (xdg && *xdg) {
    return std::string(xdg) + "/modore/modore.log";
  }
  const char* home = std::getenv("HOME");
  if (home && *home) {
    return std::string(home) + "/.config/modore/modore.log";
  }
  return std::string("/.config/modore/modore.log");
}

void mkdir_p_for_prefix(const std::string& path) {
  if (path.empty()) {
    return;
  }
  if (path[0] != '/') {
    return;
  }
  for (size_t i = 1; i <= path.size(); ++i) {
    if (i == path.size() || path[i] == '/') {
      const std::string part = path.substr(0, i);
      if (part.size() > 1) {
        if (::mkdir(part.c_str(), 0755) != 0 && errno != EEXIST) {
          // Leave g_log_file null; stderr logging still works.
        }
      }
    }
  }
}

void ensure_parent_dir(const std::string& filepath) {
  const auto pos = filepath.find_last_of('/');
  if (pos == std::string::npos || pos == 0) {
    return;
  }
  mkdir_p_for_prefix(filepath.substr(0, pos));
}

FILE* log_file_stream() {
  if (g_log_file) {
    return g_log_file;
  }
  const std::string path = modore_log_file_path();
  ensure_parent_dir(path);
  g_log_file = std::fopen(path.c_str(), "a");
  return g_log_file;
}

// One place to change the line shape; touch this, not call sites.
void write_log_line(const char* tag, const char* fmt, va_list ap) {
  std::lock_guard<std::mutex> lock(g_log_mu);

  char ts[32]{};
  format_timestamp(ts, sizeof(ts));

  va_list ap2;
  va_copy(ap2, ap);
  std::fprintf(stderr, "%s[%s] ", ts, tag);
  std::vfprintf(stderr, fmt, ap);
  std::fprintf(stderr, "\n");

  FILE* out = log_file_stream();
  if (out) {
    std::fprintf(out, "%s[%s] ", ts, tag);
    std::vfprintf(out, fmt, ap2);
    std::fprintf(out, "\n");
    std::fflush(out);
  }
  va_end(ap2);
}

}  // namespace

bool modore_e2e_trace_enabled() {
  static int cached = -1;
  if (cached < 0) {
    const char* e = std::getenv("MODORE_E2E_TRACE");
    cached = (e && e[0] && std::strcmp(e, "0") != 0) ? 1 : 0;
  }
  return cached != 0;
}

void modore_log(const char* tag, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  write_log_line(tag, fmt, ap);
  va_end(ap);
}

// Back-compat: untagged calls land in [host] until they're migrated. Same
// renderer as modore_log so the format never diverges.
void modore_logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  write_log_line("host", fmt, ap);
  va_end(ap);
}
