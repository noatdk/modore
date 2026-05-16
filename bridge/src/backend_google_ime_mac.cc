#ifdef __APPLE__

#include "backend_flow.h"
#include "backend_iface.h"
#include "mozc_bridge.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <unistd.h>

#include "protocol/commands.pb.h"

namespace modore::mozc_bridge {
namespace {

constexpr int kProtocolVersion = 3;
constexpr char kSessionService[] =
    "com.google.inputmethod.Japanese.Converter.session";
constexpr int kTimeoutMs = 2000;

struct SendMessage {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_ool_descriptor_t data;
  mach_msg_type_number_t count;
};

struct RecvMessage {
  mach_msg_header_t header;
  mach_msg_body_t body;
  mach_msg_ool_descriptor_t data;
  mach_msg_type_number_t count;
  mach_msg_trailer_t trailer;
};

const char *BootstrapErrorString(kern_return_t kr) {
  switch (kr) {
    case BOOTSTRAP_SUCCESS:
      return "success";
    case BOOTSTRAP_NOT_PRIVILEGED:
      return "not privileged";
    case BOOTSTRAP_NAME_IN_USE:
      return "name in use";
    case BOOTSTRAP_UNKNOWN_SERVICE:
      return "unknown service";
    case BOOTSTRAP_SERVICE_ACTIVE:
      return "service active";
    case BOOTSTRAP_BAD_COUNT:
      return "bad count";
    case BOOTSTRAP_NO_MEMORY:
      return "no memory";
    case BOOTSTRAP_NO_CHILDREN:
      return "no children";
    default:
      return "unknown";
  }
}

void DestroyPort(mach_port_t port) {
  if (port != MACH_PORT_NULL) {
    mach_port_deallocate(mach_task_self(), port);
  }
}

bool CallSessionService(const std::string &request,
                        std::string *response,
                        std::string *error) {
  mach_port_t server_port = MACH_PORT_NULL;
  kern_return_t kr = bootstrap_look_up(
      bootstrap_port, const_cast<char *>(kSessionService), &server_port);
  if (kr != BOOTSTRAP_SUCCESS) {
    *error = "bootstrap_look_up(" + std::string(kSessionService) + ") failed: " +
             BootstrapErrorString(kr) + " (" + std::to_string(kr) + ")";
    return false;
  }

  mach_port_t client_port = MACH_PORT_NULL;
  kr = mach_port_allocate(
      mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &client_port);
  if (kr != KERN_SUCCESS) {
    *error = "mach_port_allocate failed: " + std::to_string(kr);
    DestroyPort(server_port);
    return false;
  }

  SendMessage send{};
  send.header.msgh_bits =
      MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND) |
      MACH_MSGH_BITS_COMPLEX;
  send.header.msgh_size = sizeof(send);
  send.header.msgh_remote_port = server_port;
  send.header.msgh_local_port = client_port;
  send.header.msgh_reserved = 0;
  send.header.msgh_id = kProtocolVersion;
  send.body.msgh_descriptor_count = 1;
  send.data.address = const_cast<char *>(request.data());
  send.data.size = static_cast<mach_msg_size_t>(request.size());
  send.data.deallocate = false;
  send.data.copy = MACH_MSG_VIRTUAL_COPY;
  send.data.type = MACH_MSG_OOL_DESCRIPTOR;
  send.count = send.data.size;

  kr = mach_msg(&send.header,
                MACH_SEND_MSG | MACH_SEND_TIMEOUT,
                send.header.msgh_size,
                0,
                MACH_PORT_NULL,
                kTimeoutMs,
                MACH_PORT_NULL);
  if (kr != MACH_MSG_SUCCESS) {
    *error = "mach_msg send failed: " + std::to_string(kr);
    DestroyPort(client_port);
    DestroyPort(server_port);
    return false;
  }

  RecvMessage recv{};
  recv.header.msgh_size = sizeof(recv);
  kr = mach_msg(&recv.header,
                MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                0,
                recv.header.msgh_size,
                client_port,
                kTimeoutMs,
                MACH_PORT_NULL);
  if (kr != MACH_MSG_SUCCESS) {
    *error = "mach_msg recv failed: " + std::to_string(kr);
    DestroyPort(client_port);
    DestroyPort(server_port);
    return false;
  }
  if (recv.header.msgh_id != kProtocolVersion) {
    *error = "unexpected Mach response id: " + std::to_string(recv.header.msgh_id);
    DestroyPort(client_port);
    DestroyPort(server_port);
    return false;
  }

  response->assign(static_cast<const char *>(recv.data.address), recv.data.size);
  vm_deallocate(mach_task_self(),
                reinterpret_cast<vm_address_t>(recv.data.address),
                recv.data.size);
  DestroyPort(client_port);
  DestroyPort(server_port);
  return true;
}

bool CallInput(const mozc::commands::Input &input,
               mozc::commands::Output *output,
               std::string *error) {
  std::string request;
  if (!input.SerializeToString(&request)) {
    *error = "failed to serialize mozc input";
    return false;
  }

  std::string response_wire;
  if (!CallSessionService(request, &response_wire, error)) {
    return false;
  }

  if (!output->ParseFromString(response_wire)) {
    *error = "failed to parse mozc output";
    return false;
  }
  if (output->error_code() != mozc::commands::Output::SESSION_SUCCESS) {
    *error = "mozc session error: " + std::to_string(output->error_code());
    return false;
  }
  return true;
}

class GoogleImeMacBackend final : public Backend {
 public:
  int ConvertWithCandidatesEx(const char *romaji,
                              size_t romaji_len,
                              char *commit_buf,
                              size_t commit_cap,
                              size_t *commit_len,
                              char *cands_buf,
                              size_t cands_cap,
                              size_t *cands_total_len,
                              int max_candidates,
                              int *out_candidate_count,
                              unsigned int flags,
                              std::string *error) override;
};

bool ProbeSession(std::string *error) {
  mozc::commands::Input create;
  create.set_type(mozc::commands::Input::CREATE_SESSION);
  create.mutable_application_info()->set_process_id(static_cast<uint32_t>(getpid()));
  create.mutable_application_info()->set_thread_id(0);
  mozc::commands::Output out;
  if (!CallInput(create, &out, error)) {
    return false;
  }

  mozc::commands::Input del;
  del.set_id(out.id());
  del.set_type(mozc::commands::Input::DELETE_SESSION);
  mozc::commands::Output ignored;
  (void)CallInput(del, &ignored, error);
  return true;
}

bool SendKey(uint64_t session_id,
             const mozc::commands::KeyEvent &key,
             mozc::commands::Output *output,
             std::string *error) {
  mozc::commands::Input input;
  input.set_id(session_id);
  input.set_type(mozc::commands::Input::SEND_KEY);
  *input.mutable_key() = key;
  return CallInput(input, output, error);
}

bool SendCommand(uint64_t session_id,
                 const mozc::commands::SessionCommand &command,
                 mozc::commands::Output *output,
                 std::string *error) {
  mozc::commands::Input input;
  input.set_id(session_id);
  input.set_type(mozc::commands::Input::SEND_COMMAND);
  *input.mutable_command() = command;
  return CallInput(input, output, error);
}

void DeleteSessionBestEffort(uint64_t session_id) {
  mozc::commands::Input input;
  input.set_id(session_id);
  input.set_type(mozc::commands::Input::DELETE_SESSION);
  mozc::commands::Output ignored;
  std::string ignored_error;
  (void)CallInput(input, &ignored, &ignored_error);
}

class GoogleImeSessionDriver final : public SessionDriver {
 public:
  GoogleImeSessionDriver() = default;

  bool Begin(mozc::commands::Output *out, std::string *error) override {
    mozc::commands::Input create;
    create.set_type(mozc::commands::Input::CREATE_SESSION);
    create.mutable_application_info()->set_process_id(
        static_cast<uint32_t>(getpid()));
    create.mutable_application_info()->set_thread_id(0);
    if (!CallInput(create, out, error)) {
      return false;
    }
    session_id_ = out->id();
    return true;
  }

  bool SendKey(const mozc::commands::KeyEvent &key,
               mozc::commands::Output *out,
               const char *step,
               std::string *error) override {
    if (!::modore::mozc_bridge::SendKey(session_id_, key, out, error)) {
      if (error->empty()) {
        *error = std::string("SendKey failed at step: ") + step;
      }
      return false;
    }
    return true;
  }

  bool HighlightCandidate(int id,
                          mozc::commands::Output *out,
                          std::string *error) override {
    mozc::commands::SessionCommand command;
    command.set_type(mozc::commands::SessionCommand::HIGHLIGHT_CANDIDATE);
    command.set_id(id);
    return SendCommand(session_id_, command, out, error);
  }

  bool Submit(mozc::commands::Output *out, std::string *error) override {
    mozc::commands::SessionCommand command;
    command.set_type(mozc::commands::SessionCommand::SUBMIT);
    return SendCommand(session_id_, command, out, error);
  }

  void Finish() override {
    if (session_id_ != 0) {
      DeleteSessionBestEffort(session_id_);
      session_id_ = 0;
    }
  }

 private:
  uint64_t session_id_ = 0;
};

}  // namespace

int GoogleImeMacBackend::ConvertWithCandidatesEx(const char *romaji,
                                                 size_t romaji_len,
                                                 char *commit_buf,
                                                 size_t commit_cap,
                                                 size_t *commit_len,
                                                 char *cands_buf,
                                                 size_t cands_cap,
                                                 size_t *cands_total_len,
                                                 int max_candidates,
                                                 int *out_candidate_count,
                                                 unsigned int flags,
                                                 std::string *error) {
  GoogleImeSessionDriver driver;
  return RunConvertFlow(&driver, romaji, romaji_len, commit_buf, commit_cap,
                        commit_len, cands_buf, cands_cap, cands_total_len,
                        max_candidates, out_candidate_count, flags, error);
}

std::unique_ptr<Backend> CreateGoogleImeMacBackend(std::string *error) {
  if (!ProbeSession(error)) {
    return nullptr;
  }
  return std::make_unique<GoogleImeMacBackend>();
}

}  // namespace modore::mozc_bridge

#endif  // __APPLE__
