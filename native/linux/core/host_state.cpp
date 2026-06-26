// host_state.cpp — definitions of the host's shared global state.
// Declared extern in host_internal.hpp; centralized here so the mutable
// process state is visible in one place.

#include "host_internal.hpp"

namespace modore_host {

thread_local const char *g_log_scope_tag = "host";

std::mutex g_hypr_window_mu;
HyprWindowSnapshot g_hypr_window_snapshot{};
bool g_hypr_window_snapshot_valid = false;

CachedAtspiFocus g_cached_atspi_focus{};

std::mutex g_pickup_mu;
bool g_wayland_uses_hypr_sendshortcut = false;
unsigned int g_conversion_hotkey_modifier_mask = Mod4Mask;
std::uint64_t g_conversion_hotkey_keysym = XK_semicolon;

LinuxClipboardTimingConfig g_clipboard_timings{};

// Wakes the main thread when a client sends "pickup" on the unix socket (must
// not run AT-SPI / pickup from the IPC accept thread).
int g_pickup_pipe[2] = {-1, -1};

// XSetErrorHandler callback — sees asynchronous errors after XSync.
int g_x11_setup_error = 0;

std::string g_wtype_path;

// --- Hyprland hyprctl sendkeystate (preferred on Hyprland; routes like real
// keys)

std::string g_hyprctl_path;

thread_local PickupFocusWatch g_pickup_focus_watch{};

std::mutex g_conversion_session_mu;
bool g_has_conversion_session = false;
ConversionSession g_conversion_session{};

} // namespace modore_host
