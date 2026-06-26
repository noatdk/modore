// clipboard_pickup.cpp — Wayland acquire-flow, replacement injection,
// and the clipboard-fallback pickup path (do_clipboard_pickup).

#include "host_internal.hpp"

namespace modore_host {

bool hypr_wayland_try_select_all(const char *log_ctx) {
  (void)log_ctx;
  return wayland_send_select_all();
}

bool hypr_wayland_try_select_line_home(const char *log_ctx) {
  (void)log_ctx;
  return wayland_send_select_line_home();
}

bool hypr_wayland_try_select_word_left(const char *log_ctx) {
  (void)log_ctx;
  return wayland_send_select_word_left();
}

bool wayland_select_for_acquire(bool discord_like, const char *log_ctx) {
  if (discord_like) {
    (void)log_ctx;
    return wayland_send_select_line_home();
  }
  (void)log_ctx;
  return wayland_send_ctrl_shift_left();
}

bool pickup_focus_still_current(const char *phase) {
  return g_pickup_focus_watch.still_current(phase);
}

WaylandAcquireFlow classify_wayland_acquire_flow() {
  HyprWindowSnapshot snapshot{};
  if (!copy_hypr_window_snapshot(&snapshot)) {
    return WaylandAcquireFlow::Generic;
  }
  std::string id = lower_ascii_copy(snapshot.klass);
  std::string initial = lower_ascii_copy(snapshot.initial_class);
  std::string title = lower_ascii_copy(snapshot.title);
  modore_log(
      "ipc",
      "wayland flow classify snapshot: class=%s initialClass=%s title=%s "
      "initialTitle=%s xwayland=%s",
      snapshot.klass.empty() ? "(unset)" : snapshot.klass.c_str(),
      snapshot.initial_class.empty() ? "(unset)"
                                     : snapshot.initial_class.c_str(),
      snapshot.title.empty() ? "(unset)" : snapshot.title.c_str(),
      snapshot.initial_title.empty() ? "(unset)"
                                     : snapshot.initial_title.c_str(),
      snapshot.xwayland ? "yes" : "no");
  if ((!id.empty() && (id.find("chromium") != std::string::npos ||
                       id.find("chrome") != std::string::npos)) ||
      (!initial.empty() && (initial.find("chromium") != std::string::npos ||
                            initial.find("chrome") != std::string::npos)) ||
      (!title.empty() && (title.find("chromium") != std::string::npos ||
                          title.find("chrome") != std::string::npos))) {
    return WaylandAcquireFlow::ChromeLike;
  }
  if (focused_window_looks_like_discord()) {
    return WaylandAcquireFlow::DiscordLike;
  }
  if (focused_window_looks_like_terminal()) {
    return WaylandAcquireFlow::TerminalLike;
  }
  return WaylandAcquireFlow::Generic;
}

const char *flow_name(WaylandAcquireFlow flow) {
  switch (flow) {
  case WaylandAcquireFlow::ChromeLike:
    return "chrome-like";
  case WaylandAcquireFlow::DiscordLike:
    return "discord-like";
  case WaylandAcquireFlow::TerminalLike:
    return "terminal-like";
  default:
    return "generic";
  }
}

bool wayland_acquire_once_for_flow(WaylandAcquireFlow flow, Display *d,
                                   const std::string &baseline_clip,
                                   const std::string &baseline_primary,
                                   std::string *picked_out) {
  if (!picked_out) {
    return false;
  }
  picked_out->clear();
  if (!wl_clipboard_available()) {
    return false;
  }

  PickupActionQueue queue;
  switch (flow) {
  case WaylandAcquireFlow::ChromeLike:
    logf("pick: chrome-like flow — Ctrl+Shift+Left then copy");
    queue.push("chrome-like select", [&] {
      return hypr_wayland_try_select_word_left("Chrome acquire select");
    });
    break;
  case WaylandAcquireFlow::DiscordLike:
    logf("clipboard: discord-like flow — Shift+Home then copy");
    queue.push("discord-like select", [&] {
      return hypr_wayland_try_select_line_home("Discord acquire select");
    });
    break;
  case WaylandAcquireFlow::TerminalLike:
    logf("clipboard: terminal-like flow — Shift+Home then copy");
    queue.push("terminal-like select", [&] {
      return hypr_wayland_try_select_line_home("Terminal acquire select");
    });
    break;
  default:
    return false;
  }
  queue.push("focused-app copy", [&] {
    fake_ctrl_c_best(d);
    return true;
  });

  const auto queue_started = std::chrono::steady_clock::now();
  if (!queue.consume(flow_name(flow))) {
    picked_out->clear();
    logf("clipboard: %s flow failed — stopping", flow_name(flow));
    return false;
  }
  logf("pick: %s flow select+copy queue finished in %lld ms", flow_name(flow),
       static_cast<long long>(
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - queue_started)
               .count()));
  const int kSelectSettleMs =
      clipboard_timing_ms(g_clipboard_timings.wayland_select_settle_ms);
  logf("pick: %s flow waiting up to %dms for selection to settle before copy",
       flow_name(flow), kSelectSettleMs);
  wl_poll_until_clip_or_primary_moves(baseline_clip, baseline_primary,
                                      kSelectSettleMs);
  logf("pick: %s flow selection settle wait complete", flow_name(flow));
  std::string after_clip;
  std::string after_primary;
  read_wl_clip_offer(&after_clip);
  read_wl_primary_offer(&after_primary);
  logf("pick: %s flow post-copy offers clip=%zu primary=%zu", flow_name(flow),
       after_clip.size(), after_primary.size());
  log_text_preview("pick clip", after_clip);
  log_text_preview("pick primary", after_primary);
  if (!wl_clipboard_trimmed_empty(after_clip)) {
    *picked_out = std::move(after_clip);
  } else {
    if (!after_primary.empty()) {
      logf("clipboard: %s flow using PRIMARY because CLIPBOARD was trimmed "
           "empty (%zu bytes)",
           flow_name(flow), after_primary.size());
    }
    *picked_out = std::move(after_primary);
  }
  if (picked_out->empty()) {
    logf("clipboard: %s flow did not yield a pick", flow_name(flow));
    return false;
  }
  return true;
}

void notify_corrupted_pick_needs_recovery() {
  if (!command_ok("command -v notify-send >/dev/null 2>&1")) {
    return;
  }
  (void)std::system("notify-send -u normal -a Modore -t 9000 'Modore'"
                    " 'Stale/corrupted text in the focused field — attempting "
                    "Ctrl+A + Backspace."
                    " If it looks wrong, clear the box yourself, then type "
                    "romaji again.' >/dev/null 2>&1 &");
}

bool hypr_attempt_clear_focused_edit_field_best_effort(
    const char *log_ctx_note) {
  if (!wl_clipboard_available() || !g_wayland_uses_hypr_sendshortcut) {
    return false;
  }
  const char *block = std::getenv("MODORE_NO_MOJIBAKE_RECOVERY");
  if (block && block[0]) {
    return false;
  }
  const bool terminal_like = focused_window_looks_like_terminal();
  const bool selected =
      terminal_like
          ? hypr_wayland_try_select_line_home(
                log_ctx_note ? log_ctx_note : "Modore Shift+Home stale field")
          : hypr_wayland_try_select_all(
                log_ctx_note ? log_ctx_note : "Modore Ctrl+A stale field");
  if (!selected) {
    return false;
  }
  if (wayland_send_delete_or_backspace()) {
    nap_after_compose_event(std::chrono::milliseconds(
        clipboard_timing_ms(g_clipboard_timings.cycle_post_inject_delay_ms)));
    logf("clipboard: cleared focused field attempt — Hypr %s + "
         "delete/backspace sequence",
         terminal_like ? "Shift+Home" : "Ctrl+A");
    return true;
  }
  return false;
}

bool inject_utf8_via_wl_clipboard_paste(const std::string &utf8) {
  MODORE_E2E_LOGF("inject_wl_paste: enter utf8_bytes=%zu", utf8.size());
  if (utf8.empty() || !wl_clipboard_available()) {
    MODORE_E2E_LOGF("inject_wl_paste: abort empty or no wl-clipboard");
    return false;
  }
  const auto write_started = std::chrono::steady_clock::now();
  MODORE_E2E_LOGF("inject_wl_paste: write_clipboard start");
  if (!write_clipboard(utf8)) {
    logf("inject: wl-copy failed (%zu UTF-8 bytes)", utf8.size());
    MODORE_E2E_LOGF("inject_wl_paste: write_clipboard failed");
    return false;
  }
  MODORE_E2E_LOGF("inject_wl_paste: wl-copy wrote payload in %lld ms",
                  static_cast<long long>(
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - write_started)
                          .count()));
  // Keep the clipboard hot path minimal: wait only if a compositor needs a beat
  // to expose the new offer.
  const int pre_paste_delay_ms =
      clipboard_timing_ms(g_clipboard_timings.pre_paste_delay_ms);
  MODORE_E2E_LOGF("inject_wl_paste: pre-paste delay=%dms", pre_paste_delay_ms);
  nap_after_compose_event(std::chrono::milliseconds(pre_paste_delay_ms));
  // Brief poll — offer is usually visible in one compositor frame; cap stays
  // low for responsiveness.
  const int inject_paste_wait_ms =
      clipboard_timing_ms(g_clipboard_timings.paste_visibility_wait_ms);
  const int inject_paste_step_ms =
      clipboard_timing_ms(g_clipboard_timings.paste_visibility_step_ms);
  if (!wait_wl_clipboard_equals_normalized(utf8, inject_paste_wait_ms,
                                           inject_paste_step_ms)) {
    logf("inject: wl-copy payload not yet visible to wl-paste after %dms — "
         "sending paste anyway",
         inject_paste_wait_ms);
  }
  // Paste: Hyprland sessions use sendkeystate; other Wayland sessions keep
  // the existing wtype-first path.
  bool ok = false;
  MODORE_E2E_LOGF("inject_wl_paste: backend order hypr=%d wtype=%d",
                  g_wayland_uses_hypr_sendshortcut ? 1 : 0,
                  wtype_is_available() ? 1 : 0);
  if (wayland_send_paste_chord()) {
    if (g_wayland_uses_hypr_sendshortcut) {
      logf("inject: wl-copy+hypr paste (%zu UTF-8 bytes)", utf8.size());
    } else {
      logf("inject: wl-copy+wtype paste (%zu UTF-8 bytes)", utf8.size());
    }
    ok = true;
  } else if (g_wayland_uses_hypr_sendshortcut && wtype_is_available()) {
    logf("inject: wl-copy OK but Hypr paste / wtype fallback failed");
  } else if (!g_wayland_uses_hypr_sendshortcut && wtype_is_available()) {
    logf("inject: wl-copy OK but wtype paste failed");
  }
  MODORE_E2E_LOGF("inject_wl_paste: paste chord sent ok=%d", ok ? 1 : 0);
  if (!ok && !g_wayland_uses_hypr_sendshortcut && !wtype_is_available()) {
    logf("inject: wl-copy ok — no Hypr IPC and no wtype for paste");
  }
  if (ok) {
    MODORE_E2E_LOGF("inject_wl_paste: post-paste settle delay=0ms");
  }
  MODORE_E2E_LOGF("inject_wl_paste: done ok=%d", ok ? 1 : 0);
  return ok;
}

// Erase the converted span using one BackSpace per Unicode scalar
// (fine for ASCII romaji).
void fake_wayland_backspace_glyph_count(glong glyphs) {
  constexpr glong kMax = 384;
  if (glyphs <= 0) {
    return;
  }
  if (glyphs > kMax) {
    logf("inject: clipping BackSpace repeats from %ld to %ld glyphs",
         static_cast<long>(glyphs), static_cast<long>(kMax));
    glyphs = kMax;
  }
  MODORE_E2E_LOGF("cycle: backspace loop start glyphs=%ld backend=%s",
                  static_cast<long>(glyphs),
                  g_wayland_uses_hypr_sendshortcut ? "hyprctl" : "wtype");
  const bool ctrl_held = trigger_ctrl_is_held(nullptr);
  if (ctrl_held) {
    MODORE_E2E_LOGF("cycle: ctrl held; using plain per-glyph BackSpace");
  }
  for (glong i = 0; i < glyphs; ++i) {
    const bool ok = wayland_send_backspace_only();
    if (!ok) {
      logf("inject: BackSpace stopped early at %ld / %ld (no backend)",
           static_cast<long>(i), static_cast<long>(glyphs));
      break;
    }
    MODORE_E2E_LOGF("cycle: backspace sent %ld/%ld", static_cast<long>(i + 1),
                    static_cast<long>(glyphs));
    nap_after_compose_event(std::chrono::milliseconds(
        clipboard_timing_ms(g_clipboard_timings.cycle_backspace_step_ms)));
    yield_to_compose_pipeline();
  }
}

void inject_replacement_clear_then_type(
    Display *d, const std::string &utf8,
    const std::string *wayland_clipboard_pick_utf8,
    bool force_ctrl_a_ignore_glyph_env) {
  ScopedLogTag log_scope("clipboard");
  MODORE_E2E_LOGF(
      "inject: enter out_bytes=%zu d=%s pick_ptr=%s force_ctrl_a=%d",
      utf8.size(), d ? "X11" : "null",
      wayland_clipboard_pick_utf8 ? "yes" : "no",
      force_ctrl_a_ignore_glyph_env ? 1 : 0);
  if (utf8.empty()) {
    logf("inject: empty replacement");
    return;
  }
  logf("inject payload utf8=%s", utf8.c_str());

  if (d) {
    MODORE_E2E_LOGF("inject: entering X11 clipboard paste path");
    if (!write_clipboard(utf8)) {
      logf("inject: wl-copy/xclip failed (%zu UTF-8 bytes)", utf8.size());
      return;
    }
    fake_ctrl_letter(d, XK_v);
    logf("inject: clipboard paste via XTest Ctrl+V (%zu UTF-8 bytes)",
         utf8.size());
    return;
  }

  MODORE_E2E_LOGF("inject: entering Wayland clipboard paste path");
  MODORE_E2E_LOGF("inject: Wayland clipboard route uses chord injection only");
  if (inject_utf8_via_wl_clipboard_paste(utf8)) {
    logf("inject: clipboard paste via Wayland clipboard path (%zu UTF-8 bytes)",
         utf8.size());
    return;
  }
  logf("insert failed (AT-SPI STRING, clipboard paste)");
}

// Interpret wl-paste after a synthetic copy. When the selection already matches
// the clipboard offer, Ctrl+C is a no-op and the offer stays identical to
// baseline — we still treat non-empty clipboard as a valid pick.
void wayland_interpret_after_copy(
    const std::string &baseline_clip, const std::string &baseline_primary,
    const char *attempt_label, std::string *after, bool *got_fresh,
    bool *clipboard_offer_unchanged /* may be null */) {
  *got_fresh = false;
  after->clear();
  if (clipboard_offer_unchanged) {
    *clipboard_offer_unchanged = false;
  }
  std::string after_clip;
  read_wl_clip_offer(&after_clip);
  if (!after_clip.empty() &&
      !clipboard_normalized_equal(after_clip, baseline_clip)) {
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
    logf("clipboard: %s pick from clipboard (unchanged vs baseline; copy "
         "likely no-op, %zu bytes)",
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
    logf("clipboard: %s Wayland primary changed vs baseline (%zu bytes)",
         attempt_label, after->size());
  }
}

// Wait for selection nudges (Ctrl+Shift+Left) to show up on CLIPBOARD /
// PRIMARY.
void wl_poll_until_clip_or_primary_moves(const std::string &ref_clip,
                                         const std::string &ref_primary,
                                         int max_wait_ms) {
  if (max_wait_ms <= 0) {
    return;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);
  const int kStep =
      clipboard_timing_ms(g_clipboard_timings.wayland_copy_poll_step_ms);
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

// Poll after Ctrl+C instead of a long fixed sleep — clients often need 20–120ms
// to publish.
void wayland_poll_after_copy(const std::string &baseline_clip,
                             const std::string &baseline_primary,
                             const char *attempt_label, std::string *after,
                             bool *got_fresh, bool *clipboard_offer_unchanged,
                             int max_wait_ms) {
  *got_fresh = false;
  after->clear();
  if (clipboard_offer_unchanged) {
    *clipboard_offer_unchanged = false;
  }
  const int kStep =
      clipboard_timing_ms(g_clipboard_timings.wayland_copy_poll_step_ms);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(max_wait_ms);

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
      logf("clipboard: %s pick from clipboard (unchanged vs baseline; copy "
           "likely no-op, %zu bytes)",
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
      logf("clipboard: %s Wayland primary changed vs baseline (%zu bytes)",
           attempt_label, after->size());
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kStep));
  }
  wayland_interpret_after_copy(baseline_clip, baseline_primary, attempt_label,
                               after, got_fresh, clipboard_offer_unchanged);
}

void do_clipboard_pickup(Display *d, const std::string &clip_saved,
                         bool clipboard_clear_attempted_on_wl) {
  ScopedLogTag log_scope("clipboard");
  if (!g_pickup_focus_watch.armed) {
    g_pickup_focus_watch.arm("pickup");
  }
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

  MODORE_E2E_LOGF("do_clipboard_pickup: wl=%d d=%s clip_saved=%zu "
                  "preclear_wl=%d baseline_clip=%zu "
                  "baseline_prim=%zu wl_empty_trim=%d",
                  wl ? 1 : 0, d ? "X11" : "null", clip_saved.size(),
                  clipboard_clear_attempted_on_wl ? 1 : 0, baseline_clip.size(),
                  baseline_primary.size(), wl_clip_empty_after_trim ? 1 : 0);

  if (wl && baseline_clip.empty() && !baseline_primary.empty() &&
      wl_primary_looks_like_stale_global_chrome(baseline_primary)) {
    logf("clipboard: baseline CLIPBOARD empty; PRIMARY looks like unrelated "
         "global UI/TUI chrome — "
         "not using it; will try synthetic copy from the focused window");
  }

  std::string picked;
  bool picked_ready = false;
  const WaylandAcquireFlow wayland_flow =
      wl ? classify_wayland_acquire_flow() : WaylandAcquireFlow::Generic;
  const bool discord_like = wayland_flow == WaylandAcquireFlow::DiscordLike;
  // Pick came from PRIMARY (or CLIPBOARD mirrored to it) without trusting
  // post-Ctrl+C CLIPBOARD. Nautilus-style path bars still skip glyph erase
  // after `maybe_narrow_path_primary_pick` rewrote the pick to a final segment;
  // ordinary fields use glyph BackSpace + paste.
  bool pick_from_wayland_primary_mirror = false;

  if (wl && wayland_flow != WaylandAcquireFlow::Generic) {
    logf("clipboard: focused Wayland flow=%s", flow_name(wayland_flow));
    if (wayland_acquire_once_for_flow(wayland_flow, d, baseline_clip,
                                      baseline_primary, &picked)) {
      picked_ready = true;
      logf("pick: %s flow acquired", flow_name(wayland_flow));
      log_text_preview("pick", picked);
    } else {
      logf("clipboard: %s flow did not yield a pick — no fallback ladder",
           flow_name(wayland_flow));
      write_clipboard(clip_saved);
      return;
    }
  }

  logf("pick: stage=generic-heuristics");
  logf("pick: focus guard skipped before generic clipboard heuristics");

  // If we successfully ran wl-copy "" at pickup start, never trust "PRIMARY is
  // fresh, CLIPBOARD is stale junk" without also running synthetic copy. GTK
  // path-bar cases still get Ctrl+Insert/Ctrl+C; Chromium/Electron often stops
  // updating PRIMARY on the live selection while keeping a tiny stale CLIPBOARD
  // offer — the old shortcut then re-reads yesterday's 6-byte romaji forever
  // (see log: tesuto vs nihongo both → 6-byte pick, 9-byte テスト output).
  const bool skip_primary_vs_stale_clipboard_shortcut =
      wl && clipboard_clear_attempted_on_wl;

  // GTK3/4 often mirrors the highlighted range onto PRIMARY without any copy
  // action. Synthetic Ctrl+Insert/Ctrl+C frequently fails to refresh CLIPBOARD
  // on Wayland (Nautilus path bar), while PRIMARY still reflects the live
  // selection — there is nothing new for Walker to record either.
  if (wl && !picked_ready && !skip_primary_vs_stale_clipboard_shortcut &&
      !baseline_primary.empty() && !baseline_clip.empty()) {
    std::string prim = baseline_primary;
    trim_trailing_crlf_inplace(&prim);
    trim_in_place_ascii(&prim);
    constexpr size_t kMaxPrimPrefer = 384;
    const bool prim_single_line =
        prim.find_first_of("\n\r") == std::string::npos;
    const bool clip_multiline_or_huge = looks_like_line_copy(baseline_clip) &&
                                        baseline_clip.size() > prim.size();
    const bool clip_much_larger_than_prim =
        baseline_clip.size() > prim.size() * 3 &&
        baseline_clip.size() > prim.size() + 12;

    if (!prim.empty() && prim.size() <= kMaxPrimPrefer && prim_single_line &&
        !wl_primary_looks_like_stale_global_chrome(prim) &&
        (clip_multiline_or_huge || clip_much_larger_than_prim)) {
      picked = std::move(prim);
      picked_ready = true;
      pick_from_wayland_primary_mirror = true;
      logf("pick: using Wayland PRIMARY without synthetic copy (primary=%zu "
           "clipboard=%zu)",
           picked.size(), baseline_clip.size());
      log_text_preview("pick", picked);
    }
  }

  // Short single-line CLIPBOARD equals PRIMARY (selection text is already on
  // both offers). Still avoid glyph-count erase: synthetic keys rarely hit the
  // path bar; paste replaces selection here.
  if (wl && !picked_ready && !baseline_primary.empty() &&
      !baseline_clip.empty()) {
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
      logf("pick: CLIPBOARD equals PRIMARY short line");
      log_text_preview("pick", picked);
    }
  }

  std::string after;
  bool got_fresh = false;
  bool clip_noop_vs_baseline = false;

  if (!picked_ready) {
    logf("pick: stage=synthetic-copy");
    // XTest copy writes the X11 CLIPBOARD; wl-paste can stay stale on XWayland
    // Chromium — only poll wl-clipboard when synthetic copy also used the
    // Wayland path (d == nullptr).
    const bool use_wayland_clipboard_reads = wl && !d;
    MODORE_E2E_LOGF("pick: use_wayland_clipboard_reads=%d (wl=%d d=%s)",
                    use_wayland_clipboard_reads ? 1 : 0, wl ? 1 : 0,
                    d ? "X11" : "null");
    logf("pick: focus guard skipped before synthetic copy");
    if (wl && wl_clip_empty_after_trim) {
      if (discord_like) {
        logf("pick: Discord-like window — preferring Shift+Home before copy");
      }
      wayland_select_for_acquire(discord_like,
                                 discord_like ? "Discord pre-copy line select"
                                              : "Pre-copy selection");
      if (use_wayland_clipboard_reads) {
        wl_poll_until_clip_or_primary_moves(
            baseline_clip, baseline_primary,
            clipboard_timing_ms(g_clipboard_timings.wayland_select_settle_ms));
        logf("pick: Wayland pre-copy %s — clipboard was empty (trimmed)",
             discord_like ? "Shift+Home" : "Ctrl+Shift+Left");
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(
            clipboard_timing_ms(g_clipboard_timings.wayland_copy_poll_ms)));
        logf("pick: pre-copy %s — clipboard was empty (trimmed wl baseline); "
             "using X11 "
             "CLIPBOARD reads",
             discord_like ? "Shift+Home" : "Ctrl+Shift+Left");
      }
    }
    logf("pick: focus guard skipped before first Ctrl+C");
    fake_ctrl_c_best(d);

    if (use_wayland_clipboard_reads) {
      // Fast ceiling — most clients publish in <50ms; step=4ms polls exit early
      // when possible.
      const int kPollMsFirst =
          clipboard_timing_ms(g_clipboard_timings.wayland_copy_poll_ms);
      wayland_poll_after_copy(baseline_clip, baseline_primary,
                              "after first Ctrl+C:", &after, &got_fresh,
                              &clip_noop_vs_baseline, kPollMsFirst);
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
          logf("clipboard: synthetic copy never matched baseline — PRIMARY "
               "ascii last resort (%zu bytes; "
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
          logf("clipboard: CLIPBOARD still empty after Ctrl+C — ascii PRIMARY "
               "fallback (%zu bytes)",
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
        logf("clipboard: X11 pick from clipboard (unchanged vs baseline after "
             "copy no-op; %zu bytes)",
             after.size());
      }
      if (!got_fresh) {
        logf("clipboard: first Ctrl+C did not change clipboard vs baseline "
             "(%zu chars)",
             baseline_clip.size());
      }

      MODORE_E2E_LOGF("clipboard: after synthetic copy phase got_fresh=%d "
                      "after_sz=%zu clip_noop=%d",
                      got_fresh ? 1 : 0, after.size(),
                      clip_noop_vs_baseline ? 1 : 0);
    }

    if (!got_fresh) {
      logf("pick: no selection on first copy (empty or unchanged vs baseline)");
    } else if (looks_like_line_copy(after)) {
      std::string via_primary;
      if (wl && wl_try_primary_as_highlighted_span(baseline_primary, after,
                                                   &via_primary)) {
        picked = std::move(via_primary);
        logf("clipboard: Wayland primary span (%zu bytes) — shorter than first "
             "line of clip",
             picked.size());
      } else if (clip_noop_vs_baseline) {
        // Ctrl+C did not refresh the CLIPBOARD offer; the buffer is whatever
        // was already there. Taking "first logical line" often converts
        // unrelated paste data (see log: 500+ byte blobs) while BackSpace+paste
        // mutates the rename field the user actually sees.
        logf("pick: CLIPBOARD unchanged after Ctrl+C — refusing first-line "
             "truncation on "
             "multiline/large buffer");
      } else if (clipboard_first_reasonable_line(after, &picked)) {
        logf("pick: using first logical line");
        log_text_preview("pick", picked);
      } else {
        logf("pick: line-shaped clipboard but first line empty after trim — "
             "word-select");
      }
    } else {
      std::string via_primary_single;
      if (wl && wl_try_primary_as_highlighted_span(baseline_primary, after,
                                                   &via_primary_single)) {
        picked = std::move(via_primary_single);
        logf("clipboard: Wayland primary span (%zu bytes) inside single-line "
             "clip (%zu bytes)",
             picked.size(), after.size());
      } else if (wl && clip_noop_vs_baseline) {
        logf("pick: CLIPBOARD unchanged after Ctrl+C — refusing single-line "
             "pick; forcing word-select");
      } else {
        picked = after;
        log_text_preview("pick", picked);
      }
    }

    if (picked.empty()) {
      logf("pick: nothing to convert");
      write_clipboard(clip_saved);
      return;
    }
  }

  while (!picked.empty() && (picked.back() == '\n' || picked.back() == '\r')) {
    picked.pop_back();
  }

  logf("pick: heuristic block begin picked_ready=%d picked_bytes=%zu "
       "baseline_clip=%zu baseline_primary=%zu",
       picked_ready ? 1 : 0, picked.size(), baseline_clip.size(),
       baseline_primary.size());
  logf("pick: entering heuristic pass picked_ready=%d bytes=%zu",
       picked_ready ? 1 : 0, picked.size());
  logf("pick: heuristic block before path narrow");
  const bool path_pick_narrowed_to_segment =
      maybe_narrow_path_primary_pick(&picked);
  logf("pick: path narrow result=%d bytes=%zu",
       path_pick_narrowed_to_segment ? 1 : 0, picked.size());
  logf("pick: heuristic block before omnibox narrow");
  const bool omniboz_url_tail_narrowed =
      maybe_narrow_omnibox_url_contaminated_pick(&picked);
  logf("pick: omnibox narrow result=%d bytes=%zu",
       omniboz_url_tail_narrowed ? 1 : 0, picked.size());
  logf("pick: heuristic block before utf8 trim");
  const bool pick_trimmed_utf8_noise =
      trim_pick_leading_romaji_if_utf8_contaminated(&picked);
  logf("pick: utf8 trim result=%d bytes=%zu", pick_trimmed_utf8_noise ? 1 : 0,
       picked.size());

  logf("pick: post-acquire candidate bytes=%zu utf8=%s", picked.size(),
       utf8_preview(picked).c_str());
  MODORE_E2E_LOGF("do_clipboard_pickup: post-acquire candidate size=%zu",
                  picked.size());

  logf("pick: heuristic block before romaji-field guard");
  if (clipboard_pick_probably_not_romaji_field(picked)) {
    logf("pick: looks like a shell/command line (or modore path) — skipping "
         "Mozc to avoid "
         "mixed kana+ASCII garbage in clipboard history");
    logf("pick: blocked by romaji-field heuristic");
    write_clipboard(clip_saved);
    return;
  }
  logf("pick: romaji-field heuristic passed");

  logf("pick: heuristic block before IDE/UI guard");
  if (clipboard_pick_probably_ide_ui_hint(picked)) {
    logf("pick: blocked by IDE/UI hint heuristic");
    write_clipboard(clip_saved);
    return;
  }
  logf("pick: IDE/UI hint heuristic passed");

  logf("pick: heuristic block before mojibake guard");
  if (!pick_is_plain_ascii_romaji(picked) &&
      pick_looks_like_mojibake_garbage(picked)) {
    logf("pick: blocked as stale mojibake — not running Mozc (no UI recovery "
         "by default; "
         "set MODORE_MOJIBAKE_RECOVERY=1 for notify + Hypr Ctrl+A clear; "
         "MODORE_NO_MOJIBAKE_RECOVERY "
         "still suppresses the clear when recovery is on)");
    if (mojibake_recovery_aggressive_enabled()) {
      notify_corrupted_pick_needs_recovery();
      if (!d /* Wayland clipboard path uses Hypr chords */ &&
          hypr_attempt_clear_focused_edit_field_best_effort(
              "Modore corrupted pick — select all+clear focused field")) {
        logf("clipboard: attempted Hypr Ctrl+A + delete/backspace to clear "
             "fouled omnibox");
      }
    }
    logf("pick: blocked by mojibake heuristic");
    write_clipboard(clip_saved);
    return;
  }
  logf("pick: mojibake heuristic passed");

  auto conversion = mozc_convert_utf8_with_candidates(picked);
  if (!conversion.has_value()) {
    logf("pick: mozc_convert failed");
    MODORE_E2E_LOGF("do_clipboard_pickup: mozc_convert failed");
    write_clipboard(clip_saved);
    return;
  }
  std::string replacement = std::move(conversion->first);
  std::vector<std::string> candidates = std::move(conversion->second);

  int candidate_index = 0;
  candidates = normalize_candidate_session_state(
      replacement, std::move(candidates), &candidate_index);

  logf("pick: stage=mozc-convert");
  log_text_preview("pick", picked);
  log_text_preview("replacement (conversion)", replacement);
  logf("replacement (conversion result) utf8=%s", replacement.c_str());
  MODORE_E2E_LOGF("do_clipboard_pickup: mozc_convert succeeded out_bytes=%zu",
                  replacement.size());

  // PRIMARY mirror shortcuts previously skipped wl_erase (glyph BackSpace loop)
  // because Nautilus path bars mis-count vs the narrowed segment. Plain
  // browser/Electron IME fields need that erase: otherwise romaji stays and
  // paste appends Kanji ("henkan" + fragment).
  const bool skip_wayland_glyph_erase = pick_from_wayland_primary_mirror &&
                                        path_pick_narrowed_to_segment &&
                                        !omniboz_url_tail_narrowed;
  MODORE_E2E_LOGF("do_clipboard_pickup: inject_replacement "
                  "skip_wayland_glyph_erase=%d trimmed_noise=%d",
                  skip_wayland_glyph_erase ? 1 : 0,
                  pick_trimmed_utf8_noise ? 1 : 0);
  inject_replacement_clear_then_type(
      d, replacement, skip_wayland_glyph_erase ? nullptr : &picked,
      pick_trimmed_utf8_noise);
  MODORE_E2E_LOGF("pick: inject_replacement returned (pick %zu -> out %zu)",
                  picked.size(), replacement.size());
  ConversionSession session;
  session.backing = ConversionSession::Backing::ClipboardFallback;
  session.app_id = current_focused_app_id();
  session.current_text = replacement;
  session.current_text_chars = static_cast<glong>(g_utf8_strlen(
      replacement.c_str(), static_cast<gssize>(replacement.size())));
  session.candidates = std::move(candidates);
  session.candidate_index = candidate_index;
  session.last_touch = std::chrono::steady_clock::now();
  set_conversion_session(std::move(session));
  logf("pick: conversion complete");

  nap_after_compose_event(std::chrono::milliseconds(
      clipboard_timing_ms(g_clipboard_timings.short_restore_delay_ms)));
  if (std::getenv("WAYLAND_DISPLAY")) {
    const int restore_delay_ms =
        clipboard_timing_ms(g_clipboard_timings.short_restore_delay_ms);
    MODORE_E2E_LOGF("do_clipboard_pickup: delaying clipboard restore by %d ms",
                    restore_delay_ms);
    nap_after_compose_event(std::chrono::milliseconds(restore_delay_ms));
  }
  MODORE_E2E_LOGF(
      "do_clipboard_pickup: restoring clipboard baseline after pickup");
  log_text_preview("restore clip_saved", clip_saved);
  write_clipboard(clip_saved);
}

void snapshot_clip_for_restore(std::string *clip_saved) {
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

} // namespace modore_host
