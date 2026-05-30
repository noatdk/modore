// modore-shell — lean Unix-socket client for the shell-native conversion
// widgets.
//
// It speaks the exact same wire protocol as the bridge's
// `mozc_bridge_shell_*_remote` helpers, but links nothing from Mozc. The
// resident modore-host daemon owns the session state and the engine; this
// binary only marshals one request and echoes the reply. Keeping it dylib-free
// is the whole point: each per-keystroke widget call is a ~ms process spawn
// instead of dyld mapping the ~25 MB engine dylib (and running its static
// initializers) just to relay a socket request.
//
// Output is byte-identical to `modore-host --shell-{convert,candidates,select}`
// so the generated shell snippets parse it the same way.

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

std::string read_stdin() {
    std::string out;
    char buf[4096];
    ssize_t n;
    while ((n = ::read(STDIN_FILENO, buf, sizeof(buf))) > 0) {
        out.append(buf, static_cast<size_t>(n));
    }
    return out;
}

// Bytes a UTF-8 lead byte introduces (1 on an invalid lead, so we always make
// progress). A 4-byte sequence is one astral code point = two UTF-16 units;
// everything else is one UTF-16 unit, so callers infer units from this alone.
size_t utf8_seq_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// UTF-16 code-unit offset -> UTF-8 byte offset, matching the host's
// String.utf8ByteOffset(forUTF16Offset:). Stops on a code-point boundary if
// the target would split one.
size_t utf16_to_byte_offset(const std::string& s, size_t u16_target) {
    size_t bytepos = 0, u16 = 0;
    const size_t len = s.size();
    while (bytepos < len && u16 < u16_target) {
        size_t seq = utf8_seq_len(static_cast<unsigned char>(s[bytepos]));
        if (bytepos + seq > len) break;                 // truncated tail
        const size_t units = (seq == 4) ? 2 : 1;
        if (u16 + units > u16_target) break;            // target splits a code point
        bytepos += seq;
        u16 += units;
    }
    return bytepos;
}

// UTF-16 code-unit length of a UTF-8 string (the default caret = end).
size_t utf16_length(const std::string& s) {
    size_t u16 = 0, i = 0;
    const size_t len = s.size();
    while (i < len) {
        size_t seq = utf8_seq_len(static_cast<unsigned char>(s[i]));
        if (i + seq > len) seq = 1;
        u16 += (seq == 4) ? 2 : 1;
        i += seq;
    }
    return u16;
}

bool write_all(int fd, const char* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool read_all(int fd, std::string* out) {
    char buf[4096];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            out->append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) return true;
        if (errno == EINTR) continue;
        return false;
    }
}

// Reads `--flag value` or `--flag=value`; returns nullptr if absent.
const char* arg_value(int argc, char** argv, const char* flag) {
    const size_t flen = std::strlen(flag);
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) {
            return (i + 1 < argc) ? argv[i + 1] : nullptr;
        }
        if (std::strncmp(argv[i], flag, flen) == 0 && argv[i][flen] == '=') {
            return argv[i] + flen + 1;
        }
    }
    return nullptr;
}

bool has_flag(int argc, char** argv, const char* flag) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], flag) == 0) return true;
    }
    return false;
}

std::string default_socket_path() {
    if (const char* home = std::getenv("HOME")) {
        return std::string(home) + "/Library/Application Support/modore/shell-convert.sock";
    }
    return "";
}

}  // namespace

int main(int argc, char** argv) {
    std::string action;
    if (has_flag(argc, argv, "--shell-convert"))         action = "convert";
    else if (has_flag(argc, argv, "--shell-candidates")) action = "candidates";
    else if (has_flag(argc, argv, "--shell-select"))     action = "select";
    else {
        std::fprintf(stderr,
            "modore-shell: expected --shell-convert | --shell-candidates | --shell-select\n");
        return 2;
    }

    const std::string mode = has_flag(argc, argv, "--katakana") ? "katakana" : "primary";

    // Socket path: --socket wins, then MODORE_SHELL_SOCKET (baked into the
    // snippet by the host, the authority on where the daemon listens), then a
    // best-effort default.
    std::string socket_path;
    if (const char* s = arg_value(argc, argv, "--socket"); s && *s) {
        socket_path = s;
    }
    if (socket_path.empty()) {
        if (const char* e = std::getenv("MODORE_SHELL_SOCKET"); e && *e) {
            socket_path = e;
        }
    }
    if (socket_path.empty()) {
        socket_path = default_socket_path();
    }
    if (socket_path.empty()) {
        std::fprintf(stderr, "modore-shell: no socket path (set MODORE_SHELL_SOCKET)\n");
        return 1;
    }

    const std::string text = read_stdin();

    size_t caret_u16 = utf16_length(text);
    if (const char* c = arg_value(argc, argv, "--caret")) {
        char* end = nullptr;
        const long v = std::strtol(c, &end, 10);
        if (end && *end == '\0' && v >= 0) {
            caret_u16 = static_cast<size_t>(v);
        }
    }
    const size_t caret_byte = utf16_to_byte_offset(text, caret_u16);

    std::string payload;
    if (action == "select") {
        const char* idx = arg_value(argc, argv, "--candidate-index");
        payload = idx ? idx : "0";
    }

    const char* session = std::getenv("MODORE_SHELL_SESSION");

    // session \0 caret \0 mode \0 action \0 payload \0 text — matches the
    // request the bridge's *_remote helpers build.
    std::string request;
    request.append(session ? session : "");
    request.push_back('\0');
    request.append(std::to_string(caret_byte));
    request.push_back('\0');
    request.append(mode);
    request.push_back('\0');
    request.append(action);
    request.push_back('\0');
    request.append(payload);
    request.push_back('\0');
    request.append(text);

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::fprintf(stderr, "modore-shell: socket() failed\n");
        return 1;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path.size() >= sizeof(addr.sun_path)) {
        std::fprintf(stderr, "modore-shell: socket path too long\n");
        ::close(fd);
        return 1;
    }
    std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        // Daemon not running / socket gone: exit nonzero so the widget's
        // `|| return 1` leaves the buffer untouched, same as the old client.
        ::close(fd);
        return 1;
    }
    if (!write_all(fd, request.data(), request.size())) {
        ::close(fd);
        return 1;
    }
    ::shutdown(fd, SHUT_WR);
    std::string response;
    const bool ok = read_all(fd, &response);
    ::close(fd);
    if (!ok) return 1;

    if (!response.empty()) {
        ::fwrite(response.data(), 1, response.size(), stdout);
    }
    return 0;
}
