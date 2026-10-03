#include "platform.hpp"
#include "presentation.hpp"
#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <shellapi.h>
#include <wrl/client.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

struct Fixture
{
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / (L"better-net tests \u00fc " + std::to_wstring(GetCurrentProcessId()) +
                                                  L" " + std::to_wstring(GetTickCount64()));

    Fixture()
    {
        require(std::filesystem::create_directory(root), "Fixture directory must be new");
    }

    ~Fixture()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
};

void write_file(const std::filesystem::path& path, std::string_view bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
    require(stream.good(), "Fixture write failed");
}

void test_discovery_and_launch(const std::filesystem::path& root)
{
    const auto installation = root / L"Call of Duty";
    const auto executable = installation / L"_retail_" / L"cod.exe";
    const auto client = root / L"Battle.net.exe";
    std::string bytes(128, '\0');
    bytes[0] = 'M';
    bytes[1] = 'Z';
    bytes[60] = 64;
    bytes[64] = 'P';
    bytes[65] = 'E';
    bytes[86] = 2;
    write_file(executable, bytes);
    write_file(client, "Fixture only; never execute");

    write_file(installation / L".build.info",
               "Active!DEC:1|Version!STRING:0|Product!STRING:0\nmalformed\n1|3.14.2.123|auks\n");

    const std::array records = {bnet::InstallationRecord{L"auks", installation, {}},
                                bnet::InstallationRecord{L"AUKS", installation / L".", {}},
                                bnet::InstallationRecord{L"auks", root / L"Missing", {}}};

    const auto catalog = bnet::catalog_from_records(records, client);
    require(catalog.games.size() == 1, "Discovery must reject stale paths and deduplicate records");
    require(catalog.games.front().version == L"3.14.2.123", "Malformed metadata must not hide a valid build");
    bnet::LaunchRequest request{catalog.games.front(), client};
    int dispatches = 0;

    const bnet::ProcessAdapter adapter = [&](const bnet::PreparedLaunch& prepared)
    {
        ++dispatches;
        int count = 0;
        auto* arguments = CommandLineToArgvW(prepared.command_line.c_str(), &count);
        require(arguments != nullptr, "Launch arguments must parse");

        const bool correct =
            count == 2 && arguments[0] == client.native() && std::wstring_view(arguments[1]) == L"--exec=launch AUKS";

        LocalFree(arguments);
        require(correct && prepared.application == client, "Launch must preserve Unicode paths and product route");
        return bnet::LaunchResult{bnet::LaunchStatus::requested, L"Dry run", 0};
    };

    require(bnet::launch_game(request, {}, adapter).sent(), "Valid installation must reach the dry-run adapter");
    request.game.launch_code = L"invalid";

    require(bnet::launch_game(request, {}, adapter).status == bnet::LaunchStatus::invalid_descriptor,
            "Invalid launch routes must be rejected");

    request.game = catalog.games.front();
    std::filesystem::remove(executable);

    require(bnet::launch_game(request, {}, adapter).status == bnet::LaunchStatus::missing_installation,
            "Installation must be revalidated before dispatch");

    require(dispatches == 1, "Invalid requests must never reach the process adapter");
}

void test_launch_guard()
{
    using namespace std::chrono_literals;
    bnet::LaunchGuard guard(5s);
    const auto start = bnet::LaunchGuard::Clock::time_point{};
    const auto first = guard.try_begin(start);
    require(first.has_value(), "First launch must be accepted");
    require(!guard.try_begin(start + 1ms), "Repeated input must not dispatch another launch");
    guard.finish(*first, true, start + 2ms);
    require(!guard.try_begin(start + 4999ms), "Successful dispatch must retain its cooldown");
    const auto next = guard.try_begin(start + 5s);
    require(next.has_value(), "Launch guard must expire");
    guard.finish(*first, false, start + 5001ms);
    require(guard.busy(start + 5002ms), "Stale completion must not release a newer request");
    guard.finish(*next, false, start + 5003ms);
    require(!guard.busy(start + 5004ms), "Failed launch must allow retry");
}

void test_carousel()
{
    require(bnet::visible_game_count(0) == 0 && bnet::visible_game_count(1) == 1 && bnet::visible_game_count(2) == 2 &&
                bnet::visible_game_count(7) == 3,
            "Only installed games may occupy cards, with at most three visible");
    require(bnet::launcher_width_dip(0) == 376 && bnet::launcher_width_dip(2) == 712 &&
                bnet::launcher_width_dip(7) == 1048,
            "Window width must follow the visible card count");
    require(bnet::carousel_start(0, 0) == 0 && bnet::carousel_start(4, 2) == 0 && bnet::carousel_start(4, 3) == 1 &&
                bnet::carousel_start(7, 6) == 4 && bnet::carousel_start(7, 5, 4) == 4 &&
                bnet::carousel_start(7, 0, 4) == 0 && bnet::carousel_start(1, 6, 4) == 0,
            "Carousel must keep selection visible through boundaries, wraparound, and rescans");
}

void test_hotkey_settings(const std::filesystem::path& root)
{
    const auto path = root / L"settings.ini";
    bnet::Settings settings;
    settings.hotkey_key = 'J';
    require(bnet::save_settings(settings, path).has_value(), "Shortcut settings must save");
    const auto loaded = bnet::load_settings(path);

    require(loaded.warning.empty() && loaded.value.hotkey_key == 'J' && loaded.value.hotkey_modifiers == 3,
            "Configured shortcut must survive a restart");

    write_file(path, "truncated settings");
    const auto recovered = bnet::load_settings(path);
    require(!recovered.warning.empty() && recovered.value.hotkey_key == 'B', "Corrupt settings must recover safely");
}

void test_shortcut_toggle(const std::filesystem::path& root)
{
    const auto executable = root / L"better-net.exe";
    const auto shortcut = root / L"better-net.lnk";
    write_file(executable, "Fixture only");
    const bnet::Settings settings;
    require(bnet::create_launcher_shortcut(executable, shortcut, settings).has_value(), "Shortcut must enable");
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "Shortcut test COM setup failed");

    struct Apartment
    {
        ~Apartment()
        {
            CoUninitialize();
        }
    };

    const Apartment apartment;
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persistence;

    require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,
                                       nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_IShellLinkW,
                                       reinterpret_cast<void**>(link.GetAddressOf()))) &&
                SUCCEEDED(link.As(&persistence)),
            "Shortcut test reader must initialize");

    const auto read_hotkey = [&]
    {
        require(SUCCEEDED(persistence->Load(shortcut.c_str(), STGM_READ)),
                "Normal shortcut entry must remain readable");
        std::array<wchar_t, 32768> target{};
        std::array<wchar_t, 32> arguments{};
        WORD hotkey = 0;

        require(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH) == S_OK &&
                    executable.native() == target.data() &&
                    SUCCEEDED(link->GetArguments(arguments.data(), static_cast<int>(arguments.size()))) &&
                    std::wstring_view(arguments.data()) == L"--show" && SUCCEEDED(link->GetHotkey(&hotkey)),
                "Shortcut target and arguments must survive toggling");
        return hotkey;
    };

    const WORD enabled = MAKEWORD('B', HOTKEYF_CONTROL | HOTKEYF_ALT);
    require(read_hotkey() == enabled, "Enabled shortcut must contain the configured key");
    require(!bnet::disable_launcher_shortcut_at(root / L"other.exe", shortcut), "Foreign shortcut must be preserved");
    require(read_hotkey() == enabled, "Ownership rejection must leave the key unchanged");
    require(bnet::disable_launcher_shortcut_at(executable, shortcut).has_value(), "Owned shortcut must disable");
    require(read_hotkey() == 0, "Disabled shortcut must retain its entry without a hotkey");
    require(bnet::disable_launcher_shortcut_at(executable, shortcut).has_value(), "Repeated disable must succeed");

    require(bnet::disable_launcher_shortcut_at(executable, root / L"missing.lnk").has_value(),
            "Missing shortcut must already count as disabled");

    require(bnet::create_launcher_shortcut(executable, shortcut, settings).has_value(), "Shortcut must enable again");
    require(read_hotkey() == enabled, "Reenabled shortcut must restore its configured key");
}
}

int main()
{
    try
    {
        const Fixture fixture;
        test_discovery_and_launch(fixture.root);
        test_launch_guard();
        test_carousel();
        test_hotkey_settings(fixture.root);
        test_shortcut_toggle(fixture.root);
        std::cout << "Core checks passed without launching games.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
