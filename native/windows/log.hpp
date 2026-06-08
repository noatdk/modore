#pragma once

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace modore::windows {

enum class LogTag {
    Boot,
    Config,
    Hotkey,
    Pickup,
    Clipboard,
    Ime,
    SecureInput,
    Undo,
    Cycle,
    Panel,
    Unicode,
    Scripting,
};

class Logger {
public:
    static Logger& instance();

    void configure_disabled_roots(const std::wstring& disabled);
    void configure_disabled_roots(const std::vector<std::wstring>& disabled);
    void write(LogTag tag, const std::wstring& message);
    void set_log_path(const std::filesystem::path& path);
    std::filesystem::path log_path() const;

private:
    Logger() = default;
    std::wstring tag_name(LogTag tag) const;
    bool is_disabled(LogTag tag) const;

    mutable std::mutex mutex_;
    std::filesystem::path log_path_;
    std::atomic<uint32_t> disabled_mask_{0};
};

std::wstring join_disabled_roots(const std::vector<std::wstring>& roots);
std::wstring escape_for_log(const std::wstring& text);

} // namespace modore::windows
