#include "ime.hpp"

#include "config.hpp"
#include "mozc_convert.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <windows.h>

#include <mozc_bridge.h>

namespace modore::windows {
namespace {

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(const char* text, size_t len) {
    if (!text || len == 0) {
        return {};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(len),
        nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(len),
        out.data(), size);
    return out;
}

std::wstring last_error_wide() {
    const char* err = mozc_bridge_last_error();
    if (!err) {
        return L"unknown error";
    }
    return utf8_to_wide(err, std::strlen(err));
}

std::optional<std::wstring> run_line_convert(
    const std::string& input,
    unsigned int flags,
    Logger& logger) {
    size_t out_cap = std::max<size_t>(input.size() * 4 + 64, 256);
    size_t out_len = 0;

    for (;;) {
        std::string output(out_cap, '\0');
        const int rc = mozc_bridge_convert_line(
            input.data(),
            input.size(),
            input.size(),
            output.data(),
            output.size(),
            &out_len,
            flags);
        if (rc == 0) {
            return utf8_to_wide(output.data(), out_len);
        }
        if (rc < 0) {
            logger.write(LogTag::Ime, std::wstring(L"bridge line convert failed: ") + last_error_wide());
            return std::nullopt;
        }
        if (static_cast<size_t>(rc) > (1u << 20)) {
            logger.write(LogTag::Ime, L"bridge line convert returned unreasonably large output");
            return std::nullopt;
        }
        out_cap = static_cast<size_t>(rc) + 1;
    }
}

std::optional<ConversionResult> run_convert_with_candidates(
    const std::string& input,
    unsigned int flags,
    Logger& logger) {
    modore::common::CandidateConversion conv;
    std::string error;
    if (!modore::common::convert_with_candidates(
            input, flags, modore::common::kDefaultMaxCandidates, &conv, &error)) {
        logger.write(
            LogTag::Ime,
            std::wstring(L"bridge candidate convert failed: ") +
                utf8_to_wide(error.data(), error.size()));
        return std::nullopt;
    }

    ConversionResult result;
    result.committed = utf8_to_wide(conv.committed.data(), conv.committed.size());
    result.candidates.reserve(conv.candidates.size());
    for (const auto& candidate : conv.candidates) {
        result.candidates.push_back(
            utf8_to_wide(candidate.data(), candidate.size()));
    }
    return result;
}

bool run_warmup_convert(Logger& logger) {
    const char* input = "a";
    char commit_buf[32] = {};
    size_t commit_len = 0;
    const auto start = std::chrono::steady_clock::now();
    const int rc = mozc_bridge_convert_ex(
        input, std::strlen(input),
        commit_buf, sizeof(commit_buf), &commit_len,
        /*flags=*/0u);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    if (rc != 0) {
        logger.write(
            LogTag::Ime,
            std::wstring(L"bridge warmup skipped: ") + last_error_wide() +
                L" elapsed_ms=" + std::to_wstring(elapsed));
        return false;
    }

    logger.write(
        LogTag::Ime,
        std::wstring(L"bridge warmup complete commit_bytes=") + std::to_wstring(commit_len) +
            L" elapsed_ms=" + std::to_wstring(elapsed));
    return true;
}

} // namespace

bool bootstrap_ime(Logger& logger) {
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, [&]() {
        const auto profile = ime_profile_path().wstring();
        const std::string profile_utf8 = wide_to_utf8(profile);
        if (mozc_bridge_init(profile_utf8.c_str()) != 0) {
            logger.write(LogTag::Ime, std::wstring(L"bridge init failed: ") + last_error_wide());
            initialized = false;
            return;
        }

        logger.write(LogTag::Ime, std::wstring(L"bridge initialized (profile=") + profile + L")");
        initialized = true;
    });
    return initialized;
}

bool warmup_ime(Logger& logger) {
    static std::once_flag once;
    static bool warmed = false;
    std::call_once(once, [&]() {
        if (!bootstrap_ime(logger)) {
            warmed = false;
            return;
        }
        warmed = run_warmup_convert(logger);
    });
    return warmed;
}

std::optional<std::wstring> convert_with_ime(const std::wstring& text, bool katakana, Logger& logger) {
    if (text.empty()) {
        return std::nullopt;
    }
    if (!bootstrap_ime(logger)) {
        return std::nullopt;
    }

    const std::string input = wide_to_utf8(text);
    if (input.empty() && !text.empty()) {
        logger.write(LogTag::Ime, L"input encoding to UTF-8 failed");
        return std::nullopt;
    }

    const unsigned int flags = katakana ? MOZC_CONVERT_FLAG_KATAKANA : 0u;
    return run_line_convert(input, flags, logger);
}

std::optional<ConversionResult> convert_with_ime_candidates(
    const std::wstring& text, bool katakana, Logger& logger) {
    if (text.empty()) {
        return std::nullopt;
    }
    if (!bootstrap_ime(logger)) {
        return std::nullopt;
    }

    const std::string input = wide_to_utf8(text);
    if (input.empty() && !text.empty()) {
        logger.write(LogTag::Ime, L"input encoding to UTF-8 failed");
        return std::nullopt;
    }

    const unsigned int flags = katakana ? MOZC_CONVERT_FLAG_KATAKANA : 0u;
    return run_convert_with_candidates(input, flags, logger);
}

} // namespace modore::windows
