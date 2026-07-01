// host_internal.hpp — shared internals for the modore Linux host.
//
// main.cpp was split into focused translation units (x11_input,
// wayland_hypr, clipboard, mozc_session, pickup, ...). They all live in
// namespace modore_host and share this header: the common includes, the
// few cross-cutting structs, the global host state (declared extern here,
// defined once in host_state.cpp), and every cross-file prototype.

#pragma once

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <atspi/atspi.h>
#include <glib.h>
#include <mozc_bridge.h>

#include "config.hpp"
#include "evdev_hotkey.hpp"
#include "ipc.hpp"
#include "log.hpp"
#include "scripting.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <fstream>
#include <functional>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace modore_host {

// Subsystem tag for the logf() macro (see bottom of this header).
extern thread_local const char *g_log_scope_tag;

class ScopedLogTag {
public:
  explicit ScopedLogTag(const char *tag) : prev_(g_log_scope_tag) {
    g_log_scope_tag = tag ? tag : "host";
  }
  ~ScopedLogTag() { g_log_scope_tag = prev_; }

  ScopedLogTag(const ScopedLogTag &) = delete;
  ScopedLogTag &operator=(const ScopedLogTag &) = delete;

private:
  const char *prev_;
};

struct HyprWindowSnapshot {
  std::string klass;
  std::string initial_class;
  std::string initial_title;
  std::string app_id;
  std::string title;
  bool xwayland = false;
};

struct CachedAtspiFocus {
  std::mutex mu;
  AtspiAccessible *focus = nullptr;

  ~CachedAtspiFocus() {
    if (focus) {
      g_object_unref(focus);
      focus = nullptr;
    }
  }

  void update(AtspiAccessible *next) {
    std::lock_guard<std::mutex> lock(mu);
    if (focus) {
      g_object_unref(focus);
      focus = nullptr;
    }
    if (next) {
      focus = ATSPI_ACCESSIBLE(g_object_ref(next));
    }
  }

  AtspiAccessible *take_ref() {
    std::lock_guard<std::mutex> lock(mu);
    if (!focus) {
      return nullptr;
    }
    return ATSPI_ACCESSIBLE(g_object_ref(focus));
  }
};

struct LinuxClipboardTimingConfig {
  std::atomic<int> pre_paste_delay_ms{30};
  std::atomic<int> paste_visibility_wait_ms{6};
  std::atomic<int> paste_visibility_step_ms{1};
  std::atomic<int> short_restore_delay_ms{20};
  std::atomic<int> long_restore_delay_ms{300};
  std::atomic<int> cycle_settle_delay_ms{20};
  std::atomic<int> cycle_post_inject_delay_ms{2};
  std::atomic<int> cycle_backspace_step_ms{12};
  std::atomic<int> pickup_start_delay_ms{1};
  std::atomic<int> atspi_direct_settle_delay_ms{2};
  std::atomic<int> atspi_replacement_settle_delay_ms{3};
  std::atomic<int> clear_poll_max_wait_ms{40};
  std::atomic<int> clear_poll_step_ms{1};
  std::atomic<int> wayland_select_settle_ms{28};
  std::atomic<int> wayland_copy_poll_ms{72};
  std::atomic<int> wayland_copy_poll_step_ms{2};
};

struct ConversionSession {
  enum class Backing {
    AtspiEditable,
    ClipboardFallback,
  };

  Backing backing = Backing::ClipboardFallback;
  AtspiAccessible *focus = nullptr;
  std::string app_id;
  glong span_start = 0;
  glong span_end = 0;
  glong current_text_chars = 0;
  std::string current_text;
  std::string source_romaji; // atzc: original reading for background prefetch
  std::vector<std::string> candidates;
  int candidate_index = 0;
  std::chrono::steady_clock::time_point last_touch{};
};

enum class WaylandAcquireFlow {
  Generic,
  ChromeLike,
  DiscordLike,
  TerminalLike,
};

// ---- shared host state (defined in host_state.cpp) ----
extern std::mutex g_hypr_window_mu;
extern HyprWindowSnapshot g_hypr_window_snapshot;
extern bool g_hypr_window_snapshot_valid;
extern CachedAtspiFocus g_cached_atspi_focus;
extern std::mutex g_pickup_mu;
extern bool g_wayland_uses_hypr_sendshortcut;
extern unsigned int g_conversion_hotkey_modifier_mask;
extern std::uint64_t g_conversion_hotkey_keysym;
extern LinuxClipboardTimingConfig g_clipboard_timings;
extern int g_pickup_pipe[2];
extern int g_x11_setup_error;
extern std::string g_wtype_path;
extern std::string g_hyprctl_path;
extern std::mutex g_conversion_session_mu;
extern bool g_has_conversion_session;
extern ConversionSession g_conversion_session;
extern std::atomic<bool> g_mozc_backend_is_atzc;

inline bool mozc_uses_atzc_backend() {
  return g_mozc_backend_is_atzc.load(std::memory_order_relaxed);
}

// ---- cross-file function prototypes ----
void nap_after_compose_event(std::chrono::milliseconds d);
void update_hypr_window_snapshot(const HyprWindowSnapshot &snapshot);
bool copy_hypr_window_snapshot(HyprWindowSnapshot *out);
std::string utf8_preview(const std::string &text, size_t max_chars = 96);
void log_text_preview(const char *label, const std::string &text);
void yield_to_compose_pipeline();
unsigned int x11_current_modifier_mask(Display *d);
unsigned int active_trigger_modifier_mask(Display *d);
bool trigger_ctrl_is_held(Display *d);
int clipboard_timing_ms(const std::atomic<int> &value);
void apply_linux_config(const ModoreConfig &cfg);
void log_linux_config_timings();
void reload_config_from_disk(bool initial_load);
void start_config_reload_watcher();
bool setup_pickup_pipe();
void notify_main_pickup_pending();
void main_thread_run_pickup_after_wake();
void main_thread_run_pipe_only_loop();
int x11_quiet_error_handler(Display *, XErrorEvent *);
std::string getenv_string(const char *k, const char *def);
std::string default_profile_dir();
bool command_ok(const char *cmd);
void augment_path_for_subprocesses();
const char *resolve_wtype_executable();
bool wtype_is_available();
bool wtype_exec_chord(const char *desc_for_log,
                      const std::vector<const char *> &args);
bool wtype_chord_shift_insert();
bool wtype_chord_ctrl_v();
bool wtype_chord_ctrl_c();
bool wtype_chord_ctrl_insert_copy();
bool wtype_chord_ctrl_shift_left();
bool wtype_chord_ctrl_a();
bool wtype_chord_shift_home();
bool wtype_key_right();
bool wtype_key_delete_or_backspace();
const char *resolve_hyprctl_executable();
bool fork_hyprctl_version_ok(const char *hc_path);
bool hyprctl_query_activewindow_json(std::string *json);
bool json_string_field(const std::string &json, const char *key,
                       std::string *out);
bool json_bool_field(const std::string &json, const char *key, bool *out);
bool hyprctl_query_activewindow_snapshot(HyprWindowSnapshot *snapshot);
void log_hyprland_activewindow_snapshot(const char *context);
std::string hyprland_socket2_path();
bool set_sockaddr_un_path(const std::string &path, sockaddr_un *addr);
bool hyprland_refresh_activewindow_snapshot_from_ipc();
void hyprland_socket2_event_loop();
void start_hyprland_focus_cache_listener();
std::string current_focused_app_id();
bool hypr_focus_snapshots_match(const HyprWindowSnapshot &a,
                                const HyprWindowSnapshot &b);
std::string lower_ascii_copy(std::string s);
bool focused_window_looks_like_terminal();
bool focused_window_looks_like_discord();
bool focused_window_looks_like_chromium_or_chrome();
bool hyprctl_ipc_alive_for_wayland_keys();
bool hypr_focus_is_wayland_native();
bool hyprctl_dispatch_sendkeystate(const char *keystate_spec,
                                   const char *desc_for_log,
                                   bool log_failure = true);
bool hyprctl_dispatch_keystate_tap(const char *mod_spec, const char *key_spec,
                                   const char *desc_for_log,
                                   bool log_failure = true);
std::string hyprland_bind_mods_for_mask(unsigned int mask);
std::string hyprland_bind_key_for_keysym(KeySym ks);
std::string hyprland_hotkey_combo(const X11HotkeySpec &hotkey);
std::string hyprland_bind_state_path();
std::string read_text_file_trimmed(const std::string &path);
bool write_text_file(const std::string &path, const std::string &text);
std::string resolve_self_executable_path(const char *argv0);
bool hyprctl_keyword_value(const char *keyword, const std::string &value,
                           const char *log_ctx);
bool register_hyprland_hotkey_bind(const std::string &host_path,
                                   const X11HotkeySpec &hotkey,
                                   const std::string &description);
bool wayland_send_ctrl_c();
bool wayland_send_ctrl_shift_left();
bool wayland_send_select_all();
bool wayland_send_select_line_home();
bool wayland_send_select_word_left();
bool wayland_send_shift_left();
bool wayland_send_delete_or_backspace();
bool wayland_send_backspace_only();
bool wayland_send_paste_chord();
bool read_clipboard_cmd(const char *cmd, std::string *out);
const char *resolve_wl_paste();
const char *resolve_wl_copy();
bool wl_clipboard_available();
void trim_trailing_crlf_inplace(std::string *s);
bool clipboard_normalized_equal(const std::string &a, const std::string &b);
std::string wl_pick_text_mime(const char *primary_flag);
bool read_wl_offer_text_only(const char *primary_flag, std::string *out);
bool read_wl_clip_offer(std::string *out);
bool read_wl_primary_offer(std::string *out);
bool read_clipboard(std::string *out);
bool write_clipboard(const std::string &s);
bool wl_clipboard_trimmed_empty(const std::string &c);
bool poll_wl_clipboard_cleared(int max_wait_ms, int step_ms);
bool wait_wl_clipboard_equals_normalized(const std::string &expected,
                                         int max_wait_ms, int step_ms);
bool mozc_convert_utf8(const std::string &romaji, std::string *replacement);
std::optional<std::pair<std::string, std::vector<std::string>>>
mozc_convert_utf8_with_candidates(const std::string &romaji);
void mozc_prefetch_candidates_async(const std::string &romaji,
                                    const std::string &committed);
void clear_conversion_session_locked();
std::vector<std::string>
normalize_candidate_session_state(const std::string &replacement,
                                  std::vector<std::string> candidates,
                                  int *current_index);
void invalidate_conversion_session_for_user_input();
void set_conversion_session(ConversionSession session);
bool conversion_session_available_locked(
    std::chrono::steady_clock::time_point now);
bool hotkey_can_leak_text(std::uint64_t keysym);
glong clipboard_cycle_residue_chars();
void fake_x11_backspace_glyph_count(Display *d, glong glyphs);
void fake_backspace_glyph_count(Display *d, glong glyphs);
void word_range_chars(const gchar *text, glong caret_chars, glong n_chars,
                      glong *start, glong *end);
void utf8_substr_bytes(const gchar *text, glong start_c, glong end_c,
                       std::string *out);
void fake_ctrl_letter(Display *d, KeySym letter);
void fake_ctrl_shift_left(Display *d);
void child_clear_im_modules();
bool inject_utf8_subprocess_wtype(const std::string &utf8);
bool inject_utf8_wayland_fallback(const std::string &utf8);
void fake_ctrl_c_best(Display *d);
void fake_ctrl_shift_left_best(Display *d);
bool looks_like_line_copy(const std::string &s);
void trim_in_place_ascii(std::string *s);
bool maybe_narrow_path_primary_pick(std::string *picked);
bool clipboard_pick_probably_not_romaji_field(const std::string &s);
const char *
clipboard_first_matching_modifier_or_ui_hint_needle_ci(const std::string &s);
bool wl_primary_dominated_by_block_elements(const std::string &s);
bool wl_primary_looks_like_stale_global_chrome(const std::string &s);
bool wl_primary_is_utf8_bounded_ascii_only_fast_pick(const std::string &s);
bool clipboard_pick_probably_ide_ui_hint(const std::string &s);
bool clipboard_first_reasonable_line(const std::string &raw,
                                     std::string *picked);
bool wl_try_primary_as_highlighted_span(const std::string &baseline_primary,
                                        const std::string &clip_text,
                                        std::string *picked);
AtspiAccessible *find_focused_leaf(AtspiAccessible *obj, int depth);
AtspiAccessible *find_text_with_focus_or_active(AtspiAccessible *obj,
                                                int depth);
bool atspi_focus_event_is_gaining(const AtspiEvent *event);
void atspi_focus_cache_event_cb(const AtspiEvent *event);
void start_atspi_focus_cache_listener();
bool try_pickup_atspi(bool *direct_done, std::string *inject_utf8,
                      std::string *pick_span_for_inject);
bool try_cycle_active_conversion(Display *d);
bool inject_via_atspi_string(const std::string &utf8);
bool utf8_contains_non_ascii(const std::string &utf8);
bool pick_is_plain_ascii_romaji(const std::string &pick);
bool leading_ascii_romaji_token_prefix(const std::string &s,
                                       std::string *token);
bool trim_pick_leading_romaji_if_utf8_contaminated(std::string *picked);
size_t omniboz_earliest_url_like_marker_ci(std::string *low_ascii_out,
                                           const std::string &raw);
bool maybe_narrow_omnibox_url_contaminated_pick(std::string *picked);
bool pick_looks_like_mojibake_garbage(const std::string &pick);
bool mojibake_recovery_aggressive_enabled();
bool hypr_wayland_try_select_all(const char *log_ctx);
bool hypr_wayland_try_select_line_home(const char *log_ctx);
bool hypr_wayland_try_select_word_left(const char *log_ctx);
bool wayland_select_for_acquire(bool discord_like, const char *log_ctx);
bool pickup_focus_still_current(const char *phase);
WaylandAcquireFlow classify_wayland_acquire_flow();
const char *flow_name(WaylandAcquireFlow flow);
bool wayland_acquire_once_for_flow(WaylandAcquireFlow flow, Display *d,
                                   const std::string &baseline_clip,
                                   const std::string &baseline_primary,
                                   std::string *picked_out);
void notify_corrupted_pick_needs_recovery();
bool hypr_attempt_clear_focused_edit_field_best_effort(
    const char *log_ctx_note);
bool inject_utf8_via_wl_clipboard_paste(const std::string &utf8);
void fake_wayland_backspace_glyph_count(glong glyphs);
void inject_replacement_clear_then_type(
    Display *d, const std::string &utf8,
    const std::string *wayland_clipboard_pick_utf8,
    bool force_ctrl_a_ignore_glyph_env = false);
void wayland_interpret_after_copy(
    const std::string &baseline_clip, const std::string &baseline_primary,
    const char *attempt_label, std::string *after, bool *got_fresh,
    bool *clipboard_offer_unchanged /* may be null */);
void wl_poll_until_clip_or_primary_moves(const std::string &ref_clip,
                                         const std::string &ref_primary,
                                         int max_wait_ms);
void wayland_poll_after_copy(const std::string &baseline_clip,
                             const std::string &baseline_primary,
                             const char *attempt_label, std::string *after,
                             bool *got_fresh, bool *clipboard_offer_unchanged,
                             int max_wait_ms);
void do_clipboard_pickup(Display *d, const std::string &clip_saved,
                         bool clipboard_clear_attempted_on_wl);
void snapshot_clip_for_restore(std::string *clip_saved);
void do_pickup(Display *d);
void run_ipc_pickup();

// Defined after the prototypes their inline methods call.

struct PickupFocusWatch {
  bool armed = false;
  const char *scope = "pickup";
  HyprWindowSnapshot start{};

  void arm(const char *scope_name) {
    scope = scope_name ? scope_name : "pickup";
    armed = false;
    if (!g_wayland_uses_hypr_sendshortcut) {
      return;
    }
    if (!copy_hypr_window_snapshot(&start)) {
      if (!hyprctl_query_activewindow_snapshot(&start)) {
        modore_log("ipc", "%s focus watch could not snapshot Hypr activewindow",
                   scope);
        return;
      }
      update_hypr_window_snapshot(start);
    }
    armed = true;
    modore_log(
        "ipc",
        "%s focus watch armed: class=%s initialClass=%s title=%s "
        "initialTitle=%s xwayland=%s",
        scope, start.klass.empty() ? "(unset)" : start.klass.c_str(),
        start.initial_class.empty() ? "(unset)" : start.initial_class.c_str(),
        start.title.empty() ? "(unset)" : start.title.c_str(),
        start.initial_title.empty() ? "(unset)" : start.initial_title.c_str(),
        start.xwayland ? "yes" : "no");
  }

  bool still_current(const char *phase) {
    (void)phase;
    if (armed) {
      modore_log("pickup",
                 "%s focus guard temporarily disabled for pickup path", scope);
      armed = false;
    }
    return true;
  }
};
extern thread_local PickupFocusWatch g_pickup_focus_watch;

struct PickupActionQueue {
  struct Entry {
    std::string label;
    std::function<bool()> run;
  };

  std::vector<Entry> entries;

  void push(std::string label, std::function<bool()> run) {
    entries.push_back(Entry{std::move(label), std::move(run)});
  }

  bool consume(const char *scope) {
    for (auto &entry : entries) {
      if (!entry.run()) {
        modore_log("pickup", "%s action failed: %s", scope ? scope : "pickup",
                   entry.label.c_str());
        return false;
      }
    }
    return true;
  }
};

} // namespace modore_host

// logf(): subsystem-tagged log line. Defined after the includes so it
// shadows <cmath>'s logf, exactly as the original main.cpp did.

#define logf(...) modore_log(g_log_scope_tag, __VA_ARGS__)
