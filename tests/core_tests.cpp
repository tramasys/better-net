#include "platform.hpp"
#include <Windows.h>
#include <shellapi.h>
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
}

int main()
{
    try
    {
        const Fixture fixture;
        test_discovery_and_launch(fixture.root);
        test_launch_guard();
        test_hotkey_settings(fixture.root);
        std::cout << "Core checks passed without launching games.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
