#include "platform.hpp"
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
#include <vector>

namespace
{
void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

class Fixture
{
public:

    Fixture()
        : parent_(std::filesystem::temp_directory_path()),
          root(parent_ / (L"Better Net platform tests \u00fc \u6771\u4eac " + std::to_wstring(GetCurrentProcessId()) +
                          L" " + std::to_wstring(GetTickCount64())))
    {
        std::filesystem::create_directories(root);
    }

    ~Fixture()
    {
        if (root.is_absolute() && root.parent_path() == parent_ &&
            root.filename().native().starts_with(L"Better Net platform tests "))
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
private:
    std::filesystem::path parent_;
public:
    std::filesystem::path root;
};

void write_file(const std::filesystem::path& path, std::string_view bytes)
{
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary);
    stream << bytes;
    require(stream.good(), "fixture write failed");
}

void write_executable(const std::filesystem::path& path)
{
    std::string bytes(128, '\0');
    bytes[0] = 'M';
    bytes[1] = 'Z';
    bytes[60] = 64;
    bytes[64] = 'P';
    bytes[65] = 'E';
    bytes[86] = 2;
    write_file(path, bytes);
}

void test_quoting()
{
    const std::vector<std::wstring> values = {L"",
                                              L"simple",
                                              L"space here",
                                              L"\u00fc\u6771\u4eac",
                                              L"C:\\space here\\",
                                              L"a\"b",
                                              L"slashes\\\\\"quote",
                                              L"trailing\\\\",
                                              L"tabs\there",
                                              L"--exec=launch AUKS"};

    for (const auto& value : values)
    {
        const auto command = L"test.exe " + bnet::quote_windows_argument(value);
        int count = 0;
        LPWSTR* arguments = CommandLineToArgvW(command.c_str(), &count);
        require(arguments != nullptr, "Windows argument parser failed");
        const bool matches = count == 2 && value == arguments[1];
        LocalFree(arguments);
        require(matches, "Windows argument quote round trip changed a value");
    }
}

void test_guard()
{
    using namespace std::chrono_literals;
    bnet::LaunchGuard guard(5s);
    const auto start = bnet::LaunchGuard::Clock::time_point{};
    const auto first = guard.try_begin(start);
    require(first.has_value(), "initial launch should start");
    require(!guard.try_begin(start + 1ms), "duplicate in-flight launch should be blocked");
    guard.finish(*first, true, start + 2ms);
    require(!guard.try_begin(start + 4999ms), "successful dispatch should retain bounded cooldown");
    const auto second = guard.try_begin(start + 5s);
    require(second && *second != *first, "cooldown must expire at deadline");
    guard.finish(*first, false, start + 5001ms);
    require(guard.busy(start + 5002ms), "stale completion must not release new launch");
    guard.finish(*second, false, start + 5003ms);
    require(!guard.busy(start + 5004ms), "failure must release guard");
    require(guard.try_begin(start + 5005ms).has_value(), "retry should start after failure");
    require(guard.try_begin(start + 10005ms).has_value(), "unfinished request must expire");
    guard.cancel();
    require(!guard.busy(start + 10006ms), "cancellation must release guard");
    bnet::LaunchGuard bounded(24h);
    require(bounded.try_begin(start).has_value(), "bounded guard should start");
    require(bounded.try_begin(start + 30s).has_value(), "guard timeout must remain bounded");
}

void test_settings(const std::filesystem::path& root)
{
    const auto path = root / L"settings \u00fc" / L"settings.ini";
    const auto fresh = bnet::load_settings(path);
    require(fresh.warning.empty() && fresh.value.hotkey_key == 'B', "missing settings should use defaults quietly");
    bnet::Settings settings;
    settings.hotkey_key = 'J';
    settings.hotkey_modifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT;
    settings.start_hidden = true;
    require(bnet::save_settings(settings, path).has_value(), "settings save should succeed");
    const auto loaded = bnet::load_settings(path);

    require(loaded.warning.empty() && loaded.value.hotkey_key == 'J' && loaded.value.start_hidden &&
                loaded.value.hotkey_modifiers == settings.hotkey_modifiers,
            "settings round trip should preserve all fields");

    const auto valid_settings = settings;
    settings.hotkey_key = VK_F12;
    require(!bnet::save_settings(settings, path), "reserved F12 should be rejected");
    require(bnet::load_settings(path).value.hotkey_key == 'J', "invalid save should preserve previous settings");
    settings.hotkey_key = 'B';
    settings.hotkey_modifiers = MOD_WIN;
    require(!bnet::valid_hotkey(settings), "Windows modifier cannot be represented by shell shortcut");
    settings.hotkey_modifiers = MOD_SHIFT;
    require(!bnet::valid_hotkey(settings), "bare Shift should not consume ordinary typing");

    const std::vector<std::string> invalid = {
        "",
        "schema=1\nhotkey_key=66\n",
        "schema=9\nhotkey_modifiers=3\nhotkey_key=66\nstart_hidden=0\n",
        "schema=1\nhotkey_modifiers=3\nhotkey_key=66\nhotkey_key=77\nstart_hidden=0\n",
        "schema=1\nhotkey_modifiers=3\nhotkey_key=66bad\nstart_hidden=0\n",
        "schema=1\nhotkey_modifiers=3\nhotkey_key=66\nstart_hidden=9\n",
        std::string(4097, 'x')};

    for (const auto& bytes : invalid)
    {
        write_file(path, bytes);
        const auto recovered = bnet::load_settings(path);
        require(!recovered.warning.empty() && recovered.value.hotkey_key == 'B', "corrupt settings must recover");
    }

    require(bnet::save_settings(valid_settings, path).has_value(), "valid settings should replace corrupt settings");
    write_file(root / L"blocked parent", "file");

    require(!bnet::save_settings(valid_settings, root / L"blocked parent" / L"settings.ini"),
            "save failures should report an error");

    require(bnet::hotkey_text(valid_settings) == L"Ctrl + Alt + Shift + J", "hotkey label should match modifiers");

    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path()))
    {
        require(entry.path().extension() != L".tmp", "atomic settings should not leave temporary files");
    }
}

void test_launch(const std::filesystem::path& root)
{
    const auto client = root / L"Battle.net client \u00fc" / L"Battle.net.exe";
    const auto installation = root / L"Overwatch \u6771\u4eac";
    write_file(client, "fixture, never execute");
    write_executable(installation / L"_retail_" / L"Overwatch.exe");
    write_file(installation / L".build.info", "Product!STRING:0|Active!DEC:1|Version!STRING:0\npro|1|2.19.1.12345\n");
    const std::array records = {bnet::InstallationRecord{L"prometheus", installation, L"Overwatch"}};
    const auto catalog = bnet::catalog_from_records(records, client);
    require(catalog.games.size() == 1, "launch fixture must be a validated installation");
    bnet::LaunchRequest request{catalog.games.front(), client};
    std::size_t dispatches = 0;

    const bnet::ProcessAdapter adapter = [&](const bnet::PreparedLaunch& prepared)
    {
        ++dispatches;
        require(prepared.application == client, "adapter should receive exact Unicode application path");
        require(prepared.working_directory == client.parent_path(), "client directory should be working directory");
        int count = 0;
        LPWSTR* arguments = CommandLineToArgvW(prepared.command_line.c_str(), &count);
        require(arguments != nullptr, "launch argument parse failed");

        const bool correct =
            count == 2 && arguments[0] == client.native() && std::wstring_view(arguments[1]) == L"--exec=launch Pro";

        LocalFree(arguments);
        require(correct, "prepared launch command must preserve product and Unicode path");
        return bnet::LaunchResult{bnet::LaunchStatus::requested, L"Injected dispatch", 0};
    };

    require(bnet::launch_game(request, {}, adapter).sent(), "validated request should reach adapter");
    require(dispatches == 1, "adapter should run once");
    std::stop_source cancellation;
    cancellation.request_stop();

    require(bnet::launch_game(request, cancellation.get_token(), adapter).status == bnet::LaunchStatus::cancelled,
            "cancelled request should stop before dispatch");

    require(dispatches == 1, "cancelled request should not dispatch");
    request.game.launch_code = L"Pro\" --malformed";

    require(bnet::launch_game(request, {}, adapter).status == bnet::LaunchStatus::invalid_descriptor,
            "altered product descriptor should be rejected");

    request.game = catalog.games.front();
    request.client_path = root / L"missing.exe";

    require(bnet::launch_game(request, {}, adapter).status == bnet::LaunchStatus::missing_client,
            "missing client should be actionable");

    request.client_path = client;
    std::filesystem::remove(installation / L"_retail_" / L"Overwatch.exe");

    require(bnet::launch_game(request, {}, adapter).status == bnet::LaunchStatus::missing_installation,
            "installation must be revalidated at activation");

    require(dispatches == 1, "invalid prerequisites must not dispatch");
    write_executable(installation / L"_retail_" / L"Overwatch.exe");

    const auto failed = bnet::launch_game(request,
                                          {},
                                          [](const bnet::PreparedLaunch&)
                                          {
                                              return bnet::LaunchResult{bnet::LaunchStatus::process_error,
                                                                        L"Injected native failure",
                                                                        ERROR_ACCESS_DENIED};
                                          });

    require(failed.status == bnet::LaunchStatus::process_error && failed.native_error == ERROR_ACCESS_DENIED,
            "adapter failures should preserve structured error");

    const auto threw = bnet::launch_game(request,
                                         {},
                                         [](const bnet::PreparedLaunch&) -> bnet::LaunchResult
                                         {
                                             throw std::runtime_error("injected worker failure");
                                         });

    require(threw.status == bnet::LaunchStatus::process_error, "worker exceptions must be contained");
}

void test_shortcut(const std::filesystem::path& root)
{
    const auto executable = root / L"Better Net \u00fc.exe";
    const auto shortcut = root / L"shortcut \u6771\u4eac.lnk";
    write_file(executable, "fixture, never execute");
    bnet::Settings settings;
    settings.hotkey_key = 'J';
    require(bnet::create_launcher_shortcut(executable, shortcut, settings).has_value(), "shortcut should be created");
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    require(SUCCEEDED(initialized), "test COM initialization failed");

    {
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> persistence;

        require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,
                                           nullptr,
                                           CLSCTX_INPROC_SERVER,
                                           IID_IShellLinkW,
                                           reinterpret_cast<void**>(link.GetAddressOf()))) &&
                    SUCCEEDED(link.As(&persistence)) && SUCCEEDED(persistence->Load(shortcut.c_str(), STGM_READ)),
                "saved shortcut should load through Windows");

        std::array<wchar_t, 32768> target{};
        std::array<wchar_t, 256> arguments{};
        WORD hotkey = 0;

        require(SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)) &&
                    executable.native() == target.data(),
                "shortcut should preserve Unicode executable path");

        require(SUCCEEDED(link->GetArguments(arguments.data(), static_cast<int>(arguments.size()))) &&
                    std::wstring_view(arguments.data()) == L"--show",
                "shortcut should explicitly show launcher");

        require(SUCCEEDED(link->GetHotkey(&hotkey)) && hotkey == MAKEWORD('J', HOTKEYF_CONTROL | HOTKEYF_ALT),
                "shortcut should persist cold-start keybind");
    }

    CoUninitialize();
}
}

int main()
{
    try
    {
        Fixture fixture;
        test_quoting();
        test_guard();
        test_settings(fixture.root);
        test_launch(fixture.root);
        test_shortcut(fixture.root);
        std::cout << "Platform tests passed without launching games.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
