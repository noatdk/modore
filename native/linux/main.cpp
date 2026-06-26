// main.cpp — modore Linux host entry point: argument parsing and the X11
// hotkey-grab event loop. Subsystems live in their own translation units
// (see host_internal.hpp).

#include "host_internal.hpp"

namespace modore_host {

struct RunOptions {
  bool trigger = false;
  bool ipc_only = false;
  bool no_ipc = false;
  bool version = false;
  bool help = false;
};

bool parse_run_options(int argc, char **argv, RunOptions *o) {
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--trigger") == 0) {
      o->trigger = true;
    } else if (std::strcmp(argv[i], "--ipc-only") == 0) {
      o->ipc_only = true;
    } else if (std::strcmp(argv[i], "--no-ipc") == 0) {
      o->no_ipc = true;
    } else if (std::strcmp(argv[i], "--version") == 0) {
      o->version = true;
    } else if (std::strcmp(argv[i], "--help") == 0 ||
               std::strcmp(argv[i], "-h") == 0) {
      o->help = true;
    } else {
      logf("unknown argument: %s — use one command per run, e.g. \"%s "
           "--version\" then \"%s "
           "--trigger\" (not \"/\" between flags). Try --help.",
           argv[i], argv[0], argv[0]);
      return false;
    }
  }
  return true;
}

} // namespace modore_host

using namespace modore_host;

static void print_modore_host_usage(const char *prog) {
  std::printf("Usage:\n"
              "  %s --help | -h           this text\n"
              "  %s --version             build stamp\n"
              "  %s --trigger             signal a running host to run "
              "conversion (Unix socket)\n"
              "  %s [options]             run the host (default: hotkey + IPC "
              "listener)\n"
              "\n"
              "Run options (host mode):\n"
              "  --ipc-only               no X11 grab; only listen for "
              "--trigger / compositor exec\n"
              "  --no-ipc                 X11 grab only; disable Unix socket\n"
              "\n"
              "`--version` and `--trigger` are separate commands. Example:\n"
              "  %s --version && %s --trigger\n"
              "\n"
              "Environment (optional):\n"
              "  MODORE_IPC_SOCKET=/path   override Unix socket path "
              "(--trigger + listener); for tests or "
              "second instance\n"
              "  MODORE_E2E_TRACE=1      verbose [e2e] step logs on stderr + "
              "modore.log (Puppeteer / debugging)\n"
              "  MODORE_SKIP_ATSPI_FIRST=1  skip Accessibility pick — "
              "clipboard/Ctrl+C only (disables "
              "AT-SPI direct replace first)\n"
              "  MODORE_ATSPI_ONLY=1     refuse the clipboard fallback "
              "entirely; if AT-SPI can't read the "
              "focused field, bail (no synthetic Ctrl+C, no wl-paste reads — "
              "avoids screenshot/PNG bytes "
              "leaking in as text)\n"
              "  MODORE_PICKUP_CLEAR_CLIPBOARD=1  empty CLIPBOARD before "
              "synthetic Ctrl+C (GTK staleness; "
              "extra wl-copy)\n\n",
              prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
  RunOptions opts{};
  if (!parse_run_options(argc, argv, &opts)) {
    return 1;
  }
  if (opts.help) {
    print_modore_host_usage(argv[0]);
    return 0;
  }
  if (opts.version) {
    std::printf(
        "modore-host %s %s (multi-path --trigger + /run/user UID sock)\n",
        __DATE__, __TIME__);
    return 0;
  }
  if (opts.trigger) {
    return ipc_send_pickup();
  }

  augment_path_for_subprocesses();

  const char *disp_env = std::getenv("DISPLAY");
  const char *wl_env = std::getenv("WAYLAND_DISPLAY");
  modore_log("boot", "starting pid=%d DISPLAY=%s WAYLAND_DISPLAY=%s",
             static_cast<int>(::getpid()), disp_env ? disp_env : "(unset)",
             wl_env ? wl_env : "(unset)");
  g_wayland_uses_hypr_sendshortcut = hyprctl_ipc_alive_for_wayland_keys();
  if (wl_env && wl_env[0]) {
    modore_log("boot", "Wayland paste transport: %s (startup-detected)",
               g_wayland_uses_hypr_sendshortcut ? "hyprctl-first"
                                                : "wtype-first");
  }
  if (wl_env) {
    modore_log(
        "boot",
        "WAYLAND_DISPLAY is set — X11 hotkey grab may not fire when a native "
        "Wayland window has focus. Use compositor exec → modore-host --trigger "
        "if needed.");
    const char *sig = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    modore_log("boot",
               "Wayland pickup capabilities: hyprctl_IPC=%s "
               "HYPRLAND_INSTANCE_SIGNATURE=%s "
               "wtype=%s wl-paste=%s",
               g_wayland_uses_hypr_sendshortcut ? "yes" : "no",
               (sig && sig[0]) ? "set" : "(unset)",
               wtype_is_available() ? "yes" : "no",
               wl_clipboard_available() ? "yes" : "no");
    if (!g_wayland_uses_hypr_sendshortcut && !wtype_is_available()) {
      modore_log("boot", "Wayland: need `hyprctl` (Hyprland) or `wtype` for "
                         "synthetic keys / clipboard pickup");
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
  g_conversion_hotkey_modifier_mask =
      modore_config.conversion_hotkey.modifier_mask;
  g_conversion_hotkey_keysym = modore_config.conversion_hotkey.keysym;
  apply_linux_config(modore_config);
  modore_log("config", "[conversion] hotkey=%s — %s",
             modore_config.conversion_hotkey_description.c_str(),
             modore_config_path().c_str());
  log_linux_config_timings();
  start_config_reload_watcher();

  // Scripting engine. Empty / missing dir is a pass-through no-op.
  {
    std::string scripts_dir;
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) {
      scripts_dir = std::string(xdg) + "/modore/scripts";
    } else {
      const char *home = std::getenv("HOME");
      scripts_dir = std::string(home ? home : "") + "/.config/modore/scripts";
    }
    modore_script::boot(scripts_dir);
  }

  if (atspi_init() != 0) {
    modore_log("atspi", "init failed");
    return 1;
  }
  start_hyprland_focus_cache_listener();
  start_atspi_focus_cache_listener();

  if (!setup_pickup_pipe()) {
    return 1;
  }
  if (!opts.no_ipc) {
    ipc_start_background([]() { notify_main_pickup_pending(); });
  }

  bool evdev_hotkey_active = false;
  std::string evdev_error;
  if (wl_env && wl_env[0]) {
    evdev_hotkey_active = start_evdev_hotkey_monitor(
        modore_config.conversion_hotkey,
        modore_config.conversion_hotkey_description,
        []() { notify_main_pickup_pending(); },
        []() { invalidate_conversion_session_for_user_input(); }, &evdev_error);
    if (evdev_hotkey_active) {
      modore_log(
          "ipc",
          "native Wayland hotkey is handled by evdev /dev/input monitoring");
    } else if (!evdev_error.empty()) {
      modore_log("hotkey", "evdev hotkey monitor unavailable: %s",
                 evdev_error.c_str());
    }
  }

  bool hyprland_bind_active = false;
  if (!evdev_hotkey_active && wl_env && wl_env[0] && !opts.no_ipc &&
      g_wayland_uses_hypr_sendshortcut) {
    const std::string self_path = resolve_self_executable_path(argv[0]);
    hyprland_bind_active = register_hyprland_hotkey_bind(
        self_path, modore_config.conversion_hotkey,
        modore_config.conversion_hotkey_description);
    if (hyprland_bind_active) {
      modore_log("ipc", "native Wayland hotkey is handled by Hyprland; "
                        "compositor bind now triggers the "
                        "running host");
    }
  }

  if (evdev_hotkey_active) {
    modore_log(
        "ipc",
        "waiting on socket — evdev hotkey monitor will call pickup internally");
    main_thread_run_pipe_only_loop();
  } else if (opts.ipc_only && !hyprland_bind_active) {
    const std::string combo =
        hyprland_hotkey_combo(modore_config.conversion_hotkey);
    modore_log("ipc", "IMPORTANT — ipc-only: the conversion hotkey in "
                      "~/.config/modore/modore.conf is "
                      "not wired to this mode unless Hyprland bind "
                      "registration is available.");
    modore_log("ipc",
               "manual compositor bind example for the configured chord:\n"
               "  bind = %s, exec, %s/.local/bin/modore-host --trigger",
               combo.c_str(), getenv_string("HOME", "/HOME").c_str());
    modore_log("ipc", "waiting on socket — after adding the bind, log should "
                      "show `pickup:` when keys work");
    main_thread_run_pipe_only_loop();
  } else if (hyprland_bind_active) {
    modore_log(
        "ipc",
        "waiting on socket — Hyprland bind will call `--trigger` for pickup");
    main_thread_run_pipe_only_loop();
  }

  Display *d = XOpenDisplay(nullptr);
  if (!d) {
    modore_log(
        "hotkey",
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
  const KeySym hotkey_ks =
      static_cast<KeySym>(modore_config.conversion_hotkey.keysym);
  const KeyCode hotkey_kc = XKeysymToKeycode(d, hotkey_ks);
  if (!hotkey_kc) {
    modore_log("hotkey", "no keycode for configured keysym — check "
                         "~/.config/modore/modore.conf");
    XCloseDisplay(d);
    return 1;
  }
  const unsigned int want_mods = modore_config.conversion_hotkey.modifier_mask;

  g_x11_setup_error = 0;
  XErrorHandler prev_handler = XSetErrorHandler(x11_quiet_error_handler);
  // NumLock/CapsLock add Mod2Mask/LockMask to the modifier field; registering
  // grabs for those combinations fixes "hotkey stopped working when NumLock
  // toggled".
  constexpr unsigned grab_sets[] = {0u, Mod2Mask, LockMask,
                                    Mod2Mask | LockMask};
  bool grabbed_any = false;
  for (unsigned int extra : grab_sets) {
    g_x11_setup_error = 0;
    XGrabKey(d, hotkey_kc, want_mods | extra, root, False, GrabModeAsync,
             GrabModeAsync);
    XSync(d, False);
    if (!g_x11_setup_error) {
      grabbed_any = true;
    }
  }
  XSetErrorHandler(prev_handler);

  if (!grabbed_any) {
    modore_log("hotkey", "XGrabKey failed (X11 error). Another client may "
                         "already hold this key, or the "
                         "compositor blocked the grab. Try a different hotkey "
                         "in ~/.config/modore/modore.conf "
                         "or run under X11.");
    XCloseDisplay(d);
    return 1;
  }

  XSelectInput(d, root, KeyPressMask);
  XFlush(d);

  const char *ks_label = XKeysymToString(hotkey_ks);
  modore_log("boot", "ready: XGrabKey active (%s · keysym=%s)",
             modore_config.conversion_hotkey_description.c_str(),
             ks_label ? ks_label : "?");

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
