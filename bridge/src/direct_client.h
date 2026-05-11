// Vendored from fcitx-contrib/fcitx5-mozc, src/unix/fcitx5/mozc_direct_client.h.
//
// Changes from upstream:
//   - Namespace: fcitx -> modeless_ime::mozc
//   - Removed inheritance from MozcClientInterface (we're the only consumer;
//     the abstract base just added a translation step).
//   - Drop the createClient() factory (we instantiate directly).
//   - Header guard renamed.
//
// Upstream license is BSD 3-clause; see bridge/NOTICE.md.

#ifndef MODELESS_IME_BRIDGE_DIRECT_CLIENT_H_
#define MODELESS_IME_BRIDGE_DIRECT_CLIENT_H_

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "composer/key_event_util.h"
#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"

namespace modeless_ime::mozc_bridge {

// In-process mozc client: no IPC, no daemon, no separate server. Directly
// drives a SessionHandler / Engine pair via SessionHandler::EvalCommand.
class MozcDirectClient {
 public:
  MozcDirectClient();
  ~MozcDirectClient();

  bool EnsureConnection() { return true; }
  bool SendKeyWithContext(const mozc::commands::KeyEvent& key,
                          const mozc::commands::Context& context,
                          mozc::commands::Output* output);
  bool SendCommandWithContext(const mozc::commands::SessionCommand& command,
                              const mozc::commands::Context& context,
                              mozc::commands::Output* output);
  bool IsDirectModeCommand(const mozc::commands::KeyEvent& key) const;
  bool GetConfig(mozc::config::Config* config);
  void set_client_capability(const mozc::commands::Capability& capability);
  bool SyncData();

 private:
  void InitRequestForSvsJapanese(bool use_svs);
  bool EnsureSession();

  enum ServerStatus {
    SERVER_INVALID_SESSION,
    SERVER_OK,
  };

  void InitInput(mozc::commands::Input* input) const;
  bool CreateSession();
  bool DeleteSession();
  bool CallCommand(mozc::commands::Input::CommandType type);
  bool EnsureCallCommand(mozc::commands::Input* input,
                         mozc::commands::Output* output);
  bool Call(const mozc::commands::Input& input, mozc::commands::Output* output);

  uint64_t id_;
  std::unique_ptr<mozc::commands::Request> request_;
  ServerStatus server_status_ = SERVER_INVALID_SESSION;
  std::vector<mozc::KeyInformation> direct_mode_keys_;
  mozc::commands::Capability client_capability_;
};

}  // namespace modeless_ime::mozc_bridge

#endif  // MODELESS_IME_BRIDGE_DIRECT_CLIENT_H_
