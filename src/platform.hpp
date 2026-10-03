#pragma once
#include "catalog.hpp"
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace bnet
{
struct Settings
{
    std::uint32_t hotkey_modifiers = 3;
    std::uint32_t hotkey_key = 'B';
    bool start_hidden = false;
};

struct SettingsLoad
{
    Settings value;
    std::wstring warning;
};

bool valid_hotkey(const Settings& settings) noexcept;
std::wstring hotkey_text(const Settings& settings);
std::filesystem::path settings_path();
SettingsLoad load_settings(const std::filesystem::path& path = settings_path()) noexcept;

std::expected<void, std::wstring> save_settings(const Settings& settings,
                                                const std::filesystem::path& path = settings_path()) noexcept;

enum class LaunchStatus
{
    requested,
    missing_client,
    missing_installation,
    invalid_descriptor,
    process_error,
    cancelled
};

struct LaunchResult
{
    LaunchStatus status = LaunchStatus::process_error;
    std::wstring message;
    std::uint32_t native_error = 0;
    bool sent() const noexcept;
};

struct LaunchRequest
{
    Game game;
    std::filesystem::path client_path;
};

struct PreparedLaunch
{
    std::filesystem::path application;
    std::wstring command_line;
    std::filesystem::path working_directory;
};

using ProcessAdapter = std::function<LaunchResult(const PreparedLaunch&)>;
std::wstring quote_windows_argument(std::wstring_view value);
std::expected<PreparedLaunch, LaunchResult> prepare_launch(const LaunchRequest& request);

LaunchResult
launch_game(const LaunchRequest& request, std::stop_token stop = {}, const ProcessAdapter& adapter = {}) noexcept;

class LaunchGuard
{
public:
    using Clock = std::chrono::steady_clock;
    explicit LaunchGuard(std::chrono::milliseconds timeout = std::chrono::seconds(5));
    std::optional<std::uint64_t> try_begin(Clock::time_point now = Clock::now());
    void finish(std::uint64_t ticket, bool sent, Clock::time_point now = Clock::now());
    void cancel();
    bool busy(Clock::time_point now = Clock::now()) const;

private:
    mutable std::mutex mutex_;
    std::chrono::milliseconds timeout_;
    Clock::time_point deadline_{};
    std::uint64_t sequence_ = 0;
    std::optional<std::uint64_t> active_;
};

std::expected<void, std::wstring> create_launcher_shortcut(const std::filesystem::path& executable,
                                                           const std::filesystem::path& shortcut_path,
                                                           const Settings& settings) noexcept;

std::expected<std::filesystem::path, std::wstring> install_launcher_shortcut(const std::filesystem::path& executable,
                                                                             const Settings& settings,
                                                                             bool desktop = false) noexcept;

std::expected<void, std::wstring> disable_launcher_shortcut_at(const std::filesystem::path& executable,
                                                               const std::filesystem::path& shortcut_path) noexcept;

std::expected<void, std::wstring> disable_launcher_shortcut(const std::filesystem::path& executable,
                                                            bool desktop = false) noexcept;

bool launcher_shortcut_matches(const std::filesystem::path& executable,
                               const Settings& settings,
                               bool desktop = false) noexcept;

bool launcher_shortcut_enabled(const std::filesystem::path& executable, bool desktop = false) noexcept;
bool startup_enabled(const std::filesystem::path& executable) noexcept;
std::expected<void, std::wstring> set_startup_enabled(const std::filesystem::path& executable, bool enabled) noexcept;
}
