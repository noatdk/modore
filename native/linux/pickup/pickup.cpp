// pickup.cpp — AT-SPI focus cache, try_pickup_atspi, cycle, do_pickup.

#include "host_internal.hpp"

namespace modore_host {

// --- AT-SPI: find focused, read text, replace ----------------------------

AtspiAccessible *find_focused_leaf(AtspiAccessible *obj, int depth) {
  if (!obj || depth > 48) {
    return nullptr;
  }
  AtspiStateSet *ss = atspi_accessible_get_state_set(obj);
  if (ss && atspi_state_set_contains(ss, ATSPI_STATE_FOCUSED)) {
    g_object_unref(ss);
    return ATSPI_ACCESSIBLE(g_object_ref(obj));
  }
  if (ss) {
    g_object_unref(ss);
  }
  GError *err = nullptr;
  gint n = atspi_accessible_get_child_count(obj, &err);
  if (err) {
    g_clear_error(&err);
    return nullptr;
  }
  for (gint i = 0; i < n; ++i) {
    AtspiAccessible *ch = atspi_accessible_get_child_at_index(obj, i, &err);
    if (err) {
      g_clear_error(&err);
      continue;
    }
    AtspiAccessible *f = find_focused_leaf(ch, depth + 1);
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
AtspiAccessible *find_text_with_focus_or_active(AtspiAccessible *obj,
                                                int depth) {
  if (!obj || depth > 48) {
    return nullptr;
  }
  if (atspi_accessible_is_text(obj)) {
    AtspiStateSet *ss = atspi_accessible_get_state_set(obj);
    if (ss) {
      const bool focusish = atspi_state_set_contains(ss, ATSPI_STATE_FOCUSED) ||
                            atspi_state_set_contains(ss, ATSPI_STATE_ACTIVE);
      g_object_unref(ss);
      if (focusish) {
        return ATSPI_ACCESSIBLE(g_object_ref(obj));
      }
    }
  }
  GError *err = nullptr;
  const gint n = atspi_accessible_get_child_count(obj, &err);
  if (err) {
    g_clear_error(&err);
    return nullptr;
  }
  for (gint i = 0; i < n; ++i) {
    AtspiAccessible *ch = atspi_accessible_get_child_at_index(obj, i, &err);
    if (err) {
      g_clear_error(&err);
      continue;
    }
    AtspiAccessible *f = find_text_with_focus_or_active(ch, depth + 1);
    g_object_unref(ch);
    if (f) {
      return f;
    }
  }
  return nullptr;
}

bool atspi_focus_event_is_gaining(const AtspiEvent *event) {
  if (!event || !event->type) {
    return false;
  }
  if (event->detail1 == 0) {
    return false;
  }
  return std::strcmp(event->type, "object:state-changed:focused") == 0 ||
         std::strcmp(event->type, "object:state-changed:active") == 0;
}

void atspi_focus_cache_event_cb(const AtspiEvent *event) {
  if (!atspi_focus_event_is_gaining(event) || !event->source) {
    return;
  }
  AtspiAccessible *cached = nullptr;
  if (atspi_accessible_is_text(event->source)) {
    cached = ATSPI_ACCESSIBLE(g_object_ref(event->source));
  } else {
    cached = find_text_with_focus_or_active(event->source, 0);
  }
  if (!cached) {
    return;
  }
  g_cached_atspi_focus.update(cached);
  g_object_unref(cached);
}

void start_atspi_focus_cache_listener() {
  static bool started = false;
  if (started) {
    return;
  }
  started = true;
  GError *err = nullptr;
  if (!atspi_event_listener_register_no_data(
          atspi_focus_cache_event_cb, nullptr, "object:state-changed:focused",
          &err)) {
    modore_log("atspi", "focus cache listener register focused failed: %s",
               err ? err->message : "unknown error");
    g_clear_error(&err);
    return;
  }
  if (!atspi_event_listener_register_no_data(
          atspi_focus_cache_event_cb, nullptr, "object:state-changed:active",
          &err)) {
    modore_log("atspi", "focus cache listener register active failed: %s",
               err ? err->message : "unknown error");
    g_clear_error(&err);
    return;
  }
  std::thread([]() { atspi_event_main(); }).detach();
  modore_log("atspi", "focus cache listener active");
}

// Returns true if AT-SPI produced a result: either direct editable replace
// (*direct_done) or UTF-8 to inject with atspi_generate_keyboard_event.
// When *pick_span_for_inject is non-null, fills it with the source romaji slice
// (UTF-8) on the non-direct path so Wayland inject can glyph-erase + paste
// without relying on fake Delete. Returns false to fall through to the
// clipboard/XTest path.
bool try_pickup_atspi(bool *direct_done, std::string *inject_utf8,
                      std::string *pick_span_for_inject) {
  ScopedLogTag log_scope("atspi");
  *direct_done = false;
  inject_utf8->clear();
  if (pick_span_for_inject) {
    pick_span_for_inject->clear();
  }
  const auto atspi_started = std::chrono::steady_clock::now();
  auto atspi_elapsed_ms = [&]() -> long long {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - atspi_started)
            .count());
  };
  MODORE_E2E_LOGF("try_pickup_atspi: start");
  std::string app_id = current_focused_app_id();

  GError *err = nullptr;
  AtspiAccessible *focus = g_cached_atspi_focus.take_ref();
  if (focus) {
    AtspiStateSet *ss = atspi_accessible_get_state_set(focus);
    const bool focusish =
        ss && (atspi_state_set_contains(ss, ATSPI_STATE_FOCUSED) ||
               atspi_state_set_contains(ss, ATSPI_STATE_ACTIVE));
    if (ss) {
      g_object_unref(ss);
    }
    if (!atspi_accessible_is_text(focus) || !focusish) {
      logf("cached focused accessible rejected (text=%d focusish=%d) "
           "elapsed=%lld ms",
           atspi_accessible_is_text(focus) ? 1 : 0, focusish ? 1 : 0,
           atspi_elapsed_ms());
      g_object_unref(focus);
      focus = nullptr;
    } else {
      logf("using cached focused accessible elapsed=%lld ms",
           atspi_elapsed_ms());
    }
  }
  AtspiAccessible *found_focus = nullptr;
  const gint n_desk = atspi_get_desktop_count();
  logf("desktop_count=%d elapsed=%lld ms", static_cast<int>(n_desk),
       atspi_elapsed_ms());
  if (!focus) {
    const gint n_try = n_desk > 0 ? n_desk : 1;
    for (gint di = 0; di < n_try; ++di) {
      AtspiAccessible *desktop = atspi_get_desktop(di);
      if (!desktop) {
        continue;
      }
      logf("scanning desktop %d elapsed=%lld ms", static_cast<int>(di),
           atspi_elapsed_ms());
      found_focus = find_focused_leaf(desktop, 0);
      if (!found_focus) {
        found_focus = find_text_with_focus_or_active(desktop, 0);
        if (found_focus) {
          logf("desktop %d — Text widget FOCUSED/ACTIVE (no strict "
               "focus leaf)",
               static_cast<int>(di));
        }
      }
      g_object_unref(desktop);
      if (found_focus) {
        break;
      }
    }
    focus = found_focus;
    if (focus) {
      g_cached_atspi_focus.update(focus);
      logf("cached focus from DFS elapsed=%lld ms", atspi_elapsed_ms());
    }
  }
  if (!focus) {
    logf("no focused accessible elapsed=%lld ms", atspi_elapsed_ms());
    return false;
  }
  logf("focus located elapsed=%lld ms", atspi_elapsed_ms());
  if (!atspi_accessible_is_text(focus)) {
    logf("focused node has no Text interface elapsed=%lld ms",
         atspi_elapsed_ms());
    g_object_unref(focus);
    return false;
  }
  AtspiText *text = atspi_accessible_get_text_iface(focus);
  if (!text) {
    logf("get_text_iface failed elapsed=%lld ms", atspi_elapsed_ms());
    g_object_unref(focus);
    return false;
  }
  logf("text iface ready elapsed=%lld ms", atspi_elapsed_ms());

  gint n_chars = atspi_text_get_character_count(text, &err);
  if (err) {
    g_clear_error(&err);
    logf("character_count failed elapsed=%lld ms", atspi_elapsed_ms());
    g_object_unref(focus);
    return false;
  }
  logf("character_count=%d elapsed=%lld ms", static_cast<int>(n_chars),
       atspi_elapsed_ms());

  gchar *full = atspi_text_get_text(text, 0, n_chars, &err);
  if (err || !full) {
    g_clear_error(&err);
    logf("get_text failed elapsed=%lld ms", atspi_elapsed_ms());
    g_object_unref(focus);
    return false;
  }
  logf("get_text bytes=%zu elapsed=%lld ms", std::strlen(full),
       atspi_elapsed_ms());

  gint caret = atspi_text_get_caret_offset(text, &err);
  if (err) {
    g_clear_error(&err);
    g_free(full);
    logf("caret_offset failed elapsed=%lld ms", atspi_elapsed_ms());
    g_object_unref(focus);
    return false;
  }
  logf("caret_offset=%d elapsed=%lld ms", static_cast<int>(caret),
       atspi_elapsed_ms());

  glong span_start = 0;
  glong span_end = 0;

  // Script-driven pickup span override. Engine ABI speaks UTF-8 byte
  // offsets; AT-SPI speaks Unicode-char offsets. Convert at the boundary
  // via g_utf8_offset_to_pointer / g_utf8_pointer_to_offset.
  bool scripted_pickup = false;
  {
    const gsize full_bytes = std::strlen(full);
    // Clamp the AT-SPI caret to [0, n_chars] before glib's offset walk,
    // which otherwise reads past the NUL terminator if the app reports a
    // caret beyond the field's character count.
    const glong safe_caret = std::clamp<glong>(caret, 0, n_chars);
    const gsize caret_byte =
        static_cast<gsize>(g_utf8_offset_to_pointer(full, safe_caret) - full);
    auto scripted =
        modore_script::pickup_span(std::string(full, full_bytes), caret_byte,
                                   app_id.c_str(), /*katakana*/ false);
    logf("script pickup probe elapsed=%lld ms", atspi_elapsed_ms());
    if (scripted) {
      const std::size_t sb = std::min(scripted->first, full_bytes);
      const std::size_t eb = std::min(scripted->second, full_bytes);
      if (sb <= eb) {
        span_start = g_utf8_pointer_to_offset(full, full + sb);
        span_end = g_utf8_pointer_to_offset(full, full + eb);
        scripted_pickup = true;
        logf("script pickup span chars=[%ld..%ld] bytes=[%zu..%zu]",
             static_cast<long>(span_start), static_cast<long>(span_end), sb,
             eb);
      }
    }
  }

  if (!scripted_pickup) {
    gint n_sel = atspi_text_get_n_selections(text, &err);
    g_clear_error(&err);
    logf("n_selections=%d elapsed=%lld ms", static_cast<int>(n_sel),
         atspi_elapsed_ms());
    if (n_sel > 0) {
      AtspiRange *range = atspi_text_get_selection(text, 0, &err);
      if (!err && range) {
        span_start = range->start_offset;
        span_end = range->end_offset;
        g_free(range);
        logf("selection range [%ld..%ld] elapsed=%lld ms",
             static_cast<long>(span_start), static_cast<long>(span_end),
             atspi_elapsed_ms());
      } else {
        g_clear_error(&err);
        logf("get_selection failed, falling back to word range "
             "elapsed=%lld ms",
             atspi_elapsed_ms());
        word_range_chars(full, caret, n_chars, &span_start, &span_end);
      }
    } else {
      logf("no selection, using word range elapsed=%lld ms",
           atspi_elapsed_ms());
      word_range_chars(full, caret, n_chars, &span_start, &span_end);
    }
  }

  // Edge/Chromium often report caret=0 immediately after focus or typing —
  // `word_range_chars` then returns an empty span even though the field clearly
  // has text. Fall back to using the entire field text when it looks like a
  // single-line input (no newlines, bounded size). This matches what the user
  // wants in a search box / URL bar / single-line form input: convert
  // everything I typed, not just the word at the caret.
  if (span_start >= span_end && n_chars > 0) {
    constexpr glong kAtspiFullTextMaxChars = 256;
    if (n_chars <= kAtspiFullTextMaxChars) {
      bool single_line = true;
      for (const gchar *p = full; *p;) {
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
        logf("empty word span at caret=%ld but field has %ld chars — "
             "using entire field "
             "(single-line input)",
             static_cast<long>(caret), static_cast<long>(n_chars));
      }
    }
  }

  if (span_start >= span_end) {
    logf("empty span (caret=%ld n_chars=%ld) — falling through",
         static_cast<long>(caret), static_cast<long>(n_chars));
    g_free(full);
    g_object_unref(focus);
    return false;
  }

  std::string romaji;
  const gchar *span_a = g_utf8_offset_to_pointer(full, span_start);
  const gchar *span_b = g_utf8_offset_to_pointer(full, span_end);
  // Byte offsets stashed for the scripting layer (engine ABI is UTF-8 bytes).
  const std::size_t span_start_byte = static_cast<std::size_t>(span_a - full);
  const std::size_t span_end_byte = static_cast<std::size_t>(span_b - full);
  romaji.assign(span_a, span_b - span_a);
  g_free(full);
  MODORE_E2E_LOGF(
      "try_pickup_atspi: span_start=%ld span_end=%ld romaji_bytes=%zu",
      static_cast<long>(span_start), static_cast<long>(span_end),
      romaji.size());
  logf("pick: atspi span extracted elapsed=%lld ms", atspi_elapsed_ms());
  log_text_preview("pick", romaji);

  std::vector<std::string> candidates;
  std::string converted;
  if (mozc_uses_atzc_backend()) {
    if (!mozc_convert_utf8(romaji, &converted)) {
      logf("convert failed elapsed=%lld ms", atspi_elapsed_ms());
      g_object_unref(focus);
      return false;
    }
    candidates = {converted};
  } else {
    auto converted_with_candidates = mozc_convert_utf8_with_candidates(romaji);
    if (!converted_with_candidates.has_value()) {
      logf("convert failed elapsed=%lld ms", atspi_elapsed_ms());
      g_object_unref(focus);
      return false;
    }
    converted = std::move(converted_with_candidates->first);
    candidates = std::move(converted_with_candidates->second);
  }

  AtspiEditableText *ed = atspi_accessible_get_editable_text_iface(focus);
  if (ed && atspi_accessible_is_editable_text(focus)) {
    logf("editable path mozc_convert done elapsed=%lld ms", atspi_elapsed_ms());
    log_text_preview("replacement", converted);

    // Script-driven replacement override. Mozc's top candidate is what the
    // host would write; scripts can rewrite it before the AT-SPI delete +
    // insert. nullopt → keep Mozc's choice.
    {
      const std::vector<std::string> cands = {converted};
      auto scripted = modore_script::replacement(
          app_id.c_str(), span_start_byte, span_end_byte, cands);
      if (scripted) {
        logf("script replacement override (was '%s', now '%s')",
             converted.c_str(), scripted->c_str());
        converted = std::move(*scripted);
      }
    }

    int candidate_index = 0;
    candidates = normalize_candidate_session_state(
        converted, std::move(candidates), &candidate_index);

    gboolean ok1 =
        atspi_editable_text_delete_text(ed, span_start, span_end, &err);
    if (!ok1) {
      g_clear_error(&err);
      logf("delete_text failed elapsed=%lld ms", atspi_elapsed_ms());
      g_object_unref(focus);
      return false;
    }
    gboolean ok2 = atspi_editable_text_insert_text(
        ed, span_start, converted.c_str(), static_cast<gint>(converted.size()),
        &err);
    if (!ok2) {
      g_clear_error(&err);
      logf("insert_text failed elapsed=%lld ms", atspi_elapsed_ms());
      g_object_unref(focus);
      return false;
    }
    g_clear_error(&err);
    const gint caret_after =
        static_cast<gint>(span_start) +
        static_cast<gint>(g_utf8_strlen(converted.c_str(),
                                        static_cast<gssize>(converted.size())));
    if (!atspi_text_set_caret_offset(text, caret_after, &err)) {
      if (err) {
        logf("set_caret_offset(%d): %s", static_cast<int>(caret_after),
             err->message);
        g_clear_error(&err);
      } else {
        logf("set_caret_offset(%d) failed", static_cast<int>(caret_after));
      }
    } else {
      g_clear_error(&err);
    }
    ConversionSession session;
    session.backing = ConversionSession::Backing::AtspiEditable;
    session.focus = ATSPI_ACCESSIBLE(g_object_ref(focus));
    session.app_id = app_id;
    session.span_start = span_start;
    session.span_end = span_end;
    session.current_text_chars = static_cast<glong>(g_utf8_strlen(
        converted.c_str(), static_cast<gssize>(converted.size())));
    session.current_text = converted;
    session.source_romaji = romaji;
    session.candidates = std::move(candidates);
    session.candidate_index = candidate_index;
    session.last_touch = std::chrono::steady_clock::now();
    set_conversion_session(std::move(session));
    *direct_done = true;
    logf("editable path complete elapsed=%lld ms", atspi_elapsed_ms());
    g_object_unref(focus);
    return true;
  }

  logf("non-editable path mozc_convert start elapsed=%lld ms",
       atspi_elapsed_ms());
  *inject_utf8 = converted;
  logf("non-editable path mozc_convert done elapsed=%lld ms",
       atspi_elapsed_ms());
  log_text_preview("replacement", *inject_utf8);
  // On Wayland, set_selection frequently updates accessibility state without
  // moving the real keyboard selection, so synthetic Delete/types the wrong
  // slice — skip.
  if (!std::getenv("WAYLAND_DISPLAY")) {
    gboolean sel_ok =
        atspi_text_set_selection(text, 0, static_cast<gint>(span_start),
                                 static_cast<gint>(span_end), &err);
    if (!sel_ok) {
      if (err) {
        logf("set_selection: %s", err->message);
        g_clear_error(&err);
      } else {
        logf("set_selection failed");
      }
    }
  } else {
    logf("Wayland — skipping set_selection before inject");
  }
  g_object_unref(focus);
  if (pick_span_for_inject) {
    pick_span_for_inject->assign(romaji);
  }
  int candidate_index = 0;
  candidates = normalize_candidate_session_state(
      converted, std::move(candidates), &candidate_index);
  ConversionSession session;
  session.backing = ConversionSession::Backing::ClipboardFallback;
  session.app_id = app_id;
  session.current_text = converted;
  session.source_romaji = romaji;
  session.current_text_chars = static_cast<glong>(
      g_utf8_strlen(converted.c_str(), static_cast<gssize>(converted.size())));
  session.candidates = std::move(candidates);
  session.candidate_index = candidate_index;
  session.last_touch = std::chrono::steady_clock::now();
  set_conversion_session(std::move(session));
  logf("non-editable control — injecting conversion text elapsed=%lld "
       "ms",
       atspi_elapsed_ms());
  return true;
}

bool try_cycle_active_conversion(Display *d) {
  ScopedLogTag log_scope("cycle");

  ConversionSession snap;
  {
    std::lock_guard<std::mutex> lock(g_conversion_session_mu);
    const auto now = std::chrono::steady_clock::now();
    if (!conversion_session_available_locked(now)) {
      modore_log("cycle", "no active session in scope");
      return false;
    }
    snap = g_conversion_session;
  }

  if (snap.candidates.empty()) {
    modore_log("cycle", "session fell through: no candidates captured");
    std::lock_guard<std::mutex> lock(g_conversion_session_mu);
    clear_conversion_session_locked();
    return false;
  }

  if (mozc_uses_atzc_backend() && snap.candidates.size() <= 1) {
    std::lock_guard<std::mutex> lock(g_conversion_session_mu);
    const auto now = std::chrono::steady_clock::now();
    if (conversion_session_available_locked(now) &&
        g_conversion_session.source_romaji == snap.source_romaji &&
        g_conversion_session.candidates.size() > 1) {
      snap.candidates = g_conversion_session.candidates;
      snap.candidate_index = g_conversion_session.candidate_index;
    }
  }

  if (mozc_uses_atzc_backend() && snap.candidates.size() <= 1) {
    modore_log("cycle", "candidates prefetch still in flight");
    return false;
  }

  const int next_index =
      (snap.candidate_index + 1) % static_cast<int>(snap.candidates.size());
  const std::string &from = snap.current_text;
  const std::string &to = snap.candidates[static_cast<size_t>(next_index)];
  const glong from_chars = snap.current_text_chars > 0
                               ? snap.current_text_chars
                               : g_utf8_strlen(from.c_str(), from.size());

  bool swapped = false;
  switch (snap.backing) {
  case ConversionSession::Backing::AtspiEditable: {
    GError *err = nullptr;
    AtspiAccessible *focus = snap.focus;
    AtspiText *text = focus ? atspi_accessible_get_text_iface(focus) : nullptr;
    if (!text) {
      if (focus) {
        g_object_unref(focus);
      }
      focus = g_cached_atspi_focus.take_ref();
      if (focus) {
        modore_log("cycle", "reacquired AT-SPI focus for cycle retry");
        text = atspi_accessible_get_text_iface(focus);
      }
    }
    if (!focus || !text) {
      if (focus) {
        g_object_unref(focus);
      }
      break;
    }
    gint n_chars = atspi_text_get_character_count(text, &err);
    if (err) {
      g_clear_error(&err);
      break;
    }
    if (snap.span_start < 0 || snap.span_start >= n_chars) {
      break;
    }
    const glong delete_chars = std::max<glong>(1, from_chars);
    gchar *full = atspi_text_get_text(text, 0, n_chars, &err);
    if (err || !full) {
      g_clear_error(&err);
      g_object_unref(focus);
      break;
    }
    const gchar *span_a = g_utf8_offset_to_pointer(full, snap.span_start);
    const gchar *span_b = g_utf8_offset_to_pointer(full, snap.span_end);
    const std::string text_at_span(span_a, span_b - span_a);
    if (text_at_span != from) {
      modore_log("cycle",
                 "atspi span mismatch: session='%s' visible='%s' — replacing "
                 "anyway",
                 from.c_str(), text_at_span.c_str());
    }
    AtspiEditableText *ed = atspi_accessible_get_editable_text_iface(focus);
    if (!ed) {
      g_free(full);
      g_object_unref(focus);
      break;
    }
    gboolean ok1 = atspi_editable_text_delete_text(
        ed, snap.span_start,
        std::min<glong>(n_chars, snap.span_start + delete_chars), &err);
    if (!ok1) {
      g_clear_error(&err);
      g_free(full);
      g_object_unref(focus);
      break;
    }
    gboolean ok2 = atspi_editable_text_insert_text(
        ed, snap.span_start, to.c_str(), static_cast<gint>(to.size()), &err);
    if (!ok2) {
      g_clear_error(&err);
      g_free(full);
      g_object_unref(focus);
      break;
    }
    g_clear_error(&err);
    const gint caret_after =
        snap.span_start + static_cast<gint>(g_utf8_strlen(
                              to.c_str(), static_cast<gssize>(to.size())));
    (void)atspi_text_set_caret_offset(text, caret_after, &err);
    if (err) {
      g_clear_error(&err);
    }
    g_free(full);
    g_object_unref(focus);
    swapped = true;
    break;
  }
  case ConversionSession::Backing::ClipboardFallback: {
    std::string clip_saved;
    snapshot_clip_for_restore(&clip_saved);
    const glong residue_chars = clipboard_cycle_residue_chars();
    const glong glyphs = from_chars + residue_chars;
    log_text_preview("cycle from", from);
    log_text_preview("cycle to", to);
    if (residue_chars > 0) {
      logf("configured hotkey may leak one glyph while modifiers are "
           "still held");
    }
    logf("clipboard fallback clear %ld glyphs (%zu bytes from, %zu "
         "bytes to)",
         static_cast<long>(glyphs), from.size(), to.size());
    fake_backspace_glyph_count(d, glyphs);
    if (std::getenv("WAYLAND_DISPLAY")) {
      // Give the compositor a beat to apply the final erase before we queue
      // the replacement paste. Without this, the new candidate can land while
      // the old span is still being collapsed.
      nap_after_compose_event(std::chrono::milliseconds(
          clipboard_timing_ms(g_clipboard_timings.cycle_settle_delay_ms)));
    }
    // Reuse the same clipboard-based injection path as the normal pickup
    // conversion. That path already proved stable on Wayland and keeps the
    // paste transport identical between fresh convert and cycle.
    inject_replacement_clear_then_type(d, to, nullptr, false);
    nap_after_compose_event(std::chrono::milliseconds(
        clipboard_timing_ms(g_clipboard_timings.cycle_post_inject_delay_ms)));
    if (std::getenv("WAYLAND_DISPLAY")) {
      nap_after_compose_event(std::chrono::milliseconds(
          clipboard_timing_ms(g_clipboard_timings.cycle_settle_delay_ms)));
    }
    write_clipboard(clip_saved);
    swapped = true;
    break;
  }
  }

  if (!swapped) {
    std::lock_guard<std::mutex> lock(g_conversion_session_mu);
    clear_conversion_session_locked();
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(g_conversion_session_mu);
    if (g_has_conversion_session) {
      g_conversion_session.candidate_index = next_index;
      g_conversion_session.current_text = to;
      g_conversion_session.current_text_chars = static_cast<glong>(
          g_utf8_strlen(to.c_str(), static_cast<gssize>(to.size())));
      g_conversion_session.last_touch = std::chrono::steady_clock::now();
    }
  }

  logf("cycle: '%s' -> '%s' (%d/%zu)", from.c_str(), to.c_str(), next_index + 1,
       snap.candidates.size());
  return true;
}

// --- Clipboard pickup (macOS-style) --------------------------------------

bool inject_via_atspi_string(const std::string &utf8) {
  GError *err = nullptr;
  atspi_generate_keyboard_event(0, utf8.c_str(), ATSPI_KEY_STRING, &err);
  if (err) {
    logf("atspi_generate_keyboard_event STRING: %s", err->message);
    g_clear_error(&err);
    return false;
  }
  return true;
}

void do_pickup(Display *d) {
  ScopedLogTag log_scope("pickup");
  std::lock_guard<std::mutex> lock(g_pickup_mu);
  const auto pickup_started = std::chrono::steady_clock::now();
  MODORE_E2E_LOGF("do_pickup: enter d=%s", d ? "X11" : "null");
  g_pickup_focus_watch.arm("pickup");

  // Mirror macOS repeat-hotkey UX: if the previous conversion is still
  // alive and the user hits the conversion chord again, step to the next
  // Mozc candidate instead of starting a fresh pickup.
  if (try_cycle_active_conversion(d)) {
    logf("cycle complete");
    logf("total elapsed %lld ms",
         static_cast<long long>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - pickup_started)
                 .count()));
    return;
  }

  // Per-app routing override. Use the focused Hyprland class when available so
  // scripts can branch on app-specific quirks instead of treating every native
  // Wayland window as the same bucket. A script returning
  // "clipboard" skips the AT-SPI try and goes straight to the clipboard
  // fallback — useful for apps where AT-SPI lies about success. "ax" and
  // "keystroke" are no-ops here (existing flow already tries AT-SPI first
  // then falls back to clipboard/keystroke).
  std::string app_id = current_focused_app_id();
  auto scripted_route = modore_script::route_for(app_id.c_str());
  if (scripted_route && *scripted_route == modore_script::Route::Clipboard) {
    modore_log("scripting", "route → clipboard (user script)");
    if (std::getenv("WAYLAND_DISPLAY")) {
      nap_after_compose_event(std::chrono::milliseconds(
          clipboard_timing_ms(g_clipboard_timings.pickup_start_delay_ms)));
    }
    std::string clip_saved;
    snapshot_clip_for_restore(&clip_saved);
    do_clipboard_pickup(d, clip_saved, false);
    return;
  }

  // macOS always tries Accessibility first for the real selection + direct
  // replace. Linux used to skip AT-SPI when DISPLAY was omitted (native
  // Wayland), which forced the slow clipboard/Ctrl+C pipeline. Try AT-SPI
  // regardless of X11 Display; fall back to synthetic keys only when needed.
  const char *skip_atspi_first = std::getenv("MODORE_SKIP_ATSPI_FIRST");
  bool direct = false;
  std::string inject;
  std::string atspi_pick_span;
  if (!skip_atspi_first || !skip_atspi_first[0]) {
    const auto atspi_started = std::chrono::steady_clock::now();
    if (try_pickup_atspi(&direct, &inject, &atspi_pick_span)) {
      const auto atspi_done = std::chrono::steady_clock::now();
      logf("AT-SPI path completed in %lld ms",
           static_cast<long long>(
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   atspi_done - atspi_started)
                   .count()));
      if (direct) {
        MODORE_E2E_LOGF("do_pickup: AT-SPI direct editable replace done");
        logf("replaced via AT-SPI (editable)");
        logf("total elapsed %lld ms",
             static_cast<long long>(
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - pickup_started)
                     .count()));
        return;
      }
      std::string clip_saved;
      snapshot_clip_for_restore(&clip_saved);
      // On X11, STRING sometimes replaces the span; on Wayland,
      // Chromium/Electron often ignore it while atspi still reports success —
      // always use erase + wl-copy paste there when we have the source span
      // from AT-SPI.
      if (!std::getenv("WAYLAND_DISPLAY")) {
        if (inject_via_atspi_string(inject)) {
          logf("replaced via AT-SPI STRING inject (no wl-copy paste path)");
          nap_after_compose_event(std::chrono::milliseconds(clipboard_timing_ms(
              g_clipboard_timings.atspi_direct_settle_delay_ms)));
          write_clipboard(clip_saved);
          return;
        }
        logf("AT-SPI: STRING inject failed — synthetic delete/type / clipboard "
             "paste");
      } else {
        logf("AT-SPI: Wayland — erase + wl-copy paste (skip STRING; span %zu "
             "bytes for glyph clear)",
             atspi_pick_span.size());
      }
      inject_replacement_clear_then_type(
          d, inject, atspi_pick_span.empty() ? nullptr : &atspi_pick_span,
          false);
      MODORE_E2E_LOGF(
          "do_pickup: AT-SPI inject_replacement_clear_then_type returned");
      MODORE_E2E_LOGF("do_pickup: AT-SPI non-direct inject path finished");
      nap_after_compose_event(std::chrono::milliseconds(clipboard_timing_ms(
          g_clipboard_timings.atspi_replacement_settle_delay_ms)));
      if (std::getenv("WAYLAND_DISPLAY")) {
        const int restore_delay_ms =
            clipboard_timing_ms(g_clipboard_timings.long_restore_delay_ms);
        MODORE_E2E_LOGF("do_pickup: delaying clipboard restore by %d ms",
                        restore_delay_ms);
        nap_after_compose_event(std::chrono::milliseconds(restore_delay_ms));
      }
      write_clipboard(clip_saved);
      logf("pickup: total elapsed %lld ms",
           static_cast<long long>(
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - pickup_started)
                   .count()));
      return;
    }
    logf("AT-SPI attempt took %lld ms and did not produce a span",
         static_cast<long long>(
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - atspi_started)
                 .count()));
  }

  // Escape hatch: refuse the racy synthetic-Ctrl+C + clipboard pickup pipeline
  // entirely. AT-SPI is the only "clean" source of truth (it reads the field
  // directly and replaces the actual span); the clipboard path papers over apps
  // that don't expose AT-SPI properly, but as the user noted, it has bitten us
  // repeatedly with stale offers, MIME confusion (PNG image bytes leaking in as
  // "❱PNG"), and races in Chromium/Electron. Set MODORE_ATSPI_ONLY=1 to bail
  // clean instead.
  const char *atspi_only = std::getenv("MODORE_ATSPI_ONLY");
  if (atspi_only && atspi_only[0] && std::strcmp(atspi_only, "0") != 0) {
    logf("pickup: AT-SPI did not produce a span and MODORE_ATSPI_ONLY=1 — "
         "skipping clipboard "
         "fallback (no synthetic Ctrl+C / wl-paste reads). Make sure the "
         "focused app exposes the "
         "field via AT-SPI, or unset MODORE_ATSPI_ONLY to re-enable the "
         "clipboard pipeline.");
    return;
  }

  if (std::getenv("WAYLAND_DISPLAY")) {
    // Hypr needs a short beat after the exec that spawned --trigger so
    // sendshortcut targets the same surface; skip this delay entirely when
    // AT-SPI handled pickup above.
    nap_after_compose_event(std::chrono::milliseconds(
        clipboard_timing_ms(g_clipboard_timings.pickup_start_delay_ms)));
  }
  logf("start synthetic_keys=%s",
       g_wayland_uses_hypr_sendshortcut ? "hyprctl/sendkeystate" : "wtype");
  MODORE_E2E_LOGF("do_pickup: falling through to synthetic clipboard pickup");
  std::string clip_saved;
  snapshot_clip_for_restore(&clip_saved);
  // Default: do not clear CLIPBOARD before pickup — fewer wl-copy round-trips
  // and races in Chromium; GTK still gets a correct pick via synthetic copy +
  // PRIMARY mirrors when applicable. Opt in: MODORE_PICKUP_CLEAR_CLIPBOARD=1
  // (restores stronger "stale CLIPBOARD" avoidance).
  bool clipboard_clear_attempted_on_wl = false;
  const char *preclear_e = std::getenv("MODORE_PICKUP_CLEAR_CLIPBOARD");
  if (preclear_e && preclear_e[0] && std::strcmp(preclear_e, "0") != 0) {
    if (write_clipboard("")) {
      logf("cleared CLIPBOARD baseline (saved %zu bytes for restore "
           "after conversion)",
           clip_saved.size());
      if (wl_clipboard_available()) {
        clipboard_clear_attempted_on_wl = true;
        if (!poll_wl_clipboard_cleared(
                clipboard_timing_ms(g_clipboard_timings.clear_poll_max_wait_ms),
                clipboard_timing_ms(g_clipboard_timings.clear_poll_step_ms))) {
          logf("pickup: CLIPBOARD still non-empty after wl-copy \"\" — "
               "disabling PRIMARY-vs-stale-CLIPBOARD "
               "fast path; synthetic copy will run");
        }
      }
    }
  }
  do_clipboard_pickup(d, clip_saved, clipboard_clear_attempted_on_wl);
  if (std::getenv("WAYLAND_DISPLAY")) {
    const int restore_delay_ms =
        clipboard_timing_ms(g_clipboard_timings.long_restore_delay_ms);
    MODORE_E2E_LOGF("do_pickup: delaying clipboard restore by %d ms",
                    restore_delay_ms);
    nap_after_compose_event(std::chrono::milliseconds(restore_delay_ms));
  }
  MODORE_E2E_LOGF("do_pickup: restoring clipboard baseline after pickup");
  logf("total elapsed %lld ms",
       static_cast<long long>(
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - pickup_started)
               .count()));
}

void run_ipc_pickup() {
  ScopedLogTag log_scope("ipc");
  MODORE_E2E_LOGF(
      "run_ipc_pickup: DISPLAY=%s WAYLAND_DISPLAY=%s MODORE_IPC_SOCKET=%s",
      std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "(unset)",
      std::getenv("WAYLAND_DISPLAY") ? std::getenv("WAYLAND_DISPLAY")
                                     : "(unset)",
      std::getenv("MODORE_IPC_SOCKET") ? std::getenv("MODORE_IPC_SOCKET")
                                       : "(unset)");
  Display *work = nullptr;
  const char *xd = std::getenv("DISPLAY");
  const char *wl = std::getenv("WAYLAND_DISPLAY");
  const bool skip_x_for_native_wayland =
      xd && xd[0] && wl && wl[0] && hypr_focus_is_wayland_native();

  // When DISPLAY is set, do_pickup(non-null Display*) uses XTest for clipboard
  // fallback. AT-SPI is always attempted first (same order as macOS
  // Accessibility), even for native Wayland focus.
  if (xd && xd[0]) {
    if (skip_x_for_native_wayland) {
      logf("ipc pickup: Hypr focused client is Wayland-native — using "
           "Hypr/wtype path "
           "(DISPLAY=%s)",
           xd);
    } else {
      work = XOpenDisplay(nullptr);
      if (!work) {
        logf("ipc pickup: DISPLAY is set (%s) but XOpenDisplay failed — using "
             "pure Wayland path",
             xd);
      } else if (wl && wl[0]) {
        logf("ipc pickup: using X11 keyboard/display path (DISPLAY=%s) — best "
             "when focus is "
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

} // namespace modore_host
