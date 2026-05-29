#ifdef __APPLE__

#include "backend_flow.h"
#include "backend_iface.h"
#include "mozc_bridge.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>

#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <unistd.h>

#include "protocol/commands.pb.h"
#include "protocol/config.pb.h"

namespace modore::mozc_bridge {
namespace {

constexpr int kProtocolVersion = 3;
constexpr char kSessionService[] =
    "com.google.inputmethod.Japanese.Converter.session";
constexpr int kTimeoutMs = 2000;

mozc::commands::Request BuildDefaultRequest() {
  mozc::commands::Request request;
  mozc::commands::DecoderExperimentParams params;
  uint32_t variation_types = params.variation_character_types();
  variation_types |= mozc::commands::DecoderExperimentParams::SVS_JAPANESE;
  request.mutable_decoder_experiment_params()->set_variation_character_types(
      variation_types);
  request.set_zero_query_suggestion(true);
  request.set_mixed_conversion(true);
  request.set_update_input_mode_from_surrounding_text(false);

  if (const char *candidate_mixing_mode = std::getenv(
          "MODORE_MOZC_CANDIDATE_MIXING_MODE");
      candidate_mixing_mode != nullptr && candidate_mixing_mode[0] != '\0') {
    char *end = nullptr;
    const long value = std::strtol(candidate_mixing_mode, &end, 10);
    if (end != candidate_mixing_mode && end != nullptr && *end == '\0' &&
        value >= 0 && value <= std::numeric_limits<int32_t>::max()) {
      request.mutable_decoder_experiment_params()->set_candidate_mixing_mode(
          static_cast<int32_t>(value));
    }
  }
  return request;
}

mozc::commands::Context BuildDefaultContext() {
  mozc::commands::Context context;
  context.set_input_field_type(mozc::commands::Context::NORMAL);
  // Mozc treats revision as a typing-session identifier. modore creates a
  // fresh bridge session for each conversion, so a stable non-zero revision is
  // enough to mirror the frontend's "new typing session" shape.
  context.set_revision(1);
  return context;
}

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

bool CallInputWithContext(const mozc::commands::Input &input,
                          const mozc::commands::Context &context,
                          mozc::commands::Output *output,
                          std::string *error) {
  mozc::commands::Input with_context = input;
  *with_context.mutable_context() = context;
  return CallInput(with_context, output, error);
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

  int ConvertWithCandidateDetailsEx(
      const char *romaji,
      size_t romaji_len,
      char *commit_buf,
      size_t commit_cap,
      size_t *commit_len,
      mozc_bridge_candidate_record_t *cand_records,
      size_t cand_records_cap,
      char *cand_strings_buf,
      size_t cand_strings_cap,
      size_t *cand_strings_len,
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
  *create.mutable_context() = BuildDefaultContext();
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
  GoogleImeSessionDriver()
      : request_(BuildDefaultRequest()),
        context_(BuildDefaultContext()) {}

  ~GoogleImeSessionDriver() override {
    // Net for Begin()'s partial-failure window: RunConvertFlow only calls
    // Finish() on its in-body paths, so if Begin() creates the session
    // (CREATE_SESSION ok) but then fails at SET_REQUEST, the flow returns
    // -1 before Finish() and the external IME session would leak until its
    // own timeout. Finish() guards on session_id_ and zeroes it, so the
    // normal end-of-flow call already cleaned up — this is a no-op on the
    // success path. Destructors are noexcept; never let an IPC/protobuf
    // exception escape during unwinding.
    try {
      Finish();
    } catch (...) {
    }
  }

  bool Begin(mozc::commands::Output *out, std::string *error) override {
    mozc::commands::Input create;
    create.set_type(mozc::commands::Input::CREATE_SESSION);
    create.mutable_application_info()->set_process_id(
        static_cast<uint32_t>(getpid()));
    create.mutable_application_info()->set_thread_id(0);
    *create.mutable_context() = context_;
    if (!CallInput(create, out, error)) {
      return false;
    }
    session_id_ = out->id();
    if (session_id_ != 0) {
      mozc::commands::Input request;
      request.set_id(session_id_);
      request.set_type(mozc::commands::Input::SET_REQUEST);
      *request.mutable_request() = request_;
      mozc::commands::Output ignored;
      if (!CallInputWithContext(request, context_, &ignored, error)) {
        return false;
      }
    }
    return true;
  }

  bool SendKey(const mozc::commands::KeyEvent &key,
               mozc::commands::Output *out,
               const char *step,
               std::string *error) override {
    mozc::commands::Input input;
    input.set_id(session_id_);
    input.set_type(mozc::commands::Input::SEND_KEY);
    *input.mutable_key() = key;
    if (!CallInputWithContext(input, context_, out, error)) {
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
    mozc::commands::Input input;
    input.set_id(session_id_);
    input.set_type(mozc::commands::Input::SEND_COMMAND);
    *input.mutable_command() = command;
    return CallInputWithContext(input, context_, out, error);
  }

  bool Submit(mozc::commands::Output *out, std::string *error) override {
    mozc::commands::SessionCommand command;
    command.set_type(mozc::commands::SessionCommand::SUBMIT);
    mozc::commands::Input input;
    input.set_id(session_id_);
    input.set_type(mozc::commands::Input::SEND_COMMAND);
    *input.mutable_command() = command;
    return CallInputWithContext(input, context_, out, error);
  }

  void Finish() override {
    if (session_id_ != 0) {
      // Send REVERT before deleting so the renderer process receives a
      // "no candidate window" update and closes its window. Without this,
      // the renderer can leave the window on screen after the session is gone.
      mozc::commands::SessionCommand revert;
      revert.set_type(mozc::commands::SessionCommand::REVERT);
      mozc::commands::Input revert_input;
      revert_input.set_id(session_id_);
      revert_input.set_type(mozc::commands::Input::SEND_COMMAND);
      *revert_input.mutable_command() = revert;
      mozc::commands::Output ignored;
      std::string ignored_error;
      (void)CallInputWithContext(revert_input, context_, &ignored, &ignored_error);
      DeleteSessionBestEffort(session_id_);
      session_id_ = 0;
    }
  }

 private:
  uint64_t session_id_ = 0;
  mozc::commands::Request request_;
  mozc::commands::Context context_;
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

int GoogleImeMacBackend::ConvertWithCandidateDetailsEx(
    const char *romaji,
    size_t romaji_len,
    char *commit_buf,
    size_t commit_cap,
    size_t *commit_len,
    mozc_bridge_candidate_record_t *cand_records,
    size_t cand_records_cap,
    char *cand_strings_buf,
    size_t cand_strings_cap,
    size_t *cand_strings_len,
    int max_candidates,
    int *out_candidate_count,
    unsigned int flags,
    std::string *error) {
  GoogleImeSessionDriver driver;
  return RunConvertFlowWithDetails(
      &driver, romaji, romaji_len, commit_buf, commit_cap, commit_len,
      cand_records, cand_records_cap, cand_strings_buf, cand_strings_cap,
      cand_strings_len, max_candidates, out_candidate_count, flags, error);
}

std::unique_ptr<Backend> CreateGoogleImeMacBackend(std::string *error) {
  if (!ProbeSession(error)) {
    return nullptr;
  }
  return std::make_unique<GoogleImeMacBackend>();
}

}  // namespace modore::mozc_bridge

#endif  // __APPLE__
