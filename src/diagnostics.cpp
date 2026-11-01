#include "diagnostics.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace cryget {
namespace {
std::mutex log_mutex;

std::filesystem::path state_directory() {
#ifdef _WIN32
    DWORD needed = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (needed > 1) {
        std::wstring value(needed, L'\0');
        const DWORD written = GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), needed);
        if (written > 0 && written < needed) {
            value.resize(written);
            return std::filesystem::path(value) / L"crYGet" / L"logs";
        }
    }
#else
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
        return std::filesystem::u8path(state) / "cryget";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::u8path(home) / ".local" / "state" / "cryget";
#endif
    return std::filesystem::temp_directory_path() / "cryget-logs";
}

FILE* open_append(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"ab");
#else
    return std::fopen(path.c_str(), "ab");
#endif
}

std::string timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto value = std::chrono::system_clock::to_time_t(now);
    std::tm utc{}, local{};
#ifdef _WIN32
    gmtime_s(&utc, &value);
    localtime_s(&local, &value);
#else
    gmtime_r(&value, &utc);
    localtime_r(&value, &local);
#endif
    char buffer[32]{}, local_buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    std::strftime(local_buffer, sizeof(local_buffer), "%Y-%m-%d %H:%M:%S", &local);
    return std::string(buffer) + " (local " + local_buffer + ")";
}

std::string single_line(std::string text) {
    for (char& value : text)
        if (value == '\n' || value == '\r' || value == '\t') value = ' ';
    if (text.size() > 2048) text.resize(2048);
    return text;
}
} // namespace

std::filesystem::path log_path() {
    return state_directory() / "cryget.log";
}

void log_event(const std::string& stage, const std::string& detail) noexcept {
    try {
        std::lock_guard lock(log_mutex);
        const auto path = log_path();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error) return;
        const auto size = std::filesystem::file_size(path, error);
        if (!error && size > 5 * 1024 * 1024) {
            auto backup = path;
            backup += ".1";
            std::filesystem::remove(backup, error);
            error.clear();
            std::filesystem::rename(path, backup, error);
        }
        FILE* file = open_append(path);
        if (!file) return;
        std::ostringstream line;
        line << timestamp() << " [" << std::this_thread::get_id() << "] "
             << single_line(stage) << ": " << single_line(detail) << '\n';
        const auto bytes = line.str();
        std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
    } catch (...) {}
}

} // namespace cryget
