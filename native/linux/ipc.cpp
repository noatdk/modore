#include "ipc.hpp"
#include "log.hpp"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static bool set_sockaddr_un_path(const char* path, sockaddr_un* addr) {
  const size_t n = std::strlen(path);
  if (n >= sizeof(addr->sun_path)) {
    modore_logf("socket path too long (%zu bytes; max %zu for AF_UNIX)", n,
                sizeof(addr->sun_path) - 1);
    return false;
  }
  std::memcpy(addr->sun_path, path, n + 1);
  return true;
}

std::size_t ipc_socket_path(char* out, std::size_t cap) {
  if (!out || cap < 8) {
    return 0;
  }
  const char* ipc_override = std::getenv("MODORE_IPC_SOCKET");
  if (ipc_override && ipc_override[0]) {
    std::snprintf(out, cap, "%s", ipc_override);
    return std::strlen(out);
  }
  const char* rt = std::getenv("XDG_RUNTIME_DIR");
  if (rt && *rt) {
    std::snprintf(out, cap, "%s/modore.sock", rt);
  } else {
    // Matches systemd user session (%t = /run/user/$UID). Hyprland keybind `exec` often
    // drops XDG_RUNTIME_DIR; /tmp/modore.$uid.sock then never reached the real listener.
    std::snprintf(out, cap, "/run/user/%d/modore.sock", static_cast<int>(getuid()));
  }
  return std::strlen(out);
}

static void pickup_connect_candidates(std::vector<std::string>* out) {
  out->clear();
  const char* ipc_override = std::getenv("MODORE_IPC_SOCKET");
  if (ipc_override && ipc_override[0]) {
    out->emplace_back(ipc_override);
    return;
  }
  std::vector<std::string> raw;
  const char* rt = std::getenv("XDG_RUNTIME_DIR");
  if (rt && *rt) {
    raw.push_back(std::string(rt) + "/modore.sock");
  }
  char runuser[256];
  std::snprintf(runuser, sizeof(runuser), "/run/user/%d/modore.sock",
                static_cast<int>(getuid()));
  raw.emplace_back(runuser);
  char legacy[256];
  std::snprintf(legacy, sizeof(legacy), "/tmp/modore.%d.sock", static_cast<int>(getuid()));
  raw.emplace_back(legacy);
  for (const std::string& p : raw) {
    bool dup = false;
    for (const std::string& q : *out) {
      if (q == p) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      out->push_back(p);
    }
  }
}

int ipc_send_pickup() {
  std::vector<std::string> paths;
  pickup_connect_candidates(&paths);
  MODORE_E2E_LOGF("ipc_send_pickup: %zu path candidate(s)", paths.size());
  for (size_t i = 0; i < paths.size(); ++i) {
    MODORE_E2E_LOGF("ipc_send_pickup: candidate[%zu]=%s", i, paths[i].c_str());
  }
  std::string tried;
  for (const std::string& path : paths) {
    if (!tried.empty()) {
      tried += ", ";
    }
    tried += path;
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
      continue;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (!set_sockaddr_un_path(path.c_str(), &addr)) {
      close(fd);
      continue;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
      close(fd);
      continue;
    }
    const char msg[] = "pickup\n";
    if (write(fd, msg, sizeof(msg) - 1) != static_cast<ssize_t>(sizeof(msg) - 1)) {
      close(fd);
      continue;
    }
    close(fd);
    MODORE_E2E_LOGF("ipc_send_pickup: connected + wrote pickup -> %s", path.c_str());
    modore_logf("trigger: sent pickup to %s (if conversion does nothing, see host log for "
                "pickup:/clipboard: lines)",
                path.c_str());
    return 0;
  }
  modore_logf("trigger: connect failed for all tried paths [%s] — is modore-host running? "
              "(systemctl --user status modore-host)",
              tried.c_str());
  return 1;
}

void ipc_start_background(std::function<void()> on_pickup) {
  std::thread(
      [cb = std::move(on_pickup)]() mutable {
        char path[256]{};
        if (ipc_socket_path(path, sizeof(path)) == 0) {
          modore_logf("ipc: could not build socket path");
          return;
        }
        ::unlink(path);
        const int srv = socket(AF_UNIX, SOCK_STREAM, 0);
        if (srv < 0) {
          modore_logf("ipc: socket() failed");
          return;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (!set_sockaddr_un_path(path, &addr)) {
          close(srv);
          return;
        }
        if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
          modore_logf("ipc: bind(%s) failed — another modore-host may be running, "
                      "or stale socket; try removing the file.",
                      path);
          close(srv);
          return;
        }
        ::chmod(path, 0600);
        if (listen(srv, 8) < 0) {
          close(srv);
          return;
        }
        modore_logf("ipc listening on %s (--trigger / compositor exec)", path);
        for (;;) {
          const int c = accept(srv, nullptr, nullptr);
          if (c < 0) {
            continue;
          }
          char buf[128]{};
          const ssize_t n = read(c, buf, sizeof(buf) - 1);
          close(c);
          if (n <= 0) {
            modore_logf("ipc: accept closed before data (n=%zd)", static_cast<ssize_t>(n));
            continue;
          }
          buf[n] = '\0';
          if (std::strncmp(buf, "pickup", 6) == 0) {
            cb();
          } else {
            modore_logf("ipc: ignored non-pickup message (%.*s)", static_cast<int>(n), buf);
          }
        }
      })
      .detach();
}
