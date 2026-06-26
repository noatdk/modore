// wayland_hypr.cpp — wtype + hyprctl + Hyprland focus cache, window
// classification, and Wayland synthetic chords / injection fallbacks.

#include "host_internal.hpp"

namespace modore_host {

void update_hypr_window_snapshot(const HyprWindowSnapshot &snapshot) {
  std::lock_guard<std::mutex> lock(g_hypr_window_mu);
  g_hypr_window_snapshot = snapshot;
  g_hypr_window_snapshot_valid = true;
}

bool copy_hypr_window_snapshot(HyprWindowSnapshot *out) {
  if (!out) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_hypr_window_mu);
  if (!g_hypr_window_snapshot_valid) {
    return false;
  }
  *out = g_hypr_window_snapshot;
  return true;
}

// Resolves the `wtype` binary (Wayland key injection). Must run after
// augment_path.
const char *resolve_wtype_executable() {
  if (!g_wtype_path.empty()) {
    return g_wtype_path.c_str();
  }
  static const char *kCandidates[] = {"/usr/bin/wtype", "/usr/local/bin/wtype"};
  for (auto *p : kCandidates) {
    if (::access(p, X_OK) == 0) {
      g_wtype_path = p;
      return g_wtype_path.c_str();
    }
  }
  augment_path_for_subprocesses();
  if (command_ok("command -v wtype >/dev/null 2>&1")) {
    FILE *f = ::popen("command -v wtype", "r");
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

bool wtype_is_available() { return resolve_wtype_executable() != nullptr; }

// fork+exec (no shell): systemd and quoting cannot break chord arguments; IME
// env is cleared in the child only.
bool wtype_exec_chord(const char *desc_for_log,
                      const std::vector<const char *> &args) {
  const char *path = resolve_wtype_executable();
  if (!path) {
    logf("wtype: not found (%s)", desc_for_log);
    return false;
  }
  std::vector<char *> av;
  av.reserve(args.size() + 2);
  av.push_back(const_cast<char *>("wtype"));
  for (const char *a : args) {
    av.push_back(const_cast<char *>(a));
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

bool wtype_chord_shift_insert() {
  return wtype_exec_chord("Shift+Insert paste",
                          {"-s", "40", "-M", "shift", "-k", "Insert"});
}

bool wtype_chord_ctrl_v() {
  return wtype_exec_chord("Ctrl+V paste", {"-s", "40", "-M", "ctrl", "v"});
}

bool wtype_chord_ctrl_c() {
  return wtype_exec_chord("Ctrl+C", {"-s", "40", "-M", "ctrl", "c"});
}

// Omarchy SUPER+C → sendkeystate CTRL+Insert (Universal copy); GTK4/Wayland
// often syncs WL clipboard from this chord when plain Ctrl+C does not update
// the clipboard offer.
bool wtype_chord_ctrl_insert_copy() {
  return wtype_exec_chord("Ctrl+Insert copy",
                          {"-s", "40", "-M", "ctrl", "-k", "Insert"});
}

bool wtype_chord_ctrl_shift_left() {
  return wtype_exec_chord("Ctrl+Shift+Left", {"-s", "40", "-M", "ctrl", "-M",
                                              "shift", "-k", "Left"});
}

bool wtype_chord_ctrl_a() {
  return wtype_exec_chord("Ctrl+A", {"-s", "40", "-M", "ctrl", "a"});
}

bool wtype_chord_shift_home() {
  return wtype_exec_chord("Shift+Home",
                          {"-s", "40", "-M", "shift", "-k", "Home"});
}

bool wtype_key_right() { return wtype_exec_chord("Right", {"-k", "Right"}); }

bool wtype_key_delete_or_backspace() {
  if (wtype_exec_chord("Delete", {"-k", "Delete"})) {
    return true;
  }
  return wtype_exec_chord("BackSpace", {"-k", "BackSpace"});
}

const char *resolve_hyprctl_executable() {
  if (!g_hyprctl_path.empty()) {
    return g_hyprctl_path.c_str();
  }
  static const char *kCandidates[] = {"/usr/bin/hyprctl",
                                      "/usr/local/bin/hyprctl"};
  for (auto *p : kCandidates) {
    if (::access(p, X_OK) == 0) {
      g_hyprctl_path = p;
      return g_hyprctl_path.c_str();
    }
  }
  augment_path_for_subprocesses();
  if (command_ok("command -v hyprctl >/dev/null 2>&1")) {
    FILE *f = ::popen("command -v hyprctl", "r");
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

bool fork_hyprctl_version_ok(const char *hc_path) {
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

bool hyprctl_query_activewindow_json(std::string *json) {
  json->clear();
  if (!g_wayland_uses_hypr_sendshortcut) {
    return false;
  }
  const char *hc = resolve_hyprctl_executable();
  if (!hc) {
    return false;
  }
  int link[2];
  if (::pipe(link) != 0) {
    return false;
  }
  pid_t pid = ::fork();
  if (pid < 0) {
    ::close(link[0]);
    ::close(link[1]);
    return false;
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
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(link[0], buf, sizeof(buf));
    if (n <= 0) {
      break;
    }
    json->append(buf, static_cast<size_t>(n));
  }
  ::close(link[0]);
  int st = 0;
  (void)::waitpid(pid, &st, 0);
  return WIFEXITED(st) && WEXITSTATUS(st) == 0 && !json->empty();
}

bool json_string_field(const std::string &json, const char *key,
                       std::string *out) {
  out->clear();
  if (!key || !key[0]) {
    return false;
  }
  const std::string needle = std::string("\"") + key + "\"";
  const size_t k = json.find(needle);
  if (k == std::string::npos) {
    return false;
  }
  size_t p = json.find(':', k + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  while (++p < json.size() &&
         std::isspace(static_cast<unsigned char>(json[p]))) {
  }
  if (p >= json.size() || json[p] != '"') {
    return false;
  }
  ++p;
  std::string value;
  for (; p < json.size(); ++p) {
    const char c = json[p];
    if (c == '\\' && p + 1 < json.size()) {
      value.push_back(json[++p]);
      continue;
    }
    if (c == '"') {
      *out = std::move(value);
      return true;
    }
    value.push_back(c);
  }
  return false;
}

bool json_bool_field(const std::string &json, const char *key, bool *out) {
  if (!key || !key[0]) {
    return false;
  }
  const std::string needle = std::string("\"") + key + "\"";
  const size_t k = json.find(needle);
  if (k == std::string::npos) {
    return false;
  }
  size_t p = json.find(':', k + needle.size());
  if (p == std::string::npos) {
    return false;
  }
  while (++p < json.size() &&
         std::isspace(static_cast<unsigned char>(json[p]))) {
  }
  if (p + 3 < json.size() && json.compare(p, 4, "true") == 0) {
    *out = true;
    return true;
  }
  if (p + 4 < json.size() && json.compare(p, 5, "false") == 0) {
    *out = false;
    return true;
  }
  return false;
}

bool hyprctl_query_activewindow_snapshot(HyprWindowSnapshot *snapshot) {
  if (!snapshot) {
    return false;
  }
  snapshot->klass.clear();
  snapshot->initial_class.clear();
  snapshot->initial_title.clear();
  snapshot->app_id.clear();
  snapshot->title.clear();
  snapshot->xwayland = false;
  std::string json;
  if (!hyprctl_query_activewindow_json(&json)) {
    return false;
  }
  std::string klass;
  std::string initial_class;
  std::string title;
  std::string initial_title;
  bool xwayland = false;
  (void)json_string_field(json, "class", &klass);
  (void)json_string_field(json, "initialClass", &initial_class);
  (void)json_string_field(json, "title", &title);
  (void)json_string_field(json, "initialTitle", &initial_title);
  (void)json_bool_field(json, "xwayland", &xwayland);
  snapshot->klass = klass;
  snapshot->initial_class = initial_class;
  snapshot->initial_title = initial_title;
  snapshot->app_id = !klass.empty() ? klass : initial_class;
  snapshot->title = !title.empty() ? title : initial_title;
  snapshot->xwayland = xwayland;
  return true;
}

void log_hyprland_activewindow_snapshot(const char *context) {
  if (!g_wayland_uses_hypr_sendshortcut) {
    return;
  }
  HyprWindowSnapshot snapshot{};
  if (!hyprctl_query_activewindow_snapshot(&snapshot)) {
    modore_log("ipc", "%s focus: Hypr activewindow unavailable",
               context ? context : "pickup");
    return;
  }
  update_hypr_window_snapshot(snapshot);
  modore_log(
      "ipc",
      "%s focus: class=%s initialClass=%s title=%s initialTitle=%s xwayland=%s",
      context ? context : "pickup",
      snapshot.klass.empty() ? "(unset)" : snapshot.klass.c_str(),
      snapshot.initial_class.empty() ? "(unset)"
                                     : snapshot.initial_class.c_str(),
      snapshot.title.empty() ? "(unset)" : snapshot.title.c_str(),
      snapshot.initial_title.empty() ? "(unset)"
                                     : snapshot.initial_title.c_str(),
      snapshot.xwayland ? "yes" : "no");
}

std::string hyprland_socket2_path() {
  const char *rt = std::getenv("XDG_RUNTIME_DIR");
  const char *sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (!rt || !*rt || !sig || !*sig) {
    return {};
  }
  return std::string(rt) + "/hypr/" + sig + "/.socket2.sock";
}

bool set_sockaddr_un_path(const std::string &path, sockaddr_un *addr) {
  if (!addr) {
    return false;
  }
  if (path.size() >= sizeof(addr->sun_path)) {
    modore_log("ipc", "socket path too long (%zu bytes; max %zu for AF_UNIX)",
               path.size(), sizeof(addr->sun_path) - 1);
    return false;
  }
  std::memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  std::memcpy(addr->sun_path, path.c_str(), path.size() + 1);
  return true;
}

bool hyprland_refresh_activewindow_snapshot_from_ipc() {
  HyprWindowSnapshot snapshot{};
  if (!hyprctl_query_activewindow_snapshot(&snapshot)) {
    return false;
  }
  update_hypr_window_snapshot(snapshot);
  return true;
}

void hyprland_socket2_event_loop() {
  const std::string socket_path = hyprland_socket2_path();
  if (socket_path.empty()) {
    modore_log(
        "ipc",
        "Hyprland focus cache listener skipped: socket2 path unavailable");
    return;
  }

  HyprWindowSnapshot seeded{};
  if (hyprctl_query_activewindow_snapshot(&seeded)) {
    update_hypr_window_snapshot(seeded);
  }

  while (true) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }

    sockaddr_un addr{};
    if (!set_sockaddr_un_path(socket_path, &addr)) {
      ::close(fd);
      return;
    }
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
      ::close(fd);
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      continue;
    }

    std::string pending;
    char buf[512];
    while (true) {
      const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
      if (n <= 0) {
        break;
      }
      pending.append(buf, static_cast<size_t>(n));
      size_t newline = std::string::npos;
      while ((newline = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, newline);
        pending.erase(0, newline + 1);
        if (line.rfind("activewindow>>", 0) == 0 ||
            line.rfind("activewindowv2>>", 0) == 0 ||
            line.rfind("keyboardFocus>>", 0) == 0) {
          if (!hyprland_refresh_activewindow_snapshot_from_ipc()) {
            // Fall back to parsing the event payload if Hyprctl is briefly
            // unavailable.
            HyprWindowSnapshot snapshot{};
            const size_t sep = line.find(">>");
            const std::string payload =
                sep == std::string::npos ? std::string() : line.substr(sep + 2);
            const size_t comma = payload.find(',');
            snapshot.klass =
                comma == std::string::npos ? payload : payload.substr(0, comma);
            snapshot.initial_class = snapshot.klass;
            snapshot.title = comma == std::string::npos
                                 ? std::string()
                                 : payload.substr(comma + 1);
            snapshot.initial_title = snapshot.title;
            snapshot.app_id = snapshot.klass;
            update_hypr_window_snapshot(snapshot);
          }
        }
      }
    }

    ::close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}

void start_hyprland_focus_cache_listener() {
  static bool started = false;
  if (started) {
    return;
  }
  started = true;
  if (!g_wayland_uses_hypr_sendshortcut) {
    return;
  }
  std::thread([]() { hyprland_socket2_event_loop(); }).detach();
  modore_log("ipc", "Hyprland focus cache listener active");
}

std::string current_focused_app_id() {
  HyprWindowSnapshot snapshot{};
  if (!copy_hypr_window_snapshot(&snapshot)) {
    return {};
  }
  return snapshot.app_id;
}

bool hypr_focus_snapshots_match(const HyprWindowSnapshot &a,
                                const HyprWindowSnapshot &b) {
  return a.klass == b.klass && a.initial_class == b.initial_class &&
         a.app_id == b.app_id && a.xwayland == b.xwayland;
}

bool focused_window_looks_like_terminal() {
  HyprWindowSnapshot snapshot{};
  if (!copy_hypr_window_snapshot(&snapshot)) {
    return false;
  }
  std::string id = lower_ascii_copy(snapshot.klass);
  std::string initial = lower_ascii_copy(snapshot.initial_class);
  std::string title = lower_ascii_copy(snapshot.title);
  const char *needles[] = {
      "kitty",   "alacritty", "wezterm",    "foot",           "xterm",
      "konsole", "ghostty",   "terminator", "tilix",          "terminal",
      "st",      "qterminal", "urxvt",      "xfce4-terminal", "gnome-terminal",
      "rio"};
  for (const char *n : needles) {
    if ((!id.empty() && id.find(n) != std::string::npos) ||
        (!initial.empty() && initial.find(n) != std::string::npos) ||
        (!title.empty() && title.find(n) != std::string::npos)) {
      return true;
    }
  }
  return false;
}

bool focused_window_looks_like_discord() {
  HyprWindowSnapshot snapshot{};
  if (!copy_hypr_window_snapshot(&snapshot)) {
    return false;
  }
  std::string id = lower_ascii_copy(snapshot.klass);
  std::string initial = lower_ascii_copy(snapshot.initial_class);
  std::string title = lower_ascii_copy(snapshot.title);
  if ((!id.empty() && id.find("discord") != std::string::npos) ||
      (!initial.empty() && initial.find("discord") != std::string::npos) ||
      (!title.empty() && title.find("discord") != std::string::npos)) {
    return true;
  }
  return false;
}

bool focused_window_looks_like_chromium_or_chrome() {
  HyprWindowSnapshot snapshot{};
  if (!copy_hypr_window_snapshot(&snapshot)) {
    return false;
  }
  std::string id = lower_ascii_copy(snapshot.klass);
  std::string initial = lower_ascii_copy(snapshot.initial_class);
  std::string title = lower_ascii_copy(snapshot.title);
  if ((!id.empty() && (id.find("chromium") != std::string::npos ||
                       id.find("chrome") != std::string::npos)) ||
      (!initial.empty() && (initial.find("chromium") != std::string::npos ||
                            initial.find("chrome") != std::string::npos)) ||
      (!title.empty() && (title.find("chromium") != std::string::npos ||
                          title.find("chrome") != std::string::npos))) {
    return true;
  }
  return false;
}

bool hyprctl_ipc_alive_for_wayland_keys() {
  static int cached = -1;
  if (cached >= 0) {
    return cached == 1;
  }
  cached = 0;
  const char *wl = std::getenv("WAYLAND_DISPLAY");
  if (!wl || !wl[0]) {
    return false;
  }
  const char *sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
  if (sig && sig[0]) {
    const char *hc = resolve_hyprctl_executable();
    if (!hc) {
      return false;
    }
    cached = 1;
    return true;
  }
  const char *hc = resolve_hyprctl_executable();
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

// DISPLAY is usually set alongside WAYLAND_DISPLAY on Hypr/UWSM sessions.
// Passing a live X11 Display* to do_pickup() chooses XTest for synthetic keys,
// but XTest does not reach native Wayland clients — so conversion can run yet
// nothing appears in Kitty/GTK/etc. Hypr exposes xwayland on activewindow. On
// ambiguity (query failure / old Hypr output), assume native-Wayland and skip
// X11 injection.
bool hypr_focus_is_wayland_native() {
  std::string json;
  if (!hyprctl_query_activewindow_json(&json)) {
    return true;
  }
  if (json.find("\"xwayland\": true") != std::string::npos ||
      json.find("\"xwayland\":true") != std::string::npos) {
    return false;
  }
  return true;
}

bool hyprctl_dispatch_sendkeystate(const char *keystate_spec,
                                   const char *desc_for_log, bool log_failure) {
  const char *hc = resolve_hyprctl_executable();
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
    execl(hc, "hyprctl", "dispatch", "sendkeystate", keystate_spec, nullptr);
    _exit(127);
  }
  int st = 0;
  (void)::waitpid(pid, &st, 0);
  const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  if (!ok && log_failure) {
    const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    logf("hyprctl sendkeystate failed exit=%d (%s)", code, desc_for_log);
  }
  return ok;
}

bool hyprctl_dispatch_keystate_tap(const char *mod_spec, const char *key_spec,
                                   const char *desc_for_log, bool log_failure) {
  std::string down;
  std::string up;
  if (mod_spec && *mod_spec) {
    down = mod_spec;
  }
  down.push_back(',');
  down += key_spec;
  down += ",down,";
  if (mod_spec && *mod_spec) {
    up = mod_spec;
  }
  up.push_back(',');
  up += key_spec;
  up += ",up,";
  return hyprctl_dispatch_sendkeystate(down.c_str(), desc_for_log,
                                       log_failure) &&
         hyprctl_dispatch_sendkeystate(up.c_str(), desc_for_log, log_failure);
}

std::string hyprland_bind_mods_for_mask(unsigned int mask) {
  std::string mods;
  auto append_mod = [&](const char *token) {
    if (!mods.empty()) {
      mods.push_back('_');
    }
    mods += token;
  };
  if (mask & ControlMask) {
    append_mod("CTRL");
  }
  if (mask & ShiftMask) {
    append_mod("SHIFT");
  }
  if (mask & Mod1Mask) {
    append_mod("ALT");
  }
  if (mask & Mod4Mask) {
    append_mod("SUPER");
  }
  return mods;
}

std::string hyprland_bind_key_for_keysym(KeySym ks) {
  const char *label = XKeysymToString(ks);
  if (label && *label) {
    return label;
  }
  return {};
}

std::string hyprland_hotkey_combo(const X11HotkeySpec &hotkey) {
  const std::string mods = hyprland_bind_mods_for_mask(hotkey.modifier_mask);
  const std::string key =
      hyprland_bind_key_for_keysym(static_cast<KeySym>(hotkey.keysym));
  if (key.empty()) {
    return {};
  }
  if (mods.empty()) {
    return "," + key;
  }
  return mods + "," + key;
}

std::string hyprland_bind_state_path() {
  return default_profile_dir() + "/hyprland-bind";
}

std::string read_text_file_trimmed(const std::string &path) {
  std::ifstream f(path);
  if (!f) {
    return {};
  }
  std::string line;
  std::getline(f, line);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  return line;
}

bool write_text_file(const std::string &path, const std::string &text) {
  const std::string tmp = path + ".tmp";
  {
    std::ofstream f(tmp, std::ios::trunc);
    if (!f) {
      return false;
    }
    f << text << '\n';
    if (!f) {
      return false;
    }
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

std::string resolve_self_executable_path(const char *argv0) {
  if (argv0 && argv0[0]) {
    char resolved[4096];
    if (::realpath(argv0, resolved)) {
      return resolved;
    }
  }
  char proc[4096];
  const ssize_t n = ::readlink("/proc/self/exe", proc, sizeof(proc) - 1);
  if (n > 0) {
    proc[n] = '\0';
    return proc;
  }
  return argv0 && argv0[0] ? argv0 : "modore-host";
}

bool hyprctl_keyword_value(const char *keyword, const std::string &value,
                           const char *log_ctx) {
  const char *hc = resolve_hyprctl_executable();
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
    execl(hc, "hyprctl", "keyword", keyword, value.c_str(), nullptr);
    _exit(127);
  }
  int st = 0;
  (void)::waitpid(pid, &st, 0);
  const bool ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
  if (!ok) {
    const int code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    modore_log("hotkey", "hyprctl keyword %s failed exit=%d (%s)", keyword,
               code, log_ctx);
  }
  return ok;
}

bool register_hyprland_hotkey_bind(const std::string &host_path,
                                   const X11HotkeySpec &hotkey,
                                   const std::string &description) {
  const std::string combo = hyprland_hotkey_combo(hotkey);
  if (combo.empty()) {
    modore_log(
        "hotkey",
        "Hyprland bind skipped: could not map hotkey to a compositor combo");
    return false;
  }

  const std::string exec_cmd = host_path + " --trigger";
  const std::string bind_value = combo + ",exec," + exec_cmd;
  const std::string state_path = hyprland_bind_state_path();
  const std::string prev_combo = read_text_file_trimmed(state_path);
  if (!prev_combo.empty() && prev_combo != combo) {
    (void)hyprctl_keyword_value("unbind", prev_combo, "previous hotkey");
  }

  const std::string bind_ctx = description.empty()
                                   ? std::string("modore pickup")
                                   : description + " (" + combo + ")";
  if (!hyprctl_keyword_value("bind", bind_value, bind_ctx.c_str())) {
    return false;
  }
  if (!write_text_file(state_path, combo)) {
    modore_log("hotkey",
               "Hyprland bind active but state file write failed (%s)",
               state_path.c_str());
  }
  modore_log("hotkey", "Hyprland bind active (%s -> %s)", combo.c_str(),
             exec_cmd.c_str());
  return true;
}

bool wayland_send_ctrl_c() {
  if (g_wayland_uses_hypr_sendshortcut) {
    return hyprctl_dispatch_keystate_tap("CTRL", "C", "Ctrl+C copy");
  }
  if (wtype_is_available()) {
    if (wtype_chord_ctrl_insert_copy()) {
      return true;
    }
    return wtype_chord_ctrl_c();
  }
  return false;
}

bool wayland_send_ctrl_shift_left() {
  if (g_wayland_uses_hypr_sendshortcut) {
    return hyprctl_dispatch_keystate_tap("CTRL SHIFT", "Left",
                                         "Ctrl+Shift+Left");
  }
  return wtype_is_available() ? wtype_chord_ctrl_shift_left() : false;
}

bool wayland_send_select_all() {
  if (g_wayland_uses_hypr_sendshortcut) {
    return hyprctl_dispatch_keystate_tap("CTRL", "A", "Select all");
  }
  return wtype_is_available() ? wtype_chord_ctrl_a() : false;
}

bool wayland_send_select_line_home() {
  if (g_wayland_uses_hypr_sendshortcut) {
    return hyprctl_dispatch_keystate_tap("SHIFT", "Home", "Shift+Home");
  }
  return wtype_is_available() ? wtype_chord_shift_home() : false;
}

bool wayland_send_select_word_left() {
  if (g_wayland_uses_hypr_sendshortcut) {
    return hyprctl_dispatch_keystate_tap("CTRL SHIFT", "Left",
                                         "Ctrl+Shift+Left");
  }
  return wtype_is_available() ? wtype_chord_ctrl_shift_left() : false;
}

bool wayland_send_shift_left() {
  if (g_wayland_uses_hypr_sendshortcut) {
    return hyprctl_dispatch_keystate_tap("SHIFT", "Left", "Shift+Left");
  }
  return wtype_is_available()
             ? wtype_exec_chord("Shift+Left",
                                {"-s", "40", "-M", "shift", "-k", "Left"})
             : false;
}

bool wayland_send_delete_or_backspace() {
  if (g_wayland_uses_hypr_sendshortcut) {
    return hyprctl_dispatch_keystate_tap("", "Delete", "Delete") ||
           hyprctl_dispatch_keystate_tap("", "BackSpace", "BackSpace");
  }
  if (wtype_is_available()) {
    return wtype_key_delete_or_backspace();
  }
  return false;
}

bool wayland_send_backspace_only() {
  if (g_wayland_uses_hypr_sendshortcut) {
    return hyprctl_dispatch_keystate_tap("", "BackSpace", "BackSpace");
  }
  if (wtype_is_available()) {
    return wtype_exec_chord("BackSpace", {"-k", "BackSpace"});
  }
  return false;
}

bool wayland_send_paste_chord() {
  const bool prefer_ctrl_v = focused_window_looks_like_chromium_or_chrome();
  modore_log("wayland", "paste chord prefer_ctrl_v=%d hypr=%d wtype=%d",
             prefer_ctrl_v ? 1 : 0, g_wayland_uses_hypr_sendshortcut ? 1 : 0,
             wtype_is_available() ? 1 : 0);
  if (g_wayland_uses_hypr_sendshortcut) {
    if (prefer_ctrl_v) {
      modore_log("wayland", "paste chord hypr branch trying Ctrl+V");
      if (hyprctl_dispatch_keystate_tap("CTRL", "V", "Ctrl+V paste")) {
        return true;
      }
      modore_log("wayland",
                 "paste chord hypr Ctrl+V failed, trying Shift+Insert");
      return hyprctl_dispatch_keystate_tap("SHIFT", "Insert",
                                           "Shift+Insert paste");
    }
    modore_log("wayland", "paste chord hypr branch trying Shift+Insert");
    if (hyprctl_dispatch_keystate_tap("SHIFT", "Insert",
                                      "Shift+Insert paste")) {
      return true;
    }
    modore_log("wayland",
               "paste chord hypr Shift+Insert failed, trying Ctrl+V");
    return hyprctl_dispatch_keystate_tap("CTRL", "V", "Ctrl+V paste");
  }
  if (wtype_is_available()) {
    if (prefer_ctrl_v) {
      modore_log("wayland", "paste chord wtype branch trying Ctrl+V");
      if (wtype_chord_ctrl_v()) {
        return true;
      }
      modore_log("wayland",
                 "paste chord wtype Ctrl+V failed, trying Shift+Insert");
      return wtype_chord_shift_insert();
    }
    modore_log("wayland", "paste chord wtype branch trying Shift+Insert");
    if (wtype_chord_shift_insert()) {
      return true;
    }
    modore_log("wayland",
               "paste chord wtype Shift+Insert failed, trying Ctrl+V");
    return wtype_chord_ctrl_v();
  }
  return false;
}

bool inject_utf8_subprocess_wtype(const std::string &utf8) {
  const char *wt = resolve_wtype_executable();
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

bool inject_utf8_wayland_fallback(const std::string &utf8) {
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
    logf("ydotool type: subprocess exit=%d (%zu UTF-8 bytes)", code,
         utf8.size());
  }
  return ok;
}

} // namespace modore_host
