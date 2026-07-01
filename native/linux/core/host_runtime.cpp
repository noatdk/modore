// host_runtime.cpp — runtime config apply/reload/timings, the pickup pipe,
// and the main-thread dispatch loop.

#include "host_internal.hpp"

namespace modore_host {

int clipboard_timing_ms(const std::atomic<int> &value) {
  return value.load(std::memory_order_relaxed);
}

void apply_linux_config(const ModoreConfig &cfg) {
  g_clipboard_timings.pre_paste_delay_ms.store(cfg.clipboard_pre_paste_delay_ms,
                                               std::memory_order_relaxed);
  g_clipboard_timings.paste_visibility_wait_ms.store(
      cfg.clipboard_paste_visibility_wait_ms, std::memory_order_relaxed);
  g_clipboard_timings.paste_visibility_step_ms.store(
      cfg.clipboard_paste_visibility_step_ms, std::memory_order_relaxed);
  g_clipboard_timings.short_restore_delay_ms.store(
      cfg.clipboard_short_restore_delay_ms, std::memory_order_relaxed);
  g_clipboard_timings.long_restore_delay_ms.store(
      cfg.clipboard_long_restore_delay_ms, std::memory_order_relaxed);
  g_clipboard_timings.cycle_settle_delay_ms.store(
      cfg.clipboard_cycle_settle_delay_ms, std::memory_order_relaxed);
  g_clipboard_timings.cycle_post_inject_delay_ms.store(
      cfg.clipboard_cycle_post_inject_delay_ms, std::memory_order_relaxed);
  g_clipboard_timings.cycle_backspace_step_ms.store(
      cfg.clipboard_cycle_backspace_step_ms, std::memory_order_relaxed);
  g_clipboard_timings.pickup_start_delay_ms.store(
      cfg.clipboard_pickup_start_delay_ms, std::memory_order_relaxed);
  g_clipboard_timings.atspi_direct_settle_delay_ms.store(
      cfg.clipboard_atspi_direct_settle_delay_ms, std::memory_order_relaxed);
  g_clipboard_timings.atspi_replacement_settle_delay_ms.store(
      cfg.clipboard_atspi_replacement_settle_delay_ms,
      std::memory_order_relaxed);
  g_clipboard_timings.clear_poll_max_wait_ms.store(
      cfg.clipboard_clear_poll_max_wait_ms, std::memory_order_relaxed);
  g_clipboard_timings.clear_poll_step_ms.store(cfg.clipboard_clear_poll_step_ms,
                                               std::memory_order_relaxed);
  g_clipboard_timings.wayland_select_settle_ms.store(
      cfg.clipboard_wayland_select_settle_ms, std::memory_order_relaxed);
  g_clipboard_timings.wayland_copy_poll_ms.store(
      cfg.clipboard_wayland_copy_poll_ms, std::memory_order_relaxed);
  g_clipboard_timings.wayland_copy_poll_step_ms.store(
      cfg.clipboard_wayland_copy_poll_step_ms, std::memory_order_relaxed);
  g_mozc_backend_is_atzc.store(cfg.mozc_backend == "atzc",
                               std::memory_order_relaxed);
}

void log_linux_config_timings() {
  modore_log(
      "config",
      "clipboard timings: pre_paste=%dms paste_wait=%dms paste_step=%dms "
      "short_restore=%dms long_restore=%dms cycle_settle=%dms "
      "cycle_post_inject=%dms cycle_backspace_step=%dms pickup_start=%dms "
      "atspi_direct=%dms "
      "atspi_replacement=%dms clear_poll=%d/%dms wayland_select=%dms "
      "wayland_copy_poll=%d/%dms",
      clipboard_timing_ms(g_clipboard_timings.pre_paste_delay_ms),
      clipboard_timing_ms(g_clipboard_timings.paste_visibility_wait_ms),
      clipboard_timing_ms(g_clipboard_timings.paste_visibility_step_ms),
      clipboard_timing_ms(g_clipboard_timings.short_restore_delay_ms),
      clipboard_timing_ms(g_clipboard_timings.long_restore_delay_ms),
      clipboard_timing_ms(g_clipboard_timings.cycle_settle_delay_ms),
      clipboard_timing_ms(g_clipboard_timings.cycle_post_inject_delay_ms),
      clipboard_timing_ms(g_clipboard_timings.cycle_backspace_step_ms),
      clipboard_timing_ms(g_clipboard_timings.pickup_start_delay_ms),
      clipboard_timing_ms(g_clipboard_timings.atspi_direct_settle_delay_ms),
      clipboard_timing_ms(
          g_clipboard_timings.atspi_replacement_settle_delay_ms),
      clipboard_timing_ms(g_clipboard_timings.clear_poll_max_wait_ms),
      clipboard_timing_ms(g_clipboard_timings.clear_poll_step_ms),
      clipboard_timing_ms(g_clipboard_timings.wayland_select_settle_ms),
      clipboard_timing_ms(g_clipboard_timings.wayland_copy_poll_ms),
      clipboard_timing_ms(g_clipboard_timings.wayland_copy_poll_step_ms));
}

void reload_config_from_disk(bool initial_load) {
  ModoreConfig loaded;
  std::string cfg_err;
  if (!load_modore_config(&loaded, &cfg_err)) {
    modore_log("config", "%s — using defaults", cfg_err.c_str());
  }
  apply_linux_config(loaded);
  if (initial_load) {
    modore_log("config", "[conversion] hotkey=%s — %s",
               loaded.conversion_hotkey_description.c_str(),
               modore_config_path().c_str());
  } else {
    modore_log("config", "config reloaded from %s",
               modore_config_path().c_str());
  }
  log_linux_config_timings();
}

void start_config_reload_watcher() {
  std::thread([]() {
    const std::string path = modore_config_path();
    struct timespec last_mtime{0, 0};
    bool have_last = false;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
      last_mtime = st.st_mtim;
      have_last = true;
    }
    for (;;) {
      if (stat(path.c_str(), &st) == 0) {
        if (have_last && (st.st_mtim.tv_sec != last_mtime.tv_sec ||
                          st.st_mtim.tv_nsec != last_mtime.tv_nsec)) {
          have_last = true;
          last_mtime = st.st_mtim;
          reload_config_from_disk(false);
        }
        have_last = true;
        last_mtime = st.st_mtim;
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
      } else {
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }
    }
  }).detach();
}

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

// IPC thread only writes to the pipe; we drain and run pickup on the main
// thread (AT-SPI must run here).
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
    modore_log("ipc", "pickup pipe: capping batched triggers %d → %d",
               n_triggers, kMaxBatched);
    n_triggers = kMaxBatched;
  }
  for (int t = 0; t < n_triggers; ++t) {
    modore_log("pickup", "via IPC socket (--trigger)%s",
               t > 0 ? " (batched)" : "");
    run_ipc_pickup();
  }
}

void main_thread_run_pipe_only_loop() {
  if (g_pickup_pipe[0] < 0) {
    return;
  }
  for (;;) {
    struct pollfd pfd{};
    pfd.fd = g_pickup_pipe[0];
    pfd.events = POLLIN;
    if (poll(&pfd, 1, -1) < 0) {
      if (errno == EINTR) {
        continue;
      }
      logf("poll failed: %s", std::strerror(errno));
      return;
    }
    if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) {
      main_thread_run_pickup_after_wake();
    }
  }
}

} // namespace modore_host
