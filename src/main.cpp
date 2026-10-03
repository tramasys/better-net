#include "catalog.hpp"
#include "platform.hpp"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr wchar_t window_class[] = L"BetterNet.Main.1";
constexpr wchar_t settings_class[] = L"BetterNet.Settings.1";
constexpr UINT catalog_ready = WM_APP + 1;
constexpr UINT launch_ready = WM_APP + 2;
constexpr UINT tray_message = WM_APP + 3;
constexpr UINT restore_message = WM_APP + 4;
constexpr int refresh_id = 100;
constexpr int settings_id = 101;
constexpr int previous_id = 102;
constexpr int next_id = 103;
constexpr int exit_id = 104;
constexpr int details_id = 105;
constexpr int card_base = 200;
constexpr int hotkey_id = 300;
constexpr int save_id = 301;
constexpr int shortcut_id = 302;
constexpr int cancel_id = 303;

void require(HRESULT result)
{
    if (FAILED(result))
    {
        throw std::runtime_error("A Windows graphics operation failed.");
    }
}

D2D1_COLOR_F color(UINT32 value, float alpha = 1.0F)
{
    return D2D1::ColorF(value, alpha);
}

struct Pixels
{
    UINT width = 0;
    UINT height = 0;
    std::vector<BYTE> data;
};

Pixels decode_image(const std::filesystem::path& path)
{
    Pixels result;
    std::error_code error;

    if (path.empty() || std::filesystem::file_size(path, error) > 16 * 1024 * 1024 || error)
    {
        return result;
    }

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;

    require(CoCreateInstance(CLSID_WICImagingFactory,
                             nullptr,
                             CLSCTX_INPROC_SERVER,
                             IID_IWICImagingFactory,
                             reinterpret_cast<void**>(factory.GetAddressOf())));

    require(factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder));

    require(decoder->GetFrame(0, &frame));
    UINT width = 0;
    UINT height = 0;
    require(frame->GetSize(&width, &height));

    if (!width || !height || width > 16000 || height > 16000 || static_cast<UINT64>(width) * height > 64000000)
    {
        return result;
    }

    const float factor = std::min(1.0F, 720.0F / static_cast<float>(std::max(width, height)));
    result.width = std::max(1U, static_cast<UINT>(static_cast<float>(width) * factor));
    result.height = std::max(1U, static_cast<UINT>(static_cast<float>(height) * factor));
    require(factory->CreateBitmapScaler(&scaler));
    require(scaler->Initialize(frame.Get(), result.width, result.height, WICBitmapInterpolationModeFant));
    require(factory->CreateFormatConverter(&converter));

    require(converter->Initialize(
        scaler.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));

    result.data.resize(static_cast<std::size_t>(result.width) * result.height * 4);

    require(
        converter->CopyPixels(nullptr, result.width * 4, static_cast<UINT>(result.data.size()), result.data.data()));

    return result;
}

struct ScanResult
{
    bnet::Catalog catalog;
    std::vector<Pixels> images;
};

struct Graphics
{
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> text_factory;
    ComPtr<ID2D1HwndRenderTarget> window_target;
    ComPtr<ID2D1DCRenderTarget> control_target;
    ComPtr<ID2D1SolidColorBrush> window_brush;
    ComPtr<ID2D1SolidColorBrush> control_brush;
    std::array<ComPtr<IDWriteTextFormat>, 6> fonts;
    std::vector<ComPtr<ID2D1Bitmap>> images;
    UINT dpi = 96;

    void initialize()
    {
        require(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));

        require(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                    __uuidof(IDWriteFactory),
                                    reinterpret_cast<IUnknown**>(text_factory.GetAddressOf())));

        constexpr std::array<float, 6> sizes{12, 14, 18, 26, 38, 64};

        for (std::size_t index = 0; index < sizes.size(); ++index)
        {
            require(
                text_factory->CreateTextFormat(L"Segoe UI",
                                               nullptr,
                                               index >= 3 ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL,
                                               sizes[index],
                                               L"en-us",
                                               &fonts[index]));

            fonts[index]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }

        fonts[4]->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);

        const auto properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

        require(factory->CreateDCRenderTarget(&properties, &control_target));
        require(control_target->CreateSolidColorBrush(color(0xFFFFFF), &control_brush));
    }

    void ensure_window(HWND window)
    {
        if (window_target)
        {
            return;
        }

        RECT area{};
        GetClientRect(window, &area);

        require(factory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(area.right, area.bottom)),
            &window_target));

        window_target->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        require(window_target->CreateSolidColorBrush(color(0xFFFFFF), &window_brush));
    }

    void text(ID2D1RenderTarget* target,
              ID2D1SolidColorBrush* brush,
              std::wstring_view value,
              D2D1_RECT_F area,
              std::size_t font,
              UINT32 tint)
    {
        brush->SetColor(color(tint));

        target->DrawTextW(value.data(),
                          static_cast<UINT32>(value.size()),
                          fonts[font].Get(),
                          area,
                          brush,
                          D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void round(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, D2D1_RECT_F area, float radius, UINT32 tint)
    {
        brush->SetColor(color(tint));
        target->FillRoundedRectangle(D2D1::RoundedRect(area, radius, radius), brush);
    }

    void upload(const std::vector<Pixels>& pixels)
    {
        images.clear();
        images.resize(pixels.size());

        for (std::size_t index = 0; index < pixels.size(); ++index)
        {
            const auto& source = pixels[index];

            if (!source.data.empty())
            {
                const auto properties = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

                control_target->CreateBitmap(D2D1::SizeU(source.width, source.height),
                                             source.data.data(),
                                             source.width * 4,
                                             properties,
                                             &images[index]);
            }
        }
    }
};

struct App
{
    HWND window = nullptr;
    HWND settings_window = nullptr;
    HWND status_control = nullptr;
    HWND hotkey_control = nullptr;
    HWND settings_status = nullptr;
    std::array<HWND, 3> cards{};
    std::array<HWND, 6> buttons{};
    HFONT control_font = nullptr;
    HBRUSH background = CreateSolidBrush(RGB(18, 21, 29));
    HBRUSH panel_background = CreateSolidBrush(RGB(26, 30, 41));
    Graphics graphics;
    bnet::Settings settings;
    bnet::Catalog catalog;
    bnet::LaunchGuard launch_guard;
    std::jthread scan_worker;
    std::jthread launch_worker;
    std::mutex results_mutex;
    std::optional<ScanResult> scan_result;
    std::optional<bnet::LaunchResult> launch_result;
    std::size_t selected = 0;
    std::size_t page = 0;
    bool scanning = false;
    bool closing = false;
    bool tray_added = false;
    bool hotkey_registered = false;
    bool dry_run = false;
    bool empty_library = false;
    bool diagnostics = false;
    bool shortcut_installed = false;
    bool force_show = false;
    std::atomic<bool> launch_running = false;
    std::filesystem::path diagnostics_path;
    std::wstring status = L"Looking for installed games...";
    std::wstring count_text = L"SCANNING LOCAL INSTALLATIONS";
    std::wstring shortcut_text;
    Clock::time_point started = Clock::now();
    double interactive_ms = 0;
    double library_ms = 0;
    double selection_us = 0;
    std::atomic<std::uint32_t> dry_run_requests = 0;
    UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    ~App()
    {
        scan_worker.request_stop();
        launch_worker.request_stop();

        if (scan_worker.joinable())
        {
            scan_worker.join();
        }

        if (launch_worker.joinable())
        {
            launch_worker.join();
        }

        DeleteObject(control_font);
        DeleteObject(background);
        DeleteObject(panel_background);
    }

    int px(float value) const
    {
        return static_cast<int>(std::lround(value * static_cast<float>(graphics.dpi) / 96.0F));
    }

    void set_status(std::wstring value)
    {
        status = std::move(value);
        SetWindowTextW(status_control, status.c_str());
    }

    HWND button(HWND parent, int id, const wchar_t* title)
    {
        return CreateWindowExW(0,
                               L"BUTTON",
                               title,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               0,
                               0,
                               1,
                               1,
                               parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr),
                               nullptr);
    }

    void create_controls()
    {
        buttons[0] = button(window, refresh_id, L"Refresh");
        buttons[1] = button(window, settings_id, L"Shortcut");
        buttons[2] = button(window, previous_id, L"Previous");
        buttons[3] = button(window, next_id, L"Next");
        buttons[4] = button(window, exit_id, L"Exit");
        buttons[5] = button(window, details_id, L"Scan details");

        for (std::size_t index = 0; index < cards.size(); ++index)
        {
            cards[index] = button(window, card_base + static_cast<int>(index), L"Game");
        }

        status_control = CreateWindowExW(0,
                                         L"STATIC",
                                         status.c_str(),
                                         WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         0,
                                         0,
                                         1,
                                         1,
                                         window,
                                         nullptr,
                                         GetModuleHandleW(nullptr),
                                         nullptr);

        update_font();
    }

    void update_font()
    {
        HFONT old = control_font;

        control_font = CreateFontW(-px(13),
                                   0,
                                   0,
                                   0,
                                   FW_NORMAL,
                                   FALSE,
                                   FALSE,
                                   FALSE,
                                   DEFAULT_CHARSET,
                                   OUT_DEFAULT_PRECIS,
                                   CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY,
                                   DEFAULT_PITCH,
                                   L"Segoe UI");

        SendMessageW(status_control, WM_SETFONT, reinterpret_cast<WPARAM>(control_font), TRUE);
        DeleteObject(old);
    }

    void layout()
    {
        RECT area{};
        GetClientRect(window, &area);
        const float width = static_cast<float>(area.right) * 96.0F / static_cast<float>(graphics.dpi);
        const float height = static_cast<float>(area.bottom) * 96.0F / static_cast<float>(graphics.dpi);
        const float card_width = (width - 104) / 3;
        const float card_height = height - 255;

        for (std::size_t index = 0; index < cards.size(); ++index)
        {
            MoveWindow(cards[index],
                       px(36 + static_cast<float>(index) * (card_width + 16)),
                       px(150),
                       px(card_width),
                       px(card_height),
                       TRUE);
        }

        MoveWindow(buttons[0], px(width - 222), px(34), px(88), px(34), TRUE);
        MoveWindow(buttons[1], px(width - 126), px(34), px(90), px(34), TRUE);
        MoveWindow(buttons[2], px(width - 216), px(height - 76), px(86), px(30), TRUE);
        MoveWindow(buttons[3], px(width - 122), px(height - 76), px(86), px(30), TRUE);
        MoveWindow(buttons[4], px(width - 79), px(height - 34), px(43), px(25), TRUE);
        MoveWindow(buttons[5], px(width - 183), px(height - 34), px(96), px(25), TRUE);
        MoveWindow(status_control, px(38), px(height - 79), px(width - 290), px(38), TRUE);
        InvalidateRect(window, nullptr, FALSE);
    }

    void register_hotkey()
    {
        UnregisterHotKey(window, 1);
        std::array<wchar_t, 32768> executable{};
        GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        shortcut_installed = bnet::launcher_shortcut_matches(executable.data(), settings);

        hotkey_registered =
            shortcut_installed ||
            RegisterHotKey(window, 1, settings.hotkey_modifiers | MOD_NOREPEAT, settings.hotkey_key) != 0;

        shortcut_text = bnet::hotkey_text(settings) + L"  /  OPEN LAUNCHER";

        if (!hotkey_registered)
        {
            shortcut_text = bnet::hotkey_text(settings) + L"  /  SHORTCUT IN USE";
            set_status(L"The shortcut is in use by Windows or another app. Choose a different key in Shortcut.");
        }

        InvalidateRect(window, nullptr, FALSE);
    }

    void add_tray()
    {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = 1;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = tray_message;
        data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(data.szTip, L"Better Net - open your games");
        tray_added = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    }

    void restore()
    {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);

        if (settings_window)
        {
            SetForegroundWindow(settings_window);
            SetFocus(hotkey_control);
        }
        else
        {
            SetFocus(catalog.games.empty() ? buttons[0] : cards[selected % 3]);
        }
    }

    void hide()
    {
        if (tray_added)
        {
            ShowWindow(window, SW_HIDE);
        }
        else
        {
            ShowWindow(window, SW_MINIMIZE);
        }
    }

    void update_cards()
    {
        const bool busy = launch_guard.busy() || launch_running;

        for (std::size_t slot = 0; slot < cards.size(); ++slot)
        {
            const auto index = page * 3 + slot;
            const bool exists = index < catalog.games.size();
            EnableWindow(cards[slot], exists && !busy);

            if (exists)
            {
                const auto name = L"Play " + catalog.games[index].title + L". " + catalog.games[index].version;
                SetWindowTextW(cards[slot], name.c_str());
            }
            else
            {
                SetWindowTextW(cards[slot], L"No game in this slot");
            }

            InvalidateRect(cards[slot], nullptr, FALSE);
        }

        EnableWindow(buttons[2], page > 0);
        EnableWindow(buttons[3], (page + 1) * 3 < catalog.games.size());
        EnableWindow(buttons[0], !scanning);
        EnableWindow(buttons[5], !catalog.warnings.empty());
    }

    void scan()
    {
        if (scanning || closing || launch_guard.busy() || launch_running)
        {
            return;
        }

        scanning = true;
        set_status(L"Checking local installations...");
        update_cards();

        scan_worker = std::jthread(
            [this](std::stop_token stop)
            {
                const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                ScanResult result;

                try
                {
                    if (!empty_library)
                    {
                        result.catalog = bnet::discover(stop);
                    }

                    result.images.resize(std::min<std::size_t>(result.catalog.games.size(), 12));

                    for (std::size_t index = 0; index < result.images.size() && !stop.stop_requested(); ++index)
                    {
                        try
                        {
                            result.images[index] = decode_image(result.catalog.games[index].artwork_path);
                        }
                        catch (...)
                        {
                        }
                    }
                }
                catch (...)
                {
                    result.catalog.scan_failed = true;
                    result.catalog.warnings.push_back(L"Discovery could not finish. Press Refresh to try again.");
                }

                if (SUCCEEDED(initialized))
                {
                    CoUninitialize();
                }

                if (!stop.stop_requested())
                {
                    std::lock_guard lock(results_mutex);
                    scan_result = std::move(result);
                    PostMessageW(window, catalog_ready, 0, 0);
                }
            });
    }

    void receive_catalog()
    {
        std::optional<ScanResult> result;

        {
            std::lock_guard lock(results_mutex);
            result.swap(scan_result);
        }

        if (!result)
        {
            return;
        }

        catalog = std::move(result->catalog);
        graphics.upload(result->images);
        scanning = false;
        selected = std::min(selected, catalog.games.empty() ? 0 : catalog.games.size() - 1);
        page = selected / 3;

        count_text = std::to_wstring(catalog.games.size()) +
                     (catalog.games.size() == 1 ? L" GAME INSTALLED" : L" GAMES INSTALLED");

        if (catalog.scan_failed)
        {
            set_status(L"Some installation records could not be read. Open Scan details, then try Refresh.");
        }
        else if (catalog.games.empty())
        {
            set_status(L"No installed games found. Install a game in Battle.net, then choose Refresh.");
        }
        else if (!catalog.warnings.empty())
        {
            set_status(L"Library ready. Some records need attention; open Scan details.");
        }
        else
        {
            set_status(L"Choose a game. Battle.net handles sign-in and updates.");
        }

        update_cards();
        InvalidateRect(window, nullptr, FALSE);
        library_ms = std::chrono::duration<double, std::milli>(Clock::now() - started).count();

        if (diagnostics)
        {
            const auto before = Clock::now();

            for (int iteration = 0; iteration < 100; ++iteration)
            {
                select(iteration % 2 ? -1 : 1);
                UpdateWindow(window);

                for (HWND card : cards)
                {
                    UpdateWindow(card);
                }
            }

            selection_us = std::chrono::duration<double, std::micro>(Clock::now() - before).count() / 100;
            SetTimer(window, 2, 10000, nullptr);
        }
    }

    void select(int direction)
    {
        if (catalog.games.empty())
        {
            return;
        }

        const auto count = static_cast<std::ptrdiff_t>(catalog.games.size());
        selected = static_cast<std::size_t>((static_cast<std::ptrdiff_t>(selected) + direction + count) % count);
        page = selected / 3;
        update_cards();
        SetFocus(cards[selected % 3]);
    }

    void launch(std::size_t index)
    {
        if (index >= catalog.games.size() || scanning || closing || launch_running)
        {
            return;
        }

        const auto ticket = launch_guard.try_begin();

        if (!ticket)
        {
            return;
        }

        selected = index;
        set_status(L"Sending launch request to Battle.net...");
        update_cards();
        SetTimer(window, 1, 5200, nullptr);
        bnet::LaunchRequest request{catalog.games[index], catalog.client_path};
        launch_running = true;

        launch_worker = std::jthread(
            [this, request = std::move(request), ticket = *ticket](std::stop_token stop)
            {
                bnet::LaunchResult result;

                try
                {
                    bnet::ProcessAdapter adapter;

                    if (dry_run)
                    {
                        adapter = [this](const bnet::PreparedLaunch&)
                        {
                            ++dry_run_requests;

                            return bnet::LaunchResult{bnet::LaunchStatus::requested,
                                                      L"Dry run passed. No game or Battle.net process was started.",
                                                      0};
                        };
                    }

                    result = bnet::launch_game(request, stop, adapter);
                }
                catch (...)
                {
                    result.status = bnet::LaunchStatus::process_error;
                }

                launch_guard.finish(ticket, result.sent());
                launch_running = false;

                if (!stop.stop_requested())
                {
                    std::lock_guard lock(results_mutex);
                    launch_result = std::move(result);
                    PostMessageW(window, launch_ready, 0, 0);
                }
            });
    }

    void receive_launch()
    {
        std::optional<bnet::LaunchResult> result;

        {
            std::lock_guard lock(results_mutex);
            result.swap(launch_result);
        }

        if (result)
        {
            set_status(result->message.empty() ? L"Launch failed. Refresh and try again." : result->message);
            update_cards();
        }
    }

    void paint()
    {
        PAINTSTRUCT paint_info{};
        BeginPaint(window, &paint_info);

        try
        {
            graphics.ensure_window(window);
            auto* target = graphics.window_target.Get();
            auto* brush = graphics.window_brush.Get();
            const auto size = target->GetSize();
            target->BeginDraw();
            target->Clear(color(0x12151D));
            graphics.round(target, brush, D2D1::RectF(36, 32, 64, 60), 8, 0xB1F18A);
            graphics.text(target, brush, L"b", D2D1::RectF(44, 31, 64, 61), 2, 0x172119);
            graphics.text(target, brush, L"better net", D2D1::RectF(76, 33, 300, 61), 2, 0xEFF2F8);
            graphics.text(target, brush, L"Your games.", D2D1::RectF(36, 78, size.width - 36, 129), 4, 0xF1F3F9);
            graphics.text(target, brush, count_text, D2D1::RectF(39, 128, size.width - 36, 148), 0, 0x8D96AA);

            graphics.text(target,
                          brush,
                          L"\x2190  \x2192  SELECT     ENTER  PLAY     ESC  HIDE TO TRAY",
                          D2D1::RectF(38, size.height - 29, size.width - 230, size.height - 8),
                          0,
                          0x808B9F);

            graphics.text(target, brush, shortcut_text, D2D1::RectF(380, 40, size.width - 232, 65), 0, 0xA6B1C6);

            if (FAILED(target->EndDraw()))
            {
                graphics.window_brush.Reset();
                graphics.window_target.Reset();
            }
        }
        catch (...)
        {
            FillRect(paint_info.hdc, &paint_info.rcPaint, background);
            SetTextColor(paint_info.hdc, RGB(255, 255, 255));
            SetBkMode(paint_info.hdc, TRANSPARENT);
            RECT area{30, 30, 700, 150};

            DrawTextW(paint_info.hdc,
                      L"Graphics initialization failed. Close Better Net and try again.",
                      -1,
                      &area,
                      DT_LEFT | DT_WORDBREAK);
        }

        EndPaint(window, &paint_info);
    }

    void
    draw_card(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, D2D1_SIZE_F size, std::size_t slot, bool focused)
    {
        const auto index = page * 3 + slot;
        graphics.round(target, brush, D2D1::RectF(1, 1, size.width - 1, size.height - 1), 16, 0x1C212C);

        if (index >= catalog.games.size())
        {
            graphics.round(target, brush, D2D1::RectF(18, 18, size.width - 18, size.height - 113), 10, 0x222836);

            graphics.text(target,
                          brush,
                          L"+",
                          D2D1::RectF(size.width / 2 - 18, size.height / 2 - 88, size.width / 2 + 25, size.height / 2),
                          5,
                          0x4E5C71);

            graphics.text(target,
                          brush,
                          scanning ? L"Finding your games" : L"Room for more",
                          D2D1::RectF(21, size.height - 93, size.width - 16, size.height - 61),
                          2,
                          0xA6B1C6);

            graphics.text(target,
                          brush,
                          scanning ? L"Reading local install records" : L"Install in Battle.net, then refresh.",
                          D2D1::RectF(21, size.height - 54, size.width - 16, size.height - 26),
                          0,
                          0x77859B);

            return;
        }

        const auto& game = catalog.games[index];
        const bool active = index == selected;
        const bool beta = game.product_id.find(L"beta") != std::wstring::npos;
        const UINT32 accent = beta ? 0xABBAFD : 0xB1F18A;
        const float art_bottom = size.height - 119;
        const auto art = D2D1::RectF(13, 13, size.width - 13, art_bottom);
        graphics.round(target, brush, art, 10, beta ? 0x2B3049 : 0x2B3938);
        target->PushAxisAlignedClip(art, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        if (index < graphics.images.size() && graphics.images[index])
        {
            const auto image_size = graphics.images[index]->GetSize();
            const auto art_width = art.right - art.left;
            const auto art_height = art.bottom - art.top;
            const auto factor = std::max(art_width / image_size.width, art_height / image_size.height);
            const auto source_width = art_width / factor;
            const auto source_height = art_height / factor;

            const auto source = D2D1::RectF((image_size.width - source_width) / 2,
                                            (image_size.height - source_height) / 2,
                                            (image_size.width + source_width) / 2,
                                            (image_size.height + source_height) / 2);

            target->DrawBitmap(graphics.images[index].Get(), art, 0.72F, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, source);
        }
        else
        {
            brush->SetColor(color(beta ? 0x606EB1 : 0x839985, 0.17F));

            for (int line = 0; line < 9; ++line)
            {
                const float x = static_cast<float>(line) * 42 - 90;

                target->DrawLine(
                    D2D1::Point2F(x, art_bottom + 50), D2D1::Point2F(x + size.width * 0.75F, -40), brush, 18);
            }

            brush->SetColor(color(accent, 0.12F));
            target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(size.width - 15, 35), 125, 125), brush, 1);
            target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(size.width - 15, 35), 104, 104), brush, 1);

            graphics.text(
                target, brush, game.title, D2D1::RectF(30, 85, size.width - 24, art_bottom - 15), 4, 0xE0E7E2);
        }

        target->PopAxisAlignedClip();
        graphics.round(target, brush, D2D1::RectF(26, 27, beta ? 121 : 138, 51), 6, 0x151B23);

        graphics.text(
            target, brush, beta ? L"BETA CHANNEL" : L"INSTALLED LOCALLY", D2D1::RectF(35, 30, 160, 49), 0, accent);

        graphics.text(
            target, brush, game.title, D2D1::RectF(20, art_bottom + 13, size.width - 15, art_bottom + 44), 2, 0xF0F3F8);

        const auto version = game.version.empty() ? std::wstring(L"Version unavailable") : L"Build " + game.version;

        graphics.text(
            target, brush, version, D2D1::RectF(21, art_bottom + 42, size.width - 15, art_bottom + 63), 0, 0x8E9CB2);

        graphics.round(target,
                       brush,
                       D2D1::RectF(19, size.height - 44, size.width - 19, size.height - 15),
                       7,
                       active ? accent : 0x303A4B);

        graphics.text(target,
                      brush,
                      launch_guard.busy() || launch_running ? L"Request in progress..." : L"Play  \x2197",
                      D2D1::RectF(32, size.height - 40, size.width - 23, size.height - 16),
                      1,
                      active ? 0x19251A : 0xD4DDEB);

        if (active || focused)
        {
            brush->SetColor(color(focused ? 0xEDF5FF : accent));

            target->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(2, 2, size.width - 2, size.height - 2), 15, 15),
                                         brush,
                                         focused ? 2.0F : 1.0F);
        }
    }

    void draw_control(const DRAWITEMSTRUCT& item)
    {
        auto* target = graphics.control_target.Get();
        auto* brush = graphics.control_brush.Get();
        require(target->BindDC(item.hDC, &item.rcItem));
        target->SetDpi(static_cast<float>(graphics.dpi), static_cast<float>(graphics.dpi));
        const auto size = target->GetSize();
        target->BeginDraw();
        target->Clear(color(0x12151D));
        const bool focused = (item.itemState & ODS_FOCUS) != 0;

        if (item.CtlID >= card_base && item.CtlID < card_base + 3)
        {
            draw_card(target, brush, size, item.CtlID - card_base, focused);
        }
        else
        {
            const bool disabled = (item.itemState & ODS_DISABLED) != 0;

            graphics.round(target,
                           brush,
                           D2D1::RectF(1, 1, size.width - 1, size.height - 1),
                           7,
                           (item.itemState & ODS_SELECTED) ? 0x40506A : 0x242B39);

            std::array<wchar_t, 128> label{};
            GetWindowTextW(item.hwndItem, label.data(), static_cast<int>(label.size()));

            graphics.text(target,
                          brush,
                          label.data(),
                          D2D1::RectF(12, (size.height - 19) / 2, size.width - 5, size.height),
                          0,
                          disabled ? 0x5D6A80 : 0xCDD6E5);

            if (focused)
            {
                brush->SetColor(color(0xB1F18A));

                target->DrawRoundedRectangle(
                    D2D1::RoundedRect(D2D1::RectF(2, 2, size.width - 2, size.height - 2), 6, 6), brush);
            }
        }

        if (FAILED(target->EndDraw()))
        {
            InvalidateRect(item.hwndItem, nullptr, FALSE);
        }
    }

    void scan_details()
    {
        std::wstring details;

        for (const auto& warning : catalog.warnings)
        {
            details += warning + L"\n\n";
        }

        if (details.empty())
        {
            details = L"Local installation records were read successfully.";
        }

        MessageBoxW(window, details.c_str(), L"Better Net - scan details", MB_OK | MB_ICONINFORMATION);
    }

    void write_diagnostics()
    {
        std::ofstream output(diagnostics_path);

        output << "{\n  \"interactive_ms\": " << interactive_ms << ",\n  \"library_ms\": " << library_ms
               << ",\n  \"selection_and_paint_us\": " << selection_us << ",\n  \"games\": " << catalog.games.size()
               << ",\n  \"warnings\": " << catalog.warnings.size()
               << ",\n  \"dry_run_requests\": " << dry_run_requests.load()
               << ",\n  \"hotkey_registered\": " << (hotkey_registered ? "true" : "false") << "\n}\n";
    }

    void shutdown()
    {
        closing = true;
        scan_worker.request_stop();
        launch_worker.request_stop();
        launch_guard.cancel();
        UnregisterHotKey(window, 1);
        KillTimer(window, 1);
        KillTimer(window, 2);
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data);

        if (scan_worker.joinable())
        {
            scan_worker.join();
        }

        if (launch_worker.joinable())
        {
            launch_worker.join();
        }

        if (diagnostics)
        {
            write_diagnostics();
        }

        DestroyWindow(window);
    }

    void open_settings();
    void save_shortcut(bool install);
};

LRESULT CALLBACK settings_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    if (!app)
    {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    try
    {
        switch (message)
        {
        case WM_COMMAND:
            if (LOWORD(wparam) == save_id || LOWORD(wparam) == shortcut_id)
            {
                app->save_shortcut(LOWORD(wparam) == shortcut_id);
                return 0;
            }

            if (LOWORD(wparam) != cancel_id)
            {
                break;
            }

            [[fallthrough]];

        case WM_CLOSE:
            DestroyWindow(window);

            return 0;

        case WM_DESTROY:
            app->settings_window = nullptr;

            EnableWindow(app->window, TRUE);
            app->restore();
            return 0;

        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wparam), RGB(209, 219, 235));

            SetBkColor(reinterpret_cast<HDC>(wparam), RGB(18, 21, 29));
            return reinterpret_cast<LRESULT>(app->background);

        case WM_DRAWITEM:
            app->draw_control(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));

            return TRUE;

        default:
            break;
        }
    }
    catch (...)
    {
        SetWindowTextW(app->settings_status, L"This change could not be completed. Try again.");
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

void App::open_settings()
{
    if (settings_window)
    {
        SetForegroundWindow(settings_window);
        return;
    }

    RECT owner{};
    GetWindowRect(window, &owner);

    settings_window = CreateWindowExW(WS_EX_CONTROLPARENT,
                                      settings_class,
                                      L"Better Net - keyboard shortcut",
                                      WS_CAPTION | WS_SYSMENU | WS_POPUP,
                                      owner.left + px(180),
                                      owner.top + px(110),
                                      px(580),
                                      px(340),
                                      window,
                                      nullptr,
                                      GetModuleHandleW(nullptr),
                                      this);

    if (!settings_window)
    {
        set_status(L"Shortcut settings could not be opened. Please try again.");
        return;
    }

    const auto label = [this](const wchar_t* value, int x, int y, int width, int height)
    {
        const auto control = CreateWindowExW(0,
                                             L"STATIC",
                                             value,
                                             WS_CHILD | WS_VISIBLE,
                                             px(static_cast<float>(x)),
                                             px(static_cast<float>(y)),
                                             px(static_cast<float>(width)),
                                             px(static_cast<float>(height)),
                                             settings_window,
                                             nullptr,
                                             GetModuleHandleW(nullptr),
                                             nullptr);

        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(control_font), TRUE);
        return control;
    };

    label(L"Open Better Net from anywhere", 24, 22, 500, 26);
    label(L"Press Ctrl or Alt with a letter, number, or function key (except F12).", 24, 59, 520, 27);

    hotkey_control = CreateWindowExW(WS_EX_CLIENTEDGE,
                                     HOTKEY_CLASSW,
                                     L"Open launcher shortcut",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                     px(24),
                                     px(90),
                                     px(520),
                                     px(34),
                                     settings_window,
                                     reinterpret_cast<HMENU>(hotkey_id),
                                     GetModuleHandleW(nullptr),
                                     nullptr);

    const WORD modifiers = static_cast<WORD>(((settings.hotkey_modifiers & MOD_CONTROL) ? HOTKEYF_CONTROL : 0) |
                                             ((settings.hotkey_modifiers & MOD_ALT) ? HOTKEYF_ALT : 0) |
                                             ((settings.hotkey_modifiers & MOD_SHIFT) ? HOTKEYF_SHIFT : 0));

    SendMessageW(hotkey_control, HKM_SETHOTKEY, MAKEWORD(settings.hotkey_key, modifiers), 0);
    SendMessageW(hotkey_control, WM_SETFONT, reinterpret_cast<WPARAM>(control_font), TRUE);

    label(L"Save keeps it active while Better Net runs in the tray.\n"
          L"Create Windows shortcut also enables starting it when closed.",
          24,
          141,
          520,
          43);

    settings_status = label(L"Closing the main window hides it. Exit fully stops the launcher.", 24, 198, 520, 45);
    MoveWindow(button(settings_window, save_id, L"Save"), px(24), px(253), px(76), px(32), TRUE);

    MoveWindow(
        button(settings_window, shortcut_id, L"Create Windows shortcut"), px(111), px(253), px(209), px(32), TRUE);

    MoveWindow(button(settings_window, cancel_id, L"Done"), px(467), px(253), px(77), px(32), TRUE);
    EnableWindow(window, FALSE);
    ShowWindow(settings_window, SW_SHOW);
    SetFocus(hotkey_control);
}

void App::save_shortcut(bool install)
{
    const auto key = static_cast<WORD>(SendMessageW(hotkey_control, HKM_GETHOTKEY, 0, 0));
    const auto flags = HIBYTE(key);
    auto replacement = settings;
    replacement.hotkey_key = LOBYTE(key);

    replacement.hotkey_modifiers = ((flags & HOTKEYF_CONTROL) ? MOD_CONTROL : 0) |
                                   ((flags & HOTKEYF_ALT) ? MOD_ALT : 0) | ((flags & HOTKEYF_SHIFT) ? MOD_SHIFT : 0);

    if (!bnet::valid_hotkey(replacement))
    {
        SetWindowTextW(settings_status, L"Use Ctrl or Alt with a letter, number, or function key (except F12).");
        return;
    }

    const bool changed =
        replacement.hotkey_key != settings.hotkey_key || replacement.hotkey_modifiers != settings.hotkey_modifiers;

    const bool update_link = shortcut_installed;

    if (changed && !RegisterHotKey(window, 2, replacement.hotkey_modifiers | MOD_NOREPEAT, replacement.hotkey_key))
    {
        SetWindowTextW(settings_status, L"That shortcut is in use by another app. Try a different combination.");
        return;
    }

    UnregisterHotKey(window, 2);
    const auto saved = bnet::save_settings(replacement);

    if (!saved)
    {
        SetWindowTextW(settings_status, saved.error().c_str());
        return;
    }

    settings = replacement;

    if (install || update_link)
    {
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));

        if (!length || length >= path.size())
        {
            SetWindowTextW(settings_status, L"Could not find this launcher's executable path.");
            return;
        }

        UnregisterHotKey(window, 1);
        const auto created = bnet::install_launcher_shortcut(path.data(), settings);
        shortcut_installed = created.has_value();
        register_hotkey();

        SetWindowTextW(settings_status,
                       created ? L"Windows shortcut created in Start. This key can now start Better Net when closed."
                               : created.error().c_str());
    }
    else
    {
        register_hotkey();

        SetWindowTextW(settings_status,
                       hotkey_registered ? L"Saved. Your shortcut opens Better Net while it runs."
                                         : L"Saved, but Windows already owns this shortcut.");
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    if (message == WM_NCCREATE)
    {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    if (!app)
    {
        return DefWindowProcW(window, message, wparam, lparam);
    }

    try
    {
        if (message == app->taskbar_created)
        {
            app->add_tray();
            return 0;
        }

        switch (message)
        {
        case WM_PAINT:
            app->paint();

            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED)
            {
                app->hide();
                return 0;
            }

            if (app->graphics.window_target)
            {
                app->graphics.window_target->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
            }

            if (app->status_control)
            {
                app->layout();
            }

            return 0;

        case WM_DPICHANGED:
            app->graphics.dpi = HIWORD(wparam);

            if (app->graphics.window_target)
            {
                app->graphics.window_target->SetDpi(static_cast<float>(app->graphics.dpi),
                                                    static_cast<float>(app->graphics.dpi));
            }

            {
                const auto area = reinterpret_cast<RECT*>(lparam);

                SetWindowPos(window,
                             nullptr,
                             area->left,
                             area->top,
                             area->right - area->left,
                             area->bottom - area->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }

            app->update_font();
            app->layout();
            return 0;

        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = POINT{app->px(860), app->px(570)};

            return 0;

        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wparam), RGB(168, 181, 202));

            SetBkColor(reinterpret_cast<HDC>(wparam), RGB(18, 21, 29));
            return reinterpret_cast<LRESULT>(app->background);

        case WM_DRAWITEM:
            app->draw_control(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));

            return TRUE;

        case WM_COMMAND:
            if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) >= card_base && LOWORD(wparam) < card_base + 3)
            {
                app->launch(app->page * 3 + LOWORD(wparam) - card_base);
                return 0;
            }

            switch (LOWORD(wparam))
            {
            case refresh_id:
                app->scan();

                break;

            case settings_id:
                app->open_settings();

                break;

            case previous_id:
                if (app->page > 0)
                {
                    app->select(static_cast<int>((app->page - 1) * 3) - static_cast<int>(app->selected));
                }

                break;

            case next_id:
                if ((app->page + 1) * 3 < app->catalog.games.size())
                {
                    app->select(static_cast<int>((app->page + 1) * 3) - static_cast<int>(app->selected));
                }

                break;

            case exit_id:
                app->shutdown();

                break;

            case details_id:
                app->scan_details();

                break;

            default:
                break;
            }

            return 0;

        case WM_TIMER:
            KillTimer(window, wparam);

            if (wparam == 1)
            {
                app->update_cards();
            }
            else if (wparam == 2)
            {
                app->shutdown();
            }

            return 0;

        case catalog_ready:
            app->receive_catalog();

            return 0;

        case launch_ready:
            app->receive_launch();

            return 0;

        case restore_message:
        case WM_HOTKEY:
            app->restore();

            return 0;

        case tray_message:
            if (lparam == WM_LBUTTONUP || lparam == WM_LBUTTONDBLCLK)
            {
                app->restore();
            }
            else if (lparam == WM_RBUTTONUP)
            {
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"Open Better Net");
                AppendMenuW(menu, MF_STRING, 2, L"Exit");
                POINT position{};
                GetCursorPos(&position);
                SetForegroundWindow(window);

                const auto command =
                    TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, position.x, position.y, 0, window, nullptr);

                DestroyMenu(menu);

                if (command == 1)
                {
                    app->restore();
                }
                else if (command == 2)
                {
                    app->shutdown();
                }
            }

            return 0;

        case WM_CLOSE:
            app->hide();

            return 0;

        case WM_ENDSESSION:
            if (wparam)
            {
                app->shutdown();
            }

            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);

            return 0;

        default:
            break;
        }
    }
    catch (...)
    {
        app->set_status(L"An operation failed. Try Refresh or restart Better Net.");
    }

    return DefWindowProcW(window, message, wparam, lparam);
}

int run(HINSTANCE instance)
{
    int argument_count = 0;
    auto* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    App app;

    for (int index = 1; index < argument_count; ++index)
    {
        const std::wstring_view argument(arguments[index]);

        if (argument == L"--dry-run")
        {
            app.dry_run = true;
        }
        else if (argument == L"--empty-library")
        {
            app.empty_library = true;
        }
        else if (argument == L"--show")
        {
            app.force_show = true;
        }
        else if (argument == L"--diagnostics" && index + 1 < argument_count)
        {
            app.diagnostics = true;
            app.diagnostics_path = arguments[++index];
            app.dry_run = true;
        }
    }

    LocalFree(arguments);
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\BetterNet.SingleInstance.1");

    if (!singleton)
    {
        return 1;
    }

    const auto mutex = std::unique_ptr<void, decltype(&CloseHandle)>(singleton, &CloseHandle);

    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (const auto existing = FindWindowW(window_class, nullptr))
        {
            DWORD process = 0;
            GetWindowThreadProcessId(existing, &process);
            AllowSetForegroundWindow(process);
            PostMessageW(existing, restore_message, 0, 0);
        }

        return 0;
    }

    app.graphics.initialize();
    WNDCLASSEXW definition{};
    definition.cbSize = sizeof(definition);
    definition.lpfnWndProc = window_proc;
    definition.hInstance = instance;
    definition.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    definition.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    definition.lpszClassName = window_class;
    require(RegisterClassExW(&definition) ? S_OK : E_FAIL);
    definition.lpfnWndProc = settings_proc;
    definition.lpszClassName = settings_class;
    definition.hbrBackground = app.background;
    require(RegisterClassExW(&definition) ? S_OK : E_FAIL);
    app.graphics.dpi = GetDpiForSystem();

    app.window = CreateWindowExW(WS_EX_CONTROLPARENT,
                                 window_class,
                                 L"Better Net",
                                 WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT,
                                 CW_USEDEFAULT,
                                 app.px(1110),
                                 app.px(680),
                                 nullptr,
                                 nullptr,
                                 instance,
                                 &app);

    if (!app.window)
    {
        throw std::runtime_error("The launcher window could not be created.");
    }

    app.graphics.dpi = GetDpiForWindow(app.window);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(app.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(app.window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    app.create_controls();
    const auto loaded = bnet::load_settings();
    app.settings = loaded.value;
    app.layout();
    app.register_hotkey();
    app.add_tray();
    app.scan();
    const bool start_hidden = app.settings.start_hidden && app.tray_added && !app.diagnostics && !app.force_show;
    ShowWindow(app.window, start_hidden ? SW_HIDE : SW_SHOW);

    if (!start_hidden)
    {
        ShowWindow(app.window, SW_SHOW);
    }

    UpdateWindow(app.window);
    app.interactive_ms = std::chrono::duration<double, std::milli>(Clock::now() - app.started).count();

    if (!loaded.warning.empty())
    {
        app.set_status(loaded.warning);
    }

    MSG message{};

    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (app.settings_window)
        {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE)
            {
                SendMessageW(app.settings_window, WM_CLOSE, 0, 0);
                continue;
            }

            if (IsDialogMessageW(app.settings_window, &message))
            {
                continue;
            }
        }
        else if (message.message == WM_KEYDOWN)
        {
            const auto key = message.wParam;

            if (key == VK_LEFT || key == VK_RIGHT)
            {
                app.select(key == VK_LEFT ? -1 : 1);
                continue;
            }

            if (key == VK_RETURN && (message.lParam & (1LL << 30)) == 0)
            {
                const auto focus = GetFocus();
                const auto id = GetDlgCtrlID(focus);

                if (id >= card_base && id < card_base + 3)
                {
                    app.launch(app.page * 3 + static_cast<std::size_t>(id - card_base));
                }
                else if (focus && focus != app.window)
                {
                    SendMessageW(focus, BM_CLICK, 0, 0);
                }
                else
                {
                    app.launch(app.selected);
                }

                continue;
            }

            if (key == VK_ESCAPE)
            {
                app.hide();
                continue;
            }

            if (key == VK_F5)
            {
                app.scan();
                continue;
            }

            if (key == VK_TAB && IsDialogMessageW(app.window, &message))
            {
                continue;
            }
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    return static_cast<int>(message.wParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    int result = 1;

    try
    {
        result = run(instance);
    }
    catch (...)
    {
        MessageBoxW(nullptr,
                    L"Better Net could not start. Windows graphics or local settings may be unavailable.",
                    L"Better Net",
                    MB_OK | MB_ICONERROR);
    }

    if (SUCCEEDED(initialized))
    {
        CoUninitialize();
    }

    return result;
}
