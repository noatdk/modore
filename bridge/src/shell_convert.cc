// Shell-native line conversion helpers.
//
// This keeps the shell-facing buffer rewrite logic in the bridge layer, so
// macOS Swift only needs a thin CLI wrapper around a C ABI call.

#include "mozc_bridge.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <thread>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

static size_t clamp_to_utf8_boundary(const char* s, size_t len, size_t pos) {
    if (pos > len) pos = len;
    while (pos > 0 && pos < len && (((unsigned char)s[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

static size_t utf8_next(const char* s, size_t len, size_t pos) {
    if (pos >= len) return len;
    ++pos;
    while (pos < len && (((unsigned char)s[pos]) & 0xC0) == 0x80) ++pos;
    return pos;
}

static size_t utf8_prev(const char* s, size_t pos) {
    if (pos == 0) return 0;
    --pos;
    while (pos > 0 && (((unsigned char)s[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

static int is_ascii_ws_at(const char* s, size_t pos) {
    unsigned char c = (unsigned char)s[pos];
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int is_ascii_at(const char* s, size_t pos) {
    return ((unsigned char)s[pos]) < 0x80;
}

static unsigned int utf8_codepoint_at(const char* s, size_t len, size_t pos) {
    if (pos >= len) return 0;
    unsigned char c0 = (unsigned char)s[pos];
    if (c0 < 0x80) return c0;
    if ((c0 & 0xE0) == 0xC0 && pos + 1 < len) {
        unsigned char c1 = (unsigned char)s[pos + 1];
        if ((c1 & 0xC0) == 0x80) {
            return ((unsigned int)(c0 & 0x1F) << 6) |
                   (unsigned int)(c1 & 0x3F);
        }
    }
    if ((c0 & 0xF0) == 0xE0 && pos + 2 < len) {
        unsigned char c1 = (unsigned char)s[pos + 1];
        unsigned char c2 = (unsigned char)s[pos + 2];
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80) {
            return ((unsigned int)(c0 & 0x0F) << 12) |
                   ((unsigned int)(c1 & 0x3F) << 6) |
                   (unsigned int)(c2 & 0x3F);
        }
    }
    if ((c0 & 0xF8) == 0xF0 && pos + 3 < len) {
        unsigned char c1 = (unsigned char)s[pos + 1];
        unsigned char c2 = (unsigned char)s[pos + 2];
        unsigned char c3 = (unsigned char)s[pos + 3];
        if ((c1 & 0xC0) == 0x80 && (c2 & 0xC0) == 0x80 &&
            (c3 & 0xC0) == 0x80) {
            return ((unsigned int)(c0 & 0x07) << 18) |
                   ((unsigned int)(c1 & 0x3F) << 12) |
                   ((unsigned int)(c2 & 0x3F) << 6) |
                   (unsigned int)(c3 & 0x3F);
        }
    }
    return 0;
}

static int is_japanese_at(const char* s, size_t len, size_t pos) {
    unsigned int cp = utf8_codepoint_at(s, len, pos);
    return (cp >= 0x3040 && cp <= 0x30FF) ||
           (cp >= 0x3400 && cp <= 0x9FFF) ||
           (cp >= 0xF900 && cp <= 0xFAFF);
}

static int is_convertible_ascii_punctuation(unsigned char c) {
    return c == '.' || c == ',' || c == '-';
}

static int japanese_punctuation_span_at(
    const char* text, size_t len, size_t punct_pos,
    size_t* start_out, size_t* end_out) {
    if (punct_pos >= len ||
        !is_convertible_ascii_punctuation((unsigned char)text[punct_pos]) ||
        punct_pos == 0) {
        return 0;
    }

    size_t prev = utf8_prev(text, punct_pos);
    size_t next = utf8_next(text, len, punct_pos);
    if (!is_japanese_at(text, len, prev)) return 0;
    if (next < len && !is_japanese_at(text, len, next)) return 0;

    *start_out = punct_pos;
    *end_out = next;
    return 1;
}

static int is_ascii_alnum(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9');
}

static int is_upper_ascii(unsigned char c) { return c >= 'A' && c <= 'Z'; }
static int is_lower_ascii(unsigned char c) { return c >= 'a' && c <= 'z'; }
static int is_digit_ascii(unsigned char c) { return c >= '0' && c <= '9'; }

static int is_acronym_symbol(unsigned char c) {
    switch (c) {
        case '&': case '-': case '.': case '_': case '+':
        case '/': case ':': case '@': case '#':
            return 1;
        default:
            return 0;
    }
}

static int text_word_bounds(
    const char* text, size_t len, size_t caret_byte,
    size_t* start_out, size_t* end_out) {
    if (!text || !start_out || !end_out) return -1;
    size_t caret = clamp_to_utf8_boundary(text, len, caret_byte);

    if (caret < len && japanese_punctuation_span_at(text, len, caret, start_out, end_out)) {
        return 0;
    }
    if (caret < len && is_japanese_at(text, len, caret)) {
        size_t next = utf8_next(text, len, caret);
        if (japanese_punctuation_span_at(text, len, next, start_out, end_out)) {
            return 0;
        }
    }
    if (caret > 0) {
        size_t prev = utf8_prev(text, caret);
        if (japanese_punctuation_span_at(text, len, prev, start_out, end_out)) {
            return 0;
        }
    }

    size_t start = caret;
    while (start > 0) {
        size_t prev = utf8_prev(text, start);
        if (is_ascii_ws_at(text, prev)) break;
        if (start < len && is_ascii_at(text, prev) != is_ascii_at(text, start)) break;
        start = prev;
    }

    size_t end = caret;
    while (end < len) {
        if (is_ascii_ws_at(text, end)) break;
        if (end > 0) {
            size_t prev = utf8_prev(text, end);
            if (is_ascii_at(text, prev) != is_ascii_at(text, end)) break;
        }
        end = utf8_next(text, len, end);
    }

    if (start == end) {
        if (caret < len &&
            is_convertible_ascii_punctuation((unsigned char)text[caret]) &&
            caret > 0) {
            size_t prev = utf8_prev(text, caret);
            if (is_japanese_at(text, len, prev)) {
                *start_out = prev;
                *end_out = caret;
                return 0;
            }
        }
        if (caret < len) {
            start = caret;
            end = utf8_next(text, len, caret);
        } else if (caret > 0) {
            start = utf8_prev(text, caret);
            end = caret;
        }
    }

    *start_out = start;
    *end_out = end;
    return 0;
}

static bool split_trailing_ascii(const std::string& s, std::string* prefix, std::string* tail) {
    size_t split = s.size();
    while (split > 0) {
        unsigned char c = static_cast<unsigned char>(s[split - 1]);
        if (c >= 0x80) break;
        --split;
    }
    *prefix = s.substr(0, split);
    *tail = s.substr(split);
    return true;
}

static bool split_trailing_ascii_punctuation(const std::string& ascii, std::string* core, std::string* suffix) {
    size_t split = ascii.size();
    while (split > 0) {
        unsigned char c = static_cast<unsigned char>(ascii[split - 1]);
        if (c >= 0x80) break;
        if (is_ascii_alnum(c)) break;
        --split;
    }
    *core = ascii.substr(0, split);
    *suffix = ascii.substr(split);
    return true;
}

static bool split_leading_ascii_junk_before_lowercase(
    const std::string& ascii, std::string* prefix, std::string* tail) {
    if (ascii.empty()) {
        *prefix = "";
        *tail = "";
        return true;
    }
    size_t split = 0;
    while (split < ascii.size()) {
        unsigned char c = static_cast<unsigned char>(ascii[split]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            break;
        }
        ++split;
    }
    if (split == ascii.size()) {
        *prefix = ascii;
        *tail = "";
        return true;
    }
    unsigned char first = static_cast<unsigned char>(ascii[split]);
    if (!(first >= 'a' && first <= 'z')) {
        *prefix = "";
        *tail = ascii;
        return true;
    }
    *prefix = ascii.substr(0, split);
    *tail = ascii.substr(split);
    return true;
}

static bool split_acronym_head(const std::string& text, std::string* head, std::string* tail) {
    if (text.size() < 2 || !is_upper_ascii(static_cast<unsigned char>(text[0]))) {
        *head = "";
        *tail = text;
        return true;
    }
    size_t i = 1;
    bool saw_non_letter = false;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (is_upper_ascii(c)) {
            ++i;
        } else if (is_digit_ascii(c) || is_acronym_symbol(c)) {
            saw_non_letter = true;
            ++i;
        } else {
            break;
        }
    }
    if (i >= 2 && i < text.size() && is_lower_ascii(static_cast<unsigned char>(text[i])) &&
        (i >= 3 || saw_non_letter)) {
        *head = text.substr(0, i);
        *tail = text.substr(i);
    } else {
        *head = "";
        *tail = text;
    }
    return true;
}

static std::string convert_suffix(const std::string& suffix, unsigned int flags) {
    std::string out;
    for (unsigned char c : suffix) {
        if (c == '.' || c == ',' || c == '-') {
            char in[2] = { static_cast<char>(c), '\0' };
            char buf[32] = {0};
            size_t out_len = 0;
            if (mozc_bridge_convert_ex(in, 1, buf, sizeof(buf), &out_len, flags) == 0 && out_len > 0) {
                out.append(buf, out_len);
            } else {
                switch (c) {
                    case '.': out += "。"; break;
                    case ',': out += "、"; break;
                    case '-': out += "ー"; break;
                }
            }
        } else {
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

static bool convert_token(
    const std::string& token, unsigned int flags, std::string* replacement) {
    std::string ascii_prefix, romaji_tail;
    split_trailing_ascii(token, &ascii_prefix, &romaji_tail);

    std::string romaji_core, romaji_suffix;
    split_trailing_ascii_punctuation(romaji_tail, &romaji_core, &romaji_suffix);
    const std::string converted_suffix = convert_suffix(romaji_suffix, flags);

    if (romaji_core.empty()) {
        *replacement = ascii_prefix + converted_suffix;
        return *replacement != token;
    }

    std::string leading_junk, romaji_body;
    split_leading_ascii_junk_before_lowercase(romaji_core, &leading_junk, &romaji_body);

    std::string acronym_head, mozc_input;
    split_acronym_head(romaji_body, &acronym_head, &mozc_input);
    const std::string frozen_prefix = ascii_prefix + leading_junk + acronym_head;

    if (mozc_input.empty()) {
        *replacement = frozen_prefix + converted_suffix;
        return *replacement != token;
    }

    char commit_buf[4096] = {0};
    size_t commit_len = 0;
    const int rc = mozc_bridge_convert_ex(
        mozc_input.c_str(), mozc_input.size(),
        commit_buf, sizeof(commit_buf), &commit_len, flags);
    if (rc != 0) {
        return false;
    }
    *replacement = frozen_prefix + std::string(commit_buf, commit_len) + converted_suffix;
    return *replacement != token;
}

static int copy_output(const std::string& s, char* out_buf, size_t out_cap, size_t* out_len) {
    if (out_cap == 0) return -1;
    const size_t n = s.size();
    if (n >= out_cap) {
        return static_cast<int>(n + 1);
    }
    std::memcpy(out_buf, s.data(), n);
    out_buf[n] = '\0';
    *out_len = n;
    return 0;
}

enum class ShellBootstrapTarget {
    kBash,
    kZsh,
    kFish,
};

static std::string shell_single_quote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    out.push_back('\'');
    for (char ch : s) {
        if (ch == '\'') {
            out.append("'\\''");
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

static ShellBootstrapTarget detect_shell_bootstrap_target() {
    if (const char* shell = std::getenv("MODORE_SHELL")) {
        if (std::strcmp(shell, "fish") == 0) return ShellBootstrapTarget::kFish;
        if (std::strcmp(shell, "zsh") == 0) return ShellBootstrapTarget::kZsh;
        if (std::strcmp(shell, "bash") == 0) return ShellBootstrapTarget::kBash;
    }
    if (std::getenv("FISH_VERSION")) return ShellBootstrapTarget::kFish;
    if (std::getenv("ZSH_VERSION")) return ShellBootstrapTarget::kZsh;
    return ShellBootstrapTarget::kBash;
}

struct ShellSession {
    std::string prefix;
    std::string suffix;
    std::vector<std::string> candidates;
    size_t current_index = 0;
    std::chrono::steady_clock::time_point last_touch;

    std::string current_text() const {
        if (candidates.empty()) {
            return prefix + suffix;
        }
        size_t idx = current_index < candidates.size() ? current_index : 0;
        return prefix + candidates[idx] + suffix;
    }
};

enum class ShellConvertMode {
    kPrimary,
    kKatakana,
};

static std::mutex g_shell_session_mutex;
static std::unordered_map<std::string, ShellSession> g_shell_sessions;
static uint64_t g_next_shell_session_id = 1;

static constexpr auto kShellSessionTtl = std::chrono::minutes(30);

static void purge_expired_shell_sessions_locked(
    std::chrono::steady_clock::time_point now) {
    for (auto it = g_shell_sessions.begin(); it != g_shell_sessions.end();) {
        const auto age = now - it->second.last_touch;
        if (age > kShellSessionTtl) {
            it = g_shell_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

static std::string shell_session_id_locked() {
    return std::to_string(g_next_shell_session_id++);
}

static std::vector<std::string> split_nul_list(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t next = s.find('\0', pos);
        if (next == std::string::npos) {
            out.push_back(s.substr(pos));
            break;
        }
        out.push_back(s.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

static std::optional<std::pair<size_t, size_t>> shell_token_bounds(
    const std::string& text,
    size_t caret_byte) {
    size_t start = 0;
    size_t end = 0;
    if (text_word_bounds(text.c_str(), text.size(), caret_byte, &start, &end) != 0) {
        return std::nullopt;
    }
    return std::make_pair(start, end);
}

static std::optional<std::string> shell_convert_token(
    const std::string& token,
    unsigned int flags,
    std::vector<std::string>* candidates_out) {
    if (candidates_out) candidates_out->clear();

    char commit_buf[4096] = {0};
    size_t commit_len = 0;
    char cand_buf[16384] = {0};
    size_t cand_total_len = 0;
    int candidate_count = 0;
    int rc = mozc_bridge_convert_with_candidates_ex(
        token.c_str(),
        token.size(),
        commit_buf,
        sizeof(commit_buf),
        &commit_len,
        cand_buf,
        sizeof(cand_buf),
        &cand_total_len,
        16,
        &candidate_count,
        flags);
    if (rc != 0) {
        if (rc > 0) {
            std::vector<char> bigger(static_cast<size_t>(rc), '\0');
            rc = mozc_bridge_convert_with_candidates_ex(
                token.c_str(),
                token.size(),
                bigger.data(),
                bigger.size(),
                &commit_len,
                cand_buf,
                sizeof(cand_buf),
                &cand_total_len,
                16,
                &candidate_count,
                flags);
            if (rc != 0) return std::nullopt;
        } else {
            return std::nullopt;
        }
    }

    std::string committed(commit_buf, commit_len);
    if (candidates_out) {
        for (const std::string& cand : split_nul_list(std::string(cand_buf, cand_total_len))) {
            if (!cand.empty()) {
                candidates_out->push_back(cand);
            }
        }
        if (candidates_out->empty() || candidates_out->front() != committed) {
            candidates_out->insert(candidates_out->begin(), committed);
        }
    }
    return committed;
}

// Find the token around `caret_byte`, convert it, and build a fresh
// ShellSession (current_index 0, last_touch = now). Returns nullopt when
// there's no token or the conversion failed. The three session entry points
// share this; each keeps its own existing-session handling and how it assigns
// the id / index before storing. Must be called under g_shell_session_mutex
// (it calls shell_convert_token, matching the prior in-lock behavior).
static std::optional<ShellSession> shell_build_session(
    ShellConvertMode mode,
    const std::string& text,
    size_t caret_byte,
    std::chrono::steady_clock::time_point now) {
    auto bounds = shell_token_bounds(text, caret_byte);
    if (!bounds.has_value()) return std::nullopt;
    const auto [start, end] = *bounds;
    const std::string token = text.substr(start, end - start);

    std::vector<std::string> candidates;
    const unsigned int flags =
        mode == ShellConvertMode::kKatakana ? MOZC_CONVERT_FLAG_KATAKANA : 0u;
    std::optional<std::string> committed = shell_convert_token(token, flags, &candidates);
    if (!committed.has_value()) return std::nullopt;

    ShellSession session;
    session.prefix = text.substr(0, start);
    session.suffix = text.substr(end);
    session.candidates = std::move(candidates);
    session.current_index = 0;
    session.last_touch = now;
    return session;
}

static std::optional<std::pair<std::string, std::string>> shell_start_or_cycle(
    const std::string& session_id_in,
    ShellConvertMode mode,
    const std::string& text,
    size_t caret_byte) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_shell_session_mutex);
    purge_expired_shell_sessions_locked(now);

    if (!session_id_in.empty()) {
        auto it = g_shell_sessions.find(session_id_in);
        if (it != g_shell_sessions.end()) {
            ShellSession& session = it->second;
            if (session.current_text() == text) {
                if (!session.candidates.empty()) {
                    if (mode == ShellConvertMode::kKatakana) {
                        session.current_index =
                            (session.current_index + session.candidates.size() - 1) %
                            session.candidates.size();
                    } else {
                        session.current_index = (session.current_index + 1) % session.candidates.size();
                    }
                }
                session.last_touch = now;
                return std::make_pair(session_id_in, session.current_text());
            }
        }
    }

    auto built = shell_build_session(mode, text, caret_byte, now);
    if (!built.has_value()) return std::nullopt;
    ShellSession session = std::move(*built);

    std::string session_id = session_id_in.empty() ? shell_session_id_locked() : session_id_in;
    g_shell_sessions[session_id] = session;
    return std::make_pair(session_id, session.current_text());
}

static std::optional<std::pair<std::string, ShellSession>> shell_ensure_session(
    const std::string& session_id_in,
    ShellConvertMode mode,
    const std::string& text,
    size_t caret_byte) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_shell_session_mutex);
    purge_expired_shell_sessions_locked(now);

    if (!session_id_in.empty()) {
        auto it = g_shell_sessions.find(session_id_in);
        if (it != g_shell_sessions.end()) {
            ShellSession& session = it->second;
            if (session.current_text() == text) {
                session.last_touch = now;
                return std::make_pair(session_id_in, session);
            }
        }
    }

    auto built = shell_build_session(mode, text, caret_byte, now);
    if (!built.has_value()) return std::nullopt;
    ShellSession session = std::move(*built);

    std::string session_id = session_id_in.empty() ? shell_session_id_locked() : session_id_in;
    g_shell_sessions[session_id] = session;
    return std::make_pair(session_id, session);
}

static std::optional<std::pair<std::string, ShellSession>> shell_select_candidate(
    const std::string& session_id_in,
    ShellConvertMode mode,
    const std::string& text,
    size_t caret_byte,
    size_t selected_index) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(g_shell_session_mutex);
    purge_expired_shell_sessions_locked(now);

    if (!session_id_in.empty()) {
        auto it = g_shell_sessions.find(session_id_in);
        if (it != g_shell_sessions.end()) {
            ShellSession& session = it->second;
            if (session.current_text() == text) {
                if (session.candidates.empty() || selected_index >= session.candidates.size()) {
                    return std::nullopt;
                }
                session.current_index = selected_index;
                session.last_touch = now;
                return std::make_pair(session_id_in, session);
            }
        }
    }
    auto built = shell_build_session(mode, text, caret_byte, now);
    if (!built.has_value()) return std::nullopt;
    ShellSession session = std::move(*built);
    if (session.candidates.empty() || selected_index >= session.candidates.size()) {
        return std::nullopt;
    }
    session.current_index = selected_index;

    std::string session_id = session_id_in.empty() ? shell_session_id_locked() : session_id_in;
    g_shell_sessions[session_id] = session;
    return std::make_pair(session_id, session);
}

static std::string shell_candidates_text(const ShellSession& session) {
    std::string out;
    for (size_t i = 0; i < session.candidates.size(); ++i) {
        if (i > 0) out.push_back('\n');
        out.append(std::to_string(i));
        out.push_back('\t');
        out.append(session.candidates[i]);
    }
    return out;
}

static bool write_all_fd(int fd, const char* data, size_t len);
static bool read_all_fd(int fd, std::string* out);
static bool connect_unix_socket(const std::string& socket_path, int* fd_out);
static int shell_request_remote(const char *socket_path,
                                const char *session_id_in,
                                const char *mode_in,
                                const char *action,
                                const std::string& payload,
                                const char *text,
                                size_t text_len,
                                size_t caret_byte,
                                char *out_buf,
                                size_t out_cap,
                                size_t *out_len) {
    if (!socket_path || !text || !out_buf || !out_len || out_cap == 0) {
        mozc_bridge_set_error("invalid args");
        return -1;
    }
    int fd = -1;
    if (!connect_unix_socket(socket_path, &fd)) {
        mozc_bridge_set_error("failed to connect shell convert socket");
        return -1;
    }
    std::string request = session_id_in ? session_id_in : "";
    request.push_back('\0');
    request.append(std::to_string(caret_byte));
    request.push_back('\0');
    request.append(mode_in ? mode_in : "");
    request.push_back('\0');
    request.append(action ? action : "");
    request.push_back('\0');
    request.append(payload);
    request.push_back('\0');
    request.append(text, text_len);
    if (!write_all_fd(fd, request.data(), request.size())) {
        ::close(fd);
        mozc_bridge_set_error("failed to write shell request");
        return -1;
    }
    ::shutdown(fd, SHUT_WR);
    std::string response;
    bool ok = read_all_fd(fd, &response);
    ::close(fd);
    if (!ok) {
        mozc_bridge_set_error("failed to read shell response");
        return -1;
    }
    return copy_output(response, out_buf, out_cap, out_len);
}

static bool write_all_fd(int fd, const char* data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = send(fd, data + written, len - written, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

static bool read_all_fd(int fd, std::string* out) {
    char buf[4096];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            out->append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

static bool connect_unix_socket(const std::string& socket_path, int* fd_out) {
    if (!fd_out) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        close(fd);
        errno = ENAMETOOLONG;
        return false;
    }
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());

    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        int err = errno;
        close(fd);
        errno = err;
        return false;
    }
    *fd_out = fd;
    return true;
}

struct ShellConvertServerState {
    std::string socket_path;
    int listen_fd = -1;
    std::thread worker;
    bool running = false;
};

static std::mutex g_shell_server_mutex;
static std::unique_ptr<ShellConvertServerState> g_shell_server;

static std::optional<std::string> handle_shell_request(const std::string& request) {
    size_t first = request.find('\0');
    if (first == std::string::npos) return std::nullopt;
    size_t second = request.find('\0', first + 1);
    if (second == std::string::npos) return std::nullopt;
    size_t third = request.find('\0', second + 1);
    if (third == std::string::npos) return std::nullopt;
    size_t fourth = request.find('\0', third + 1);
    if (fourth == std::string::npos) return std::nullopt;
    size_t fifth = request.find('\0', fourth + 1);

    const std::string session_id_in = request.substr(0, first);
    const std::string caret_s = request.substr(first + 1, second - first - 1);
    const std::string mode_s = request.substr(second + 1, third - second - 1);
    const std::string action_s = request.substr(third + 1, fourth - third - 1);
    const std::string payload = request.substr(fourth + 1);
    const std::string text = fifth == std::string::npos ? payload : request.substr(fifth + 1);

    size_t caret = 0;
    try {
        caret = static_cast<size_t>(std::stoul(caret_s));
    } catch (...) {
        return std::nullopt;
    }

    ShellConvertMode mode = ShellConvertMode::kPrimary;
    if (mode_s == "katakana") {
        mode = ShellConvertMode::kKatakana;
    }

    if (action_s == "convert") {
        auto result = shell_start_or_cycle(session_id_in, mode, text, caret);
        if (!result.has_value()) return std::nullopt;
        return result->first + "\n" + result->second;
    }
    if (action_s == "candidates") {
        auto result = shell_ensure_session(session_id_in, mode, text, caret);
        if (!result.has_value()) return std::nullopt;
        return result->first + "\n" + std::to_string(result->second.current_index) +
               "\n" + shell_candidates_text(result->second);
    }
    if (action_s == "select") {
        size_t selected_index = 0;
        try {
            selected_index = static_cast<size_t>(std::stoul(payload));
        } catch (...) {
            return std::nullopt;
        }
        auto result = shell_select_candidate(session_id_in, mode, text, caret, selected_index);
        if (!result.has_value()) return std::nullopt;
        return result->first + "\n" + result->second.current_text();
    }
    return std::nullopt;
}

static void shell_server_worker(ShellConvertServerState* state) {
    while (state->running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(state->listen_fd, &rfds);
        timeval tv{};
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        int ready = select(state->listen_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (ready == 0) continue;
        if (!FD_ISSET(state->listen_fd, &rfds)) continue;
        while (true) {
            int client_fd = accept(state->listen_fd, nullptr, nullptr);
            if (client_fd < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                break;
            }
            std::string request;
            if (read_all_fd(client_fd, &request)) {
                auto response = handle_shell_request(request);
                if (response.has_value()) {
                    (void)write_all_fd(client_fd, response->data(), response->size());
                }
            }
            close(client_fd);
        }
    }
}

static void replace_all(std::string* s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s->find(from, pos)) != std::string::npos) {
        s->replace(pos, from.size(), to);
        pos += to.size();
    }
}

// Substitutes the snippet placeholders. `picker_default` and
// `candidate_window` are pre-sanitized by the caller (a fixed enum word and
// "0"/"1"), so they go in unquoted; only the host path needs shell quoting.
static std::string render_shell_template(
    std::string tpl,
    const std::string& host,
    const std::string& socket_path,
    const std::string& picker_default,
    const std::string& candidate_window) {
    replace_all(&tpl, "{{HOST}}", shell_single_quote(host));
    replace_all(&tpl, "{{SOCKET}}", shell_single_quote(socket_path));
    replace_all(&tpl, "{{PICKER_DEFAULT}}", picker_default);
    replace_all(&tpl, "{{CANDIDATE_WINDOW}}", candidate_window);
    return tpl;
}

static constexpr const char* kBashShellBootstrapTemplate = R"SH(
# Generated by modore-host. Source this from your shell startup file.
# Where the resident host listens; the lean client reads this.
export MODORE_SHELL_SOCKET={{SOCKET}}
# Candidate picker: honors MODORE_SHELL_PICKER (fzf|gum|numeric), else
# auto-detects fzf, then gum, then a dependency-free numbered prompt.
modore_shell_pick_candidate() {
  local candidates="$1" picker="${MODORE_SHELL_PICKER:-{{PICKER_DEFAULT}}}"
  case "$picker" in
    fzf|gum|numeric) ;;
    *) picker="" ;;
  esac
  if [ -z "$picker" ]; then
    if command -v fzf >/dev/null 2>&1; then
      picker=fzf
    elif command -v gum >/dev/null 2>&1; then
      picker=gum
    else
      picker=numeric
    fi
  fi
  case "$picker" in
    fzf)
      printf '%s\n' "$candidates" | fzf --height 40% --reverse --no-sort --delimiter=$'\t' --with-nth=2..
      ;;
    gum)
      printf '%s\n' "$candidates" | gum choose --height 15
      ;;
    numeric)
      local line n=1 sel
      while IFS= read -r line; do
        [ -z "$line" ] && continue
        printf '%2d) %s\n' "$n" "${line#*$'\t'}" >&2
        n=$((n + 1))
      done <<< "$candidates"
      printf 'modore> ' >&2
      IFS= read -r sel </dev/tty || return 1
      case "$sel" in
        ''|*[!0-9]*) return 1 ;;
      esac
      printf '%s\n' "$candidates" | sed -n "${sel}p"
      ;;
  esac
}
modore_shell_choose_candidate() {
  local candidate_response candidate_session candidate_lines choice chosen_index response session converted
  candidate_response="$(MODORE_SHELL_SESSION="${MODORE_SHELL_SESSION-}" {{HOST}} --shell-candidates --caret "$READLINE_POINT" <<< "$READLINE_LINE")" || return 1
  candidate_session=${candidate_response%%$'\n'*}
  candidate_response=${candidate_response#*$'\n'}
  candidate_response=${candidate_response#*$'\n'}
  candidate_lines=$candidate_response
  choice="$(modore_shell_pick_candidate "$candidate_lines")" || return 1
  chosen_index=${choice%%$'\t'*}
  response="$(MODORE_SHELL_SESSION="$candidate_session" {{HOST}} --shell-select --candidate-index "$chosen_index" --caret "$READLINE_POINT" <<< "$READLINE_LINE")" || return 1
  session=${response%%$'\n'*}
  if [ "$session" = "$response" ]; then
    return 1
  fi
  converted=${response#*$'\n'}
  MODORE_SHELL_SESSION="$session"
  READLINE_LINE="$converted"
  READLINE_POINT=${#READLINE_LINE}
}
modore_shell_convert() {
  local response session converted
  response="$(MODORE_SHELL_SESSION="${MODORE_SHELL_SESSION-}" {{HOST}} --shell-convert --caret "$READLINE_POINT" <<< "$READLINE_LINE")" || {
    return 1
  }
  session=${response%%$'\n'*}
  if [ "$session" = "$response" ]; then
    return 1
  fi
  converted=${response#*$'\n'}
  MODORE_SHELL_SESSION="$session"
  READLINE_LINE="$converted"
  READLINE_POINT=${#READLINE_LINE}
}
modore_shell_convert_katakana() {
  local response session converted
  response="$(MODORE_SHELL_SESSION="${MODORE_SHELL_SESSION-}" {{HOST}} --shell-convert --katakana --caret "$READLINE_POINT" <<< "$READLINE_LINE")" || {
    return 1
  }
  session=${response%%$'\n'*}
  if [ "$session" = "$response" ]; then
    return 1
  fi
  converted=${response#*$'\n'}
  MODORE_SHELL_SESSION="$session"
  READLINE_LINE="$converted"
  READLINE_POINT=${#READLINE_LINE}
}
bind -x "\"\C-x\C-j\":modore_shell_convert"
bind -x "\"\C-x\C-l\":modore_shell_choose_candidate"
bind -x "\"\C-x\C-k\":modore_shell_convert_katakana"
)SH";

static constexpr const char* kZshShellBootstrapTemplate = R"SH(
# Generated by modore-host. Source this from your shell startup file.
# Where the resident host listens; the lean client reads this.
export MODORE_SHELL_SOCKET={{SOCKET}}
autoload -Uz add-zle-hook-widget 2>/dev/null
# Candidate picker for the explicit chooser. Honors MODORE_SHELL_PICKER
# (fzf|gum|numeric), else auto-detects fzf, then gum, then a numbered prompt.
modore_shell_pick_candidate() {
  local candidates="$1" picker="${MODORE_SHELL_PICKER:-{{PICKER_DEFAULT}}}"
  case "$picker" in
    fzf|gum|numeric) ;;
    *) picker="" ;;
  esac
  if [ -z "$picker" ]; then
    if command -v fzf >/dev/null 2>&1; then
      picker=fzf
    elif command -v gum >/dev/null 2>&1; then
      picker=gum
    else
      picker=numeric
    fi
  fi
  case "$picker" in
    fzf)
      printf '%s\n' "$candidates" | fzf --height 40% --reverse --no-sort --delimiter=$'\t' --with-nth=2..
      ;;
    gum)
      printf '%s\n' "$candidates" | gum choose --height 15
      ;;
    numeric)
      local line n=1 sel
      while IFS= read -r line; do
        [ -z "$line" ] && continue
        print -u2 -- "$(printf '%2d) %s' "$n" "${line#*$'\t'}")"
        n=$((n + 1))
      done <<< "$candidates"
      print -n -u2 -- 'modore> '
      IFS= read -r sel </dev/tty || return 1
      case "$sel" in
        ''|*[!0-9]*) return 1 ;;
      esac
      printf '%s\n' "$candidates" | sed -n "${sel}p"
      ;;
  esac
}
# Inline candidate window shown below the prompt while cycling. Uses ZLE's
# POSTDISPLAY (the same read-only region zsh-autosuggestions draws into), so
# it needs nothing installed. The current pick is wrapped in brackets.
modore_shell_render_strip() {
  local cur="$1" lines="$2"
  local -a rows
  rows=("${(@f)lines}")
  local out="" row idx cand
  for row in "${rows[@]}"; do
    [ -z "$row" ] && continue
    idx=${row%%$'\t'*}
    cand=${row#*$'\t'}
    if [ "$idx" = "$cur" ]; then
      out+=" [${cand}]"
    else
      out+="  ${cand} "
    fi
  done
  if [ -z "$out" ]; then
    POSTDISPLAY=""
  else
    POSTDISPLAY=$'\n'"modore:${out}"
  fi
  typeset -g MODORE_SHELL_LAST_BUFFER="$BUFFER"
}
modore_shell_show_strip() {
  if [ "{{CANDIDATE_WINDOW}}" != "1" ]; then
    POSTDISPLAY=""
    return 0
  fi
  local resp rest cur lines
  resp="$(MODORE_SHELL_SESSION="${MODORE_SHELL_SESSION-}" {{HOST}} --shell-candidates --caret "$CURSOR" <<< "$BUFFER")" || {
    POSTDISPLAY=""
    return 0
  }
  rest=${resp#*$'\n'}
  if [ "$rest" = "$resp" ]; then
    POSTDISPLAY=""
    return 0
  fi
  cur=${rest%%$'\n'*}
  lines=${rest#*$'\n'}
  modore_shell_render_strip "$cur" "$lines"
}
# Drop the strip as soon as the buffer changes out from under it (a normal
# keystroke) or the line is accepted, so it never lingers stale.
if (( $+functions[add-zle-hook-widget] )); then
  modore_shell__clear_on_change() {
    if [ -n "$POSTDISPLAY" ] && [ "$BUFFER" != "${MODORE_SHELL_LAST_BUFFER-}" ]; then
      POSTDISPLAY=""
    fi
  }
  modore_shell__clear_on_finish() {
    POSTDISPLAY=""
  }
  add-zle-hook-widget line-pre-redraw modore_shell__clear_on_change 2>/dev/null
  add-zle-hook-widget line-finish modore_shell__clear_on_finish 2>/dev/null
fi
modore_shell_choose_candidate() {
  local candidate_response candidate_session candidate_lines choice chosen_index response session converted
  zle -I
  candidate_response="$(MODORE_SHELL_SESSION="${MODORE_SHELL_SESSION-}" {{HOST}} --shell-candidates --caret "$CURSOR" <<< "$BUFFER")" || return 1
  candidate_session=${candidate_response%%$'\n'*}
  candidate_response=${candidate_response#*$'\n'}
  candidate_response=${candidate_response#*$'\n'}
  candidate_lines=$candidate_response
  choice="$(modore_shell_pick_candidate "$candidate_lines")" || return 1
  [ -z "$choice" ] && return 1
  chosen_index=${choice%%$'\t'*}
  response="$(MODORE_SHELL_SESSION="$candidate_session" {{HOST}} --shell-select --candidate-index "$chosen_index" --caret "$CURSOR" <<< "$BUFFER")" || return 1
  session=${response%%$'\n'*}
  if [ "$session" = "$response" ]; then
    return 1
  fi
  converted=${response#*$'\n'}
  typeset -g MODORE_SHELL_SESSION="$session"
  BUFFER="$converted"
  CURSOR=${#BUFFER}
  modore_shell_show_strip
  zle -R
}
modore_shell_convert() {
  local response session converted
  response="$(MODORE_SHELL_SESSION="${MODORE_SHELL_SESSION-}" {{HOST}} --shell-convert --caret "$CURSOR" <<< "$BUFFER")" || {
    return 1
  }
  session=${response%%$'\n'*}
  if [ "$session" = "$response" ]; then
    return 1
  fi
  converted=${response#*$'\n'}
  typeset -g MODORE_SHELL_SESSION="$session"
  BUFFER="$converted"
  CURSOR=${#BUFFER}
  modore_shell_show_strip
  zle -R
}
modore_shell_convert_katakana() {
  local response session converted
  response="$(MODORE_SHELL_SESSION="${MODORE_SHELL_SESSION-}" {{HOST}} --shell-convert --katakana --caret "$CURSOR" <<< "$BUFFER")" || {
    return 1
  }
  session=${response%%$'\n'*}
  if [ "$session" = "$response" ]; then
    return 1
  fi
  converted=${response#*$'\n'}
  typeset -g MODORE_SHELL_SESSION="$session"
  BUFFER="$converted"
  CURSOR=${#BUFFER}
  modore_shell_show_strip
  zle -R
}
zle -N modore_shell_convert
zle -N modore_shell_convert_katakana
zle -N modore_shell_choose_candidate
bindkey -M emacs '^X^J' modore_shell_convert
bindkey -M emacs '^X^L' modore_shell_choose_candidate
bindkey -M emacs '^X^K' modore_shell_convert_katakana
bindkey -M viins '^X^J' modore_shell_convert
bindkey -M viins '^X^L' modore_shell_choose_candidate
bindkey -M viins '^X^K' modore_shell_convert_katakana
bindkey -M vicmd '^X^J' modore_shell_convert
bindkey -M vicmd '^X^L' modore_shell_choose_candidate
bindkey -M vicmd '^X^K' modore_shell_convert_katakana
if typeset -f _zsh_highlight_bind_widgets >/dev/null 2>&1; then
  _zsh_highlight_bind_widgets
fi
)SH";

static constexpr const char* kFishShellBootstrapTemplate = R"SH(
# Generated by modore-host. Source this from your shell startup file.
# Where the resident host listens; the lean client reads this.
set -gx MODORE_SHELL_SOCKET {{SOCKET}}
function modore_shell_pick_candidate
    set -l candidates $argv
    set -l picker $MODORE_SHELL_PICKER
    if test -z "$picker"
        set picker {{PICKER_DEFAULT}}
    end
    if not contains -- "$picker" fzf gum numeric
        set picker ""
    end
    if test -z "$picker"
        if command -q fzf
            set picker fzf
        else if command -q gum
            set picker gum
        else
            set picker numeric
        end
    end
    switch $picker
        case fzf
            printf '%s\n' $candidates | fzf --height 40% --reverse --no-sort --delimiter \t --with-nth 2..
        case gum
            printf '%s\n' $candidates | gum choose --height 15
        case numeric
            set -l n 1
            for line in $candidates
                printf '%2d) %s\n' $n (string split -m 1 \t -- $line)[2] >&2
                set n (math $n + 1)
            end
            printf 'modore> ' >&2
            read -l sel
            or return 1
            if not string match -qr '^[0-9]+$' -- "$sel"
                return 1
            end
            if test $sel -lt 1 -o $sel -gt (count $candidates)
                return 1
            end
            printf '%s\n' $candidates[$sel]
    end
end
function modore_shell_choose_candidate
    set -l candidate_response (env MODORE_SHELL_SESSION="$MODORE_SHELL_SESSION" {{HOST}} --shell-candidates --caret (commandline -C) | string collect)
    or return 1
    set -l parts (string split -m 2 \n -- $candidate_response)
    if test (count $parts) -lt 3
        return 1
    end
    set -l candidate_session $parts[1]
    set -l candidate_index $parts[2]
    set -l candidates (string split \n -- $parts[3])
    set -l choice (modore_shell_pick_candidate $candidates)
    or return 1
    set -l chosen_parts (string split -m 1 \t -- $choice)
    set -l chosen_index $chosen_parts[1]
    set -l response (env MODORE_SHELL_SESSION="$candidate_session" {{HOST}} --shell-select --candidate-index $chosen_index --caret (commandline -C) | string collect)
    or return 1
    set -l selected_parts (string split -m 1 \n -- $response)
    if test (count $selected_parts) -lt 2
        return 1
    end
    set -g MODORE_SHELL_SESSION $selected_parts[1]
    set -l converted $selected_parts[2]
    commandline -r -- "$converted"
    commandline -C (string length -- $converted)
end
function modore_shell_convert
    set -l response (env MODORE_SHELL_SESSION="$MODORE_SHELL_SESSION" {{HOST}} --shell-convert --caret (commandline -C) | string collect)
    or return 1
    set -l parts (string split -m 1 \n -- $response)
    if test (count $parts) -lt 2
        return 1
    end
    set -g MODORE_SHELL_SESSION $parts[1]
    set -l converted $parts[2]
    commandline -r -- "$converted"
    commandline -C (string length -- $converted)
end
function modore_shell_convert_katakana
    set -l response (env MODORE_SHELL_SESSION="$MODORE_SHELL_SESSION" {{HOST}} --shell-convert --katakana --caret (commandline -C) | string collect)
    or return 1
    set -l parts (string split -m 1 \n -- $response)
    if test (count $parts) -lt 2
        return 1
    end
    set -g MODORE_SHELL_SESSION $parts[1]
    set -l converted $parts[2]
    commandline -r -- "$converted"
    commandline -C (string length -- $converted)
end
bind ctrl-x,ctrl-j modore_shell_convert
bind ctrl-x,ctrl-l modore_shell_choose_candidate
bind ctrl-x,ctrl-k modore_shell_convert_katakana
)SH";

static std::string shell_bootstrap_script(
    ShellBootstrapTarget shell,
    const std::string& host_executable_path,
    const std::string& socket_path,
    const std::string& picker_default,
    const std::string& candidate_window) {
    const std::string host_bin =
        host_executable_path.empty() ? std::string("modore-host")
                                     : host_executable_path;
    switch (shell) {
        case ShellBootstrapTarget::kBash:
            return render_shell_template(kBashShellBootstrapTemplate, host_bin,
                                         socket_path, picker_default, candidate_window);
        case ShellBootstrapTarget::kZsh:
            return render_shell_template(kZshShellBootstrapTemplate, host_bin,
                                         socket_path, picker_default, candidate_window);
        case ShellBootstrapTarget::kFish:
            return render_shell_template(kFishShellBootstrapTemplate, host_bin,
                                         socket_path, picker_default, candidate_window);
    }
    return "";
}

static std::string shell_socket_from_env() {
    if (const char* s = std::getenv("MODORE_SHELL_CFG_SOCKET")) {
        return s;
    }
    return "";
}

// Snippet knobs from `[shell]` config, handed over as env by the host (same
// channel bridge runtime tuning uses). Sanitized to a fixed vocabulary here so
// a stray value can never inject shell into the generated snippet.
static std::string shell_picker_default_from_env() {
    if (const char* p = std::getenv("MODORE_SHELL_CFG_PICKER")) {
        const std::string s = p;
        if (s == "auto" || s == "fzf" || s == "gum" || s == "numeric") {
            return s;
        }
    }
    return "auto";
}

static std::string shell_candidate_window_from_env() {
    if (const char* w = std::getenv("MODORE_SHELL_CFG_CANDIDATE_WINDOW")) {
        if (std::strcmp(w, "0") == 0) return "0";
    }
    return "1";
}

}  // namespace

extern "C" int mozc_bridge_convert_line(const char *text,
                                        size_t text_len,
                                        size_t caret_byte,
                                        char *out_buf,
                                        size_t out_cap,
                                        size_t *out_len,
                                        unsigned int flags) {
    if (!text || !out_buf || !out_len || out_cap == 0) {
        return -1;
    }
    size_t start = 0;
    size_t end = 0;
    if (text_word_bounds(text, text_len, caret_byte, &start, &end) != 0) {
        return -1;
    }
    std::string prefix(text, start);
    std::string token(text + start, end - start);
    std::string suffix(text + end, text_len - end);
    std::string replacement;
    if (!convert_token(token, flags, &replacement)) {
        if (copy_output(std::string(text, text_len), out_buf, out_cap, out_len) == 0) {
            return 0;
        }
        return static_cast<int>(text_len + 1);
    }
    std::string result = prefix + replacement + suffix;
    const int rc = copy_output(result, out_buf, out_cap, out_len);
    return rc;
}

extern "C" int mozc_bridge_shell_server_start(const char *socket_path) {
    if (!socket_path || !*socket_path) {
        mozc_bridge_set_error("missing socket path");
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_shell_server_mutex);
    if (g_shell_server) {
        mozc_bridge_clear_error();
        return 0;
    }

    std::filesystem::path path(socket_path);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        mozc_bridge_set_error("failed to create shell socket directory");
        return -1;
    }

    ::unlink(socket_path);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        mozc_bridge_set_error("socket() failed");
        return -1;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (std::strlen(socket_path) >= sizeof(addr.sun_path)) {
        ::close(fd);
        mozc_bridge_set_error("socket path too long");
        return -1;
    }
    std::memcpy(addr.sun_path, socket_path, std::strlen(socket_path));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        mozc_bridge_set_error("bind failed");
        return -1;
    }
    if (listen(fd, 8) != 0) {
        ::close(fd);
        mozc_bridge_set_error("listen failed");
        return -1;
    }
    (void)fcntl(fd, F_SETFL, O_NONBLOCK);
    chmod(socket_path, 0600);

    auto state = std::make_unique<ShellConvertServerState>();
    state->socket_path = socket_path;
    state->listen_fd = fd;
    state->running = true;
    state->worker = std::thread(shell_server_worker, state.get());
    g_shell_server = std::move(state);
    mozc_bridge_clear_error();
    return 0;
}

extern "C" void mozc_bridge_shell_server_stop(void) {
    std::lock_guard<std::mutex> lock(g_shell_server_mutex);
    if (!g_shell_server) return;
    g_shell_server->running = false;
    if (g_shell_server->listen_fd >= 0) {
        ::shutdown(g_shell_server->listen_fd, SHUT_RDWR);
        ::close(g_shell_server->listen_fd);
        g_shell_server->listen_fd = -1;
    }
    if (g_shell_server->worker.joinable()) {
        g_shell_server->worker.join();
    }
    if (!g_shell_server->socket_path.empty()) {
        ::unlink(g_shell_server->socket_path.c_str());
    }
    g_shell_server.reset();
    mozc_bridge_clear_error();
}

extern "C" int mozc_bridge_shell_convert_remote(const char *socket_path,
                                                const char *session_id_in,
                                                const char *mode_in,
                                                const char *text,
                                                size_t text_len,
                                                size_t caret_byte,
                                                char *out_buf,
                                                size_t out_cap,
                                                size_t *out_len) {
    return shell_request_remote(socket_path, session_id_in, mode_in, "convert",
                                "", text, text_len, caret_byte, out_buf, out_cap,
                                out_len);
}

extern "C" int mozc_bridge_shell_candidates_remote(const char *socket_path,
                                                   const char *session_id_in,
                                                   const char *mode_in,
                                                   const char *text,
                                                   size_t text_len,
                                                   size_t caret_byte,
                                                   char *out_buf,
                                                   size_t out_cap,
                                                   size_t *out_len) {
    return shell_request_remote(socket_path, session_id_in, mode_in, "candidates",
                                "", text, text_len, caret_byte, out_buf, out_cap,
                                out_len);
}

extern "C" int mozc_bridge_shell_select_remote(const char *socket_path,
                                               const char *session_id_in,
                                               const char *mode_in,
                                               size_t selected_index,
                                               const char *text,
                                               size_t text_len,
                                               size_t caret_byte,
                                               char *out_buf,
                                               size_t out_cap,
                                               size_t *out_len) {
    return shell_request_remote(socket_path, session_id_in, mode_in, "select",
                                std::to_string(selected_index), text, text_len,
                                caret_byte, out_buf, out_cap, out_len);
}

extern "C" int mozc_bridge_shell_bootstrap(const char *hotkey_display_name,
                                           const char *host_executable_path,
                                           char *out_buf,
                                           size_t out_cap,
                                           size_t *out_len) {
    if (!out_buf || !out_len || out_cap == 0) {
        return -1;
    }
    const ShellBootstrapTarget shell = detect_shell_bootstrap_target();
    (void)hotkey_display_name;
    const std::string script = shell_bootstrap_script(
        shell,
        host_executable_path ? host_executable_path : "",
        shell_socket_from_env(),
        shell_picker_default_from_env(),
        shell_candidate_window_from_env());
    return copy_output(script, out_buf, out_cap, out_len);
}
