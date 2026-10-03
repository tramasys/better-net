#include "platform.hpp"
#include <Windows.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <limits>
#include <memory>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace bnet
{
namespace
{
constexpr wchar_t startup_key[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
using RegistryHandle = std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>;

LSTATUS read_startup_command(HKEY key, std::wstring& command)
{
    std::array<wchar_t, 261> buffer{};
    DWORD bytes = static_cast<DWORD>(sizeof(buffer));
    DWORD type = 0;

    const auto result =
        RegQueryValueExW(key, L"better-net", nullptr, &type, reinterpret_cast<BYTE*>(buffer.data()), &bytes);

    if (result != ERROR_SUCCESS)
    {
        return result;
    }

    if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0 ||
        buffer[bytes / sizeof(wchar_t) - 1] != L'\0')
    {
        return ERROR_INVALID_DATA;
    }

    const std::wstring_view value(buffer.data(), bytes / sizeof(wchar_t) - 1);

    if (value.find(L'\0') != std::wstring_view::npos)
    {
        return ERROR_INVALID_DATA;
    }

    command = value;
    return ERROR_SUCCESS;
}

class Handle
{
public:
    explicit Handle(HANDLE value) noexcept : value_(value)
    {
    }

    ~Handle()
    {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value_);
        }
    }

    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

private:
    HANDLE value_;
};

class TemporaryFile
{
public:
    explicit TemporaryFile(std::filesystem::path path) : path_(std::move(path))
    {
    }

    ~TemporaryFile()
    {
        if (!path_.empty())
        {
            DeleteFileW(path_.c_str());
        }
    }

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    void release() noexcept
    {
        path_.clear();
    }

private:
    std::filesystem::path path_;
};

std::wstring windows_error(DWORD code)
{
    std::array<wchar_t, 512> buffer{};

    const DWORD count = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                       nullptr,
                                       code,
                                       0,
                                       buffer.data(),
                                       static_cast<DWORD>(buffer.size()),
                                       nullptr);

    std::wstring text(buffer.data(), count);

    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
    {
        text.pop_back();
    }

    if (text.empty())
    {
        text = L"Windows error " + std::to_wstring(code);
    }

    return text;
}

std::filesystem::path known_folder(REFKNOWNFOLDERID id)
{
    PWSTR value = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &value);

    if (FAILED(result))
    {
        return {};
    }

    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owner(value, CoTaskMemFree);
    return std::filesystem::path(owner.get());
}

std::filesystem::path temporary_path(const std::filesystem::path& destination)
{
    static std::atomic<std::uint64_t> serial{0};
    auto result = destination;

    result += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L"." +
              std::to_wstring(serial.fetch_add(1)) + L".tmp";

    return result;
}

std::expected<void, std::wstring> save_shell_link(IPersistFile& persistence, const std::filesystem::path& path)
{
    const bool replacing = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    const HRESULT result = persistence.Save(path.c_str(), TRUE);

    if (result != S_OK)
    {
        return std::unexpected(L"Windows could not save the launcher shortcut: " +
                               windows_error(static_cast<DWORD>(result)));
    }

    persistence.SaveCompleted(path.c_str());
    SHChangeNotify(replacing ? SHCNE_UPDATEITEM : SHCNE_CREATE, SHCNF_PATHW | SHCNF_FLUSH, path.c_str(), nullptr);
    return {};
}

std::expected<void, std::wstring> atomic_write(const std::filesystem::path& path, std::string_view content)
{
    if (path.empty() || !path.is_absolute())
    {
        return std::unexpected(L"The local settings directory could not be located.");
    }

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);

    if (error)
    {
        return std::unexpected(L"The settings directory could not be created.");
    }

    const auto temporary = temporary_path(path);
    TemporaryFile cleanup(temporary);

    {
        const HANDLE file =
            CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (file == INVALID_HANDLE_VALUE)
        {
            return std::unexpected(L"Settings could not be written: " + windows_error(GetLastError()));
        }

        const Handle handle(file);
        DWORD written = 0;

        if (!WriteFile(file, content.data(), static_cast<DWORD>(content.size()), &written, nullptr) ||
            written != content.size() || !FlushFileBuffers(file))
        {
            return std::unexpected(L"Settings could not be saved: " + windows_error(GetLastError()));
        }
    }

    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        return std::unexpected(L"Settings could not be replaced: " + windows_error(GetLastError()));
    }

    cleanup.release();
    return {};
}

std::optional<std::uint32_t> parse_number(std::string_view value)
{
    std::uint32_t number = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);

    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
    {
        return std::nullopt;
    }

    return number;
}

std::optional<Settings> parse_settings(std::string_view bytes)
{
    Settings settings;
    unsigned int present = 0;

    while (!bytes.empty())
    {
        const auto end = bytes.find('\n');
        auto line = bytes.substr(0, end);
        bytes = end == std::string_view::npos ? std::string_view{} : bytes.substr(end + 1);

        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }

        const auto separator = line.find('=');

        if (separator == std::string_view::npos)
        {
            return std::nullopt;
        }

        const auto key = line.substr(0, separator);
        const auto value = parse_number(line.substr(separator + 1));

        if (!value)
        {
            return std::nullopt;
        }

        unsigned int bit = 0;

        if (key == "schema" && *value == 1)
        {
            bit = 1;
        }
        else if (key == "hotkey_modifiers")
        {
            bit = 2;
            settings.hotkey_modifiers = *value;
        }
        else if (key == "hotkey_key")
        {
            bit = 4;
            settings.hotkey_key = *value;
        }
        else if (key == "start_hidden" && *value <= 1)
        {
            bit = 8;
            settings.start_hidden = *value != 0;
        }
        else
        {
            return std::nullopt;
        }

        if ((present & bit) != 0)
        {
            return std::nullopt;
        }

        present |= bit;
    }

    if (present != 15 || !valid_hotkey(settings))
    {
        return std::nullopt;
    }

    return settings;
}

LaunchResult create_process(const PreparedLaunch& launch)
{
    std::vector<wchar_t> command(launch.command_line.begin(), launch.command_line.end());
    command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    if (!CreateProcessW(launch.application.c_str(),
                        command.data(),
                        nullptr,
                        nullptr,
                        FALSE,
                        0,
                        nullptr,
                        launch.working_directory.c_str(),
                        &startup,
                        &process))
    {
        const DWORD error = GetLastError();
        return {LaunchStatus::process_error, L"Battle.net could not be started: " + windows_error(error), error};
    }

    const Handle process_handle(process.hProcess);
    const Handle thread_handle(process.hThread);

    return {
        LaunchStatus::requested, L"Request sent to Battle.net. Sign in or finish updates in Battle.net if needed.", 0};
}
}

bool valid_hotkey(const Settings& settings) noexcept
{
    const auto key = settings.hotkey_key;

    const bool supported_key =
        (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || (key >= VK_F1 && key <= VK_F24 && key != VK_F12);

    return (settings.hotkey_modifiers & ~7U) == 0 && (settings.hotkey_modifiers & 3U) != 0 && supported_key;
}

std::wstring hotkey_text(const Settings& settings)
{
    std::wstring text;

    if ((settings.hotkey_modifiers & MOD_CONTROL) != 0)
    {
        text += L"Ctrl + ";
    }

    if ((settings.hotkey_modifiers & MOD_ALT) != 0)
    {
        text += L"Alt + ";
    }

    if ((settings.hotkey_modifiers & MOD_SHIFT) != 0)
    {
        text += L"Shift + ";
    }

    if (settings.hotkey_key >= VK_F1 && settings.hotkey_key <= VK_F24)
    {
        text += L"F" + std::to_wstring(settings.hotkey_key - VK_F1 + 1);
    }
    else
    {
        text += static_cast<wchar_t>(settings.hotkey_key);
    }

    return text;
}

std::filesystem::path settings_path()
{
    const auto root = known_folder(FOLDERID_LocalAppData);

    if (root.empty())
    {
        return {};
    }

    return root / L"BetterNet" / L"settings.ini";
}

SettingsLoad load_settings(const std::filesystem::path& path) noexcept
{
    try
    {
        const HANDLE file = CreateFileW(path.c_str(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);

        if (file == INVALID_HANDLE_VALUE)
        {
            const DWORD error = GetLastError();

            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            {
                return {};
            }

            return {{}, L"Settings could not be read. The default shortcut is active."};
        }

        const Handle handle(file);
        LARGE_INTEGER size{};

        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4096)
        {
            return {{}, L"Settings were invalid. The default shortcut is active."};
        }

        std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
        DWORD read = 0;

        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) || read != bytes.size())
        {
            return {{}, L"Settings could not be read. The default shortcut is active."};
        }

        const auto settings = parse_settings(bytes);

        if (!settings)
        {
            return {{}, L"Settings were invalid. The default shortcut is active."};
        }

        return {*settings, {}};
    }
    catch (...)
    {
        return {{}, L"Settings could not be loaded. Defaults are active."};
    }
}

std::expected<void, std::wstring> save_settings(const Settings& settings, const std::filesystem::path& path) noexcept
{
    try
    {
        if (!valid_hotkey(settings))
        {
            return std::unexpected(L"Choose Ctrl or Alt with a letter, number, or function key other than F12.");
        }

        const std::string bytes = "schema=1\nhotkey_modifiers=" + std::to_string(settings.hotkey_modifiers) +
                                  "\nhotkey_key=" + std::to_string(settings.hotkey_key) +
                                  "\nstart_hidden=" + (settings.start_hidden ? "1\n" : "0\n");

        return atomic_write(path, bytes);
    }
    catch (...)
    {
        return std::unexpected(L"Settings could not be saved. The current shortcut remains active for this session.");
    }
}

bool LaunchResult::sent() const noexcept
{
    return status == LaunchStatus::requested;
}

std::wstring quote_windows_argument(std::wstring_view value)
{
    std::wstring result = L"\"";
    std::size_t backslashes = 0;

    for (const wchar_t character : value)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (character == L'"')
        {
            result.append(backslashes * 2 + 1, L'\\');
        }
        else
        {
            result.append(backslashes, L'\\');
        }

        backslashes = 0;
        result += character;
    }

    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

std::expected<PreparedLaunch, LaunchResult> prepare_launch(const LaunchRequest& request)
{
    const auto expected_code = launch_code_for(request.game.product_id);

    if (!expected_code || *expected_code != request.game.launch_code || expected_code->empty() ||
        !std::ranges::all_of(*expected_code,
                             [](wchar_t character)
                             {
                                 return (character >= L'A' && character <= L'Z') ||
                                        (character >= L'a' && character <= L'z') ||
                                        (character >= L'0' && character <= L'9') || character == L'_';
                             }))
    {
        return std::unexpected(LaunchResult{
            LaunchStatus::invalid_descriptor, L"This installation has no verified Battle.net launch route.", 0});
    }

    std::error_code error;

    if (!request.client_path.is_absolute() || !std::filesystem::is_regular_file(request.client_path, error))
    {
        return std::unexpected(LaunchResult{
            LaunchStatus::missing_client, L"Battle.net is missing. Install or repair it, then press F5 to rescan.", 0});
    }

    if (!validate_game(request.game))
    {
        return std::unexpected(LaunchResult{LaunchStatus::missing_installation,
                                            L"The game installation changed or is missing. Press F5 to rescan.",
                                            0});
    }

    PreparedLaunch prepared;
    prepared.application = request.client_path;
    prepared.working_directory = request.client_path.parent_path();

    prepared.command_line = quote_windows_argument(request.client_path.native()) + L" --exec=" +
                            quote_windows_argument(L"launch " + *expected_code);

    return prepared;
}

LaunchResult launch_game(const LaunchRequest& request, std::stop_token stop, const ProcessAdapter& adapter) noexcept
{
    try
    {
        if (stop.stop_requested())
        {
            return {LaunchStatus::cancelled, L"Launch request cancelled.", 0};
        }

        auto prepared = prepare_launch(request);

        if (!prepared)
        {
            return prepared.error();
        }

        if (stop.stop_requested())
        {
            return {LaunchStatus::cancelled, L"Launch request cancelled.", 0};
        }

        return adapter ? adapter(*prepared) : create_process(*prepared);
    }
    catch (...)
    {
        return {LaunchStatus::process_error, L"The launch request failed. Press F5 to rescan and try again.", 0};
    }
}

LaunchGuard::LaunchGuard(std::chrono::milliseconds timeout)
    : timeout_(std::clamp(timeout, std::chrono::milliseconds(1), std::chrono::milliseconds(30000)))
{
}

std::optional<std::uint64_t> LaunchGuard::try_begin(Clock::time_point now)
{
    const std::scoped_lock lock(mutex_);

    if (active_ && now < deadline_)
    {
        return std::nullopt;
    }

    active_ = ++sequence_;
    deadline_ = now + timeout_;
    return active_;
}

void LaunchGuard::finish(std::uint64_t ticket, bool sent, Clock::time_point now)
{
    const std::scoped_lock lock(mutex_);

    if (active_ == ticket && (!sent || now >= deadline_))
    {
        active_.reset();
    }
}

void LaunchGuard::cancel()
{
    const std::scoped_lock lock(mutex_);
    active_.reset();
}

bool LaunchGuard::busy(Clock::time_point now) const
{
    const std::scoped_lock lock(mutex_);
    return active_.has_value() && now < deadline_;
}

std::expected<void, std::wstring> create_launcher_shortcut(const std::filesystem::path& executable,
                                                           const std::filesystem::path& shortcut_path,
                                                           const Settings& settings) noexcept
{
    try
    {
        std::error_code error;

        if (!valid_hotkey(settings) || !executable.is_absolute() ||
            !std::filesystem::is_regular_file(executable, error) || !shortcut_path.is_absolute() ||
            shortcut_path.extension() != L".lnk")
        {
            return std::unexpected(L"The launcher path or shortcut key is invalid.");
        }

        std::filesystem::create_directories(shortcut_path.parent_path(), error);

        if (error)
        {
            return std::unexpected(L"The shortcut directory is unavailable.");
        }

        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        {
            return std::unexpected(L"Windows could not create a shortcut in this session.");
        }

        struct Apartment
        {
            bool owned;

            ~Apartment()
            {
                if (owned)
                {
                    CoUninitialize();
                }
            }
        };

        const Apartment apartment{SUCCEEDED(initialized)};
        Microsoft::WRL::ComPtr<IShellLinkW> link;

        HRESULT result = CoCreateInstance(CLSID_ShellLink,
                                          nullptr,
                                          CLSCTX_INPROC_SERVER,
                                          IID_IShellLinkW,
                                          reinterpret_cast<void**>(link.GetAddressOf()));

        BYTE modifiers = 0;

        if ((settings.hotkey_modifiers & MOD_CONTROL) != 0)
        {
            modifiers |= HOTKEYF_CONTROL;
        }

        if ((settings.hotkey_modifiers & MOD_ALT) != 0)
        {
            modifiers |= HOTKEYF_ALT;
        }

        if ((settings.hotkey_modifiers & MOD_SHIFT) != 0)
        {
            modifiers |= HOTKEYF_SHIFT;
        }

        if (SUCCEEDED(result))
        {
            result = link->SetPath(executable.c_str());
        }

        if (SUCCEEDED(result))
        {
            result = link->SetWorkingDirectory(executable.parent_path().c_str());
        }

        if (SUCCEEDED(result))
        {
            result = link->SetDescription(L"Open better-net");
        }

        if (SUCCEEDED(result))
        {
            result = link->SetArguments(L"--show");
        }

        if (SUCCEEDED(result))
        {
            result = link->SetHotkey(MAKEWORD(static_cast<BYTE>(settings.hotkey_key), modifiers));
        }

        if (SUCCEEDED(result))
        {
            result = link->SetShowCmd(SW_SHOWNORMAL);
        }

        if (SUCCEEDED(result))
        {
            result = link->SetIconLocation(executable.c_str(), 0);
        }

        Microsoft::WRL::ComPtr<IPersistFile> persistence;

        if (SUCCEEDED(result))
        {
            result = link.As(&persistence);
        }

        if (FAILED(result))
        {
            return std::unexpected(L"Windows could not save the launcher shortcut: " +
                                   windows_error(static_cast<DWORD>(result)));
        }

        return save_shell_link(*persistence.Get(), shortcut_path);
    }
    catch (...)
    {
        return std::unexpected(L"The launcher shortcut could not be created.");
    }
}

std::expected<std::filesystem::path, std::wstring>
install_launcher_shortcut(const std::filesystem::path& executable, const Settings& settings, bool desktop) noexcept
{
    try
    {
        const auto folder = known_folder(desktop ? FOLDERID_Desktop : FOLDERID_Programs);

        if (folder.empty())
        {
            return std::unexpected(L"Windows could not locate your shortcut directory.");
        }

        const auto destination = folder / L"better-net.lnk";
        const auto result = create_launcher_shortcut(executable, destination, settings);

        if (!result)
        {
            return std::unexpected(result.error());
        }

        return destination;
    }
    catch (...)
    {
        return std::unexpected(L"The launcher shortcut could not be installed.");
    }
}

std::expected<void, std::wstring> disable_launcher_shortcut_at(const std::filesystem::path& executable,
                                                               const std::filesystem::path& shortcut_path) noexcept
{
    try
    {
        if (!executable.is_absolute() || !shortcut_path.is_absolute() || shortcut_path.extension() != L".lnk")
        {
            return std::unexpected(L"The launcher path or shortcut path is invalid.");
        }

        const DWORD attributes = GetFileAttributesW(shortcut_path.c_str());

        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = GetLastError();

            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            {
                return {};
            }

            return std::unexpected(L"The Windows shortcut could not be read: " + windows_error(error));
        }

        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        {
            return std::unexpected(L"Windows could not update the launcher shortcut in this session.");
        }

        struct Apartment
        {
            bool owned;

            ~Apartment()
            {
                if (owned)
                {
                    CoUninitialize();
                }
            }
        };

        const Apartment apartment{SUCCEEDED(initialized)};
        Microsoft::WRL::ComPtr<IShellLinkW> link;
        Microsoft::WRL::ComPtr<IPersistFile> persistence;

        if (FAILED(CoCreateInstance(CLSID_ShellLink,
                                    nullptr,
                                    CLSCTX_INPROC_SERVER,
                                    IID_IShellLinkW,
                                    reinterpret_cast<void**>(link.GetAddressOf()))) ||
            FAILED(link.As(&persistence)) || FAILED(persistence->Load(shortcut_path.c_str(), STGM_READ)))
        {
            return std::unexpected(L"Windows could not read the launcher shortcut.");
        }

        std::array<wchar_t, 32768> target{};

        if (link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH) != S_OK ||
            CompareStringOrdinal(target.data(), -1, executable.c_str(), -1, TRUE) != CSTR_EQUAL)
        {
            return std::unexpected(L"This shortcut belongs to another application and was left unchanged.");
        }

        WORD hotkey = 0;

        if (FAILED(link->GetHotkey(&hotkey)))
        {
            return std::unexpected(L"Windows could not read the shortcut key.");
        }

        if (hotkey == 0)
        {
            return {};
        }

        if (FAILED(link->SetHotkey(0)))
        {
            return std::unexpected(L"Windows could not disable the shortcut key.");
        }

        return save_shell_link(*persistence.Get(), shortcut_path);
    }
    catch (...)
    {
        return std::unexpected(L"The Windows shortcut key could not be disabled.");
    }
}

std::expected<void, std::wstring> disable_launcher_shortcut(const std::filesystem::path& executable,
                                                            bool desktop) noexcept
{
    try
    {
        const auto folder = known_folder(desktop ? FOLDERID_Desktop : FOLDERID_Programs);

        if (folder.empty())
        {
            return std::unexpected(L"Windows could not locate your shortcut directory.");
        }

        return disable_launcher_shortcut_at(executable, folder / L"better-net.lnk");
    }
    catch (...)
    {
        return std::unexpected(L"The Windows shortcut key could not be disabled.");
    }
}

namespace
{
std::optional<WORD> read_owned_shortcut_hotkey(const std::filesystem::path& executable, bool desktop)
{
    const auto folder = known_folder(desktop ? FOLDERID_Desktop : FOLDERID_Programs);

    if (folder.empty())
    {
        return std::nullopt;
    }

    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
    {
        return std::nullopt;
    }

    struct Apartment
    {
        bool owned;

        ~Apartment()
        {
            if (owned)
            {
                CoUninitialize();
            }
        }
    };

    const Apartment apartment{SUCCEEDED(initialized)};
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persistence;

    if (FAILED(CoCreateInstance(CLSID_ShellLink,
                                nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW,
                                reinterpret_cast<void**>(link.GetAddressOf()))) ||
        FAILED(link.As(&persistence)) || FAILED(persistence->Load((folder / L"better-net.lnk").c_str(), STGM_READ)))
    {
        return std::nullopt;
    }

    std::array<wchar_t, 32768> target{};
    WORD hotkey = 0;

    if (link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH) != S_OK ||
        FAILED(link->GetHotkey(&hotkey)) ||
        CompareStringOrdinal(target.data(), -1, executable.c_str(), -1, TRUE) != CSTR_EQUAL)
    {
        return std::nullopt;
    }

    return hotkey;
}
}

bool launcher_shortcut_matches(const std::filesystem::path& executable, const Settings& settings, bool desktop) noexcept
{
    try
    {
        const auto hotkey = read_owned_shortcut_hotkey(executable, desktop);
        BYTE modifiers = 0;

        if ((settings.hotkey_modifiers & MOD_CONTROL) != 0)
        {
            modifiers |= HOTKEYF_CONTROL;
        }

        if ((settings.hotkey_modifiers & MOD_ALT) != 0)
        {
            modifiers |= HOTKEYF_ALT;
        }

        if ((settings.hotkey_modifiers & MOD_SHIFT) != 0)
        {
            modifiers |= HOTKEYF_SHIFT;
        }

        return hotkey && *hotkey == MAKEWORD(static_cast<BYTE>(settings.hotkey_key), modifiers);
    }
    catch (...)
    {
        return false;
    }
}

bool launcher_shortcut_enabled(const std::filesystem::path& executable, bool desktop) noexcept
{
    try
    {
        const auto hotkey = read_owned_shortcut_hotkey(executable, desktop);
        return hotkey && *hotkey != 0;
    }
    catch (...)
    {
        return false;
    }
}

bool startup_enabled(const std::filesystem::path& executable) noexcept
{
    try
    {
        if (!executable.is_absolute())
        {
            return false;
        }

        HKEY raw = nullptr;

        if (RegOpenKeyExW(HKEY_CURRENT_USER, startup_key, 0, KEY_QUERY_VALUE, &raw) != ERROR_SUCCESS)
        {
            return false;
        }

        const RegistryHandle key(raw, RegCloseKey);
        std::wstring current;

        return read_startup_command(key.get(), current) == ERROR_SUCCESS &&
               current == quote_windows_argument(executable.native()) + L" --background";
    }
    catch (...)
    {
        return false;
    }
}

std::expected<void, std::wstring> set_startup_enabled(const std::filesystem::path& executable, bool enabled) noexcept
{
    try
    {
        if (!executable.is_absolute())
        {
            return std::unexpected(L"The launcher path must be absolute to change Windows startup.");
        }

        const auto command = quote_windows_argument(executable.native()) + L" --background";

        if (enabled)
        {
            std::error_code error;
            DWORD binary_type = 0;

            if (!std::filesystem::is_regular_file(executable, error) ||
                !GetBinaryTypeW(executable.c_str(), &binary_type))
            {
                return std::unexpected(L"The launcher executable could not be found or validated.");
            }

            if (command.size() > 260)
            {
                return std::unexpected(
                    L"The launcher path is too long for Windows startup. Move it to a shorter path.");
            }
        }

        HKEY raw = nullptr;

        const auto opened =
            enabled ? RegCreateKeyExW(HKEY_CURRENT_USER,
                                      startup_key,
                                      0,
                                      nullptr,
                                      0,
                                      KEY_QUERY_VALUE | KEY_SET_VALUE,
                                      nullptr,
                                      &raw,
                                      nullptr)
                    : RegOpenKeyExW(HKEY_CURRENT_USER, startup_key, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &raw);

        if (!enabled && (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND))
        {
            return {};
        }

        if (opened != ERROR_SUCCESS)
        {
            return std::unexpected(L"Windows startup settings could not be opened: " + windows_error(opened));
        }

        const RegistryHandle key(raw, RegCloseKey);
        std::wstring current;
        const auto read = read_startup_command(key.get(), current);

        if (read != ERROR_SUCCESS && read != ERROR_FILE_NOT_FOUND)
        {
            return std::unexpected(L"The existing Windows startup entry could not be read: " + windows_error(read));
        }

        if (read == ERROR_SUCCESS && current != command)
        {
            return std::unexpected(L"The better-net startup entry belongs to another command and was left unchanged.");
        }

        if ((enabled && read == ERROR_SUCCESS) || (!enabled && read == ERROR_FILE_NOT_FOUND))
        {
            return {};
        }

        const auto result = enabled ? RegSetValueExW(key.get(),
                                                     L"better-net",
                                                     0,
                                                     REG_SZ,
                                                     reinterpret_cast<const BYTE*>(command.c_str()),
                                                     static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)))
                                    : RegDeleteValueW(key.get(), L"better-net");

        if (result != ERROR_SUCCESS)
        {
            return std::unexpected(L"Windows startup settings could not be changed: " + windows_error(result));
        }

        return {};
    }
    catch (...)
    {
        return std::unexpected(L"Windows startup settings could not be changed.");
    }
}
}
