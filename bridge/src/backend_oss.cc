#include "backend_flow.h"
#include "backend_iface.h"
#include "mozc_bridge.h"

#include <errno.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <filesystem>
#include <string>

#include "absl/log/initialize.h"
#include "base/system_util.h"
#include "config/config_handler.h"
#include "direct_client.h"
#include "protocol/commands.pb.h"

namespace modore::mozc_bridge {
namespace {

// Mozc calls SetUserProfileDirectory directly, so the directory must exist
// before any singleton touches config or data-manager state.
void ensure_directory_tree(const std::string &absolute_dir) {
  if (absolute_dir.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(absolute_dir), ec);
  if (ec) {
    // Ignore; downstream writes surface their own failures.
  }
}

bool send_key(MozcDirectClient *client,
              const mozc::commands::KeyEvent &key,
              mozc::commands::Output *out,
              const char *step,
              std::string *error) {
  if (!client->SendKeyWithContext(
          key, mozc::commands::Context::default_instance(), out)) {
    *error = std::string("SendKey failed at step: ") + step;
    return false;
  }
  return true;
}

void reset_session_best_effort(MozcDirectClient *client) {
  mozc::commands::SessionCommand reset;
  reset.set_type(mozc::commands::SessionCommand::RESET_CONTEXT);
  mozc::commands::Output tail;
  (void)client->SendCommandWithContext(
      reset, mozc::commands::Context::default_instance(), &tail);
}

class OssSessionDriver final : public SessionDriver {
 public:
  explicit OssSessionDriver(MozcDirectClient *client) : client_(client) {}

  bool Begin(mozc::commands::Output *out, std::string *error) override {
    mozc::commands::SessionCommand reset;
    reset.set_type(mozc::commands::SessionCommand::RESET_CONTEXT);
    if (!client_->SendCommandWithContext(
            reset, mozc::commands::Context::default_instance(), out)) {
      *error = "SendCommand RESET_CONTEXT failed (pre-convert)";
      return false;
    }
    return true;
  }

  bool SendKey(const mozc::commands::KeyEvent &key,
               mozc::commands::Output *out,
               const char *step,
               std::string *error) override {
    return send_key(client_, key, out, step, error);
  }

  bool HighlightCandidate(int id,
                          mozc::commands::Output *out,
                          std::string *error) override {
    mozc::commands::SessionCommand command;
    command.set_type(mozc::commands::SessionCommand::HIGHLIGHT_CANDIDATE);
    command.set_id(id);
    if (!client_->SendCommandWithContext(
            command, mozc::commands::Context::default_instance(), out)) {
      *error = "HighlightCandidate failed";
      return false;
    }
    return true;
  }

  bool Submit(mozc::commands::Output *out, std::string *error) override {
    mozc::commands::SessionCommand command;
    command.set_type(mozc::commands::SessionCommand::SUBMIT);
    if (!client_->SendCommandWithContext(
            command, mozc::commands::Context::default_instance(), out)) {
      *error = "Submit failed";
      return false;
    }
    return true;
  }

  void Finish() override { reset_session_best_effort(client_); }

 private:
  MozcDirectClient *client_;
};

class OssBackend final : public Backend {
 public:
  explicit OssBackend(std::unique_ptr<MozcDirectClient> client)
      : client_(std::move(client)) {}

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

 private:
  std::unique_ptr<MozcDirectClient> client_;
};

std::unique_ptr<Backend> CreateOssBackend(const char *user_profile_dir,
                                          std::string *error) {
  // Mozc resolves `user://config1.db` via SystemUtil::GetUserProfileDirectory(),
  // not via MOZC_USER_PROFILE_DIRECTORY in this tree. Set the directory before
  // any Mozc singleton touches ConfigHandler.
  if (user_profile_dir && *user_profile_dir) {
    const std::string p(user_profile_dir);
    ensure_directory_tree(p);
#ifdef _WIN32
    _putenv_s("MOZC_USER_PROFILE_DIRECTORY", p.c_str());
#else
    setenv("MOZC_USER_PROFILE_DIRECTORY", p.c_str(), /*overwrite=*/1);
#endif
    mozc::SystemUtil::SetUserProfileDirectory(p);
  }

  // Quiet absl's flag-parse log spam. Calls without flags init are fine
  // for embedded use.
  absl::InitializeLog();

  std::unique_ptr<MozcDirectClient> client;
  try {
    client = std::make_unique<MozcDirectClient>();
  } catch (const std::exception &e) {
    *error = std::string("MozcDirectClient ctor threw: ") + e.what();
    return nullptr;
  } catch (...) {
    *error = "MozcDirectClient ctor threw unknown exception";
    return nullptr;
  }

  // When config1.db is missing, Reload() keeps an empty protobuf and
  // NormalizeConfig() does not restore CreateDefaultConfig()'s character-form
  // rules — conversion can silently misbehave. Seed defaults once when the
  // loaded config looks uninitialized.
  //
  // Also force `history_learning_level = NO_HISTORY`. modore drives Mozc
  // as a one-shot "convert this span" tool, not as an interactive IME —
  // every successful convert lands a synthetic ENTER, which Mozc otherwise
  // treats as "user picked this candidate" and reinforces in
  // segment_history (segment.db / boundary.db). The katakana flow makes
  // this catastrophic: a single katakana-chord press commits ニホンゴ, and
  // from then on a plain hotkey press on `nihongo` ranks ニホンゴ first — and each
  // subsequent commit reinforces it further. With NO_HISTORY, neither
  // user_history_predictor nor user_segment_history_rewriter writes,
  // and ranking stays consistent across sessions.
  {
    std::shared_ptr<const mozc::config::Config> cur =
        mozc::config::ConfigHandler::GetSharedConfig();
    mozc::config::Config seeded;
    if (!cur || cur->character_form_rules_size() == 0) {
      seeded = *mozc::config::ConfigHandler::GetSharedDefaultConfig();
    } else {
      seeded = *cur;
    }
    seeded.set_history_learning_level(mozc::config::Config::NO_HISTORY);
    mozc::config::ConfigHandler::SetConfig(std::move(seeded));
  }

  return std::make_unique<OssBackend>(std::move(client));
}

}  // namespace

int OssBackend::ConvertWithCandidatesEx(const char *romaji,
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
  OssSessionDriver driver(client_.get());
  return RunConvertFlow(&driver, romaji, romaji_len, commit_buf, commit_cap,
                        commit_len, cands_buf, cands_cap, cands_total_len,
                        max_candidates, out_candidate_count, flags, error);
}

int OssBackend::ConvertWithCandidateDetailsEx(
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
  OssSessionDriver driver(client_.get());
  return RunConvertFlowWithDetails(
      &driver, romaji, romaji_len, commit_buf, commit_cap, commit_len,
      cand_records, cand_records_cap, cand_strings_buf, cand_strings_cap,
      cand_strings_len, max_candidates, out_candidate_count, flags, error);
}

std::unique_ptr<Backend> CreateBackend(const char *requested_backend,
                                       const char *user_profile_dir,
                                       std::string *error) {
  const std::string name =
      (requested_backend && *requested_backend) ? requested_backend : "oss";
  if (name == "oss") {
    return CreateOssBackend(user_profile_dir, error);
  }

#ifdef __APPLE__
  if (name == "google-ime") {
    return CreateGoogleImeMacBackend(error);
  }
#endif

#ifdef MODORE_ENABLE_ATZC
  if (name == "atzc") {
    return CreateAtzcBackend(error);
  }
#endif

  *error = "unknown mozc backend: " + name;
  return nullptr;
}

}  // namespace modore::mozc_bridge
