#include "evdev_hotkey.hpp"

#include "log.hpp"

#include <X11/keysym.h>

#include <linux/input.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct EvdevHotkeySpec {
  unsigned short key_code = 0;
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
  bool super = false;
};

struct EvdevDevice {
  int fd = -1;
  std::string path;
};

struct ModState {
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
  bool super = false;
};

static bool bit_is_set(const unsigned char *bits, size_t bit) {
  return (bits[bit / 8] & (1u << (bit % 8))) != 0;
}

static bool event_device_is_readable_keyboard(int fd) {
  unsigned char evbits[(EV_MAX + 8) / 8]{};
  if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
    return false;
  }
  return bit_is_set(evbits, EV_KEY);
}

static bool open_evdev_device(const char *path, EvdevDevice *out) {
  out->fd = ::open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (out->fd < 0) {
    return false;
  }
  if (!event_device_is_readable_keyboard(out->fd)) {
    ::close(out->fd);
    out->fd = -1;
    return false;
  }
  out->path = path;
  return true;
}

static std::vector<EvdevDevice> scan_evdev_devices() {
  std::vector<EvdevDevice> devices;
  DIR *dir = ::opendir("/dev/input");
  if (!dir) {
    return devices;
  }
  for (dirent *ent = ::readdir(dir); ent != nullptr; ent = ::readdir(dir)) {
    if (std::strncmp(ent->d_name, "event", 5) != 0) {
      continue;
    }
    std::string path = std::string("/dev/input/") + ent->d_name;
    EvdevDevice dev{};
    if (open_evdev_device(path.c_str(), &dev)) {
      devices.push_back(std::move(dev));
    }
  }
  ::closedir(dir);
  return devices;
}

static bool evdev_update_modifier_state(unsigned short code, int value,
                                        ModState *mods) {
  const bool down = value != 0;
  switch (code) {
  case KEY_LEFTCTRL:
  case KEY_RIGHTCTRL:
    mods->ctrl = down;
    return true;
  case KEY_LEFTSHIFT:
  case KEY_RIGHTSHIFT:
    mods->shift = down;
    return true;
  case KEY_LEFTALT:
  case KEY_RIGHTALT:
    mods->alt = down;
    return true;
  case KEY_LEFTMETA:
  case KEY_RIGHTMETA:
    mods->super = down;
    return true;
  default:
    return false;
  }
}

static bool evdev_keycode_for_keysym(std::uint64_t keysym,
                                     unsigned short *out) {
  if (!out) {
    return false;
  }
  switch (static_cast<KeySym>(keysym)) {
  case XK_a:
  case XK_A:
    *out = KEY_A;
    return true;
  case XK_b:
  case XK_B:
    *out = KEY_B;
    return true;
  case XK_c:
  case XK_C:
    *out = KEY_C;
    return true;
  case XK_d:
  case XK_D:
    *out = KEY_D;
    return true;
  case XK_e:
  case XK_E:
    *out = KEY_E;
    return true;
  case XK_f:
  case XK_F:
    *out = KEY_F;
    return true;
  case XK_g:
  case XK_G:
    *out = KEY_G;
    return true;
  case XK_h:
  case XK_H:
    *out = KEY_H;
    return true;
  case XK_i:
  case XK_I:
    *out = KEY_I;
    return true;
  case XK_j:
  case XK_J:
    *out = KEY_J;
    return true;
  case XK_k:
  case XK_K:
    *out = KEY_K;
    return true;
  case XK_l:
  case XK_L:
    *out = KEY_L;
    return true;
  case XK_m:
  case XK_M:
    *out = KEY_M;
    return true;
  case XK_n:
  case XK_N:
    *out = KEY_N;
    return true;
  case XK_o:
  case XK_O:
    *out = KEY_O;
    return true;
  case XK_p:
  case XK_P:
    *out = KEY_P;
    return true;
  case XK_q:
  case XK_Q:
    *out = KEY_Q;
    return true;
  case XK_r:
  case XK_R:
    *out = KEY_R;
    return true;
  case XK_s:
  case XK_S:
    *out = KEY_S;
    return true;
  case XK_t:
  case XK_T:
    *out = KEY_T;
    return true;
  case XK_u:
  case XK_U:
    *out = KEY_U;
    return true;
  case XK_v:
  case XK_V:
    *out = KEY_V;
    return true;
  case XK_w:
  case XK_W:
    *out = KEY_W;
    return true;
  case XK_x:
  case XK_X:
    *out = KEY_X;
    return true;
  case XK_y:
  case XK_Y:
    *out = KEY_Y;
    return true;
  case XK_z:
  case XK_Z:
    *out = KEY_Z;
    return true;
  case XK_1:
    *out = KEY_1;
    return true;
  case XK_2:
    *out = KEY_2;
    return true;
  case XK_3:
    *out = KEY_3;
    return true;
  case XK_4:
    *out = KEY_4;
    return true;
  case XK_5:
    *out = KEY_5;
    return true;
  case XK_6:
    *out = KEY_6;
    return true;
  case XK_7:
    *out = KEY_7;
    return true;
  case XK_8:
    *out = KEY_8;
    return true;
  case XK_9:
    *out = KEY_9;
    return true;
  case XK_0:
    *out = KEY_0;
    return true;
  case XK_semicolon:
  case XK_colon:
    *out = KEY_SEMICOLON;
    return true;
  case XK_slash:
  case XK_question:
    *out = KEY_SLASH;
    return true;
  case XK_period:
  case XK_greater:
    *out = KEY_DOT;
    return true;
  case XK_comma:
  case XK_less:
    *out = KEY_COMMA;
    return true;
  case XK_apostrophe:
    *out = KEY_APOSTROPHE;
    return true;
  case XK_grave:
    *out = KEY_GRAVE;
    return true;
  case XK_minus:
  case XK_underscore:
    *out = KEY_MINUS;
    return true;
  case XK_equal:
  case XK_plus:
    *out = KEY_EQUAL;
    return true;
  case XK_space:
    *out = KEY_SPACE;
    return true;
  case XK_Return:
    *out = KEY_ENTER;
    return true;
  case XK_Tab:
    *out = KEY_TAB;
    return true;
  case XK_Escape:
    *out = KEY_ESC;
    return true;
  case XK_BackSpace:
    *out = KEY_BACKSPACE;
    return true;
  case XK_Delete:
    *out = KEY_DELETE;
    return true;
  case XK_Home:
    *out = KEY_HOME;
    return true;
  case XK_End:
    *out = KEY_END;
    return true;
  case XK_Page_Up:
    *out = KEY_PAGEUP;
    return true;
  case XK_Page_Down:
    *out = KEY_PAGEDOWN;
    return true;
  case XK_Left:
    *out = KEY_LEFT;
    return true;
  case XK_Right:
    *out = KEY_RIGHT;
    return true;
  case XK_Up:
    *out = KEY_UP;
    return true;
  case XK_Down:
    *out = KEY_DOWN;
    return true;
  case XK_bracketleft:
    *out = KEY_LEFTBRACE;
    return true;
  case XK_bracketright:
    *out = KEY_RIGHTBRACE;
    return true;
  case XK_backslash:
    *out = KEY_BACKSLASH;
    return true;
  case XK_F1:
    *out = KEY_F1;
    return true;
  case XK_F2:
    *out = KEY_F2;
    return true;
  case XK_F3:
    *out = KEY_F3;
    return true;
  case XK_F4:
    *out = KEY_F4;
    return true;
  case XK_F5:
    *out = KEY_F5;
    return true;
  case XK_F6:
    *out = KEY_F6;
    return true;
  case XK_F7:
    *out = KEY_F7;
    return true;
  case XK_F8:
    *out = KEY_F8;
    return true;
  case XK_F9:
    *out = KEY_F9;
    return true;
  case XK_F10:
    *out = KEY_F10;
    return true;
  case XK_F11:
    *out = KEY_F11;
    return true;
  case XK_F12:
    *out = KEY_F12;
    return true;
  default:
    return false;
  }
}

static bool evdev_hotkey_from_config(const X11HotkeySpec &hotkey,
                                     EvdevHotkeySpec *out) {
  if (!out) {
    return false;
  }
  out->ctrl = (hotkey.modifier_mask & ControlMask) != 0;
  out->shift = (hotkey.modifier_mask & ShiftMask) != 0;
  out->alt = (hotkey.modifier_mask & Mod1Mask) != 0;
  out->super = (hotkey.modifier_mask & Mod4Mask) != 0;
  return evdev_keycode_for_keysym(hotkey.keysym, &out->key_code);
}

static bool hotkey_matches(const ModState &mods, const EvdevHotkeySpec &spec) {
  return mods.ctrl == spec.ctrl && mods.shift == spec.shift &&
         mods.alt == spec.alt && mods.super == spec.super;
}

static void monitor_loop(std::vector<EvdevDevice> devices, EvdevHotkeySpec spec,
                         std::function<void()> on_trigger) {
  if (devices.empty()) {
    modore_log("hotkey",
               "evdev monitor has no readable /dev/input/event* devices");
    return;
  }

  std::vector<pollfd> fds(devices.size());
  ModState mods{};

  for (;;) {
    for (size_t i = 0; i < devices.size(); ++i) {
      fds[i].fd = devices[i].fd;
      fds[i].events = POLLIN;
      fds[i].revents = 0;
    }
    const int ret = ::poll(fds.data(), fds.size(), -1);
    if (ret < 0) {
      if (errno == EINTR) {
        continue;
      }
      modore_log("hotkey", "evdev poll failed: %s", std::strerror(errno));
      return;
    }

    for (size_t i = 0; i < devices.size(); ++i) {
      if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) {
        continue;
      }
      bool remove_device = false;
      for (;;) {
        input_event ev{};
        const ssize_t n = ::read(devices[i].fd, &ev, sizeof(ev));
        if (n == 0) {
          remove_device = true;
          break;
        }
        if (n < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
          }
          if (errno == ENODEV || errno == EIO) {
            remove_device = true;
          }
          break;
        }
        if (n != static_cast<ssize_t>(sizeof(ev))) {
          continue;
        }
        if (ev.type != EV_KEY) {
          continue;
        }
        if (evdev_update_modifier_state(ev.code, ev.value, &mods)) {
          continue;
        }
        if (ev.code != spec.key_code) {
          continue;
        }
        if (ev.value == 1 && hotkey_matches(mods, spec)) {
          modore_log("hotkey",
                     "evdev hotkey matched (/dev/input event stream)");
          on_trigger();
        }
      }
      if (remove_device) {
        ::close(devices[i].fd);
        devices[i].fd = -1;
      }
    }

    devices.erase(std::remove_if(devices.begin(), devices.end(),
                                 [](const EvdevDevice &d) { return d.fd < 0; }),
                  devices.end());
    if (devices.empty()) {
      modore_log("hotkey", "evdev monitor lost all devices");
      return;
    }
    fds.resize(devices.size());
  }
}

} // namespace

bool start_evdev_hotkey_monitor(const X11HotkeySpec &hotkey,
                                const std::string &description,
                                std::function<void()> on_trigger,
                                std::string *error_message) {
  if (error_message) {
    error_message->clear();
  }
  EvdevHotkeySpec spec{};
  if (!evdev_hotkey_from_config(hotkey, &spec)) {
    if (error_message) {
      *error_message = "could not map configured hotkey to /dev/input codes";
    }
    return false;
  }
  auto devices = scan_evdev_devices();
  if (devices.empty()) {
    if (error_message) {
      *error_message = "no readable /dev/input/event* devices";
    }
    modore_log("hotkey",
               "evdev hotkey unavailable (%s) — check /dev/input/event* access "
               "or CAP_DAC_OVERRIDE",
               description.c_str());
    return false;
  }
  std::thread([devices = std::move(devices), spec,
               on_trigger = std::move(on_trigger)]() mutable {
    monitor_loop(std::move(devices), spec, std::move(on_trigger));
  }).detach();
  modore_log("hotkey", "evdev hotkey active (%s)", description.c_str());
  return true;
}
