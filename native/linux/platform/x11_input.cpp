// x11_input.cpp — X11 modifier state and synthetic key injection (XTest).

#include "host_internal.hpp"

namespace modore_host {

unsigned int x11_current_modifier_mask(Display *d) {
  if (!d) {
    return 0;
  }
  unsigned char keymap[32]{};
  if (!XQueryKeymap(d, reinterpret_cast<char *>(keymap))) {
    return 0;
  }

  XModifierKeymap *modmap = XGetModifierMapping(d);
  if (!modmap) {
    return 0;
  }

  const auto keycode_pressed = [&](KeyCode code) {
    if (!code) {
      return false;
    }
    const unsigned idx = static_cast<unsigned>(code / 8);
    const unsigned bit = static_cast<unsigned>(code % 8);
    return idx < sizeof(keymap) && (keymap[idx] & (1u << bit));
  };

  const auto modifier_pressed = [&](int modifier_index) {
    if (!modmap || modifier_index < 0) {
      return false;
    }
    const int start = modifier_index * modmap->max_keypermod;
    for (int i = 0; i < modmap->max_keypermod; ++i) {
      if (keycode_pressed(modmap->modifiermap[start + i])) {
        return true;
      }
    }
    return false;
  };

  unsigned int mask = 0;
  if (modifier_pressed(ShiftMapIndex)) {
    mask |= ShiftMask;
  }
  if (modifier_pressed(ControlMapIndex)) {
    mask |= ControlMask;
  }
  if (modifier_pressed(Mod1MapIndex)) {
    mask |= Mod1Mask;
  }
  if (modifier_pressed(Mod4MapIndex)) {
    mask |= Mod4Mask;
  }
  if (modifier_pressed(Mod5MapIndex)) {
    mask |= Mod5Mask;
  }
  XFreeModifiermap(modmap);
  return mask;
}

unsigned int active_trigger_modifier_mask(Display *d) {
  if (d) {
    return x11_current_modifier_mask(d);
  }
  return evdev_current_modifier_mask();
}

bool trigger_ctrl_is_held(Display *d) {
  return (active_trigger_modifier_mask(d) & ControlMask) != 0;
}
int x11_quiet_error_handler(Display *, XErrorEvent *) {
  g_x11_setup_error = 1;
  return 0;
}

bool hotkey_can_leak_text(std::uint64_t keysym) {
  return keysym >= 0x20 && keysym <= 0x7e;
}

void fake_x11_backspace_glyph_count(Display *d, glong glyphs) {
  if (!d || glyphs <= 0) {
    return;
  }
  const bool ctrl_held = trigger_ctrl_is_held(d);
  constexpr glong kMax = 384;
  if (glyphs > kMax) {
    logf("inject: clipping X11 BackSpace repeats from %ld to %ld glyphs",
         static_cast<long>(glyphs), static_cast<long>(kMax));
    glyphs = kMax;
  }
  KeyCode backspace = XKeysymToKeycode(d, XK_BackSpace);
  KeyCode delete_key = XKeysymToKeycode(d, XK_Delete);
  KeyCode left_key = XKeysymToKeycode(d, XK_Left);
  KeyCode shift_l = XKeysymToKeycode(d, XK_Shift_L);
  if (!backspace) {
    logf("inject: no X11 keycode for BackSpace");
    return;
  }
  KeyCode ctrl_l = XKeysymToKeycode(d, XK_Control_L);
  KeyCode ctrl_r = XKeysymToKeycode(d, XK_Control_R);
  if (ctrl_held) {
    if (!delete_key || !left_key || !shift_l) {
      logf("inject: missing X11 keycode for selection-delete fallback");
    }
    MODORE_E2E_LOGF("cycle: ctrl held; selecting glyph span before Delete");
    if (ctrl_l) {
      XTestFakeKeyEvent(d, ctrl_l, False, CurrentTime);
    }
    if (ctrl_r) {
      XTestFakeKeyEvent(d, ctrl_r, False, CurrentTime);
    }
    XFlush(d);
    if (delete_key && left_key && shift_l) {
      XTestFakeKeyEvent(d, shift_l, True, CurrentTime);
      for (glong i = 0; i < glyphs; ++i) {
        XTestFakeKeyEvent(d, left_key, True, CurrentTime);
        XTestFakeKeyEvent(d, left_key, False, CurrentTime);
        XFlush(d);
        yield_to_compose_pipeline();
      }
      XTestFakeKeyEvent(d, shift_l, False, CurrentTime);
      XTestFakeKeyEvent(d, delete_key, True, CurrentTime);
      XTestFakeKeyEvent(d, delete_key, False, CurrentTime);
      XFlush(d);
      yield_to_compose_pipeline();
    } else {
      for (glong i = 0; i < glyphs; ++i) {
        XTestFakeKeyEvent(d, backspace, True, CurrentTime);
        XTestFakeKeyEvent(d, backspace, False, CurrentTime);
        XFlush(d);
        yield_to_compose_pipeline();
      }
    }
    if (ctrl_l) {
      XTestFakeKeyEvent(d, ctrl_l, True, CurrentTime);
    }
    if (ctrl_r) {
      XTestFakeKeyEvent(d, ctrl_r, True, CurrentTime);
    }
    XFlush(d);
    return;
  }
  for (glong i = 0; i < glyphs; ++i) {
    XTestFakeKeyEvent(d, backspace, True, CurrentTime);
    XTestFakeKeyEvent(d, backspace, False, CurrentTime);
    XFlush(d);
    yield_to_compose_pipeline();
  }
  if (ctrl_held) {
    if (ctrl_l) {
      XTestFakeKeyEvent(d, ctrl_l, True, CurrentTime);
    }
    if (ctrl_r) {
      XTestFakeKeyEvent(d, ctrl_r, True, CurrentTime);
    }
    XFlush(d);
  }
}

void fake_backspace_glyph_count(Display *d, glong glyphs) {
  if (d) {
    fake_x11_backspace_glyph_count(d, glyphs);
    return;
  }
  fake_wayland_backspace_glyph_count(glyphs);
}

// --- XTest synthetic keys -------------------------------------------------

void fake_ctrl_letter(Display *d, KeySym letter) {
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

void fake_ctrl_shift_left(Display *d) {
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

// --- wtype / ydotool (optional; used when Display* is null) ----------------

void child_clear_im_modules() {
  unsetenv("GTK_IM_MODULE");
  unsetenv("QT_IM_MODULE");
  unsetenv("SDL_IM_MODULE");
  unsetenv("XMODIFIERS");
  unsetenv("INPUT_METHOD");
}

void fake_ctrl_c_best(Display *d) {
  MODORE_E2E_LOGF("fake_ctrl_c_best: backend=%s",
                  d ? "XTest Ctrl+C" : "Hypr/wtype copy");
  if (d) {
    fake_ctrl_letter(d, XK_c);
    MODORE_E2E_LOGF("fake_ctrl_c_best: XTest Ctrl+C sent");
    return;
  }
  if (!wayland_send_ctrl_c()) {
    logf("Wayland: need Hyprland `hyprctl sendkeystate` or `wtype` — cannot "
         "simulate universal "
         "copy (Ctrl+Insert / Ctrl+C)");
  } else {
    MODORE_E2E_LOGF("fake_ctrl_c_best: Wayland hypr_ok=%d wtype_ok=%d",
                    g_wayland_uses_hypr_sendshortcut ? 1 : 0,
                    wtype_is_available() ? 1 : 0);
  }
}

void fake_ctrl_shift_left_best(Display *d) {
  if (d) {
    fake_ctrl_shift_left(d);
    return;
  }
  (void)wayland_send_ctrl_shift_left();
}

} // namespace modore_host
