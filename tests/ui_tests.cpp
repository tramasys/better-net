#include "platform.hpp"
#include <Windows.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{
using Clock = std::chrono::steady_clock;
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

std::wstring window_text(HWND window)
{
    std::array<wchar_t, 2048> text{};
    DWORD_PTR result = 0;

    require(SendMessageTimeoutW(window,
                                WM_GETTEXT,
                                text.size(),
                                reinterpret_cast<LPARAM>(text.data()),
                                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                1000,
                                &result) != 0,
            "Window text query timed out");

    return text.data();
}

struct WindowQuery
{
    DWORD process;
    const wchar_t* class_name;
    HWND found = nullptr;
};

BOOL CALLBACK find_window(HWND window, LPARAM parameter)
{
    auto& query = *reinterpret_cast<WindowQuery*>(parameter);
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    std::array<wchar_t, 128> name{};
    GetClassNameW(window, name.data(), static_cast<int>(name.size()));

    if (process == query.process && std::wstring_view(name.data()) == query.class_name)
    {
        query.found = window;
        return FALSE;
    }

    return TRUE;
}

HWND process_window(DWORD process, const wchar_t* class_name)
{
    WindowQuery query{process, class_name};
    EnumWindows(find_window, reinterpret_cast<LPARAM>(&query));
    return query.found;
}

HWND status_window(HWND parent)
{
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    {
        std::array<wchar_t, 64> name{};
        GetClassNameW(child, name.data(), static_cast<int>(name.size()));

        if (std::wstring_view(name.data()) == L"Static")
        {
            return child;
        }
    }

    return nullptr;
}

HWND focused_window(HWND parent)
{
    GUITHREADINFO information{};
    information.cbSize = sizeof(information);
    const DWORD thread = GetWindowThreadProcessId(parent, nullptr);
    require(GetGUIThreadInfo(thread, &information) != FALSE, "Could not inspect launcher keyboard focus");
    return information.hwndFocus;
}

class TestProcess
{
public:

    TestProcess(const std::filesystem::path& executable, const std::filesystem::path& report, bool empty)
        : process_(nullptr, CloseHandle), job_(CreateJobObjectW(nullptr, nullptr), CloseHandle)
    {
        require(job_ != nullptr, "Could not create UI test process container");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;

        require(SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)) !=
                    FALSE,
                "Could not configure UI test process cleanup");

        std::wstring command = bnet::quote_windows_argument(executable.native()) + L" --dry-run --show --diagnostics " +
                               bnet::quote_windows_argument(report.native());

        if (empty)
        {
            command += L" --empty-library";
        }

        std::vector<wchar_t> arguments(command.begin(), command.end());
        arguments.push_back(L'\0');
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION created{};

        require(CreateProcessW(executable.c_str(),
                               arguments.data(),
                               nullptr,
                               nullptr,
                               FALSE,
                               CREATE_SUSPENDED,
                               nullptr,
                               executable.parent_path().c_str(),
                               &startup,
                               &created) != FALSE,
                "Could not start dry-run launcher process");

        process_.reset(created.hProcess);
        const Handle thread(created.hThread, CloseHandle);
        process_id_ = created.dwProcessId;

        if (!AssignProcessToJobObject(job_.get(), process_.get()))
        {
            TerminateProcess(process_.get(), 1);
            throw std::runtime_error("Could not contain owned dry-run launcher process");
        }

        require(ResumeThread(thread.get()) != static_cast<DWORD>(-1), "Could not resume dry-run launcher process");

        wait(
            [this]
            {
                window = process_window(process_id_, L"BetterNet.Main.1");
                return window != nullptr;
            },
            std::chrono::seconds(15),
            "Launcher window did not appear; close any existing Better Net instance");

        wait(
            [this]
            {
                const HWND refresh = GetDlgItem(window, 100);
                const HWND status = status_window(window);

                if (!refresh || !IsWindowEnabled(refresh) || !status)
                {
                    return false;
                }

                const auto text = window_text(status);
                return !text.starts_with(L"Looking for") && !text.starts_with(L"Checking local");
            },
            std::chrono::seconds(15),
            "Local library scan did not finish");
    }

    ~TestProcess()
    {
        if (window && IsWindow(window) && WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT)
        {
            PostMessageW(window, WM_COMMAND, 104, 0);
            WaitForSingleObject(process_.get(), 2000);
        }
    }

    TestProcess(const TestProcess&) = delete;
    TestProcess& operator=(const TestProcess&) = delete;

    template <class Predicate> void wait(Predicate predicate, std::chrono::milliseconds timeout, const char* message)
    {
        const auto deadline = Clock::now() + timeout;

        while (!predicate())
        {
            require(Clock::now() < deadline, message);
            require(WaitForSingleObject(process_.get(), 10) == WAIT_TIMEOUT, "Dry-run launcher exited unexpectedly");
        }
    }

    void key(WPARAM value, bool repeated = false)
    {
        const HWND focus = focused_window(window);
        DWORD focus_process = 0;
        GetWindowThreadProcessId(focus, &focus_process);
        const HWND target = focus && focus_process == process_id_ ? focus : window;

        require(PostMessageW(target, WM_KEYDOWN, value, repeated ? (1LL << 30) | 1 : 1) != FALSE,
                "Could not queue a launcher keyboard message");
    }

    void command(int identifier, HWND target = nullptr)
    {
        DWORD_PTR result = 0;

        require(SendMessageTimeoutW(target ? target : window,
                                    WM_COMMAND,
                                    static_cast<WPARAM>(identifier),
                                    0,
                                    SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                    2000,
                                    &result) != 0,
                "Launcher command did not respond");
    }

    HWND settings() const
    {
        return process_window(process_id_, L"BetterNet.Settings.1");
    }

    std::wstring status() const
    {
        const HWND control = status_window(window);
        require(control != nullptr, "Launcher status text is missing");
        return window_text(control);
    }

    void finish()
    {
        command(104);
        require(WaitForSingleObject(process_.get(), 5000) == WAIT_OBJECT_0, "Launcher workers did not exit promptly");
        DWORD code = 1;
        require(GetExitCodeProcess(process_.get(), &code) != FALSE && code == 0, "Launcher exited with an error");
        window = nullptr;
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};

        require(QueryInformationJobObject(
                    job_.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) !=
                        FALSE &&
                    accounting.ActiveProcesses == 0,
                "UI test left a process running");
    }

    HWND window = nullptr;
private:
    Handle process_;
    Handle job_;
    DWORD process_id_ = 0;
};

void check_layout(HWND window, int expected_left)
{
    RECT parent{};
    GetClientRect(window, &parent);
    LONG previous_right = 0;

    for (int identifier = 200; identifier < 203; ++identifier)
    {
        const HWND card = GetDlgItem(window, identifier);
        RECT bounds{};
        require(GetWindowRect(card, &bounds) != FALSE, "Card bounds unavailable");
        MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&bounds), 2);

        require(bounds.left >= previous_right && bounds.right <= parent.right && bounds.bottom <= parent.bottom &&
                    bounds.top >= 0 && bounds.right - bounds.left >= 100 && bounds.bottom - bounds.top >= 100,
                "Cards overlap or exceed resized client bounds");

        if (identifier == 200)
        {
            require(bounds.left == expected_left, "Card margin did not follow requested DPI");
        }

        previous_right = bounds.right;
    }
}

void test_layout(TestProcess& app)
{
    require(SetWindowPos(app.window, nullptr, 30, 30, 1160, 730, SWP_NOZORDER | SWP_NOACTIVATE) != FALSE,
            "Launcher resize failed");

    const UINT actual_dpi = GetDpiForWindow(app.window);
    check_layout(app.window, MulDiv(36, static_cast<int>(actual_dpi), 96));
    RECT suggested{30, 30, 1470, 1050};
    DWORD_PTR result = 0;

    require(SendMessageTimeoutW(app.window,
                                WM_DPICHANGED,
                                MAKEWPARAM(144, 144),
                                reinterpret_cast<LPARAM>(&suggested),
                                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                2000,
                                &result) != 0,
            "Synthetic DPI change did not respond");

    check_layout(app.window, 54);
    RECT restored{30, 30, 1190, 760};

    require(SendMessageTimeoutW(app.window,
                                WM_DPICHANGED,
                                MAKEWPARAM(actual_dpi, actual_dpi),
                                reinterpret_cast<LPARAM>(&restored),
                                SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                2000,
                                &result) != 0,
            "DPI restore did not respond");
}

void test_settings_dialog(TestProcess& app)
{
    app.command(101);

    app.wait(
        [&]
        {
            return app.settings() != nullptr;
        },
        std::chrono::seconds(2),
        "Shortcut dialog did not open");

    const HWND dialog = app.settings();
    require(!IsWindowEnabled(app.window), "Shortcut dialog must disable its owner");
    const HWND initial_focus = focused_window(dialog);
    require(initial_focus == GetDlgItem(dialog, 300), "Shortcut dialog should focus key capture");
    app.key(VK_TAB);

    app.wait(
        [&]
        {
            return focused_window(dialog) != initial_focus;
        },
        std::chrono::seconds(2),
        "Tab did not move focus inside the shortcut dialog");

    app.key(VK_ESCAPE);

    app.wait(
        [&]
        {
            return !app.settings() && IsWindowEnabled(app.window);
        },
        std::chrono::seconds(2),
        "Escape did not close shortcut dialog and restore owner");
}

unsigned int test_installed_library(TestProcess& app)
{
    const HWND first = GetDlgItem(app.window, 200);

    if (!IsWindowEnabled(first))
    {
        std::cout << "Installed-library dispatch checks skipped: this machine has no launchable local game.\n";
        return 0;
    }

    app.key(VK_RIGHT);
    const HWND expected = IsWindowEnabled(GetDlgItem(app.window, 201)) ? GetDlgItem(app.window, 201) : first;

    app.wait(
        [&]
        {
            return focused_window(app.window) == expected;
        },
        std::chrono::seconds(2),
        "Right arrow did not select and focus next game");

    app.key(VK_LEFT);

    app.wait(
        [&]
        {
            return focused_window(app.window) == first;
        },
        std::chrono::seconds(2),
        "Left arrow did not restore first game");

    app.key(VK_TAB);

    app.wait(
        [&]
        {
            return focused_window(app.window) != first;
        },
        std::chrono::seconds(2),
        "Tab did not move keyboard focus");

    app.key(VK_RIGHT);

    app.wait(
        [&]
        {
            return focused_window(app.window) == expected;
        },
        std::chrono::seconds(2),
        "Keyboard selection did not recover after Tab");

    app.key(VK_LEFT);

    app.wait(
        [&]
        {
            return focused_window(app.window) == first;
        },
        std::chrono::seconds(2),
        "Could not select first game before dry-run launch");

    app.key(VK_RETURN);

    app.wait(
        [&]
        {
            return app.status().starts_with(L"Dry run passed.");
        },
        std::chrono::seconds(3),
        "Enter did not reach dry-run launch adapter");

    require(!IsWindowEnabled(first), "Launch request must disable repeat activation during cooldown");

    for (int iteration = 0; iteration < 12; ++iteration)
    {
        app.key(VK_RETURN, iteration % 2 == 0);
        app.command(200);
    }

    app.wait(
        [&]
        {
            return IsWindowEnabled(first) != FALSE;
        },
        std::chrono::seconds(6),
        "Launch controls did not recover after bounded cooldown");

    app.command(200);

    app.wait(
        [&]
        {
            return app.status().starts_with(L"Dry run passed.");
        },
        std::chrono::seconds(2),
        "Launch could not be dispatched after cooldown");

    return 2;
}

void verify_report(const std::filesystem::path& path, unsigned int expected_requests, bool empty)
{
    std::ifstream input(path, std::ios::binary);
    const std::string report{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const std::string expected = "\"dry_run_requests\": " + std::to_string(expected_requests) + ",";
    require(report.find(expected) != std::string::npos, "Unexpected dry-run dispatch count; duplicate guard failed");

    if (empty)
    {
        require(report.find("\"games\": 0,") != std::string::npos, "Empty-library scenario discovered games");
    }
}

void test_empty_library(TestProcess& app)
{
    for (int identifier = 200; identifier < 203; ++identifier)
    {
        const HWND card = GetDlgItem(app.window, identifier);

        require(!IsWindowEnabled(card) && window_text(card) == L"No game in this slot",
                "Empty-library cards should remain unavailable");
    }

    require(app.status().find(L"No installed games found") != std::wstring::npos,
            "Empty library should show an actionable status");

    app.key(VK_LEFT);
    app.key(VK_RIGHT);
    app.command(200);
    app.key(VK_RETURN, true);
    app.command(101);

    app.wait(
        [&]
        {
            return app.settings() != nullptr;
        },
        std::chrono::seconds(2),
        "Empty library should leave launcher controls responsive");

    app.command(303, app.settings());
}
}

int wmain(int count, wchar_t** values)
{
    try
    {
        require(SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) != FALSE,
                "Could not set UI test DPI awareness");

        require(count == 2, "Usage: ui_tests <absolute path to better-net.exe>");
        const std::filesystem::path executable = std::filesystem::absolute(values[1]);
        require(std::filesystem::is_regular_file(executable), "Launcher executable does not exist");

        const auto directory =
            std::filesystem::temp_directory_path() / (L"Better Net UI tests " + std::to_wstring(GetCurrentProcessId()) +
                                                      L" " + std::to_wstring(GetTickCount64()));

        std::filesystem::create_directories(directory);
        const auto installed_report = directory / L"installed.json";
        unsigned int requests = 0;

        {
            TestProcess app(executable, installed_report, false);
            test_layout(app);
            test_settings_dialog(app);
            requests = test_installed_library(app);
            app.finish();
        }

        verify_report(installed_report, requests, false);
        const auto empty_report = directory / L"empty.json";

        {
            TestProcess app(executable, empty_report, true);
            test_empty_library(app);
            app.finish();
        }

        verify_report(empty_report, 0, true);
        std::cout << "UI tests passed: keyboard, launch guard, resizing, synthetic DPI, dialog, empty library, exit.\n";
        std::cout << "All launches used the dry-run adapter; no settings or Windows shortcuts were changed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
