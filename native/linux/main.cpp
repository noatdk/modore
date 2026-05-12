// modore — Linux native host (X11 + AT-SPI2 + Mozc bridge).
//
// Configurable global hotkey (default Ctrl+/; see ~/.config/modore/modore.conf).

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <atspi/atspi.h>
#include <glib.h>
#include <mozc_bridge.h>

#include "config.hpp"
#include "ipc.hpp"
#include "log.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

#define logf modore_logf

// Synthetic keys are dispatched asynchronously in the focused client. `hyprctl dispatch` is
// synchronous for *our* process (waitpid), not for the app. Multi-ms sleeps are usually wasted on
// fast machines. Prefer yields plus a short optional nap (0–12ms) where we cannot observe readiness
// (select-all expansion, paste delivery, clipboard daemon).
static void yield_to_compose_pipeline() {
  std::this_thread::yield();
  std::this_thread::yield();
}

// Experiment: 1 = all nap_after_compose_event() are no-ops (breaks Chromium paste-after-Ctrl+A).
#define MODORE_ZERO_NAP_EXPERIMENT 0

#if MODORE_ZERO_NAP_EXPERIMENT
static void nap_after_compose_event(std::chrono::milliseconds /*unused*/) {}
#else
static void nap_after_compose_event(std::chrono::milliseconds d) {
  yield_to_compose_pipeline();
  if (d.count() > 0) {
    std::this_thread::sleep_for(d);
  }
}
#endif

// Clears IME env for wtype/ydotool children (implemented with Wayland inject helpers).
static void child_clear_im_modules();

std::mutex g_pickup_mu;

// Wakes the main thread when a client sends "pickup" on the unix socket (must
// not run AT-SPI / pickup from the IPC accept thread).
int g_pickup_pipe[2] = {-1, -1};

bool setup_pickup_pipe() {
  if (pipe(g_pickup_pipe) != 0) {
    modore_log("ipc", "pickup pipe: pipe() failed (%s)", std::strerror(errno));
    return false;
  }
  const int flags = fcntl(g_pickup_pipe[1], F_GETFL);
  if (flags >= 0) {
    (void)fcntl(g_pickup_pipe[1], F_SETFL, flags | O_NONBLOCK);
  }
  return true;
}

void notify_main_pickup_pending() {
  if (g_pickup_pipe[1] < 0) {
    return;
  }
  const char k = '\1';
  if (write(g_pickup_pipe[1], &k, 1) < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK) {
      modore_log("ipc", "pickup pipe: write failed (%s)", std::strerror(errno));
    }
  }
}

void run_ipc_pickup();

// IPC thread only writes to the pipe; we drain and run pickup on the main thread
// (AT-SPI must run here).
void main_thread_run_pickup_after_wake() {
  if (g_pickup_pipe[0] < 0) {
    return;
  }
  char buf[256];
  const ssize_t n = read(g_pickup_pipe[0], buf, sizeof(buf));
  if (n <= 0) {
    if (n == 0) {
      modore_log("ipc", "pickup pipe EOF (internal error)");
      _Exit(1);
    }
    if (errno == EINTR) {
      return;
    }
    modore_log("ipc", "pickup pipe read: %s", std::strerror(errno));
    return;
  }
  int n_triggers = 0;
  for (ssize_t i = 0; i < n; ++i) {
    if (buf[static_cast<size_t>(i)] == '\1') {
      ++n_triggers;
    }
  }
  if (n_triggers <= 0) {
    modore_log("ipc", "pickup pipe: ignoring %zd-byte non-trigger payload",
               static_cast<ssize_t>(n));
    return;
  }
  constexpr int kMaxBatched = 6;
  if (n_triggers > kMaxBatched) {
    modore_log("ipc", "pickup pipe: capping batched triggers %d → %d", n_triggers, kMaxBatched);
    n_triggers = kMaxBatched;
  }
  for (int t = 0; t < n_triggers; ++t) {
    modore_log("pickup", "via IPC socket (--trigger)%s", t > 0 ? " (batched)" : "");
    run_ipc_pickup();
  }
}

// XSetErrorHandler callback — sees asynchronous errors after XSync.
static int g_x11_setup_error = 0;
static int x11_quiet_error_handler(Display*, XErrorEvent*) {
  g_x11_setup_error = 1;
  return 0;
}

std::string getenv_string(const char* k, const char* def) {
  const char* s = std::getenv(k);
  return s ? std::string(s) : std::string(def);
}

std::string default_profile_dir() {
  std::string base = getenv_string("XDG_STATE_HOME", "");
  if (base.empty()) {
    base = getenv_string("HOME", "/tmp") + "/.local/state";
  }
  return base + "/modore";
}

// --- Clipboard helpers (xclip on X11, wl-paste on Wayland) -------------

bool command_ok(const char* cmd) { return std::system(cmd) == 0; }

// systemd --user and minimal environments often omit /usr/bin from PATH.
static void augment_path_for_subprocesses() {
  const char* cur = std::getenv("PATH");
  const char* prefix = "/usr/local/sbin:/usr/local/bin:/usr/bin:/bin";
  if (!cur || !*cur) {
    ::setenv("PATH", prefix, 1);
    return;
  }
  std::string merged = std::string(prefix) + ":" + cur;
  ::setenv("PATH", merged.c_str(), 1);
}

static std::string g_wtype_path;

// Resolves the `wtype` binary (Wayland key injection). Must run after augment_path.
static const char* resolve_wtype_executable() {
  if (!g_wtype_path.empty()) {
    return g_wtype_path.c_str();
  }
  static const char* kCandidates[] = {"/usr/bin/wtype", "/usr/local/bin/wtype"};
  for (auto* p : kCandidates) {
    if (::access(p, X_OK) == 0) {
      g_wtype_path = p;
      return g_wtype_path.c_str();
    }
  }
  augment_path_for_subprocesses();
  if (command_ok("command -v wtype >/dev/null 2>&1")) {
    FILE* f = ::popen("command -v wtype", "r");
    if (f) {
      char line[2048];
      if (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
          s.pop_back();
        }
        if (!s.empty() && ::access(s.c_str(), X_OK) == 0) {
          g_wtype_path = std::move(s);
          ::pclose(f);
          return g_wtype_path.c_str();
        }
      }
      ::pclose(f);
    }
  }
  return nullptr;
}

static bool wtype_is_available() { return resolve_wtype_executable() != nullptr; }

// fork+exec (no shell): systemd and quoting cannot break chord arguments; IME
// env is cleared in the child only.
static bool wtype_exec_chord(const char* desc_for_log,
                             const std::vector<const char*>& args) {
  const char* path = resolve_wtype_executable();
  if (!path) {
    logf("wtype: not found (%s)", desc_for_log);
    return false;
  }
  std::vector<char*> av;
  av.reserve(args.size() + 2);
  av.push_back(const_cast<char*>("wtype"));
  for (const char* a : args) {
    av.push_back(const_cast<char*>(a));
  }
  av.push_back(nullptr);

  pid_t pid = fork();
  if (pid < 0) {
    logf("wtype: fork failed (%s)", desc_for_log);
    return false;
  }
  if (pid == 0) {
    child_clear_im_modules();
    execv(path, av.data());
    _exit(127);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  if (!ok) {
    const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    logf("wtype: exit %d (%s)", code, desc_for_log);
  }
  return ok;
}

static bool wtype_chord_shift_insert() {
  return wtype_exec_chord(
      "Shift+Insert paste",
      {"-s", "40", "-M", "shift", "-k", "Insert"});
}

static bool wtype_chord_ctrl_v() {
  return wtype_exec_chord("Ctrl+V paste", {"-s", "40", "-M", "ctrl", "v"});
}

static bool wtype_chord_ctrl_c() {
  return wtype_exec_chord("Ctrl+C", {"-s", "40", "-M", "ctrl", "c"});
}

// Omarchy SUPER+C → sendshortcut CTRL+Insert (Universal copy); GTK4/Wayland often syncs WL
// clipboard from this chord when plain Ctrl+C does not update the clipboard offer.
static bool wtype_chord_ctrl_insert_copy() {
  return wtype_exec_chord("Ctrl+Insert copy",
                          {"-s", "40", "-M", "ctrl", "-k", "Insert"});
}

static bool wtype_chord_ctrl_shift_left() {
  return wtype_exec_chord("Ctrl+Shift+Left",
                          {"-s", "40", "-M", "ctrl", "-M", "shift", "-k", "Left"});
}

static bool wtype_key_right() {
  return wtype_exec_chord("Right", {"-k", "Right"});
}

static bool wtype_key_delete_or_backspace() {
  if (wtype_exec_chord("Delete", {"-k", "Delete"})) {
    return true;
  }
  return wtype_exec_chord("BackSpace", {"-k", "BackSpace"});
}

// --- Hyprland hyprctl sendshortcut (preferred on Hyprland; routes like real keys)

static std::string g_hyprctl_path;

static const char* resolve_hyprctl_executable() {
  if (!g_hyprctl_path.empty()) {
    return g_hyprctl_path.c_str();
  }
  static const char* kCandidates[] = {"/usr/bin/hyprctl", "/usr/local/bin/hyprctl"};
  for (auto* p : kCandidates) {
    if (::access(p, X_OK) == 0) {
      g_hyprctl_path = p;
      return g_hyprctl_path.c_str();
    }
  }
  augment_path_for_subprocesses();
  if (command_ok("command -v hyprctl >/dev/null 2>&1")) {
    FILE* f = ::popen("command -v hyprctl", "r");
    if (f) {
      char line[2048];
      if (std::fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
          s.pop_back();
        }
        if (!s.empty() && ::access(s.c_str(), X_OK) == 0) {
          g_hyprctl_path = std::move(s);
          ::pclose(f);
          return g_hyprctl_path.c_str();
        }
      }
      ::pclose(f);
    }
  }
  return nullptr;
}

static bool fork_hyprctl_version_ok(const char* hc_path) {
  pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
      (void)::dup2(fd, STDOUT_FILENO);
      (void)::dup2(fd, STDERR_FILENO);
      (void)::close(fd);
    }
    execl(hc_path, "hyprctl", "version", nullptr);
    _exit(127);
  }
  int st = 0;
  (void)::waitpid(pid, &st, 0);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static bool hyprctl_ipc_alive_for_wayland_keys() {
  static int cached = -1;
  if (cached >= 0) {
    return cached == 1;
  }
  cached = 0;
  const char* wl = std::getenv("WAYLAND_DISPLAY");
  if (!wl || !wl[0]) {
    return false;
  }
  const char* sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (sig && sig[0]) {
    const char* hc = resolve_hyprctl_executable();
    if (!hc) {
      return false;
    }
    cached = 1;
    return true;
  }
  const char* hc = resolve_hyprctl_executable();
  if (!hc) {
    return false;
  }
  augment_path_for_subprocesses();
  if (fork_hyprctl_version_ok(hc)) {
    cached = 1;
    return true;
  }
  return false;
}

// DISPLAY is usually set alongside WAYLAND_DISPLAY on Hypr/UWSM sessions. Passing a live X11 Display*
// to do_pickup() chooses XTest for synthetic keys, but XTest does not reach native Wayland clients
// — so conversion can run yet nothing appears in Kitty/GTK/etc. Hypr exposes xwayland on activewindow.
// On ambiguity (query failure / old Hypr output), assume native-Wayland and skip X11 injection.
static bool hypr_focus_is_wayland_native() {
  if (!hyprctl_ipc_alive_for_wayland_keys()) {
    return false;
  }
  const char* hc = resolve_hyprctl_executable();
  if (!hc) {
    return false;
  }
  int link[2];
  if (::pipe(link) != 0) {
    return true;
  }
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(link[0]);
    ::close(link[1]);
    return true;
  }
  if (pid == 0) {
    (void)::dup2(link[1], STDOUT_FILENO);
    ::close(link[0]);
    ::close(link[1]);
    int nerr = ::open("/dev/null", O_WRONLY);
    if (nerr >= 0) {
      (void)::dup2(nerr, STDERR_FILENO);
      ::close(nerr);
    }
    ::execl(hc, "hyprctl", "activewindow", "-j", nullptr);
    ::_exit(127);
  }
  ::close(link[1]);
  std::string json;
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(link[0], buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    json.append(buf, static_cast<size_t>(n));
  }
  ::close(link[0]);
  int st = 0;
  (void)::waitpid(pid, &st, 0);
  if (!WIFEXITED(st) || WEXITSTATUS(st) != 0 || json.empty()) {
    return true;
  }
  if (json.find("\"xwayland\": true") != std::string::npos ||
      json.find("\"xwayland\":true") != std::string::npos) {
    return false;
  }
  return true;
}

static bool hyprctl_dispatch_sendshortcut(const char* shortcut_spec, const char* desc_for_log,
                                          bool log_failure = true) {
  const char* hc = resolve_hyprctl_executable();
  if (!hc) {
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    int fd = open("/dev/null", O_WRONLY);
    if (fd >= 0) {
      (void)::dup2(fd, STDOUT_FILENO);
      (void)::dup2(fd, STDERR_FILENO);
      (void)::close(fd);
    }
    execl(hc, "hyprctl", "dispatch", "sendshortcut", shortcut_spec, nullptr);
    _exit(127);
  }
  int st = 0;
  (void)::waitpid(pid, &st, 0);
  const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  if (!ok && log_failure) {
    const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    logf("hyprctl sendshortcut failed exit=%d (%s)", code, desc_for_log);
  }
  return ok;
}

static bool hyprctl_wayland_copy_selection() {
  // Omarchy SUPER+C sends Ctrl+Insert only; many apps also honor Ctrl+C. Hypr can exit 0 for a
  // chord that never reaches the focused GTK surface, so we always send both (brief pause between)
  // — same idea as mashing Super+C on the same selection twice.
  const bool insert =
      hyprctl_dispatch_sendshortcut("CTRL,Insert,",
                                    "Ctrl+Insert (Omarchy SUPER+C universal copy)",
                                    /*log_failure=*/false);
  nap_after_compose_event(std::chrono::milliseconds(1));
  const bool letter_c =
      hyprctl_dispatch_sendshortcut("CTRL,C,", "Ctrl+C (paired with universal copy)",
                                   /*log_failure=*/false);
  const bool any = insert || letter_c;
  if (!any) {
    logf(
        "hyprctl: universal copy pair failed (both Ctrl+Insert and Ctrl+C sendshortcut rejected)");
  }
  return any;
}

static bool hyprctl_wayland_ctrl_shift_left() {
  return hyprctl_dispatch_sendshortcut("CTRL SHIFT,Left,", "Ctrl+Shift+Left");
}

static bool hyprctl_wayland_key_right() {
  return hyprctl_dispatch_sendshortcut(",Right,", "Right");
}

static bool hyprctl_wayland_delete_or_backspace() {
  if (hyprctl_dispatch_sendshortcut(",DELETE,", "Delete")) {
    return true;
  }
  return hyprctl_dispatch_sendshortcut(",BACKSPACE,", "BACKSPACE");
}

static const char* wayland_pickup_synthetic_backend_label(Display* d) {
  if (d) {
    return "XTest";
  }
  if (hyprctl_ipc_alive_for_wayland_keys()) {
    return "hyprctl";
  }
  if (wtype_is_available()) {
    return "wtype";
  }
  return "none";
}

bool read_clipboard_cmd(const char* cmd, std::string* out) {
  out->clear();
  FILE* f = popen(cmd, "r");
  if (!f) {
    return false;
  }
  char buf[4096];
  while (size_t n = fread(buf, 1, sizeof(buf), f)) {
    out->append(buf, n);
  }
  int st = pclose(f);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0 && !out->empty();
}

static const char* resolve_wl_paste() {
  static const char* cached = nullptr;
  if (cached) {
    return cached;
  }
  static const char* kCandidates[] = {"/usr/bin/wl-paste", "/usr/local/bin/wl-paste"};
  for (auto* p : kCandidates) {
    if (::access(p, X_OK) == 0) {
      cached = p;
      return cached;
    }
  }
  augment_path_for_subprocesses();
  if (command_ok("command -v wl-paste >/dev/null 2>&1")) {
    cached = "wl-paste";
    return cached;
  }
  cached = "wl-paste";  // best effort for error messages
  return cached;
}

static const char* resolve_wl_copy() {
  static const char* cached = nullptr;
  if (cached) {
    return cached;
  }
  static const char* kCandidates[] = {"/usr/bin/wl-copy", "/usr/local/bin/wl-copy"};
  for (auto* p : kCandidates) {
    if (::access(p, X_OK) == 0) {
      cached = p;
      return cached;
    }
  }
  augment_path_for_subprocesses();
  if (command_ok("command -v wl-copy >/dev/null 2>&1")) {
    cached = "wl-copy";
    return cached;
  }
  cached = "wl-copy";
  return cached;
}

static bool wl_clipboard_available() {
  if (!std::getenv("WAYLAND_DISPLAY")) {
    return false;
  }
  const char* p = resolve_wl_paste();
  if (p && p[0] == '/' && ::access(p, X_OK) == 0) {
    return true;
  }
  augment_path_for_subprocesses();
  return command_ok("command -v wl-paste >/dev/null 2>&1");
}

static void trim_trailing_crlf_inplace(std::string* s) {
  if (!s) {
    return;
  }
  while (!s->empty() && (s->back() == '\n' || s->back() == '\r')) {
    s->pop_back();
  }
}

static bool clipboard_normalized_equal(const std::string& a, const std::string& b) {
  std::string x = a;
  std::string y = b;
  trim_trailing_crlf_inplace(&x);
  trim_trailing_crlf_inplace(&y);
  return x == y;
}

static void trim_in_place_ascii(std::string* s);

// Pick the most preferred text MIME wl-clipboard advertises right now. Returns "" when the
// current offer is binary-only (image/png from a screenshot, application/octet-stream, etc).
// Avoids the historical foot-gun where `wl-paste 2>/dev/null` dumps raw PNG bytes into a
// std::string and downstream sees "\x89PNG\r\n\x1a\n" as garbage in the focused field.
static std::string wl_pick_text_mime(const char* primary_flag) {
  if (!wl_clipboard_available()) {
    return std::string();
  }
  const std::string base = resolve_wl_paste();
  std::string cmd = base + (primary_flag ? std::string(" ") + primary_flag : std::string()) +
                    " -l 2>/dev/null";
  std::string list;
  if (!read_clipboard_cmd(cmd.c_str(), &list) || list.empty()) {
    return std::string();
  }
  static const char* kPreferred[] = {
      "UTF8_STRING",
      "text/plain;charset=utf-8",
      "text/plain;charset=UTF-8",
      "text/plain",
      "STRING",
      "TEXT",
  };
  for (const char* want : kPreferred) {
    std::string needle = std::string("\n") + want + "\n";
    std::string hay = std::string("\n") + list + "\n";
    if (hay.find(needle) != std::string::npos) {
      return std::string(want);
    }
  }
  return std::string();
}

// Drop the read entirely when wl-paste hands us bytes that aren't valid UTF-8 — text fields
// can't display arbitrary binary anyway, and feeding it to Mozc / typing it back is the bug
// the user saw as "he❱PNG" (PNG file header leaking through a generic wl-paste call).
static bool read_wl_offer_text_only(const char* primary_flag, std::string* out) {
  out->clear();
  if (!wl_clipboard_available()) {
    return false;
  }
  const std::string mime = wl_pick_text_mime(primary_flag);
  if (mime.empty()) {
    return false;
  }
  const std::string base = resolve_wl_paste();
  std::string cmd = base + (primary_flag ? std::string(" ") + primary_flag : std::string()) +
                    " -t " + mime + " 2>/dev/null";
  std::string raw;
  if (!read_clipboard_cmd(cmd.c_str(), &raw)) {
    return false;
  }
  if (!raw.empty() &&
      !g_utf8_validate(raw.c_str(), static_cast<gssize>(raw.size()), nullptr)) {
    logf("clipboard: dropping %zu-byte wl-paste read — not valid UTF-8 (binary leaked via %s)",
         raw.size(), mime.c_str());
    return false;
  }
  out->swap(raw);
  return true;
}

// wl-paste without --primary: clipboard selection only (not primary buffer).
static bool read_wl_clip_offer(std::string* out) {
  return read_wl_offer_text_only(nullptr, out);
}

// wl-paste --primary only (Wayland middle-click buffer; often synced from highlight).
static bool read_wl_primary_offer(std::string* out) {
  return read_wl_offer_text_only("--primary", out);
}

bool read_clipboard(std::string* out) {
  if (wl_clipboard_available()) {
    if (read_wl_offer_text_only(nullptr, out)) {
      return true;
    }
    return read_wl_offer_text_only("--primary", out);
  }
  if (read_clipboard_cmd("xclip -selection clipboard -o 2>/dev/null", out)) {
    if (!out->empty() &&
        !g_utf8_validate(out->c_str(), static_cast<gssize>(out->size()), nullptr)) {
      logf("clipboard: dropping %zu-byte xclip CLIPBOARD read — not valid UTF-8 (binary?)",
           out->size());
      out->clear();
    } else {
      return true;
    }
  }
  if (read_clipboard_cmd("xclip -selection primary -o 2>/dev/null", out)) {
    if (!out->empty() &&
        !g_utf8_validate(out->c_str(), static_cast<gssize>(out->size()), nullptr)) {
      logf("clipboard: dropping %zu-byte xclip PRIMARY read — not valid UTF-8 (binary?)",
           out->size());
      out->clear();
      return false;
    }
    return true;
  }
  return false;
}

bool write_clipboard(const std::string& s) {
  if (wl_clipboard_available()) {
    const char* variants[] = {
        " --type text/plain;charset=utf-8 2>/dev/null",
        " --type text/plain 2>/dev/null",
        " 2>/dev/null",
    };
    for (const char* suf : variants) {
      std::string cmd = std::string(resolve_wl_copy()) + suf;
      FILE* f = popen(cmd.c_str(), "w");
      if (!f) {
        continue;
      }
      fwrite(s.data(), 1, s.size(), f);
      (void)std::fflush(f);
      int st = pclose(f);
      if (WIFEXITED(st) && WEXITSTATUS(st) == 0) {
        return true;
      }
    }
    return false;
  }
  FILE* f = popen("xclip -selection clipboard", "w");
  if (!f) {
    return false;
  }
  fwrite(s.data(), 1, s.size(), f);
  int st = pclose(f);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

static bool wl_clipboard_trimmed_empty(const std::string& c) {
  std::string x = c;
  trim_trailing_crlf_inplace(&x);
  trim_in_place_ascii(&x);
  return x.empty();
}

// After wl-copy "", the daemon often updates in <20ms; poll instead of a long blind sleep.
static bool poll_wl_clipboard_cleared(int max_wait_ms, int step_ms) {
  if (!wl_clipboard_available() || max_wait_ms <= 0) {
    return true;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    std::string c;
    read_wl_clip_offer(&c);
    if (wl_clipboard_trimmed_empty(c)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
  }
  return false;
}

// Wait until wl-paste matches `expected` (e.g. fresh payload before synthetic Ctrl+V).
static bool wait_wl_clipboard_equals_normalized(const std::string& expected, int max_wait_ms,
                                                int step_ms) {
  if (!wl_clipboard_available() || max_wait_ms <= 0) {
    return false;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
  if (wl_clipboard_trimmed_empty(expected)) {
    while (std::chrono::steady_clock::now() < deadline) {
      std::string c;
      read_wl_clip_offer(&c);
      if (wl_clipboard_trimmed_empty(c)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
    }
    return false;
  }
  while (std::chrono::steady_clock::now() < deadline) {
    std::string c;
    read_wl_clip_offer(&c);
    if (clipboard_normalized_equal(c, expected)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
  }
  return false;
}


bool mozc_convert_utf8(const std::string& romaji, std::string* replacement) {
  if (romaji.empty()) {
    return false;
  }
  size_t cap = std::max<size_t>(romaji.size() * 4 + 64, 256);
  for (;;) {
    std::string buf(cap, '\0');
    size_t out_len = 0;
    int rc = mozc_bridge_convert(romaji.data(), romaji.size(), buf.data(), buf.size(),
                                 &out_len);
    if (rc == 0) {
      replacement->assign(buf.data(), out_len);
      if (*replacement == romaji) {
        logf("mozc: output identical to input — engine did not transform this span");
      }
      return true;
    }
    if (rc < 0) {
      logf("mozc_bridge_convert: %s", mozc_bridge_last_error() ? mozc_bridge_last_error()
                                                                : "unknown error");
      return false;
    }
    if (static_cast<size_t>(rc) > (1u << 20)) {
      logf("mozc_bridge_convert: unreasonably large output (%d)", rc);
      return false;
    }
    cap = static_cast<size_t>(rc) + 1;
  }
}

// --- UTF-8 word boundaries (glib offsets = Unicode character indices) ---

void word_range_chars(const gchar* text, glong caret_chars, glong n_chars, glong* start,
                      glong* end) {
  glong len = n_chars;
  glong c = std::clamp<glong>(caret_chars, 0, len);
  auto is_ws = [](gunichar ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
  };
  glong st = c;
  while (st > 0) {
    const gchar* p = g_utf8_offset_to_pointer(text, st - 1);
    gunichar ch = g_utf8_get_char(p);
    if (is_ws(ch)) {
      break;
    }
    st--;
  }
  glong en = c;
  while (en < len) {
    const gchar* p = g_utf8_offset_to_pointer(text, en);
    gunichar ch = g_utf8_get_char(p);
    if (is_ws(ch)) {
      break;
    }
    en++;
  }
  if (st == en) {
    if (c < len) {
      *start = c;
      *end = c + 1;
      return;
    }
    if (c > 0) {
      *start = c - 1;
      *end = c;
      return;
    }
  }
  *start = st;
  *end = en;
}

void utf8_substr_bytes(const gchar* text, glong start_c, glong end_c, std::string* out) {
  const gchar* a = g_utf8_offset_to_pointer(text, start_c);
  const gchar* b = g_utf8_offset_to_pointer(text, end_c);
  out->assign(a, b - a);
}

// --- XTest synthetic keys -------------------------------------------------

void fake_ctrl_letter(Display* d, KeySym letter) {
  KeyCode ctrl_l = XKeysymToKeycode(d, XK_Control_L);
  KeyCode key = XKeysymToKeycode(d, letter);
  if (!ctrl_l || !key) {
    logf("XKeysymToKeycode failed for ctrl+key");
    return;
  }
  XTestFakeKeyEvent(d, ctrl_l, True, CurrentTime);
  XTestFakeKeyEvent(d, key, True, CurrentTime);
  XTestFakeKeyEvent(d, key, False, CurrentTime);
  XTestFakeKeyEvent(d, ctrl_l, False, CurrentTime);
  XFlush(d);
}

void fake_ctrl_shift_left(Display* d) {
  KeyCode ctrl = XKeysymToKeycode(d, XK_Control_L);
  KeyCode shift = XKeysymToKeycode(d, XK_Shift_L);
  KeyCode left = XKeysymToKeycode(d, XK_Left);
  if (!ctrl || !shift || !left) {
    return;
  }
  XTestFakeKeyEvent(d, shift, True, CurrentTime);
  XTestFakeKeyEvent(d, ctrl, True, CurrentTime);
  XTestFakeKeyEvent(d, left, True, CurrentTime);
  XTestFakeKeyEvent(d, left, False, CurrentTime);
  XTestFakeKeyEvent(d, ctrl, False, CurrentTime);
  XTestFakeKeyEvent(d, shift, False, CurrentTime);
  XFlush(d);
}

void fake_right_arrow(Display* d) {
  KeyCode left = XKeysymToKeycode(d, XK_Right);
  if (left) {
    XTestFakeKeyEvent(d, left, True, CurrentTime);
    XTestFakeKeyEvent(d, left, False, CurrentTime);
    XFlush(d);
  }
}

// --- wtype / ydotool (optional; used when Display* is null) ----------------

static void child_clear_im_modules() {
  unsetenv("GTK_IM_MODULE");
  unsetenv("QT_IM_MODULE");
  unsetenv("SDL_IM_MODULE");
  unsetenv("XMODIFIERS");
  unsetenv("INPUT_METHOD");
}

bool inject_utf8_subprocess_wtype(const std::string& utf8) {
  const char* wt = resolve_wtype_executable();
  if (!wt) {
    return false;
  }
  pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    child_clear_im_modules();
    execl(wt, "wtype", utf8.c_str(), nullptr);
    _exit(127);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  if (!ok) {
    const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    logf("wtype type: subprocess exit=%d (%zu UTF-8 bytes)", code, utf8.size());
  }
  return ok;
}

bool inject_utf8_wayland_fallback(const std::string& utf8) {
  if (inject_utf8_subprocess_wtype(utf8)) {
    return true;
  }
  pid_t pid = fork();
  if (pid < 0) {
    return false;
  }
  if (pid == 0) {
    child_clear_im_modules();
    execlp("ydotool", "ydotool", "type", utf8.c_str(), nullptr);
    _exit(127);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  if (!ok) {
    const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    logf("ydotool type: subprocess exit=%d (%zu UTF-8 bytes)", code, utf8.size());
  }
  return ok;
}

void fake_ctrl_c_best(Display* d) {
  MODORE_E2E_LOGF("fake_ctrl_c_best: backend=%s", d ? "XTest Ctrl+C" : "Hypr/wtype copy");
  if (d) {
    fake_ctrl_letter(d, XK_c);
    MODORE_E2E_LOGF("fake_ctrl_c_best: XTest Ctrl+C sent");
    return;
  }
  using namespace std::chrono_literals;
  bool hypr_any = false;
  bool w_any = false;
  if (hyprctl_ipc_alive_for_wayland_keys()) {
    hypr_any = hyprctl_wayland_copy_selection();
  }
  // Brief yield before wtype mirrors — often <15ms is enough; avoids stacking multi-hundred-ms delays.
  nap_after_compose_event(std::chrono::milliseconds(1));
  if (wtype_is_available()) {
    w_any = wtype_chord_ctrl_insert_copy();
    nap_after_compose_event(std::chrono::milliseconds(1));
    w_any = wtype_chord_ctrl_c() || w_any;
  }
  if (!hypr_any && !w_any) {
    logf("Wayland: need Hyprland `hyprctl sendshortcut` or `wtype` — cannot simulate universal "
         "copy (Ctrl+Insert / Ctrl+C)");
  } else {
    MODORE_E2E_LOGF("fake_ctrl_c_best: Wayland hypr_ok=%d wtype_ok=%d", hypr_any ? 1 : 0,
                    w_any ? 1 : 0);
  }
}

void fake_ctrl_shift_left_best(Display* d) {
  if (d) {
    fake_ctrl_shift_left(d);
    return;
  }
  if (hyprctl_ipc_alive_for_wayland_keys()) {
    if (hyprctl_wayland_ctrl_shift_left()) {
      return;
    }
  }
  if (wtype_is_available()) {
    (void)wtype_chord_ctrl_shift_left();
  }
}

void fake_right_arrow_best(Display* d) {
  if (d) {
    fake_right_arrow(d);
    return;
  }
  if (hyprctl_ipc_alive_for_wayland_keys()) {
    if (hyprctl_wayland_key_right()) {
      return;
    }
  }
  if (wtype_is_available()) {
    (void)wtype_key_right();
  }
}

// Remove selected text before STRING injection (many Gtk/Qt controls ignore
// synthetic ATSPI_KEY_STRING when replacing a selection; Delete+type matches
// physical keyboard behaviour).
void fake_delete_selection_best(Display* d) {
  if (d) {
    KeyCode del = XKeysymToKeycode(d, XK_Delete);
    if (!del) {
      del = XKeysymToKeycode(d, XK_BackSpace);
    }
    if (del) {
      XTestFakeKeyEvent(d, del, True, CurrentTime);
      XTestFakeKeyEvent(d, del, False, CurrentTime);
      XFlush(d);
    }
    return;
  }
  if (hyprctl_ipc_alive_for_wayland_keys()) {
    if (hyprctl_wayland_delete_or_backspace()) {
      return;
    }
  }
  if (wtype_is_available()) {
    (void)wtype_key_delete_or_backspace();
  }
}

// Heuristic matching macOS: line-copy detection ---------------------------

bool looks_like_line_copy(const std::string& s) {
  if (s.find('\n') != std::string::npos || s.find('\r') != std::string::npos) {
    return true;
  }
  // Long single-line copies are usually the real selection from browser/UI widgets.
  // The old threshold (>200 bytes) matched normal Romaji picks and wrongly forced
  // Ctrl+Shift+Left ("stopping at selection") when the trimmed first-line rule failed too.
  return s.size() > 524288;
}

static void trim_in_place_ascii(std::string* s) {
  if (!s) {
    return;
  }
  while (!s->empty() && std::isspace(static_cast<unsigned char>(s->front()))) {
    s->erase(s->begin());
  }
  while (!s->empty() && std::isspace(static_cast<unsigned char>(s->back()))) {
    s->pop_back();
  }
}

// Nautilus path bar exposes PRIMARY as a full POSIX path (~34 chars) while the user only sees the
// last segment highlighted (e.g. "henkan"). Convert the tail so Mozc gets the visible token.
// Returns true iff we rewrote picked to that final segment — then glyph-count erase is unsafe.
static bool maybe_narrow_path_primary_pick(std::string* picked) {
  if (!picked || picked->empty()) {
    return false;
  }
  if (picked->find('/') == std::string::npos) {
    return false;
  }
  const size_t last_slash = picked->find_last_of('/');
  if (last_slash == std::string::npos || last_slash + 1 >= picked->size()) {
    return false;
  }
  std::string tail = picked->substr(last_slash + 1);
  trim_trailing_crlf_inplace(&tail);
  trim_in_place_ascii(&tail);
  if (tail.empty() || tail.find_first_of("\n\r") != std::string::npos) {
    return false;
  }
  constexpr size_t kMaxTail = 512;
  if (tail.size() <= kMaxTail && tail.size() < picked->size()) {
    logf("clipboard: narrowed path-like PRIMARY (%zu bytes) → final segment (%zu bytes)",
         picked->size(), tail.size());
    picked->swap(tail);
    return true;
  }
  return false;
}

// Mozc will “convert” arbitrary Latin (paths, flags) into phonetic kana/kanji + leftover ASCII,
// which shows up as garbage in Walker (e.g. ~/.local/bin/modore-host --trigger → okashi·bin·…).
static bool clipboard_pick_probably_not_romaji_field(const std::string& s) {
  if (s.find("modore-host") != std::string::npos) {
    return true;
  }
  if (s.find("#!/") != std::string::npos) {
    return true;
  }
  const bool has_pathish = s.find('/') != std::string::npos || s.find('~') != std::string::npos;
  const bool has_double_dash = s.find("--") != std::string::npos;
  if (has_pathish && has_double_dash) {
    return true;
  }
  if (has_pathish && (s.find("bin/") != std::string::npos || s.find(".local/") != std::string::npos)) {
    return true;
  }
  return false;
}

// Cursor / VS Code / Electron / CLIs often mirror placeholder or shortcut hint text into PRIMARY
// ("Add a follow-up", "ctrl+c to stop") — Mozc turns that into mojibake soup. PRIMARY is also
// **global**: a focused terminal or TUI can publish this while the user thinks another app
// (browser) "owns" the edit field — modore must not trust that PRIMARY as the pick.
static const char* clipboard_first_matching_modifier_or_ui_hint_needle_ci(const std::string& s) {
  if (s.size() < 6) {
    return nullptr;
  }
  std::string low;
  low.reserve(s.size());
  for (unsigned char c : s) {
    low.push_back(static_cast<char>(std::tolower(c)));
  }
  static const char* kNeedles[] = {
      "ctrl+",       "cmd+",      "meta+",     "shift+",    "alt+",
      "super+",      "follow-up", "follow up", "to stop",   "esc to",
      "press esc",   "shortcut",  "keyboard",  "add a follow", "addafollow",
  };
  for (const char* n : kNeedles) {
    if (low.find(n) != std::string::npos) {
      return n;
    }
  }
  return nullptr;
}

// TUI progress bars (e.g. streaming CLIs) often land in PRIMARY as UTF-8 block glyphs (U+2580–U+259F).
static bool wl_primary_dominated_by_block_elements(const std::string& s) {
  size_t n_block = 0;
  for (size_t i = 0; i + 3 <= s.size();) {
    if (static_cast<unsigned char>(s[i]) == 0xe2 &&
        static_cast<unsigned char>(s[i + 1]) == 0x96) {
      const unsigned char t = static_cast<unsigned char>(s[i + 2]);
      if (t >= 0x80 && t <= 0x9f) {
        ++n_block;
        i += 3;
        continue;
      }
    }
    ++i;
  }
  return n_block >= 8;
}

static bool wl_primary_looks_like_stale_global_chrome(const std::string& s) {
  return clipboard_first_matching_modifier_or_ui_hint_needle_ci(s) != nullptr ||
         wl_primary_dominated_by_block_elements(s);
}

// PRIMARY fast-path only (empty CLIPBOARD + skip Ctrl+C): must not fire when GTK mirrors a
// post-conversion CJK slice — mozore often clears CLIPBOARD first so baseline_clip is empty,
// which incorrectly made PRIMARY the pick every time.
static bool wl_primary_is_utf8_bounded_ascii_only_fast_pick(const std::string& s) {
  if (s.empty()) {
    return false;
  }
  for (unsigned char c : s) {
    if (c >= 0x80u) {
      return false;
    }
  }
  return true;
}

static bool clipboard_pick_probably_ide_ui_hint(const std::string& s) {
  const char* n = clipboard_first_matching_modifier_or_ui_hint_needle_ci(s);
  if (!n) {
    return false;
  }
  logf("clipboard: pick embeds IDE/UI shortcut hint (matched '%s') — skipping Mozc", n);
  return true;
}

// Many inputs arrive as one logical line; truncate very large blobs (browser dumps).
static bool clipboard_first_reasonable_line(const std::string& raw, std::string* picked) {
  picked->clear();
  size_t pos = 0;
  constexpr size_t kMaxFirstLinePick = 262144;
  while (pos < raw.size()) {
    while (pos < raw.size() && (raw[pos] == '\n' || raw[pos] == '\r')) {
      ++pos;
    }
    const size_t nl = raw.find_first_of("\n\r", pos);
    std::string first = nl == std::string::npos ? raw.substr(pos) : raw.substr(pos, nl - pos);
    trim_in_place_ascii(&first);
    if (!first.empty()) {
      if (first.size() > kMaxFirstLinePick) {
        first.resize(kMaxFirstLinePick);
      }
      picked->swap(first);
      return true;
    }
    if (nl == std::string::npos) {
      break;
    }
    pos = nl;
  }
  return false;
}

// GTK/Qt/WebKit mirror the highlighted range to PRIMARY. After Ctrl+C the clipboard often
// contains a whole line (or paragraph) while PRIMARY tracks the user's smaller selection.
static bool wl_try_primary_as_highlighted_span(const std::string& baseline_primary,
                                               const std::string& clip_text,
                                               std::string* picked) {
  picked->clear();
  if (!wl_clipboard_available() || clip_text.empty()) {
    return false;
  }
  std::string prim;
  if (!read_wl_primary_offer(&prim)) {
    return false;
  }
  trim_trailing_crlf_inplace(&prim);
  trim_in_place_ascii(&prim);
  if (prim.empty() || clipboard_normalized_equal(prim, baseline_primary)) {
    return false;
  }
  if (wl_primary_looks_like_stale_global_chrome(prim)) {
    return false;
  }
  const size_t nl = clip_text.find_first_of("\n\r");
  std::string fl = nl == std::string::npos ? clip_text : clip_text.substr(0, nl);
  trim_trailing_crlf_inplace(&fl);
  trim_in_place_ascii(&fl);
  if (fl.empty() || fl.find(prim) == std::string::npos || prim.size() > fl.size()) {
    return false;
  }
  if (prim.size() < fl.size()) {
    picked->assign(prim);
    return true;
  }
  return false;
}

// --- AT-SPI: find focused, read text, replace ----------------------------

AtspiAccessible* find_focused_leaf(AtspiAccessible* obj, int depth) {
  if (!obj || depth > 48) {
    return nullptr;
  }
  AtspiStateSet* ss = atspi_accessible_get_state_set(obj);
  if (ss && atspi_state_set_contains(ss, ATSPI_STATE_FOCUSED)) {
    g_object_unref(ss);
    return ATSPI_ACCESSIBLE(g_object_ref(obj));
  }
  if (ss) {
    g_object_unref(ss);
  }
  GError* err = nullptr;
  gint n = atspi_accessible_get_child_count(obj, &err);
  if (err) {
    g_clear_error(&err);
    return nullptr;
  }
  for (gint i = 0; i < n; ++i) {
    AtspiAccessible* ch = atspi_accessible_get_child_at_index(obj, i, &err);
    if (err) {
      g_clear_error(&err);
      continue;
    }
    AtspiAccessible* f = find_focused_leaf(ch, depth + 1);
    g_object_unref(ch);
    if (f) {
      return f;
    }
  }
  return nullptr;
}

// Fallback when no node has STATE_FOCUSED (some Wayland stacks omit it): first
// Text object with FOCUSED or ACTIVE along a DFS (ACTIVE often marks the live
// search/caret field).
AtspiAccessible* find_text_with_focus_or_active(AtspiAccessible* obj, int depth) {
  if (!obj || depth > 48) {
    return nullptr;
  }
  if (atspi_accessible_is_text(obj)) {
    AtspiStateSet* ss = atspi_accessible_get_state_set(obj);
    if (ss) {
      const bool focusish =
          atspi_state_set_contains(ss, ATSPI_STATE_FOCUSED) ||
          atspi_state_set_contains(ss, ATSPI_STATE_ACTIVE);
      g_object_unref(ss);
      if (focusish) {
        return ATSPI_ACCESSIBLE(g_object_ref(obj));
      }
    }
  }
  GError* err = nullptr;
  const gint n = atspi_accessible_get_child_count(obj, &err);
  if (err) {
    g_clear_error(&err);
    return nullptr;
  }
  for (gint i = 0; i < n; ++i) {
    AtspiAccessible* ch = atspi_accessible_get_child_at_index(obj, i, &err);
    if (err) {
      g_clear_error(&err);
      continue;
    }
    AtspiAccessible* f = find_text_with_focus_or_active(ch, depth + 1);
    g_object_unref(ch);
    if (f) {
      return f;
    }
  }
  return nullptr;
}

// Returns true if AT-SPI produced a result: either direct editable replace
// (*direct_done) or UTF-8 to inject with atspi_generate_keyboard_event.
// When *pick_span_for_inject is non-null, fills it with the source romaji slice (UTF-8) on the
// non-direct path so Wayland inject can glyph-erase + paste without relying on fake Delete.
// Returns false to fall through to the clipboard/XTest path.
bool try_pickup_atspi(bool* direct_done, std::string* inject_utf8,
                      std::string* pick_span_for_inject) {
  *direct_done = false;
  inject_utf8->clear();
  if (pick_span_for_inject) {
    pick_span_for_inject->clear();
  }
  MODORE_E2E_LOGF("try_pickup_atspi: start");

  GError* err = nullptr;
  AtspiAccessible* focus = nullptr;
  const gint n_desk = atspi_get_desktop_count();
  const gint n_try = n_desk > 0 ? n_desk : 1;
  for (gint di = 0; di < n_try; ++di) {
    AtspiAccessible* desktop = atspi_get_desktop(di);
    if (!desktop) {
      continue;
    }
    focus = find_focused_leaf(desktop, 0);
    if (!focus) {
      focus = find_text_with_focus_or_active(desktop, 0);
      if (focus) {
        logf("AT-SPI: desktop %d — Text widget FOCUSED/ACTIVE (no strict focus leaf)",
             static_cast<int>(di));
      }
    }
    g_object_unref(desktop);
    if (focus) {
      break;
    }
  }
  if (!focus) {
    logf("AT-SPI: no focused accessible");
    return false;
  }
  if (!atspi_accessible_is_text(focus)) {
    logf("AT-SPI: focused node has no Text interface");
    g_object_unref(focus);
    return false;
  }
  AtspiText* text = atspi_accessible_get_text_iface(focus);
  if (!text) {
    g_object_unref(focus);
    return false;
  }

  gint n_chars = atspi_text_get_character_count(text, &err);
  if (err) {
    g_clear_error(&err);
    g_object_unref(focus);
    return false;
  }

  gchar* full = atspi_text_get_text(text, 0, n_chars, &err);
  if (err || !full) {
    g_clear_error(&err);
    g_object_unref(focus);
    return false;
  }

  gint caret = atspi_text_get_caret_offset(text, &err);
  if (err) {
    g_clear_error(&err);
    g_free(full);
    g_object_unref(focus);
    return false;
  }

  glong span_start = 0;
  glong span_end = 0;
  gint n_sel = atspi_text_get_n_selections(text, &err);
  g_clear_error(&err);
  if (n_sel > 0) {
    AtspiRange* range = atspi_text_get_selection(text, 0, &err);
    if (!err && range) {
      span_start = range->start_offset;
      span_end = range->end_offset;
      g_free(range);
    } else {
      g_clear_error(&err);
      word_range_chars(full, caret, n_chars, &span_start, &span_end);
    }
  } else {
    word_range_chars(full, caret, n_chars, &span_start, &span_end);
  }

  // Edge/Chromium often report caret=0 immediately after focus or typing — `word_range_chars`
  // then returns an empty span even though the field clearly has text. Fall back to using the
  // entire field text when it looks like a single-line input (no newlines, bounded size). This
  // matches what the user wants in a search box / URL bar / single-line form input: convert
  // everything I typed, not just the word at the caret.
  if (span_start >= span_end && n_chars > 0) {
    constexpr glong kAtspiFullTextMaxChars = 256;
    if (n_chars <= kAtspiFullTextMaxChars) {
      bool single_line = true;
      for (const gchar* p = full; *p;) {
        gunichar ch = g_utf8_get_char(p);
        if (ch == '\n' || ch == '\r') {
          single_line = false;
          break;
        }
        p = g_utf8_next_char(p);
      }
      if (single_line) {
        span_start = 0;
        span_end = n_chars;
        logf("AT-SPI: empty word span at caret=%ld but field has %ld chars — using entire field "
             "(single-line input)",
             static_cast<long>(caret), static_cast<long>(n_chars));
      }
    }
  }

  if (span_start >= span_end) {
    logf("AT-SPI: empty span (caret=%ld n_chars=%ld) — falling through",
         static_cast<long>(caret), static_cast<long>(n_chars));
    g_free(full);
    g_object_unref(focus);
    return false;
  }

  std::string romaji;
  utf8_substr_bytes(full, span_start, span_end, &romaji);
  g_free(full);
  MODORE_E2E_LOGF("try_pickup_atspi: span_start=%ld span_end=%ld romaji_bytes=%zu", static_cast<long>(span_start),
                  static_cast<long>(span_end), romaji.size());

  AtspiEditableText* ed = atspi_accessible_get_editable_text_iface(focus);
  if (ed && atspi_accessible_is_editable_text(focus)) {
    std::string converted;
    if (!mozc_convert_utf8(romaji, &converted)) {
      g_object_unref(focus);
      return false;
    }
    gboolean ok1 = atspi_editable_text_delete_text(ed, span_start, span_end, &err);
    if (!ok1) {
      g_clear_error(&err);
      g_object_unref(focus);
      return false;
    }
    gboolean ok2 =
        atspi_editable_text_insert_text(ed, span_start, converted.c_str(),
                                        static_cast<gint>(converted.size()), &err);
    if (!ok2) {
      g_clear_error(&err);
      g_object_unref(focus);
      return false;
    }
    g_clear_error(&err);
    const gint caret_after =
        static_cast<gint>(span_start) +
        static_cast<gint>(g_utf8_strlen(converted.c_str(), static_cast<gssize>(converted.size())));
    if (!atspi_text_set_caret_offset(text, caret_after, &err)) {
      if (err) {
        logf("AT-SPI: set_caret_offset(%d): %s", static_cast<int>(caret_after), err->message);
        g_clear_error(&err);
      } else {
        logf("AT-SPI: set_caret_offset(%d) failed", static_cast<int>(caret_after));
      }
    } else {
      g_clear_error(&err);
    }
    *direct_done = true;
    g_object_unref(focus);
    return true;
  }

  if (!mozc_convert_utf8(romaji, inject_utf8)) {
    logf("AT-SPI: convert failed (non-editable field)");
    g_object_unref(focus);
    return false;
  }
  // On Wayland, set_selection frequently updates accessibility state without moving
  // the real keyboard selection, so synthetic Delete/types the wrong slice — skip.
  if (!std::getenv("WAYLAND_DISPLAY")) {
    gboolean sel_ok =
        atspi_text_set_selection(text, 0, static_cast<gint>(span_start),
                                 static_cast<gint>(span_end), &err);
    if (!sel_ok) {
      if (err) {
        logf("AT-SPI: set_selection: %s", err->message);
        g_clear_error(&err);
      } else {
        logf("AT-SPI: set_selection failed");
      }
    }
  } else {
    logf("AT-SPI: Wayland — skipping set_selection before inject");
  }
  g_object_unref(focus);
  if (pick_span_for_inject) {
    pick_span_for_inject->assign(romaji);
  }
  logf("AT-SPI: non-editable control — injecting conversion text");
  return true;
}

// --- Clipboard pickup (macOS-style) --------------------------------------

bool inject_via_atspi_string(const std::string& utf8) {
  GError* err = nullptr;
  atspi_generate_keyboard_event(0, utf8.c_str(), ATSPI_KEY_STRING, &err);
  if (err) {
    logf("atspi_generate_keyboard_event STRING: %s", err->message);
    g_clear_error(&err);
    return false;
  }
  return true;
}

// Hyprland/Omarchy universal paste is Shift+Insert; wtype often fails for CJK,
// and AT-SPI STRING is unreliable on Wayland.
static bool utf8_contains_non_ascii(const std::string& utf8) {
  for (unsigned char c : utf8) {
    if (c >= 0x80) {
      return true;
    }
  }
  return false;
}

// Clipboard span that still looks like "henkan" / romaji ASCII only (Mozc roman input slice).
static bool pick_is_plain_ascii_romaji(const std::string& pick) {
  if (pick.empty() || pick.size() > 1536) {
    return false;
  }
  for (unsigned char c : pick) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      continue;
    }
    if (c == '-' || c == '_' || c == '\'') {
      continue;
    }
    return false;
  }
  return true;
}

// Omnibox / IME often merges editable romaji with a pasted or suggested YouTube/query tail
// (`v=jZX...`, `&list=`). Sending the whole blob to Mozc destroys the URL ("じ" replacing "j").
// Returns true iff *picked shrank to a leading ASCII-only romaji slice.
static bool leading_ascii_romaji_token_prefix(const std::string& s, std::string* token) {
  token->clear();
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
    ++i;
  }
  const size_t start = i;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c >= 0x80) {
      break;
    }
    if (std::isalnum(c) || c == '-' || c == '_' || c == '\'') {
      ++i;
      continue;
    }
    break;
  }
  if (i <= start) {
    return false;
  }
  token->assign(s, start, i - start);
  return pick_is_plain_ascii_romaji(*token);
}

// Clipboard/omnibox picks sometimes include stray Unicode (e.g. U+2771 `❱`, RTL marks, or random
// multibyte glyphs from earlier IME state) wedged between romaji and an ASCII suffix like "PNG".
// Mozc converts the prefix but the suffix survives as literal ASCII (visible as "he❱PNG" stays
// "he❱PNG" or becomes "へPNG"). Trim to the leading romaji token; caller forces Ctrl+A clear so
// glyph BackSpace can't leave the trailing junk behind.
static bool trim_pick_leading_romaji_if_utf8_contaminated(std::string* picked) {
  if (!picked || picked->empty()) {
    return false;
  }
  bool any_multibyte = false;
  for (unsigned char c : *picked) {
    if (c >= 0x80) {
      any_multibyte = true;
      break;
    }
  }
  if (!any_multibyte) {
    return false;
  }
  std::string token;
  if (!leading_ascii_romaji_token_prefix(*picked, &token) || token.empty() ||
      token.size() >= picked->size()) {
    return false;
  }
  logf("clipboard: trimmed UTF-8–contaminated pick before Mozc (%zu → %zu bytes)", picked->size(),
       token.size());
  picked->swap(token);
  return true;
}

static size_t omniboz_earliest_url_like_marker_ci(std::string* low_ascii_out, const std::string& raw) {
  low_ascii_out->clear();
  low_ascii_out->reserve(raw.size());
  for (unsigned char c : raw) {
    low_ascii_out->push_back(static_cast<char>(std::tolower(c)));
  }
  const std::string& low = *low_ascii_out;
  size_t cut = std::string::npos;
  static const char* kMarkers[] = {
      "http://",    "https://",   "www.youtube", "youtube.com", "youtu.be/",
      "youtube.",   "&list=",     "&index=",     "&feature=", "&v=", "?v=", "/watch", "watch?",
  };
  for (const char* m : kMarkers) {
    const size_t p = low.find(m);
    if (p != std::string::npos) {
      cut = std::min(cut, p);
    }
  }
  // `v=jZX...` query param — avoid matching "...inv=alice" (`v=` must follow ? / & / / )
  for (size_t i = 0; i + 2 < low.size(); ++i) {
    if (low[i] == 'v' && low[i + 1] == '=' && (i == 0 || low[i - 1] == '?' || low[i - 1] == '&' ||
                                               low[i - 1] == '/' || low[i - 1] == '#')) {
      cut = std::min(cut, i);
    }
  }
  return cut;
}

static bool maybe_narrow_omnibox_url_contaminated_pick(std::string* picked) {
  if (!picked || picked->size() < 8) {
    return false;
  }
  std::string low;
  const size_t cut = omniboz_earliest_url_like_marker_ci(&low, *picked);
  if (cut == std::string::npos || cut == 0) {
    return false;
  }

  std::string prefix = picked->substr(0, cut);
  trim_trailing_crlf_inplace(&prefix);
  trim_in_place_ascii(&prefix);
  if (prefix.empty()) {
    return false;
  }

  std::string token;
  if (!leading_ascii_romaji_token_prefix(prefix, &token)) {
    return false;
  }
  constexpr size_t kMinTok = 2;
  constexpr size_t kMaxTok = 512;
  if (token.size() < kMinTok || token.size() > kMaxTok) {
    return false;
  }

  const bool shrunk = token.size() < picked->size();
  if (!shrunk) {
    return false;
  }

  logf(
      "clipboard: omnibox/url tail detected — Mozc slice \"%s\" (%zu bytes) from %zu-byte pick "
      "(dropped YouTube/query junk)",
      token.c_str(), token.size(), picked->size());
  picked->swap(token);
  return true;
}

// Long picks with substantial real kana/CJK are unlikely to be Latin-on-UTF-8 mojibake; blocking
// those yields false positives (notify + field nuking) while paste/sync is already handled elsewhere.
static bool pick_looks_like_mojibake_garbage(const std::string& pick) {
  if (pick.empty() || pick.size() < 12) {
    return false;
  }
  if (!g_utf8_validate(pick.c_str(), static_cast<gssize>(pick.size()), nullptr)) {
    return true;
  }
  const gchar* p = pick.c_str();
  glong latin_stutter = 0;
  glong jp_mass = 0;
  gunichar uch;
  auto jp_glyph = [](gunichar u) -> bool {
    return (u >= 0x3040 && u <= 0x309f) || (u >= 0x30a0 && u <= 0x30ff) ||
           (u >= 0x4e00 && u <= 0x9fff) || (u >= 0x3400 && u <= 0x4dbf);
  };
  while (*p) {
    uch = g_utf8_get_char(p);
    if (jp_glyph(uch)) {
      ++jp_mass;
    }
    if (uch == 0xfffd || uch == 0x25a1 || uch == 0x2592 || uch == 0x25af) {
      latin_stutter += 12;
    }
    if ((uch >= 0x0080 && uch <= 0x00bf) ||
        uch == 0x00c2 || uch == 0x00c3 || uch == 0x00c4 || uch == 0x00e2 || uch == 0x00e3 ||
        uch == 0x00aa || uch == 0x00ba || uch == 0x00ac || uch == 0x00a1 || uch == 0x00a3 ||
        uch == 0x00a9 || uch == 0x00ae || uch == 0x2020 || uch == 0x00b1) {
      latin_stutter += 3;
    }
    if ((uch >= 0x2000 && uch <= 0x206f && uch != 0x3000)) {
      ++latin_stutter;
    }
    if (latin_stutter > 800) {
      break;
    }
    p = g_utf8_next_char(p);
  }
  if (jp_mass >= 8 && jp_mass * 3 >= latin_stutter) {
    return false;
  }
  const glong thresh = pick.size() / 60 + static_cast<glong>(54);
  const bool noisy = latin_stutter >= thresh || latin_stutter >= 175;
  if (noisy) {
    logf(
        "clipboard: pick resembles latin mojibake (latin_score=%ld, jp_glyphs≈%ld, len=%zu) — "
        "block Mozc on this blob",
        static_cast<long>(latin_stutter), static_cast<long>(jp_mass), pick.size());
  }
  return noisy;
}

static bool mojibake_recovery_aggressive_enabled() {
  const char* v = std::getenv("MODORE_MOJIBAKE_RECOVERY");
  return v && v[0] != '\0' && std::strcmp(v, "0") != 0;
}

static bool hypr_wayland_try_select_all(const char* log_ctx) {
  if (!hyprctl_ipc_alive_for_wayland_keys()) {
    return false;
  }
  if (hyprctl_dispatch_sendshortcut("CONTROL,A,", log_ctx ? log_ctx : "Select all")) {
    // No AT-SPI signal when the widget has expanded selection — keep a small window only.
    nap_after_compose_event(std::chrono::milliseconds(12));
    return true;
  }
  return false;
}

static void notify_corrupted_pick_needs_recovery() {
  if (!command_ok("command -v notify-send >/dev/null 2>&1")) {
    return;
  }
  (void)std::system(
      "notify-send -u normal -a Modore -t 9000 'Modore'"
      " 'Stale/corrupted text in the focused field — attempting Ctrl+A + Backspace."
      " If it looks wrong, clear the box yourself, then type romaji again.' >/dev/null 2>&1 &");
}

static bool hypr_attempt_clear_focused_edit_field_best_effort(const char* log_ctx_note) {
  if (!wl_clipboard_available() || !hyprctl_ipc_alive_for_wayland_keys()) {
    return false;
  }
  const char* block = std::getenv("MODORE_NO_MOJIBAKE_RECOVERY");
  if (block && block[0]) {
    return false;
  }
  if (!hypr_wayland_try_select_all(log_ctx_note ? log_ctx_note : "Modore Ctrl+A stale field")) {
    return false;
  }
  if (hyprctl_wayland_delete_or_backspace()) {
    nap_after_compose_event(std::chrono::milliseconds(6));
    logf("clipboard: cleared focused field attempt — Hypr Ctrl+A + delete/backspace sequence");
    return true;
  }
  return false;
}

static bool inject_utf8_via_wl_clipboard_paste(const std::string& utf8) {
  MODORE_E2E_LOGF("inject_wl_paste: enter utf8_bytes=%zu", utf8.size());
  if (utf8.empty() || !wl_clipboard_available()) {
    MODORE_E2E_LOGF("inject_wl_paste: abort empty or no wl-clipboard");
    return false;
  }
  if (!write_clipboard(utf8)) {
    logf("inject: wl-copy failed (%zu UTF-8 bytes)", utf8.size());
    MODORE_E2E_LOGF("inject_wl_paste: write_clipboard failed");
    return false;
  }
  MODORE_E2E_LOGF("inject_wl_paste: wl-copy wrote payload");
  // Brief poll — offer is usually visible in one compositor frame; cap stays low for responsiveness.
  constexpr int kInjectPasteWaitMs = 48;
  constexpr int kInjectPasteStepMs = 1;
  if (!wait_wl_clipboard_equals_normalized(utf8, kInjectPasteWaitMs, kInjectPasteStepMs)) {
    logf("inject: wl-copy payload not yet visible to wl-paste after %dms — sending paste anyway",
         kInjectPasteWaitMs);
  }
  // Paste: Hypr sendshortcut first — it targets the same focused surface as universal copy on
  // Hyprland; wtype is a fallback when Hypr IPC is unavailable.
  bool ok = false;
  if (hyprctl_ipc_alive_for_wayland_keys()) {
    if (hyprctl_dispatch_sendshortcut("SHIFT,Insert,", "Shift+Insert paste (Omarchy SUPER+V)")) {
      logf("inject: wl-copy+hypr Shift+Insert (%zu UTF-8 bytes)", utf8.size());
      ok = true;
    } else if (hyprctl_dispatch_sendshortcut("CONTROL,V,", "Ctrl+V paste")) {
      logf("inject: wl-copy+hypr Ctrl+V (%zu UTF-8 bytes)", utf8.size());
      ok = true;
    }
    if (!ok) {
      logf("inject: wl-copy OK but Hypr Shift+Insert / Ctrl+V failed");
    }
  }
  if (!ok && wtype_is_available()) {
    if (wtype_chord_shift_insert()) {
      logf("inject: wl-copy+wtype Shift+Insert (%zu UTF-8 bytes)", utf8.size());
      ok = true;
    } else if (wtype_chord_ctrl_v()) {
      logf("inject: wl-copy+wtype Ctrl+V (%zu UTF-8 bytes)", utf8.size());
      ok = true;
    } else {
      logf("inject: wl-copy OK but wtype Shift+Insert / Ctrl+V failed");
    }
  }
  if (!ok && !wtype_is_available() && !hyprctl_ipc_alive_for_wayland_keys()) {
    logf("inject: wl-copy ok — no Hypr IPC and no wtype for paste");
  }
  if (ok) {
    nap_after_compose_event(std::chrono::milliseconds(3));
  }
  MODORE_E2E_LOGF("inject_wl_paste: done ok=%d", ok ? 1 : 0);
  return ok;
}

// After Ctrl+C many clients collapse the selection; a single Delete/Backspace is wrong.
// Erase the converted span using one BackSpace per Unicode scalar (fine for ASCII romaji).
static void fake_wayland_backspace_glyph_count(glong glyphs) {
  constexpr glong kMax = 384;
  if (glyphs <= 0) {
    return;
  }
  if (glyphs > kMax) {
    logf("inject: clipping BackSpace repeats from %ld to %ld glyphs", static_cast<long>(glyphs),
         static_cast<long>(kMax));
    glyphs = kMax;
  }
  const bool hc = hyprctl_ipc_alive_for_wayland_keys();
  const bool wt = wtype_is_available();
  for (glong i = 0; i < glyphs; ++i) {
    bool ok = false;
    if (hc) {
      ok = hyprctl_dispatch_sendshortcut(",BACKSPACE,", "BackSpace erase pick", false);
    }
    if (!ok && wt) {
      ok = wtype_exec_chord("BackSpace", {"-k", "BackSpace"});
    }
    if (!ok) {
      logf("inject: BackSpace stopped early at %ld / %ld (no backend)", static_cast<long>(i),
           static_cast<long>(glyphs));
      break;
    }
    yield_to_compose_pipeline();
  }
}

void inject_replacement_clear_then_type(Display* d, const std::string& utf8,
                                        const std::string* wayland_clipboard_pick_utf8,
                                        bool force_ctrl_a_ignore_glyph_env = false) {
  MODORE_E2E_LOGF("inject: enter out_bytes=%zu d=%s pick_ptr=%s force_ctrl_a=%d", utf8.size(),
                  d ? "X11" : "null", wayland_clipboard_pick_utf8 ? "yes" : "no",
                  force_ctrl_a_ignore_glyph_env ? 1 : 0);
  // Glyph erase + wl-copy paste needs wl-paste/wl-copy and a UTF-8 byte span matching the live
  // field. Native Wayland uses d=nullptr; XWayland sessions often have d!=nullptr but still set
  // WAYLAND_DISPLAY — the erase/paste path uses Hypr/wtype (not XTest), so allow wl_erase there too
  // when AT-SPI supplies the span (clipboard pickup still passes the same pointer shape).
  const char* wl_disp = std::getenv("WAYLAND_DISPLAY");
  const bool on_wayland_desktop = wl_disp != nullptr && wl_disp[0] != '\0';
  const bool wl_erase = wayland_clipboard_pick_utf8 && !wayland_clipboard_pick_utf8->empty() &&
      wl_clipboard_available() && (!d || on_wayland_desktop);
  MODORE_E2E_LOGF("inject: wl_erase=%d on_wayland_desktop=%d", wl_erase ? 1 : 0,
                  on_wayland_desktop ? 1 : 0);
  if (wl_erase) {
    const std::string& pick_ref = *wayland_clipboard_pick_utf8;
    const glong n = g_utf8_strlen(pick_ref.c_str(), static_cast<gssize>(pick_ref.size()));
    logf("inject: span for glyph erase — %zu bytes (~%ld Unicode scalars) before replace",
         pick_ref.size(), static_cast<long>(n));

    // Chromium/Electron omnibox: per-glyph BackSpace often races IME — leftover romaji + paste
    // becomes garbage (e.g. "henx 1 1"). Ctrl+A + paste is reliable for single-line fields.
    // GTK mixed fields: set MODORE_WL_GLYPH_ERASE_ROMANJI=1 to force glyph erase for plain romaji.
    bool cleared_via_select_all = false;
    if (hyprctl_ipc_alive_for_wayland_keys() && pick_is_plain_ascii_romaji(pick_ref)) {
      const char* force_glyph = std::getenv("MODORE_WL_GLYPH_ERASE_ROMANJI");
      const bool glyph_only =
          !force_ctrl_a_ignore_glyph_env && force_glyph && force_glyph[0] &&
          std::strcmp(force_glyph, "0") != 0;
      if (!glyph_only) {
        logf("inject: Ctrl+A (select all) — plain ascii romaji pick");
        cleared_via_select_all =
            hypr_wayland_try_select_all("Select all before Modore replace (Ctrl+A)");
        if (cleared_via_select_all) {
          nap_after_compose_event(std::chrono::milliseconds(6));
        }
      } else {
        logf("inject: MODORE_WL_GLYPH_ERASE_ROMANJI — using glyph BackSpace for plain romaji");
      }
    }
    if (!cleared_via_select_all) {
      logf("inject: glyph BackSpace erase — ~%ld scalars", static_cast<long>(n));
      fake_wayland_backspace_glyph_count(n);
      nap_after_compose_event(std::chrono::milliseconds(4));
    }
    const bool prefer_clipboard = utf8_contains_non_ascii(utf8);
    // Fcitx/Mozc (and Chromium IME) intercept wtype unicode as composition: raw UTF-8 becomes
    // garbage (partial kana, mojibake). wl-copy then Shift+Insert commits cleanly.
    if (prefer_clipboard) {
      logf(
          "inject: wl-copy+pasting first (%zu UTF-8 bytes) — IME breaks raw wtype for non-ASCII",
          utf8.size());
      if (inject_utf8_via_wl_clipboard_paste(utf8)) {
        return;
      }
    }
    logf("inject: trying Unicode via wtype/ydotool after erase (%zu UTF-8 bytes)", utf8.size());
    if (inject_utf8_wayland_fallback(utf8)) {
      logf("inject: direct wtype/ydotool after erase succeeded — GTK often ignores paste "
           "simulation");
      return;
    }
    if (inject_utf8_via_wl_clipboard_paste(utf8)) {
      logf(prefer_clipboard ? "inject: retry wl-copy+paste after wtype failed (%zu UTF-8 bytes)"
                            : "inject: wl-copy+pasted after typed inject failed (%zu UTF-8 bytes)",
           utf8.size());
      return;
    }
    if (inject_via_atspi_string(utf8)) {
      logf("inject: AT-SPI STRING after erase (often no-ops under Wayland)");
      return;
    }
    logf("insert failed after erase (wtype/ydotool, wl-copy+paste, AT-SPI STRING)");
    return;
  }

  MODORE_E2E_LOGF("inject: non-wl_erase path -> fake_delete_selection_best");
  fake_delete_selection_best(d);
  // Wayland / IME-heavy apps need a beat after synthetic Delete before typing.
  if (d) {
    nap_after_compose_event(std::chrono::milliseconds(6));
  } else {
    nap_after_compose_event(std::chrono::milliseconds(12));
    const bool prefer_clipboard = utf8_contains_non_ascii(utf8);
    if (prefer_clipboard) {
      logf(
          "inject: Wayland — wl-copy+pasting first (%zu UTF-8 bytes; IME breaks raw wtype unicode)",
          utf8.size());
      if (inject_utf8_via_wl_clipboard_paste(utf8)) {
        return;
      }
    }
    logf(
        "inject: Wayland (no wl_erase) — Unicode keystrokes (%zu UTF-8 bytes); paste after if "
        "needed",
        utf8.size());
    if (inject_utf8_wayland_fallback(utf8)) {
      logf("inject: wtype/ydotool typed replacement (often replaces GTK selection outright)",
           utf8.size());
      return;
    }
    nap_after_compose_event(std::chrono::milliseconds(0));
    if (inject_utf8_via_wl_clipboard_paste(utf8)) {
      logf(prefer_clipboard ? "inject: wl-copy+paste retry after wtype failed (%zu UTF-8 bytes)"
                            : "inject: wl-copy+pasted after typed inject failed (%zu UTF-8 bytes)",
           utf8.size());
      return;
    }
    if (inject_via_atspi_string(utf8)) {
      logf("inject: AT-SPI STRING fallback (often no-ops under Wayland if you saw no text)");
      return;
    }
    logf("insert failed (wl-copy+Hypr paste, wtype/ydotool, AT-SPI STRING)");
    return;
  }

  if (inject_via_atspi_string(utf8)) {
    return;
  }
  if (inject_utf8_wayland_fallback(utf8)) {
    return;
  }
  logf("insert failed (AT-SPI STRING, wtype, ydotool)");
}

// Interpret wl-paste after a synthetic copy. When the selection already matches the
// clipboard offer, Ctrl+C is a no-op and the offer stays identical to baseline — we
// still treat non-empty clipboard as a valid pick.
static void wayland_interpret_after_copy(const std::string& baseline_clip,
                                         const std::string& baseline_primary,
                                         const char* attempt_label, std::string* after,
                                         bool* got_fresh,
                                         bool* clipboard_offer_unchanged /* may be null */) {
  *got_fresh = false;
  after->clear();
  if (clipboard_offer_unchanged) {
    *clipboard_offer_unchanged = false;
  }
  std::string after_clip;
  read_wl_clip_offer(&after_clip);
  if (!after_clip.empty() && !clipboard_normalized_equal(after_clip, baseline_clip)) {
    *got_fresh = true;
    *after = std::move(after_clip);
    return;
  }
  if (!after_clip.empty()) {
    *got_fresh = true;
    *after = std::move(after_clip);
    if (clipboard_offer_unchanged) {
      *clipboard_offer_unchanged = true;
    }
    logf("clipboard: %s pick from clipboard (unchanged vs baseline; copy likely no-op, %zu bytes)",
         attempt_label, after->size());
    return;
  }
  std::string after_primary;
  read_wl_primary_offer(&after_primary);
  if (!after_primary.empty() &&
      !wl_primary_looks_like_stale_global_chrome(after_primary) &&
      !clipboard_normalized_equal(after_primary, baseline_primary)) {
    *got_fresh = true;
    *after = std::move(after_primary);
    logf("clipboard: %s Wayland primary changed vs baseline (%zu bytes)", attempt_label,
         after->size());
  }
}

// Wait for selection nudges (Ctrl+Shift+Left) to show up on CLIPBOARD / PRIMARY.
static void wl_poll_until_clip_or_primary_moves(const std::string& ref_clip,
                                                const std::string& ref_primary,
                                                int max_wait_ms) {
  if (max_wait_ms <= 0) {
    return;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
  constexpr int kStep = 2;
  while (std::chrono::steady_clock::now() < deadline) {
    std::string c;
    std::string p;
    read_wl_clip_offer(&c);
    read_wl_primary_offer(&p);
    if (!clipboard_normalized_equal(c, ref_clip)) {
      return;
    }
    if (!clipboard_normalized_equal(p, ref_primary)) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kStep));
  }
}

// Poll after Ctrl+C instead of a long fixed sleep — clients often need 20–120ms to publish.
static void wayland_poll_after_copy(const std::string& baseline_clip,
                                    const std::string& baseline_primary,
                                    const char* attempt_label, std::string* after, bool* got_fresh,
                                    bool* clipboard_offer_unchanged, int max_wait_ms) {
  *got_fresh = false;
  after->clear();
  if (clipboard_offer_unchanged) {
    *clipboard_offer_unchanged = false;
  }
  constexpr int kStep = 2;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);

  while (std::chrono::steady_clock::now() < deadline) {
    std::string after_clip;
    read_wl_clip_offer(&after_clip);
    if (!after_clip.empty()) {
      if (!clipboard_normalized_equal(after_clip, baseline_clip)) {
        *got_fresh = true;
        after->swap(after_clip);
        return;
      }
      *got_fresh = true;
      after->swap(after_clip);
      if (clipboard_offer_unchanged) {
        *clipboard_offer_unchanged = true;
      }
      logf(
          "clipboard: %s pick from clipboard (unchanged vs baseline; copy likely no-op, %zu bytes)",
          attempt_label, after->size());
      return;
    }
    std::string after_primary;
    read_wl_primary_offer(&after_primary);
    if (!after_primary.empty() &&
        !wl_primary_looks_like_stale_global_chrome(after_primary) &&
        !clipboard_normalized_equal(after_primary, baseline_primary)) {
      *got_fresh = true;
      after->swap(after_primary);
      logf("clipboard: %s Wayland primary changed vs baseline (%zu bytes)", attempt_label,
           after->size());
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kStep));
  }
  wayland_interpret_after_copy(baseline_clip, baseline_primary, attempt_label, after, got_fresh,
                               clipboard_offer_unchanged);
}

void do_clipboard_pickup(Display* d, const std::string& clip_saved,
                         bool clipboard_clear_attempted_on_wl) {
  const bool wl = wl_clipboard_available();
  std::string baseline_clip;
  std::string baseline_primary;
  bool wl_clip_empty_after_trim = false;
  if (wl) {
    read_wl_clip_offer(&baseline_clip);
    read_wl_primary_offer(&baseline_primary);
    {
      std::string probe = baseline_clip;
      trim_trailing_crlf_inplace(&probe);
      trim_in_place_ascii(&probe);
      wl_clip_empty_after_trim = probe.empty();
    }
  } else if (!read_clipboard(&baseline_clip)) {
    baseline_clip.clear();
  }

  MODORE_E2E_LOGF("do_clipboard_pickup: wl=%d d=%s clip_saved=%zu preclear_wl=%d baseline_clip=%zu "
                  "baseline_prim=%zu wl_empty_trim=%d",
                  wl ? 1 : 0, d ? "X11" : "null", clip_saved.size(),
                  clipboard_clear_attempted_on_wl ? 1 : 0, baseline_clip.size(), baseline_primary.size(),
                  wl_clip_empty_after_trim ? 1 : 0);

  if (wl && baseline_clip.empty() && !baseline_primary.empty() &&
      wl_primary_looks_like_stale_global_chrome(baseline_primary)) {
    logf(
        "clipboard: baseline CLIPBOARD empty; PRIMARY looks like unrelated global UI/TUI chrome — "
        "not using it; will try synthetic copy from the focused window");
  }

  std::string picked;
  bool picked_ready = false;
  // Pick came from PRIMARY (or CLIPBOARD mirrored to it) without trusting post-Ctrl+C CLIPBOARD.
  // Nautilus-style path bars still skip glyph erase after `maybe_narrow_path_primary_pick` rewrote
  // the pick to a final segment; ordinary fields use glyph BackSpace + paste.
  bool pick_from_wayland_primary_mirror = false;

  // If we successfully ran wl-copy "" at pickup start, never trust "PRIMARY is fresh, CLIPBOARD is
  // stale junk" without also running synthetic copy. GTK path-bar cases still get Ctrl+Insert/Ctrl+C;
  // Chromium/Electron often stops updating PRIMARY on the live selection while keeping a tiny stale
  // CLIPBOARD offer — the old shortcut then re-reads yesterday's 6-byte romaji forever (see log:
  // tesuto vs nihongo both → 6-byte pick, 9-byte テスト output).
  const bool skip_primary_vs_stale_clipboard_shortcut =
      wl && clipboard_clear_attempted_on_wl;

  // GTK3/4 often mirrors the highlighted range onto PRIMARY without any copy action. Synthetic
  // Ctrl+Insert/Ctrl+C frequently fails to refresh CLIPBOARD on Wayland (Nautilus path bar), while
  // PRIMARY still reflects the live selection — there is nothing new for Walker to record either.
  if (wl && !picked_ready && !skip_primary_vs_stale_clipboard_shortcut && !baseline_primary.empty() &&
      !baseline_clip.empty()) {
    std::string prim = baseline_primary;
    trim_trailing_crlf_inplace(&prim);
    trim_in_place_ascii(&prim);
    constexpr size_t kMaxPrimPrefer = 384;
    const bool prim_single_line = prim.find_first_of("\n\r") == std::string::npos;
    const bool clip_multiline_or_huge =
        looks_like_line_copy(baseline_clip) && baseline_clip.size() > prim.size();
    const bool clip_much_larger_than_prim =
        baseline_clip.size() > prim.size() * 3 && baseline_clip.size() > prim.size() + 12;

    if (!prim.empty() && prim.size() <= kMaxPrimPrefer && prim_single_line &&
        !wl_primary_looks_like_stale_global_chrome(prim) &&
        (clip_multiline_or_huge || clip_much_larger_than_prim)) {
      picked = std::move(prim);
      picked_ready = true;
      pick_from_wayland_primary_mirror = true;
      logf(
          "clipboard: using Wayland PRIMARY (%zu bytes) without synthetic copy — CLIPBOARD offer "
          "looks unrelated/stale (%zu bytes); aligns with GTK selection when universal copy never "
          "reaches wl-paste/Walker",
          picked.size(), baseline_clip.size());
    }
  }

  // Short single-line CLIPBOARD equals PRIMARY (selection text is already on both offers). Still
  // avoid glyph-count erase: synthetic keys rarely hit the path bar; paste replaces selection here.
  if (wl && !picked_ready && !baseline_primary.empty() && !baseline_clip.empty()) {
    std::string prim = baseline_primary;
    std::string clip = baseline_clip;
    trim_trailing_crlf_inplace(&prim);
    trim_trailing_crlf_inplace(&clip);
    trim_in_place_ascii(&prim);
    trim_in_place_ascii(&clip);
    constexpr size_t kMaxMirror = 384;
    if (!prim.empty() && prim == clip && prim.size() <= kMaxMirror &&
        !wl_primary_looks_like_stale_global_chrome(prim) &&
        prim.find_first_of("\n\r") == std::string::npos) {
      picked = std::move(prim);
      picked_ready = true;
      pick_from_wayland_primary_mirror = true;
      logf(
          "clipboard: CLIPBOARD equals PRIMARY short line (%zu bytes) — using as pick "
          "(path-bar narrowing may skip glyph erase; otherwise wl_erase applies)",
          picked.size());
    }
  }

  std::string after;
  bool got_fresh = false;
  bool clip_noop_vs_baseline = false;
  bool did_force_select = false;

  if (!picked_ready) {
    // XTest copy writes the X11 CLIPBOARD; wl-paste can stay stale on XWayland Chromium — only poll
    // wl-clipboard when synthetic copy also used the Wayland path (d == nullptr).
    const bool use_wayland_clipboard_reads = wl && !d;
    MODORE_E2E_LOGF("clipboard: use_wayland_clipboard_reads=%d (wl=%d d=%s)",
                    use_wayland_clipboard_reads ? 1 : 0, wl ? 1 : 0, d ? "X11" : "null");
    if (wl && wl_clip_empty_after_trim) {
      fake_ctrl_shift_left_best(d);
      if (use_wayland_clipboard_reads) {
        wl_poll_until_clip_or_primary_moves(baseline_clip, baseline_primary, 28);
        logf("clipboard: Wayland pre-copy Ctrl+Shift+Left — clipboard was empty (trimmed)");
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(72));
        logf("clipboard: pre-copy Ctrl+Shift+Left — clipboard was empty (trimmed wl baseline); using "
             "X11 CLIPBOARD reads");
      }
    }
    fake_ctrl_c_best(d);

    if (use_wayland_clipboard_reads) {
      // Fast ceiling — most clients publish in <50ms; step=4ms polls exit early when possible.
      constexpr int kPollMsFirst = 72;
      constexpr int kPollMsAfterNudge = 100;
      constexpr int kPollMsBareRetry = 72;
      wayland_poll_after_copy(baseline_clip, baseline_primary, "after first Ctrl+C:", &after,
                              &got_fresh, &clip_noop_vs_baseline, kPollMsFirst);
      if (!got_fresh) {
        // Chromium/Electron Wayland: prepickup CLIPBOARD is often non-empty noise, so we skip the
        // pre-copy Ctrl+Shift+Left block — but then there is no selection and universal-copy is a
        // no-op. Nudge word-left to establish a span, then copy again.
        logf(
            "clipboard: first copy did not surface fresh clip/primary (clip=%zu prim=%zu baseline) — "
            "Ctrl+Shift+Left then copy (electron/wayland)",
            baseline_clip.size(), baseline_primary.size());
        fake_ctrl_shift_left_best(d);
        {
          std::string c0, p0;
          read_wl_clip_offer(&c0);
          read_wl_primary_offer(&p0);
          wl_poll_until_clip_or_primary_moves(c0, p0, 32);
        }
        fake_ctrl_c_best(d);
        wayland_poll_after_copy(baseline_clip, baseline_primary, "after word-nudge Ctrl+C:", &after,
                                &got_fresh, &clip_noop_vs_baseline, kPollMsAfterNudge);
      }
      if (!got_fresh) {
        nap_after_compose_event(std::chrono::milliseconds(0));
        fake_ctrl_c_best(d);
        wayland_poll_after_copy(baseline_clip, baseline_primary, "after bare retry Ctrl+C:", &after,
                                &got_fresh, &clip_noop_vs_baseline, kPollMsBareRetry);
      }
      if (!got_fresh && skip_primary_vs_stale_clipboard_shortcut &&
          !baseline_primary.empty() &&
          wl_primary_is_utf8_bounded_ascii_only_fast_pick(baseline_primary) &&
          !wl_primary_looks_like_stale_global_chrome(baseline_primary) &&
          baseline_primary.size() <= 512) {
        after.assign(baseline_primary);
        trim_trailing_crlf_inplace(&after);
        trim_in_place_ascii(&after);
        if (!after.empty()) {
          got_fresh = true;
          clip_noop_vs_baseline = true;
          pick_from_wayland_primary_mirror = true;
          logf(
              "clipboard: synthetic copy never matched baseline — PRIMARY ascii last resort (%zu bytes; "
              "may lag behind caret in some Electron builds)",
              after.size());
        }
      }
      if (!got_fresh && wl_clip_empty_after_trim && !baseline_primary.empty() &&
          wl_primary_is_utf8_bounded_ascii_only_fast_pick(baseline_primary) &&
          !wl_primary_looks_like_stale_global_chrome(baseline_primary) &&
          baseline_primary.size() <= 8192) {
        after.assign(baseline_primary);
        trim_trailing_crlf_inplace(&after);
        trim_in_place_ascii(&after);
        if (!after.empty()) {
          got_fresh = true;
          clip_noop_vs_baseline = false;
          pick_from_wayland_primary_mirror = true;
          logf("clipboard: CLIPBOARD still empty after Ctrl+C — ascii PRIMARY fallback (%zu bytes)",
               after.size());
        }
      }
    } else {
      if (!read_clipboard(&after)) {
        after.clear();
      }
      if (!after.empty() && !clipboard_normalized_equal(after, baseline_clip)) {
        got_fresh = true;
      } else if (!after.empty()) {
        got_fresh = true;
        logf("clipboard: X11 pick from clipboard (unchanged vs baseline after copy no-op; %zu bytes)",
             after.size());
      }
      if (!got_fresh) {
        logf("clipboard: first Ctrl+C did not change clipboard vs baseline (%zu chars) — retry",
             baseline_clip.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        fake_ctrl_c_best(d);
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        if (!read_clipboard(&after)) {
          after.clear();
        }
        if (!after.empty() && !clipboard_normalized_equal(after, baseline_clip)) {
          got_fresh = true;
        } else if (!after.empty()) {
          got_fresh = true;
          logf(
              "clipboard: X11 retry pick from clipboard (unchanged vs baseline; %zu bytes)",
              after.size());
        }
      }
    }

    MODORE_E2E_LOGF("clipboard: after synthetic copy phase got_fresh=%d after_sz=%zu clip_noop=%d",
                    got_fresh ? 1 : 0, after.size(), clip_noop_vs_baseline ? 1 : 0);

    if (!got_fresh) {
      logf("clipboard: no selection on first copy (empty or unchanged vs baseline)");
    } else if (looks_like_line_copy(after)) {
      std::string via_primary;
      if (wl && wl_try_primary_as_highlighted_span(baseline_primary, after, &via_primary)) {
        picked = std::move(via_primary);
        logf("clipboard: Wayland primary span (%zu bytes) — shorter than first line of clip",
             picked.size());
      } else if (clip_noop_vs_baseline) {
        // Ctrl+C did not refresh the CLIPBOARD offer; the buffer is whatever was already there.
        // Taking "first logical line" often converts unrelated paste data (see log: 500+ byte
        // blobs) while BackSpace+paste mutates the rename field the user actually sees.
        logf("clipboard: CLIPBOARD unchanged after Ctrl+C — refusing first-line truncation on "
             "multiline/large buffer (%zu bytes); forcing word-select",
             after.size());
      } else if (clipboard_first_reasonable_line(after, &picked)) {
        logf("clipboard: using first logical line (%zu bytes), skipping word-select",
             picked.size());
      } else {
        logf("clipboard: line-shaped clipboard but first line empty after trim — word-select");
      }
    } else {
      std::string via_primary_single;
      if (wl &&
          wl_try_primary_as_highlighted_span(baseline_primary, after, &via_primary_single)) {
        picked = std::move(via_primary_single);
        logf("clipboard: Wayland primary span (%zu bytes) inside single-line clip (%zu bytes)",
             picked.size(), after.size());
      } else if (wl && clip_noop_vs_baseline) {
        logf("clipboard: CLIPBOARD unchanged after Ctrl+C — refusing single-line pick (%zu "
             "bytes); forcing word-select",
             after.size());
      } else {
        picked = after;
      }
    }

    if (picked.empty()) {
      std::string snap_clip;
      std::string snap_prim;
      if (use_wayland_clipboard_reads) {
        read_wl_clip_offer(&snap_clip);
        read_wl_primary_offer(&snap_prim);
      } else if (!read_clipboard(&snap_clip)) {
        snap_clip.clear();
      }

      fake_ctrl_shift_left_best(d);
      did_force_select = true;
      if (use_wayland_clipboard_reads) {
        wl_poll_until_clip_or_primary_moves(snap_clip, snap_prim, 48);
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(70));
      }

      std::string baseline_ws_clip;
      std::string baseline_ws_primary;
      if (use_wayland_clipboard_reads) {
        read_wl_clip_offer(&baseline_ws_clip);
        read_wl_primary_offer(&baseline_ws_primary);
      } else if (!read_clipboard(&baseline_ws_clip)) {
        baseline_ws_clip.clear();
      }

      fake_ctrl_c_best(d);
      constexpr int kPollWordSelectCopyMs = 90;

      if (use_wayland_clipboard_reads) {
        bool gs = false;
        bool noop_ws = false;
        wayland_poll_after_copy(baseline_ws_clip, baseline_ws_primary,
                                "after word-select Ctrl+C:", &after, &gs, &noop_ws,
                                kPollWordSelectCopyMs);
        if (!gs) {
          after.clear();
        } else if (noop_ws && wl) {
          std::string narrowed_after_noop;
          if (wl_try_primary_as_highlighted_span(baseline_ws_primary, after, &narrowed_after_noop)) {
            after.assign(narrowed_after_noop);
            logf(
                "clipboard: word-select + primary narrowed after clipboard-noop Ctrl+C (%zu bytes)",
                after.size());
          } else {
            logf("clipboard: word-select Ctrl+C left CLIPBOARD unchanged vs snapshot — refusing "
                 "likely-stale clip (%zu bytes)",
                 after.size());
            after.clear();
          }
        }
      } else {
        constexpr int kStepMs = 8;
        after.clear();
        const auto ddl = std::chrono::steady_clock::now() + std::chrono::milliseconds(380);
        while (std::chrono::steady_clock::now() < ddl) {
          std::string c;
          if (!read_clipboard(&c)) {
            c.clear();
          }
          if (!c.empty()) {
            after.swap(c);
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(kStepMs));
        }
      }

      if (after.empty()) {
        logf("clipboard: nothing to convert");
        if (did_force_select) {
          fake_right_arrow_best(d);
        }
        write_clipboard(clip_saved);
        return;
      }
      picked = after;
      std::string via_primary_ws;
      if (wl && wl_try_primary_as_highlighted_span(baseline_ws_primary, after, &via_primary_ws)) {
        picked = std::move(via_primary_ws);
        logf("clipboard: word-select + primary narrowed pick (%zu bytes)", picked.size());
      }
    }
  }

  while (!picked.empty() && (picked.back() == '\n' || picked.back() == '\r')) {
    picked.pop_back();
  }

  const bool path_pick_narrowed_to_segment = maybe_narrow_path_primary_pick(&picked);
  const bool omniboz_url_tail_narrowed = maybe_narrow_omnibox_url_contaminated_pick(&picked);
  const bool pick_trimmed_utf8_noise = trim_pick_leading_romaji_if_utf8_contaminated(&picked);

  if (clipboard_pick_probably_not_romaji_field(picked)) {
    logf("clipboard: pick looks like a shell/command line (or modore path) — skipping Mozc to avoid "
         "mixed kana+ASCII garbage in clipboard history");
    if (did_force_select) {
      fake_right_arrow_best(d);
    }
    write_clipboard(clip_saved);
    return;
  }

  if (clipboard_pick_probably_ide_ui_hint(picked)) {
    if (did_force_select) {
      fake_right_arrow_best(d);
    }
    write_clipboard(clip_saved);
    return;
  }

  if (!pick_is_plain_ascii_romaji(picked) && pick_looks_like_mojibake_garbage(picked)) {
    logf(
        "clipboard: pick blocked as stale mojibake — not running Mozc (no UI recovery by default; "
        "set MODORE_MOJIBAKE_RECOVERY=1 for notify + Hypr Ctrl+A clear; MODORE_NO_MOJIBAKE_RECOVERY "
        "still suppresses the clear when recovery is on)");
    if (mojibake_recovery_aggressive_enabled()) {
      notify_corrupted_pick_needs_recovery();
      if (!d /* Wayland clipboard path uses Hypr chords */ &&
          hypr_attempt_clear_focused_edit_field_best_effort(
              "Modore corrupted pick — select all+clear focused field")) {
        logf("clipboard: attempted Hypr Ctrl+A + delete/backspace to clear fouled omnibox");
      }
    }
    if (did_force_select) {
      fake_right_arrow_best(d);
    }
    write_clipboard(clip_saved);
    return;
  }

  std::string replacement;
  MODORE_E2E_LOGF("do_clipboard_pickup: calling mozc_convert pick_sz=%zu", picked.size());
  if (!mozc_convert_utf8(picked, &replacement)) {
    logf("clipboard: mozc_convert failed (%zu-byte pick)", picked.size());
    if (did_force_select) {
      fake_right_arrow_best(d);
    }
    write_clipboard(clip_saved);
    return;
  }

  // PRIMARY mirror shortcuts previously skipped wl_erase (glyph BackSpace loop) because Nautilus
  // path bars mis-count vs the narrowed segment. Plain browser/Electron IME fields need that erase:
  // otherwise romaji stays and paste appends Kanji ("henkan" + fragment).
  const bool skip_wayland_glyph_erase =
      pick_from_wayland_primary_mirror && path_pick_narrowed_to_segment && !omniboz_url_tail_narrowed;
  inject_replacement_clear_then_type(d, replacement, skip_wayland_glyph_erase ? nullptr : &picked,
                                     pick_trimmed_utf8_noise);
  MODORE_E2E_LOGF("do_clipboard_pickup: inject_replacement returned (pick %zu -> out %zu)", picked.size(),
                  replacement.size());
  logf("clipboard: conversion %zu-byte pick → %zu-byte output (see inject: lines above)",
       picked.size(), replacement.size());

  nap_after_compose_event(std::chrono::milliseconds(2));
  write_clipboard(clip_saved);
  if (did_force_select) {
    fake_right_arrow_best(d);
  }
}

static void snapshot_clip_for_restore(std::string* clip_saved) {
  clip_saved->clear();
  if (wl_clipboard_available()) {
    if (!read_wl_clip_offer(clip_saved)) {
      clip_saved->clear();
    }
    return;
  }
  if (!read_clipboard(clip_saved)) {
    clip_saved->clear();
  }
}

void do_pickup(Display* d) {
  std::lock_guard<std::mutex> lock(g_pickup_mu);
  MODORE_E2E_LOGF("do_pickup: enter d=%s", d ? "X11" : "null");
  // macOS always tries Accessibility first for the real selection + direct replace. Linux used to
  // skip AT-SPI when DISPLAY was omitted (native Wayland), which forced the slow clipboard/Ctrl+C
  // pipeline. Try AT-SPI regardless of X11 Display; fall back to synthetic keys only when needed.
  const char* skip_atspi_first = std::getenv("MODORE_SKIP_ATSPI_FIRST");
  bool direct = false;
  std::string inject;
  std::string atspi_pick_span;
  if (!skip_atspi_first || !skip_atspi_first[0]) {
    if (try_pickup_atspi(&direct, &inject, &atspi_pick_span)) {
      if (direct) {
        MODORE_E2E_LOGF("do_pickup: AT-SPI direct editable replace done");
        logf("replaced via AT-SPI (editable)");
        return;
      }
      std::string clip_saved;
      snapshot_clip_for_restore(&clip_saved);
      // On X11, STRING sometimes replaces the span; on Wayland, Chromium/Electron often ignore it
      // while atspi still reports success — always use erase + wl-copy paste there when we have the
      // source span from AT-SPI.
      if (!std::getenv("WAYLAND_DISPLAY")) {
        if (inject_via_atspi_string(inject)) {
          logf("replaced via AT-SPI STRING inject (no wl-copy paste path)");
          nap_after_compose_event(std::chrono::milliseconds(2));
          write_clipboard(clip_saved);
          return;
        }
        logf("AT-SPI: STRING inject failed — synthetic delete/type / clipboard paste");
      } else {
        logf("AT-SPI: Wayland — erase + wl-copy paste (skip STRING; span %zu bytes for glyph clear)",
             atspi_pick_span.size());
      }
      inject_replacement_clear_then_type(d, inject,
                                         atspi_pick_span.empty() ? nullptr : &atspi_pick_span,
                                         false);
      MODORE_E2E_LOGF("do_pickup: AT-SPI non-direct inject path finished");
      nap_after_compose_event(std::chrono::milliseconds(3));
      write_clipboard(clip_saved);
      return;
    }
  }

  // Escape hatch: refuse the racy synthetic-Ctrl+C + clipboard pickup pipeline entirely. AT-SPI
  // is the only "clean" source of truth (it reads the field directly and replaces the actual
  // span); the clipboard path papers over apps that don't expose AT-SPI properly, but as the user
  // noted, it has bitten us repeatedly with stale offers, MIME confusion (PNG image bytes leaking
  // in as "❱PNG"), and races in Chromium/Electron. Set MODORE_ATSPI_ONLY=1 to bail clean instead.
  const char* atspi_only = std::getenv("MODORE_ATSPI_ONLY");
  if (atspi_only && atspi_only[0] && std::strcmp(atspi_only, "0") != 0) {
    logf("pickup: AT-SPI did not produce a span and MODORE_ATSPI_ONLY=1 — skipping clipboard "
         "fallback (no synthetic Ctrl+C / wl-paste reads). Make sure the focused app exposes the "
         "field via AT-SPI, or unset MODORE_ATSPI_ONLY to re-enable the clipboard pipeline.");
    return;
  }

  if (std::getenv("WAYLAND_DISPLAY")) {
    // Hypr needs a short beat after the exec that spawned --trigger so sendshortcut targets the
    // same surface; skip this delay entirely when AT-SPI handled pickup above.
    nap_after_compose_event(std::chrono::milliseconds(1));
  }
  logf("pickup: start synthetic_keys=%s", wayland_pickup_synthetic_backend_label(d));
  MODORE_E2E_LOGF("do_pickup: falling through to synthetic clipboard pickup");
  std::string clip_saved;
  snapshot_clip_for_restore(&clip_saved);
  // Default: do not clear CLIPBOARD before pickup — fewer wl-copy round-trips and races in
  // Chromium; GTK still gets a correct pick via synthetic copy + PRIMARY mirrors when applicable.
  // Opt in: MODORE_PICKUP_CLEAR_CLIPBOARD=1 (restores stronger "stale CLIPBOARD" avoidance).
  bool clipboard_clear_attempted_on_wl = false;
  const char* preclear_e = std::getenv("MODORE_PICKUP_CLEAR_CLIPBOARD");
  if (preclear_e && preclear_e[0] && std::strcmp(preclear_e, "0") != 0) {
    if (write_clipboard("")) {
      logf("pickup: cleared CLIPBOARD baseline (saved %zu bytes for restore after conversion)",
           clip_saved.size());
      if (wl_clipboard_available()) {
        clipboard_clear_attempted_on_wl = true;
        if (!poll_wl_clipboard_cleared(40, 1)) {
          logf(
              "pickup: CLIPBOARD still non-empty after wl-copy \"\" — disabling PRIMARY-vs-stale-CLIPBOARD "
              "fast path; synthetic copy will run");
        }
      }
    }
  }
  do_clipboard_pickup(d, clip_saved, clipboard_clear_attempted_on_wl);
}

struct RunOptions {
  bool trigger = false;
  bool ipc_only = false;
  bool no_ipc = false;
  bool version = false;
  bool help = false;
};

bool parse_run_options(int argc, char** argv, RunOptions* o) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--trigger") == 0) {
      o->trigger = true;
    } else if (std::strcmp(argv[i], "--ipc-only") == 0) {
      o->ipc_only = true;
    } else if (std::strcmp(argv[i], "--no-ipc") == 0) {
      o->no_ipc = true;
    } else if (std::strcmp(argv[i], "--version") == 0) {
      o->version = true;
    } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      o->help = true;
    } else {
      logf("unknown argument: %s — use one command per run, e.g. \"%s --version\" then \"%s "
           "--trigger\" (not \"/\" between flags). Try --help.",
           argv[i], argv[0], argv[0]);
      return false;
    }
  }
  return true;
}

void run_ipc_pickup() {
  MODORE_E2E_LOGF("run_ipc_pickup: DISPLAY=%s WAYLAND_DISPLAY=%s MODORE_IPC_SOCKET=%s",
                  std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(unset)",
                  std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY") : "(unset)",
                  std::getenv("MODORE_IPC_SOCKET") ? std::getenv("MODORE_IPC_SOCKET") : "(unset)");
  Display* work = nullptr;
  const char* xd = std::getenv("DISPLAY");
  const char* wl = std::getenv("WAYLAND_DISPLAY");
  const bool skip_x_for_native_wayland =
      xd && xd[0] && wl && wl[0] && hypr_focus_is_wayland_native();

  // When DISPLAY is set, do_pickup(non-null Display*) uses XTest for clipboard fallback. AT-SPI is
  // always attempted first (same order as macOS Accessibility), even for native Wayland focus.
  if (xd && xd[0]) {
    if (skip_x_for_native_wayland) {
      logf("ipc pickup: Hypr focused client is Wayland-native — using Hypr/wtype path "
           "(DISPLAY=%s)",
           xd);
    } else {
      work = XOpenDisplay(nullptr);
      if (!work) {
        logf("ipc pickup: DISPLAY is set (%s) but XOpenDisplay failed — using pure Wayland path",
             xd);
      } else if (wl && wl[0]) {
        logf("ipc pickup: using X11 keyboard/display path (DISPLAY=%s) — best when focus is "
             "XWayland (Chromium Electron X11, etc.)",
             xd);
      } else {
        logf("ipc pickup: using X11 keyboard/display path (DISPLAY=%s)", xd);
      }
    }
  } else if (wl && wl[0]) {
    logf(
        "ipc pickup: DISPLAY unset — using synthetic Hypr/wtype + wl clipboard "
        "(Ozone/Electron/Wayland clients)");
  }
  if (work) {
    MODORE_E2E_LOGF("run_ipc_pickup: do_pickup(Display*)");
    do_pickup(work);
    XCloseDisplay(work);
  } else {
    MODORE_E2E_LOGF("run_ipc_pickup: do_pickup(nullptr)");
    do_pickup(nullptr);
  }
}

}  // namespace

static void print_modore_host_usage(const char* prog) {
  std::printf(
      "Usage:\n"
      "  %s --help | -h           this text\n"
      "  %s --version             build stamp\n"
      "  %s --trigger             signal a running host to run conversion (Unix socket)\n"
      "  %s [options]             run the host (default: hotkey + IPC listener)\n"
      "\n"
      "Run options (host mode):\n"
      "  --ipc-only               no X11 grab; only listen for --trigger / compositor exec\n"
      "  --no-ipc                 X11 grab only; disable Unix socket\n"
      "\n"
      "`--version` and `--trigger` are separate commands. Example:\n"
      "  %s --version && %s --trigger\n"
      "\n"
      "Environment (optional):\n"
      "  MODORE_IPC_SOCKET=/path   override Unix socket path (--trigger + listener); for tests or "
      "second instance\n"
      "  MODORE_E2E_TRACE=1      verbose [e2e] step logs on stderr + modore.log (Puppeteer / debugging)\n"
      "  MODORE_SKIP_ATSPI_FIRST=1  skip Accessibility pick — clipboard/Ctrl+C only (disables "
      "AT-SPI romaji span erase for Chromium on Wayland)\n"
      "  MODORE_ATSPI_ONLY=1     refuse the clipboard fallback entirely; if AT-SPI can't read the "
      "focused field, bail (no synthetic Ctrl+C, no wl-paste reads — avoids screenshot/PNG bytes "
      "leaking in as text)\n"
      "  MODORE_PICKUP_CLEAR_CLIPBOARD=1  empty CLIPBOARD before synthetic Ctrl+C (GTK staleness; "
      "extra wl-copy)\n"
      "  MODORE_WL_GLYPH_ERASE_ROMANJI=1  Wayland: per-glyph BackSpace for plain romaji (default: "
      "Ctrl+A clear — better for Chromium omnibox; use for GTK mixed fields)\n\n",
      prog, prog, prog, prog, prog, prog);
}

int main(int argc, char** argv) {
  RunOptions opts{};
  if (!parse_run_options(argc, argv, &opts)) {
    return 1;
  }
  if (opts.help) {
    print_modore_host_usage(argv[0]);
    return 0;
  }
  if (opts.version) {
    std::printf("modore-host %s %s (multi-path --trigger + /run/user UID sock)\n", __DATE__,
                __TIME__);
    return 0;
  }
  if (opts.trigger) {
    return ipc_send_pickup();
  }

  augment_path_for_subprocesses();

  const char* disp_env = std::getenv("DISPLAY");
  const char* wl_env = std::getenv("WAYLAND_DISPLAY");
  modore_log("boot", "starting pid=%d DISPLAY=%s WAYLAND_DISPLAY=%s",
             static_cast<int>(::getpid()), disp_env ? disp_env : "(unset)",
             wl_env ? wl_env : "(unset)");
  if (wl_env) {
    modore_log("boot",
               "WAYLAND_DISPLAY is set — X11 hotkey grab may not fire when a native "
               "Wayland window has focus. Use compositor exec → modore-host --trigger if needed.");
    const char* sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    modore_log("boot",
               "Wayland pickup capabilities: hyprctl_IPC=%s HYPRLAND_INSTANCE_SIGNATURE=%s "
               "wtype=%s wl-paste=%s",
               hyprctl_ipc_alive_for_wayland_keys() ? "yes" : "no",
               (sig && sig[0]) ? "set" : "(unset)",
               wtype_is_available() ? "yes" : "no",
               wl_clipboard_available() ? "yes" : "no");
    if (!hyprctl_ipc_alive_for_wayland_keys() && !wtype_is_available()) {
      modore_log("boot",
                 "Wayland: need `hyprctl` (Hyprland) or `wtype` for synthetic keys / clipboard pickup");
    }
  }

  std::string profile = default_profile_dir();
  if (mozc_bridge_init(profile.c_str()) != 0) {
    modore_log("mozc", "bridge init failed: %s",
               mozc_bridge_last_error() ? mozc_bridge_last_error() : "?");
    return 1;
  }
  modore_log("mozc", "bridge initialized (profile=%s)", profile.c_str());

  ModoreConfig modore_config;
  std::string cfg_err;
  if (!load_modore_config(&modore_config, &cfg_err)) {
    modore_log("config", "%s — using defaults", cfg_err.c_str());
  }
  modore_log("config", "[conversion] hotkey=%s — ~/.config/modore/modore.conf",
             modore_config.conversion_hotkey_description.c_str());

  if (atspi_init() != 0) {
    modore_log("atspi", "init failed");
    return 1;
  }

  if (!opts.no_ipc) {
    if (!setup_pickup_pipe()) {
      return 1;
    }
    ipc_start_background([]() { notify_main_pickup_pending(); });
  }

  if (opts.ipc_only) {
    modore_log("ipc",
               "IMPORTANT — ipc-only: the conversion hotkey in ~/.config/modore/modore.conf is "
               "NOT wired to this mode. systemd/Omarchy almost always ships --ipc-only, so "
               "presses do nothing.");
    modore_log("ipc",
               "ipc-only fix: bind a chord in Hypr/Omarchy to exec your host trigger, for example "
               "(avoid Ctrl+Shift+` in Edge — it opens devtools); adjust path if needed:\n"
               "  binddp = CONTROL SHIFT ALT, J, Modore pickup, exec, %s/.local/bin/modore-host "
               "--trigger",
               getenv_string("HOME", "/HOME").c_str());
    modore_log("ipc",
               "waiting on socket — after adding the bind, log should show `pickup:` when keys work");
    for (;;) {
      main_thread_run_pickup_after_wake();
    }
  }

  Display* d = XOpenDisplay(nullptr);
  if (!d) {
    modore_log("hotkey",
               "XOpenDisplay failed — an X11 display is required for the hotkey grab "
               "(or run with --ipc-only and trigger via socket)");
    return 1;
  }

  int ignore = 0;
  if (!XTestQueryExtension(d, &ignore, &ignore, &ignore, &ignore)) {
    modore_log("hotkey", "XTest extension unavailable — install libXtst");
    XCloseDisplay(d);
    return 1;
  }

  Window root = DefaultRootWindow(d);
  const KeySym hotkey_ks = static_cast<KeySym>(modore_config.conversion_hotkey.keysym);
  const KeyCode hotkey_kc = XKeysymToKeycode(d, hotkey_ks);
  if (!hotkey_kc) {
    modore_log("hotkey", "no keycode for configured keysym — check ~/.config/modore/modore.conf");
    XCloseDisplay(d);
    return 1;
  }
  const unsigned int want_mods = modore_config.conversion_hotkey.modifier_mask;

  g_x11_setup_error = 0;
  XErrorHandler prev_handler = XSetErrorHandler(x11_quiet_error_handler);
  // NumLock/CapsLock add Mod2Mask/LockMask to the modifier field; registering grabs for those
  // combinations fixes "hotkey stopped working when NumLock toggled".
  constexpr unsigned grab_sets[] = {0u, Mod2Mask, LockMask, Mod2Mask | LockMask};
  bool grabbed_any = false;
  for (unsigned int extra : grab_sets) {
    g_x11_setup_error = 0;
    XGrabKey(d, hotkey_kc, want_mods | extra, root, False, GrabModeAsync, GrabModeAsync);
    XSync(d, False);
    if (!g_x11_setup_error) {
      grabbed_any = true;
    }
  }
  XSetErrorHandler(prev_handler);

  if (!grabbed_any) {
    modore_log("hotkey",
               "XGrabKey failed (X11 error). Another client may already hold this key, or the "
               "compositor blocked the grab. Try a different hotkey in ~/.config/modore/modore.conf "
               "or run under X11.");
    XCloseDisplay(d);
    return 1;
  }

  XSelectInput(d, root, KeyPressMask);
  XFlush(d);

  const char* ks_label = XKeysymToString(hotkey_ks);
  modore_log("boot", "ready: XGrabKey active (%s · keysym=%s)",
             modore_config.conversion_hotkey_description.c_str(), ks_label ? ks_label : "?");

  const int x_fd = ConnectionNumber(d);

  for (;;) {
    struct pollfd fds[2];
    int nfds = 1;
    fds[0].fd = x_fd;
    fds[0].events = POLLIN;
    if (g_pickup_pipe[0] >= 0) {
      fds[1].fd = g_pickup_pipe[0];
      fds[1].events = POLLIN;
      nfds = 2;
    }
    if (poll(fds, nfds, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      logf("poll failed: %s", std::strerror(errno));
      break;
    }

    if (nfds > 1 && (fds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
      main_thread_run_pickup_after_wake();
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      while (XPending(d) > 0) {
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type != KeyPress) {
          continue;
        }
        if (ev.xkey.keycode != hotkey_kc) {
          continue;
        }
        const unsigned core_mods_mask =
            ShiftMask | ControlMask | Mod1Mask | Mod4Mask | Mod5Mask;
        const unsigned mods_effective = ev.xkey.state & core_mods_mask;
        if (mods_effective != want_mods) {
          continue;
        }

        run_ipc_pickup();
      }
    }
  }

  XCloseDisplay(d);
  return 0;
}
