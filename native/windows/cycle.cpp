#include "cycle.hpp"

#include "textio.hpp"

#include <chrono>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace modore::windows {
namespace {

using Clock = std::chrono::steady_clock;

struct CycleSession {
    std::wstring field_text;
    std::wstring committed_text;
    std::vector<std::wstring> candidates;
    size_t current_index = 0;
    Clock::time_point touched = Clock::now();
};

std::mutex g_cycle_mutex;
std::optional<CycleSession> g_cycle_session;

constexpr auto kCycleSessionTtl = std::chrono::seconds(10);

void clear_cycle_session_locked() {
    g_cycle_session.reset();
}

std::optional<std::wstring> replace_unique_occurrence(
    const std::wstring& text,
    const std::wstring& needle,
    const std::wstring& replacement) {
    if (needle.empty()) {
        return std::nullopt;
    }
    const size_t first = text.find(needle);
    if (first == std::wstring::npos) {
        return std::nullopt;
    }
    if (text.find(needle, first + needle.size()) != std::wstring::npos) {
        return std::nullopt;
    }
    std::wstring updated = text;
    updated.replace(first, needle.size(), replacement);
    return updated;
}

bool cycle_session_expired(const CycleSession& session, Clock::time_point now) {
    return now - session.touched > kCycleSessionTtl;
}

std::optional<std::wstring> next_cycle_candidate(const CycleSession& session, size_t* next_index) {
    if (session.candidates.size() <= 1) {
        return std::nullopt;
    }
    const size_t idx = (session.current_index + 1) % session.candidates.size();
    if (next_index) {
        *next_index = idx;
    }
    return session.candidates[idx];
}

std::optional<std::wstring> previous_cycle_candidate(const CycleSession& session, size_t* next_index) {
    if (session.candidates.size() <= 1) {
        return std::nullopt;
    }
    const size_t idx = (session.current_index + session.candidates.size() - 1) % session.candidates.size();
    if (next_index) {
        *next_index = idx;
    }
    return session.candidates[idx];
}

bool update_cycle_session_after_replace(
    const std::wstring& previous_text,
    const std::wstring& next_text,
    size_t next_index) {
    std::scoped_lock lock(g_cycle_mutex);
    if (!g_cycle_session) {
        return false;
    }
    if (g_cycle_session->field_text != previous_text) {
        return false;
    }

    const size_t pos = g_cycle_session->field_text.find(g_cycle_session->committed_text);
    if (pos == std::wstring::npos) {
        clear_cycle_session_locked();
        return false;
    }
    g_cycle_session->field_text.replace(pos, g_cycle_session->committed_text.size(), next_text);
    g_cycle_session->committed_text = next_text;
    g_cycle_session->current_index = next_index;
    g_cycle_session->touched = Clock::now();
    return true;
}

} // namespace

void reset_cycle() {
    std::scoped_lock lock(g_cycle_mutex);
    clear_cycle_session_locked();
}

void remember_cycle(
    const std::wstring& field_text,
    const PickupConversionResult& conversion) {
    std::scoped_lock lock(g_cycle_mutex);
    if (conversion.candidates.empty()) {
        clear_cycle_session_locked();
        return;
    }
    g_cycle_session = CycleSession{
        field_text,
        conversion.committed,
        conversion.candidates,
        0,
        Clock::now()
    };
}

bool cycle_last_pickup(Logger& logger) {
    const auto now = Clock::now();
    std::optional<CycleSession> session;
    {
        std::scoped_lock lock(g_cycle_mutex);
        if (!g_cycle_session) {
            return false;
        }
        if (cycle_session_expired(*g_cycle_session, now)) {
            clear_cycle_session_locked();
            return false;
        }
        session = g_cycle_session;
    }

    if (!session || session->candidates.size() <= 1) {
        return false;
    }

    const auto current_field = focused_editable_text();
    if (!current_field || *current_field != session->field_text) {
        return false;
    }

    size_t next_index = 0;
    const auto next = next_cycle_candidate(*session, &next_index);
    if (!next) {
        return false;
    }

    if (!replace_focused_selection_text(session->committed_text, *next)) {
        return false;
    }

    const auto updated = replace_unique_occurrence(*current_field, session->committed_text, *next);
    if (!updated) {
        return false;
    }

    if (!update_cycle_session_after_replace(*current_field, *next, next_index)) {
        return false;
    }

    logger.write(
        LogTag::Cycle,
        std::wstring(L"cycled to \"") + escape_for_log(*next) + L"\" [" +
        std::to_wstring(next_index + 1) + L"/" + std::to_wstring(session->candidates.size()) + L"]");
    return true;
}

bool cycle_previous_pickup(Logger& logger) {
    const auto now = Clock::now();
    std::optional<CycleSession> session;
    {
        std::scoped_lock lock(g_cycle_mutex);
        if (!g_cycle_session) {
            return false;
        }
        if (cycle_session_expired(*g_cycle_session, now)) {
            clear_cycle_session_locked();
            return false;
        }
        session = g_cycle_session;
    }

    if (!session || session->candidates.size() <= 1) {
        return false;
    }

    const auto current_field = focused_editable_text();
    if (!current_field || *current_field != session->field_text) {
        return false;
    }

    size_t next_index = 0;
    const auto next = previous_cycle_candidate(*session, &next_index);
    if (!next) {
        return false;
    }

    if (!replace_focused_selection_text(session->committed_text, *next)) {
        return false;
    }

    const auto updated = replace_unique_occurrence(*current_field, session->committed_text, *next);
    if (!updated) {
        return false;
    }

    if (!update_cycle_session_after_replace(*current_field, *next, next_index)) {
        return false;
    }

    logger.write(
        LogTag::Cycle,
        std::wstring(L"cycled to \"") + escape_for_log(*next) + L"\" [" +
        std::to_wstring(next_index + 1) + L"/" + std::to_wstring(session->candidates.size()) + L"]");
    return true;
}

} // namespace modore::windows
