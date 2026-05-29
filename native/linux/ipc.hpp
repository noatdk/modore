// Unix socket IPC: compositor or CLI can trigger pickup without an X11 grab.
//
// If MODORE_IPC_SOCKET is set to a filesystem path, the listener and --trigger
// use only that path (e2e / multiple hosts). Otherwise the socket is
// $XDG_RUNTIME_DIR/modore.sock (see ipc.cpp).

#pragma once

#include <cstddef>
#include <functional>

// Fills `out` with the socket path; returns bytes written (excluding NUL) or 0
// on error.
[[nodiscard]] std::size_t ipc_socket_path(char *out, std::size_t cap);

// `modore-host --trigger` — notify a running daemon. Returns 0 on success.
[[nodiscard]] int ipc_send_pickup();

// Spawns a background accept loop; each "pickup" line invokes `on_pickup`
// (typically a non-blocking notify so the real work runs on the main thread;
// AT-SPI is not safe from the IPC thread).
void ipc_start_background(std::function<void()> on_pickup);
