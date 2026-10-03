#include "catalog.hpp"
#include "platform.hpp"
#include "presentation.hpp"
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
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
constexpr wchar_t window_class[] = L"BetterNet.Main.1";
constexpr UINT catalog_ready = WM_APP + 1;
constexpr UINT launch_ready = WM_APP + 2;
constexpr UINT tray_message = WM_APP + 3;
constexpr UINT restore_message = WM_APP + 4;
constexpr UINT processes_ready = WM_APP + 5;
constexpr int app_icon_resource = 101;
constexpr int app_logo_resource = 102;
constexpr int settings_id = 101;
constexpr int exit_id = 104;
constexpr int minimize_id = 106;
constexpr int close_id = 107;
constexpr int card_base = 200;
constexpr int hotkey_id = 300;
constexpr int shortcut_id = 302;
constexpr int cancel_id = 303;
constexpr int startup_id = 304;
constexpr UINT32 background_color = 0x111113;
constexpr UINT32 surface_color = 0x1A1A1D;
constexpr UINT32 border_color = 0x2B2A30;
constexpr UINT32 text_color = 0xF2F0F5;
constexpr UINT32 muted_color = 0x99959F;
constexpr UINT32 accent_color = 0xAB9DF2;
constexpr UINT32 accent_surface = 0x302A42;
constexpr UINT32 accent_hover_color = 0x554574;
constexpr UINT32 accent_hover_border_color = 0x9E8DCA;

void require(HRESULT result) {
    if (FAILED(result)) {
        throw std::runtime_error("A Windows graphics operation failed.");
    }
}

D2D1_COLOR_F color(UINT32 value, float alpha = 1.0F) {
    return D2D1::ColorF(value, alpha);
}

D2D1_RECT_F play_bounds(float width, float height) {
    return D2D1::RectF(20, height - 54, width - 20, height - 14);
}

struct Pixels {
    UINT width = 0;
    UINT height = 0;
    std::vector<BYTE> data;
};

Pixels decode_image(const std::filesystem::path& path) {
    Pixels result;
    std::error_code error;

    if (path.empty() || std::filesystem::file_size(path, error) > 16 * 1024 * 1024 || error) {
        return result;
    }

    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;

    require(CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void**>(factory.GetAddressOf())));

    require(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder));

    require(decoder->GetFrame(0, &frame));
    UINT width = 0;
    UINT height = 0;
    require(frame->GetSize(&width, &height));

    if (!width || !height || width > 16000 || height > 16000 || static_cast<UINT64>(width) * height > 64000000) {
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

    require(converter->CopyPixels(nullptr, result.width * 4, static_cast<UINT>(result.data.size()), result.data.data()));

    return result;
}

struct ScanResult {
    bnet::Catalog catalog;
    std::vector<Pixels> images;
};

struct ProcessResult {
    std::uint64_t generation = 0;
    std::optional<std::vector<bool>> running;
};

struct Graphics {
    ComPtr<ID2D1Factory> factory;
    ComPtr<IDWriteFactory> text_factory;
    ComPtr<ID2D1HwndRenderTarget> window_target;
    ComPtr<ID2D1DCRenderTarget> control_target;
    ComPtr<ID2D1SolidColorBrush> window_brush;
    ComPtr<ID2D1SolidColorBrush> control_brush;
    ComPtr<ID2D1Bitmap> logo;
    std::array<ComPtr<IDWriteTextFormat>, 6> fonts;
    std::vector<ComPtr<ID2D1Bitmap>> images;
    UINT dpi = 96;

    void initialize() {
        require(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()));

        require(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(text_factory.GetAddressOf())));

        constexpr std::array<float, 6> sizes{12, 14, 18, 24, 32, 48};

        for (std::size_t index = 0; index < sizes.size(); ++index) {
            require(text_factory->CreateTextFormat(L"Segoe UI",
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

        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

        require(factory->CreateDCRenderTarget(&properties, &control_target));
        require(control_target->CreateSolidColorBrush(color(0xFFFFFF), &control_brush));
    }

    void ensure_window(HWND window) {
        if (window_target) {
            return;
        }

        RECT area{};
        GetClientRect(window, &area);

        require(factory->CreateHwndRenderTarget(D2D1::RenderTargetProperties(),
                                                D2D1::HwndRenderTargetProperties(window, D2D1::SizeU(area.right, area.bottom)),
                                                &window_target));

        window_target->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
        require(window_target->CreateSolidColorBrush(color(0xFFFFFF), &window_brush));
        const auto instance = GetModuleHandleW(nullptr);
        const auto resource = FindResourceW(instance, MAKEINTRESOURCEW(app_logo_resource), RT_RCDATA);
        require(resource ? S_OK : E_FAIL);
        const auto loaded = LoadResource(instance, resource);
        const auto bytes = static_cast<BYTE*>(LockResource(loaded));
        const auto size = SizeofResource(instance, resource);
        require(bytes && size ? S_OK : E_FAIL);
        ComPtr<IWICImagingFactory> image_factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;

        require(CoCreateInstance(CLSID_WICImagingFactory,
                                 nullptr,
                                 CLSCTX_INPROC_SERVER,
                                 IID_IWICImagingFactory,
                                 reinterpret_cast<void**>(image_factory.GetAddressOf())));

        require(image_factory->CreateStream(&stream));
        require(stream->InitializeFromMemory(bytes, size));
        require(image_factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder));
        require(decoder->GetFrame(0, &frame));
        require(image_factory->CreateFormatConverter(&converter));

        require(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));

        require(window_target->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &logo));
    }

    void text(ID2D1RenderTarget* target,
              ID2D1SolidColorBrush* brush,
              std::wstring_view value,
              D2D1_RECT_F area,
              std::size_t font,
              UINT32 tint,
              DWRITE_TEXT_ALIGNMENT alignment = DWRITE_TEXT_ALIGNMENT_LEADING,
              bool middle = false,
              bool wrap = false) {
        brush->SetColor(color(tint));
        fonts[font]->SetTextAlignment(alignment);
        fonts[font]->SetParagraphAlignment(middle ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        fonts[font]->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);

        target->DrawTextW(value.data(), static_cast<UINT32>(value.size()), fonts[font].Get(), area, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void round(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, D2D1_RECT_F area, float radius, UINT32 tint) {
        brush->SetColor(color(tint));
        target->FillRoundedRectangle(D2D1::RoundedRect(area, radius, radius), brush);
    }

    void upload(const std::vector<Pixels>& pixels) {
        images.clear();
        images.resize(pixels.size());

        for (std::size_t index = 0; index < pixels.size(); ++index) {
            const auto& source = pixels[index];

            if (!source.data.empty()) {
                const auto properties =
                    D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

                control_target->CreateBitmap(
                    D2D1::SizeU(source.width, source.height), source.data.data(), source.width * 4, properties, &images[index]);
            }
        }
    }
};

struct App;
LRESULT CALLBACK button_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

struct App {
    HWND window = nullptr;
    HWND status_control = nullptr;
    HWND hovered = nullptr;
    HWND hovered_play = nullptr;
    std::array<HWND, 3> cards{};
    std::array<HWND, 3> caption_buttons{};
    std::array<HWND, 4> setting_buttons{};
    HFONT control_font = nullptr;
    HICON window_icon = nullptr;
    HICON small_icon = nullptr;
    HBRUSH background = CreateSolidBrush(RGB(17, 17, 19));
    Graphics graphics;
    bnet::Settings settings;
    bnet::Catalog catalog;
    bnet::LaunchGuard launch_guard{std::chrono::seconds(30)};
    std::jthread scan_worker;
    std::jthread launch_worker;
    std::jthread process_worker;
    std::mutex results_mutex;
    std::optional<ScanResult> scan_result;
    std::optional<bnet::LaunchResult> launch_result;
    std::optional<ProcessResult> process_result;
    std::vector<bool> game_running;
    std::uint64_t process_generation = 0;
    std::uint64_t launch_ticket = 0;
    std::size_t selected = 0;
    std::size_t first = 0;
    std::size_t launching_index = 0;
    bool scanning = false;
    bool checking_processes = false;
    bool fresh_processes_required = true;
    bool closing = false;
    bool tray_added = false;
    bool hotkey_registered = false;
    int active_hotkey_id = 1;
    ULONGLONG hotkey_retry_until = 0;
    bool shortcut_suspended = false;
    bool shortcut_installed = false;
    bool shortcut_matches = false;
    bool startup_enabled = false;
    bool settings_open = false;
    bool recording = false;
    bool dry_run = false;
    bool empty_library = false;
    bool force_show = false;
    bool background_start = false;
    bool initial_scan = true;
    bool dragging = false;
    bool dragged = false;
    int drag_origin = 0;
    int wheel_delta = 0;
    std::atomic<bool> launch_running = false;
    std::wstring library_status = L"Looking for installed games...";
    std::wstring settings_status;
    std::wstring count_text;
    UINT taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");

    ~App() {
        scan_worker.request_stop();
        launch_worker.request_stop();
        process_worker.request_stop();
        if (scan_worker.joinable()) {
            scan_worker.join();
        }
        if (launch_worker.joinable()) {
            launch_worker.join();
        }

        if (process_worker.joinable()) {
            process_worker.join();
        }

        remove_tray();

        if (window_icon) {
            DestroyIcon(window_icon);
        }

        if (small_icon) {
            DestroyIcon(small_icon);
        }

        if (control_font) {
            DeleteObject(control_font);
        }
        DeleteObject(background);
    }

    int px(float value) const {
        return static_cast<int>(std::lround(value * static_cast<float>(graphics.dpi) / 96.0F));
    }

    D2D1_SIZE_F client_size() const {
        RECT area{};
        GetClientRect(window, &area);
        const auto scale = 96.0F / static_cast<float>(graphics.dpi);
        return D2D1::SizeF(static_cast<float>(area.right) * scale, static_cast<float>(area.bottom) * scale);
    }

    void refresh_status() {
        SetWindowTextW(status_control, (settings_open ? settings_status : library_status).c_str());
        InvalidateRect(status_control, nullptr, TRUE);
    }

    void set_status(std::wstring value, bool for_settings = false) {
        std::replace(value.begin(), value.end(), L';', L'.');
        std::replace(value.begin(), value.end(), L'\u2014', L'.');
        const bool had_library_status = !library_status.empty();
        (for_settings ? settings_status : library_status) = std::move(value);
        if (!for_settings && had_library_status != !library_status.empty()) {
            layout();
        } else {
            refresh_status();
        }
    }

    HWND button(int id, const wchar_t* label) {
        const auto result = CreateWindowExW(0,
                                            L"BUTTON",
                                            label,
                                            WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
                                            0,
                                            0,
                                            0,
                                            0,
                                            window,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                            nullptr,
                                            nullptr);
        require(result ? S_OK : E_FAIL);
        SetWindowSubclass(result, button_proc, 1, reinterpret_cast<DWORD_PTR>(this));
        return result;
    }

    void update_font() {
        const auto replacement = CreateFontW(-px(12),
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
        SendMessageW(status_control, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
        if (control_font) {
            DeleteObject(control_font);
        }
        control_font = replacement;
    }

    void update_icons() {
        const auto instance = GetModuleHandleW(nullptr);
        const auto load = [&](int width_metric, int height_metric) {
            return static_cast<HICON>(LoadImageW(instance,
                                                 MAKEINTRESOURCEW(app_icon_resource),
                                                 IMAGE_ICON,
                                                 GetSystemMetricsForDpi(width_metric, graphics.dpi),
                                                 GetSystemMetricsForDpi(height_metric, graphics.dpi),
                                                 0));
        };

        const auto loaded_large = load(SM_CXICON, SM_CYICON);
        const auto loaded_small = load(SM_CXSMICON, SM_CYSMICON);

        if (!loaded_large || !loaded_small) {
            if (loaded_large) {
                DestroyIcon(loaded_large);
            }

            if (loaded_small) {
                DestroyIcon(loaded_small);
            }

            return;
        }

        SendMessageW(window, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(loaded_large));
        SendMessageW(window, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(loaded_small));

        if (tray_added) {
            NOTIFYICONDATAW data{};
            data.cbSize = sizeof(data);
            data.hWnd = window;
            data.uID = 1;
            data.uFlags = NIF_ICON;
            data.hIcon = loaded_small;
            Shell_NotifyIconW(NIM_MODIFY, &data);
        }

        if (window_icon) {
            DestroyIcon(window_icon);
        }

        if (small_icon) {
            DestroyIcon(small_icon);
        }

        window_icon = loaded_large;
        small_icon = loaded_small;
    }

    void create_controls() {
        caption_buttons = {button(settings_id, L"Shortcut settings"), button(minimize_id, L"Minimize"), button(close_id, L"Close to tray")};
        for (std::size_t index = 0; index < cards.size(); ++index) {
            cards[index] = button(card_base + static_cast<int>(index), L"Play");
        }
        setting_buttons = {button(cancel_id, L"Back to library"),
                           button(hotkey_id, L"Change shortcut"),
                           button(shortcut_id, L"Launch when closed. Off"),
                           button(startup_id, L"Add to startup. Off")};
        status_control = CreateWindowExW(
            0, L"STATIC", library_status.c_str(), WS_CHILD | WS_VISIBLE | SS_CENTER, 0, 0, 0, 0, window, nullptr, nullptr, nullptr);
        update_font();
    }

    void center_window(bool resize) {
        POINT cursor{};
        GetCursorPos(&cursor);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
        const auto& area = monitor.rcWork;
        for (int pass = 0; pass < 2; ++pass) {
            const auto dpi = graphics.dpi;
            RECT current{};
            GetWindowRect(window, &current);
            const int width =
                std::min(resize ? px(static_cast<float>(bnet::launcher_width_dip(catalog.games.size()))) : current.right - current.left,
                         area.right - area.left - 24);
            const int height = std::min(resize ? px(bnet::launcher_height_dip) : current.bottom - current.top, area.bottom - area.top - 24);
            const int left = area.left + (area.right - area.left - width) / 2;
            const int top = area.top + (area.bottom - area.top - height) / 2;
            const bool same_size = width == current.right - current.left && height == current.bottom - current.top;
            if (same_size && left == current.left && top == current.top) {
                break;
            }
            SetWindowPos(window, nullptr, left, top, width, height, SWP_NOZORDER | SWP_NOACTIVATE | (same_size ? SWP_NOSIZE : 0));
            if (graphics.dpi == dpi) {
                break;
            }
        }
    }

    void layout() {
        if (hovered_play) {
            InvalidateRect(hovered_play, nullptr, FALSE);
            hovered_play = nullptr;
        }
        const auto [width, height] = client_size();
        const auto move = [this](HWND control, float x, float y, float w, float h, bool visible) {
            SetWindowPos(control, nullptr, px(x), px(y), px(w), px(h), SWP_NOZORDER | SWP_NOACTIVATE);
            ShowWindow(control, visible ? SW_SHOW : SW_HIDE);
        };
        for (std::size_t index = 0; index < caption_buttons.size(); ++index) {
            move(caption_buttons[index], width - 136.0F + static_cast<float>(index) * 38.0F, 16, 32, 32, true);
        }
        const auto count = bnet::visible_game_count(catalog.games.size());
        const float card_width =
            count ? std::min(320.0F, (width - 56.0F - static_cast<float>(count - 1) * 16.0F) / static_cast<float>(count)) : 320.0F;
        const float status_height = library_status.empty() ? 0.0F : 52.0F;
        const float available_height = height - 148.0F - status_height;
        const float card_height = std::min(460.0F, available_height);
        const float group_width = static_cast<float>(count) * (card_width + 16.0F) - 16.0F;
        const float top = 120.0F + (available_height - card_height) / 2.0F;
        for (std::size_t index = 0; index < cards.size(); ++index) {
            move(cards[index],
                 (width - group_width) / 2.0F + static_cast<float>(index) * (card_width + 16.0F),
                 top,
                 card_width,
                 card_height,
                 !settings_open && index < count);
        }
        const float panel_width = std::min(320.0F, width - 56.0F);
        const float left = (width - panel_width) / 2.0F;
        move(setting_buttons[0], (width - 120.0F) / 2.0F, 88, 120, 36, settings_open);
        move(setting_buttons[1], left, 188, panel_width, 52, settings_open);
        move(setting_buttons[2], left, 292, panel_width, 56, settings_open);
        move(setting_buttons[3], left, 360, panel_width, 56, settings_open);
        const float status_width = std::min(640.0F, width - 56.0F);
        move(status_control,
             (width - status_width) / 2.0F,
             settings_open ? 432.0F : height - 66.0F,
             status_width,
             settings_open ? 82.0F : 38.0F,
             settings_open || !library_status.empty());
        refresh_status();
        InvalidateRect(window, nullptr, TRUE);
    }

    bool is_running(std::size_t index) const {
        return index < game_running.size() && game_running[index];
    }

    const wchar_t* play_label(std::size_t index) const {
        if (is_running(index)) {
            return L"Running";
        }

        return index == launching_index && (launch_running || launch_guard.busy()) ? L"Opening..." : L"Play";
    }

    void focus_selection() {
        const auto card = selected >= first && selected - first < cards.size() ? cards[selected - first] : nullptr;
        SetFocus(card && IsWindowEnabled(card) ? card : window);
    }

    void refresh_running_games(bool fresh = false) {
        if (fresh) {
            ++process_generation;
            fresh_processes_required = true;
            update_cards(false);
        }

        if (closing || settings_open || !IsWindowVisible(window) || IsIconic(window) || catalog.games.empty()) {
            KillTimer(window, 3);
            return;
        }

        SetTimer(window, 3, 1000, nullptr);

        if (checking_processes) {
            return;
        }

        checking_processes = true;

        try {
            process_worker = std::jthread([this, games = catalog.games, generation = process_generation](std::stop_token stop) {
                ProcessResult result{generation, bnet::running_games(games, stop)};
                std::lock_guard lock(results_mutex);
                process_result = std::move(result);
                PostMessageW(window, processes_ready, 0, 0);
            });
        } catch (...) {
            checking_processes = false;
            fresh_processes_required = false;
            update_cards(false);
        }
    }

    void receive_running_games() {
        std::optional<ProcessResult> result;

        {
            std::lock_guard lock(results_mutex);
            result.swap(process_result);
        }

        checking_processes = false;

        if (!result || result->generation != process_generation) {
            refresh_running_games();
            return;
        }

        bool changed = fresh_processes_required;
        fresh_processes_required = false;

        if (result->running && result->running->size() == catalog.games.size()) {
            changed = changed || game_running != *result->running;
            game_running = std::move(*result->running);
        }

        if (is_running(launching_index) && launch_guard.busy()) {
            launch_guard.cancel();
            KillTimer(window, 1);
            changed = true;
        }

        if (changed) {
            update_cards(false);
        }
    }

    void update_cards(bool relayout = true) {
        first = bnet::carousel_start(catalog.games.size(), selected, first);
        for (std::size_t index = 0; index < cards.size(); ++index) {
            const auto game_index = first + index;
            const bool exists = game_index < catalog.games.size();
            EnableWindow(cards[index],
                         exists && !scanning && !launch_running && !launch_guard.busy() && !fresh_processes_required &&
                             !is_running(game_index));

            if (!IsWindowEnabled(cards[index]) && GetFocus() == cards[index]) {
                SetFocus(window);
            }

            if (exists) {
                const auto& game = catalog.games[game_index];
                const auto label = std::wstring(play_label(game_index)) + L" " + game.title + L". " + game.version;
                SetWindowTextW(cards[index], label.c_str());
            }
            InvalidateRect(cards[index], nullptr, TRUE);
        }
        count_text = std::to_wstring(catalog.games.size()) + L" installed";
        if (relayout) {
            layout();
        }
    }

    void open_settings(bool open = true) {
        settings_open = open;
        recording = false;
        SetWindowTextW(setting_buttons[1], bnet::hotkey_text(settings).c_str());
        if (open) {
            refresh_shortcut_state();
            refresh_startup_state();
            set_status(shortcut_matches     ? L"Shortcut can start better-net when it is closed."
                       : shortcut_installed ? L"Launch when closed uses another key. Turn it off and on to update it."
                       : hotkey_registered  ? L"Your shortcut works while better-net is running."
                                            : L"That shortcut is already in use. Try another key combination.",
                       true);
        }
        layout();
        refresh_running_games(true);

        if (open || catalog.games.empty()) {
            SetFocus(open ? setting_buttons[1] : caption_buttons[0]);
        } else {
            focus_selection();
        }
    }

    void record_key(WPARAM key) {
        if (key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT || key == VK_LWIN || key == VK_RWIN) {
            return;
        }
        auto candidate = settings;
        candidate.hotkey_key = static_cast<std::uint32_t>(key);
        candidate.hotkey_modifiers = (GetKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) | (GetKeyState(VK_MENU) < 0 ? MOD_ALT : 0) |
                                     (GetKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0) |
                                     ((GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) ? MOD_WIN : 0);
        if (!bnet::valid_hotkey(candidate)) {
            set_status(L"Use Ctrl or Alt with a letter, number or function key.", true);
            return;
        }
        recording = false;
        save_shortcut(candidate);
        SetWindowTextW(setting_buttons[1], bnet::hotkey_text(settings).c_str());
        InvalidateRect(window, nullptr, TRUE);
    }

    void select(int direction, bool focus = true) {
        const auto count = catalog.games.size();
        if (count == 0 || settings_open) {
            return;
        }
        selected = static_cast<std::size_t>((static_cast<int>(selected) + direction + static_cast<int>(count)) % static_cast<int>(count));
        update_cards();
        if (focus) {
            focus_selection();
        }
    }

    void scroll(int delta) {
        if (catalog.games.size() <= 3 || settings_open) {
            return;
        }
        wheel_delta += delta;
        while (std::abs(wheel_delta) >= WHEEL_DELTA) {
            select(wheel_delta > 0 ? -1 : 1);
            wheel_delta += wheel_delta > 0 ? -WHEEL_DELTA : WHEEL_DELTA;
        }
    }

    void paint() {
        PAINTSTRUCT state{};
        const auto dc = BeginPaint(window, &state);
        try {
            graphics.ensure_window(window);
            auto* target = graphics.window_target.Get();
            auto* brush = graphics.window_brush.Get();
            const auto [width, height] = client_size();
            target->BeginDraw();
            target->Clear(color(background_color));
            target->DrawBitmap(graphics.logo.Get(), D2D1::RectF(26, 20, 50, 44));
            graphics.text(
                target, brush, L"better-net", D2D1::RectF(58, 16, width - 142, 48), 1, text_color, DWRITE_TEXT_ALIGNMENT_LEADING, true);
            brush->SetColor(color(border_color));
            target->DrawLine(D2D1::Point2F(28, 64), D2D1::Point2F(width - 28, 64), brush);
            if (settings_open) {
                graphics.text(
                    target, brush, L"Shortcut", D2D1::RectF(28, 142, width - 28, 174), 3, text_color, DWRITE_TEXT_ALIGNMENT_CENTER);
                graphics.text(target,
                              brush,
                              recording ? L"Press your shortcut. Esc cancels." : L"Click to change. Saves automatically.",
                              D2D1::RectF(28, 252, width - 28, 280),
                              0,
                              muted_color,
                              DWRITE_TEXT_ALIGNMENT_CENTER);
            } else {
                graphics.text(target, brush, L"Library", D2D1::RectF(28, 84, width - 150, 110), 2, text_color);
                graphics.text(target,
                              brush,
                              count_text,
                              D2D1::RectF(width - 160, 87, width - 28, 110),
                              0,
                              muted_color,
                              DWRITE_TEXT_ALIGNMENT_TRAILING);
                if (catalog.games.empty()) {
                    graphics.text(target,
                                  brush,
                                  scanning ? L"Finding your games" : L"Your library is empty",
                                  D2D1::RectF(28, height / 2 - 28, width - 28, height / 2 + 8),
                                  2,
                                  text_color,
                                  DWRITE_TEXT_ALIGNMENT_CENTER);
                    graphics.text(target,
                                  brush,
                                  L"Installed games will appear here.",
                                  D2D1::RectF(28, height / 2 + 14, width - 28, height / 2 + 42),
                                  0,
                                  muted_color,
                                  DWRITE_TEXT_ALIGNMENT_CENTER);
                }
                if (catalog.games.size() > 3) {
                    const float track_left = (width - 96.0F) / 2.0F;
                    const float unit = 96.0F / static_cast<float>(catalog.games.size());
                    graphics.round(target, brush, D2D1::RectF(track_left, height - 15, track_left + 96, height - 12), 1.5F, border_color);
                    graphics.round(target,
                                   brush,
                                   D2D1::RectF(track_left + static_cast<float>(first) * unit,
                                               height - 15,
                                               track_left + static_cast<float>(first + 3) * unit,
                                               height - 12),
                                   1.5F,
                                   accent_color);
                }
            }
            if (target->EndDraw() == D2DERR_RECREATE_TARGET) {
                graphics.logo.Reset();
                graphics.window_brush.Reset();
                graphics.window_target.Reset();
                InvalidateRect(window, nullptr, FALSE);
            }
        } catch (...) {
            graphics.logo.Reset();
            graphics.window_brush.Reset();
            graphics.window_target.Reset();
            FillRect(dc, &state.rcPaint, background);
            SetTextColor(dc, RGB(242, 240, 245));
            SetBkMode(dc, TRANSPARENT);
            RECT area{};
            GetClientRect(window, &area);
            DrawTextW(dc, L"Unable to draw better-net. Please restart the app.", -1, &area, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
        EndPaint(window, &state);
    }

    void draw_control(const DRAWITEMSTRUCT& item) {
        auto* target = graphics.control_target.Get();
        auto* brush = graphics.control_brush.Get();
        require(target->BindDC(item.hDC, &item.rcItem));
        target->SetDpi(static_cast<float>(graphics.dpi), static_cast<float>(graphics.dpi));
        const float scale = 96.0F / static_cast<float>(graphics.dpi);
        const float width = static_cast<float>(item.rcItem.right - item.rcItem.left) * scale;
        const float height = static_cast<float>(item.rcItem.bottom - item.rcItem.top) * scale;
        const bool focused = (item.itemState & ODS_FOCUS) != 0;
        const bool down = (item.itemState & ODS_SELECTED) != 0;
        const bool hover = hovered == item.hwndItem;
        target->BeginDraw();
        target->Clear(color(background_color));
        if (item.CtlID >= card_base && item.CtlID < card_base + cards.size()) {
            const auto index = first + item.CtlID - card_base;
            if (index < catalog.games.size()) {
                const auto& game = catalog.games[index];
                const auto frame = D2D1::RoundedRect(D2D1::RectF(0.75F, 0.75F, width - 0.75F, height - 0.75F), 12, 12);
                brush->SetColor(color(surface_color));
                target->FillRoundedRectangle(frame, brush);
                brush->SetColor(color(index == selected ? 0x82759D : hover ? 0x4A4357 : border_color));
                target->DrawRoundedRectangle(frame, brush, 1.5F);
                const auto art = D2D1::RectF(12, 12, width - 12, height - 120);
                graphics.round(target, brush, art, 7, 0x141416);
                if (index < graphics.images.size() && graphics.images[index]) {
                    const auto size = graphics.images[index]->GetSize();
                    const float factor = std::max((art.right - art.left) / size.width, (art.bottom - art.top) / size.height);
                    const float art_width = size.width * factor;
                    const float art_height = size.height * factor;
                    const float left = (art.left + art.right - art_width) / 2.0F;
                    const float top = (art.top + art.bottom - art_height) / 2.0F;
                    const auto properties =
                        D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_CLAMP, D2D1_EXTEND_MODE_CLAMP, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    ComPtr<ID2D1BitmapBrush> artwork_brush;
                    require(target->CreateBitmapBrush(graphics.images[index].Get(), &properties, nullptr, artwork_brush.GetAddressOf()));
                    artwork_brush->SetTransform(D2D1::Matrix3x2F::Scale(factor, factor) * D2D1::Matrix3x2F::Translation(left, top));
                    target->FillRoundedRectangle(D2D1::RoundedRect(art, 7, 7), artwork_brush.Get());
                } else {
                    const auto center = D2D1::Point2F(width / 2, (art.top + art.bottom) / 2);
                    graphics.text(target,
                                  brush,
                                  game.title,
                                  D2D1::RectF(32, center.y - 45, width - 32, center.y + 45),
                                  3,
                                  text_color,
                                  DWRITE_TEXT_ALIGNMENT_CENTER,
                                  true,
                                  true);
                }
                graphics.text(target, brush, game.title, D2D1::RectF(20, art.bottom + 12, width - 20, art.bottom + 38), 2, text_color);
                graphics.text(target,
                              brush,
                              game.version.empty() ? L"Ready to play" : game.version,
                              D2D1::RectF(20, art.bottom + 40, width - 20, art.bottom + 59),
                              0,
                              muted_color);
                graphics.round(target,
                               brush,
                               play_bounds(width, height),
                               7,
                               !IsWindowEnabled(item.hwndItem)                                   ? 0x242227
                               : down                                                            ? 0x453958
                               : hovered_play == item.hwndItem && IsWindowEnabled(item.hwndItem) ? accent_hover_color
                               : (hover || focused || index == selected)                         ? accent_surface
                                                                                                 : 0x27252E);
                const bool play_hover = hovered_play == item.hwndItem && IsWindowEnabled(item.hwndItem);
                if (play_hover) {
                    const auto bounds = play_bounds(width, height);
                    brush->SetColor(color(accent_hover_border_color));
                    target->DrawRoundedRectangle(
                        D2D1::RoundedRect(
                            D2D1::RectF(bounds.left + 0.5F, bounds.top + 0.5F, bounds.right - 0.5F, bounds.bottom - 0.5F), 7, 7),
                        brush);
                }
                graphics.text(target,
                              brush,
                              play_label(index),
                              D2D1::RectF(36, height - 54, width - 36, height - 14),
                              1,
                              !IsWindowEnabled(item.hwndItem) ? muted_color
                              : play_hover                    ? text_color
                                                              : 0xD5CBF4,
                              DWRITE_TEXT_ALIGNMENT_CENTER,
                              true);
            }
        } else if (item.CtlID == settings_id || item.CtlID == minimize_id || item.CtlID == close_id) {
            if (hover || focused || down) {
                graphics.round(target, brush, D2D1::RectF(0, 0, width, height), 7, down ? 0x333039 : 0x242227);
            }
            brush->SetColor(color(hover || focused ? text_color : muted_color));
            const auto line = [target, brush](float x1, float y1, float x2, float y2) {
                target->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush, 1.4F);
            };
            if (item.CtlID == settings_id) {
                line(9, 12, 23, 12);
                line(9, 20, 23, 20);
                brush->SetColor(color(background_color));
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(13, 12), 2.5F, 2.5F), brush);
                target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(20, 20), 2.5F, 2.5F), brush);
                brush->SetColor(color(hover || focused ? text_color : muted_color));
                target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(13, 12), 2.5F, 2.5F), brush, 1.4F);
                target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(20, 20), 2.5F, 2.5F), brush, 1.4F);
            } else if (item.CtlID == minimize_id) {
                line(10, 16, 22, 16);
            } else {
                line(11, 11, 21, 21);
                line(21, 11, 11, 21);
            }
        } else if (item.CtlID == shortcut_id || item.CtlID == startup_id) {
            const bool enabled = item.CtlID == startup_id ? startup_enabled : shortcut_installed;
            const auto frame = D2D1::RoundedRect(D2D1::RectF(0.75F, 0.75F, width - 0.75F, height - 0.75F), 8, 8);
            brush->SetColor(color(down ? 0x453958 : hover ? 0x242227 : surface_color));
            target->FillRoundedRectangle(frame, brush);
            brush->SetColor(color(focused ? accent_color : hover ? accent_hover_border_color : border_color));
            target->DrawRoundedRectangle(frame, brush);
            graphics.text(target,
                          brush,
                          item.CtlID == startup_id ? L"Add to startup" : L"Launch when closed",
                          D2D1::RectF(16, 0, width - 108, height),
                          1,
                          text_color,
                          DWRITE_TEXT_ALIGNMENT_LEADING,
                          true);
            graphics.text(target,
                          brush,
                          enabled ? L"On" : L"Off",
                          D2D1::RectF(width - 100, 0, width - 72, height),
                          1,
                          enabled ? accent_color : muted_color,
                          DWRITE_TEXT_ALIGNMENT_CENTER,
                          true);
            const float middle = height / 2;
            const auto track = D2D1::RectF(width - 60, middle - 12, width - 16, middle + 12);
            graphics.round(
                target, brush, track, 12, enabled ? (hover ? accent_hover_border_color : accent_color) : (hover ? 0x48444F : 0x343139));
            brush->SetColor(color(enabled ? background_color : text_color));
            target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(enabled ? width - 28 : width - 48, middle), 8, 8), brush);
        } else {
            const auto frame = D2D1::RoundedRect(D2D1::RectF(0.75F, 0.75F, width - 0.75F, height - 0.75F), 8, 8);
            brush->SetColor(color(down ? 0x453958 : hover ? 0x242227 : surface_color));
            target->FillRoundedRectangle(frame, brush);
            brush->SetColor(color(focused ? accent_color : border_color));
            target->DrawRoundedRectangle(frame, brush);
            std::array<wchar_t, 256> label{};
            GetWindowTextW(item.hwndItem, label.data(), static_cast<int>(label.size()));
            graphics.text(target,
                          brush,
                          label.data(),
                          D2D1::RectF(16, 0, width - 16, height),
                          item.CtlID == hotkey_id ? 2 : 1,
                          text_color,
                          DWRITE_TEXT_ALIGNMENT_CENTER,
                          true);
        }
        require(target->EndDraw());
    }

    LRESULT hit_test(LPARAM position) {
        POINT point{GET_X_LPARAM(position), GET_Y_LPARAM(position)};
        ScreenToClient(window, &point);
        RECT area{};
        GetClientRect(window, &area);
        if (!IsZoomed(window)) {
            const int edge = px(6);
            const bool left = point.x < edge;
            const bool right = point.x >= area.right - edge;
            const bool top = point.y < edge;
            const bool bottom = point.y >= area.bottom - edge;
            if (top) {
                return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
            }
            if (bottom) {
                return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
            }
            if (left) {
                return HTLEFT;
            }
            if (right) {
                return HTRIGHT;
            }
        }
        if (point.y < px(64) && ChildWindowFromPointEx(window, point, CWP_SKIPINVISIBLE | CWP_SKIPDISABLED) == window) {
            return HTCAPTION;
        }
        return HTCLIENT;
    }

    void command(int id) {
        if (id >= card_base && id < card_base + 3) {
            launch(first + static_cast<std::size_t>(id - card_base));
            return;
        }
        switch (id) {
        case settings_id:
            open_settings(!settings_open);
            break;
        case minimize_id:
            ShowWindow(window, SW_MINIMIZE);
            break;
        case close_id:
            hide();
            break;
        case cancel_id:
            open_settings(false);
            break;
        case hotkey_id:
            recording = true;
            SetWindowTextW(setting_buttons[1], L"Press your keys");
            InvalidateRect(window, nullptr, TRUE);
            break;
        case shortcut_id:
            toggle_launch_when_closed();
            break;
        case startup_id:
            toggle_startup();
            break;
        case exit_id:
            shutdown();
            break;
        default:
            break;
        }
    }

    void refresh_startup_state() {
        startup_enabled = bnet::startup_enabled(launcher_path());
        SetWindowTextW(setting_buttons[3], startup_enabled ? L"Add to startup. On" : L"Add to startup. Off");
        InvalidateRect(setting_buttons[3], nullptr, FALSE);
    }

    void toggle_startup() {
        recording = false;
        SetWindowTextW(setting_buttons[1], bnet::hotkey_text(settings).c_str());
        refresh_startup_state();
        const auto updated = bnet::set_startup_enabled(launcher_path(), !startup_enabled);
        refresh_startup_state();
        set_status(!updated ? updated.error() : startup_enabled ? L"Starts quietly with Windows." : L"Startup is off.", true);
        InvalidateRect(window, nullptr, FALSE);
    }

    void refresh_shortcut_state() {
        if (!shortcut_suspended) {
            const auto executable = launcher_path();
            shortcut_matches = !executable.empty() && bnet::launcher_shortcut_matches(executable, settings);
            shortcut_installed = !executable.empty() && bnet::launcher_shortcut_enabled(executable);
        }
        SetWindowTextW(setting_buttons[2], shortcut_installed ? L"Launch when closed. On" : L"Launch when closed. Off");
        InvalidateRect(setting_buttons[2], nullptr, FALSE);
    }

    std::filesystem::path launcher_path() const {
        std::array<wchar_t, 32768> executable{};
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        return length > 0 && length < executable.size() ? std::filesystem::path(executable.data()) : std::filesystem::path{};
    }

    void retry_hotkey() {
        if (closing || !hotkey_retry_until) {
            return;
        }

        hotkey_registered = RegisterHotKey(window, active_hotkey_id, settings.hotkey_modifiers | MOD_NOREPEAT, settings.hotkey_key) != 0;
        if (!hotkey_registered && GetTickCount64() < hotkey_retry_until) {
            return;
        }

        KillTimer(window, 2);
        hotkey_retry_until = 0;
        if (shortcut_suspended) {
            const auto restored = bnet::install_launcher_shortcut(launcher_path(), settings);
            if (!restored) {
                shortcut_suspended = false;
                refresh_shortcut_state();
                set_status(L"Launch when closed could not be restored. " + restored.error(), true);
                return;
            }
            shortcut_suspended = false;
        }
        refresh_shortcut_state();
        set_status(hotkey_registered ? L"Your shortcut is ready." : L"That shortcut is still in use. Choose another key in settings.",
                   true);
        InvalidateRect(window, nullptr, FALSE);
    }

    void begin_hotkey_retry() {
        hotkey_retry_until = GetTickCount64() + 2000;
        if (!SetTimer(window, 2, 50, nullptr)) {
            hotkey_retry_until = GetTickCount64();
        }
        retry_hotkey();
    }

    void register_hotkey() {
        if (hotkey_registered || hotkey_retry_until || closing) {
            return;
        }
        SendMessageW(window, WM_SETHOTKEY, 0, 0);
        refresh_shortcut_state();
        hotkey_registered = RegisterHotKey(window, active_hotkey_id, settings.hotkey_modifiers | MOD_NOREPEAT, settings.hotkey_key) != 0;
        if (hotkey_registered) {
            return;
        }
        if (shortcut_matches) {
            const auto disabled = bnet::disable_launcher_shortcut(launcher_path());
            if (!disabled) {
                set_status(disabled.error(), true);
                return;
            }
            shortcut_suspended = true;
            set_status(L"Setting up your shortcut...", true);
            begin_hotkey_retry();
            return;
        }
        set_status(L"That shortcut is already in use. Choose another key in settings.", true);
    }

    void add_tray() {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = 1;
        data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        data.uCallbackMessage = tray_message;
        data.hIcon = small_icon ? small_icon : LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(app_icon_resource));
        wcscpy_s(data.szTip, L"better-net");
        tray_added = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    }

    void remove_tray() {
        if (!tray_added) {
            return;
        }

        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window;
        data.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &data);
        tray_added = false;
    }

    void restore() {
        if (recording) {
            recording = false;
            SetWindowTextW(setting_buttons[1], bnet::hotkey_text(settings).c_str());
            set_status(L"Shortcut unchanged.", true);
        }
        if (IsIconic(window) || IsZoomed(window)) {
            ShowWindow(window, SW_RESTORE);
        }
        center_window(false);
        selected = 0;
        first = 0;
        wheel_delta = 0;
        update_cards(false);
        ShowWindow(window, SW_SHOW);
        ShowWindow(window, SW_SHOW);
        refresh_running_games(true);
        SetForegroundWindow(window);

        if (settings_open) {
            SetFocus(setting_buttons[1]);
        } else if (catalog.games.empty()) {
            SetFocus(caption_buttons[0]);
        } else {
            focus_selection();
        }
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    }

    void hide() {
        recording = false;
        SetWindowTextW(setting_buttons[1], bnet::hotkey_text(settings).c_str());
        ShowWindow(window, tray_added ? SW_HIDE : SW_MINIMIZE);
        refresh_running_games(true);
    }

    void scan() {
        if (scanning || closing || launch_guard.busy() || launch_running) {
            return;
        }

        scanning = true;
        set_status(L"Finding your games...");
        update_cards();

        try {
            scan_worker = std::jthread([this](std::stop_token stop) {
                const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                ScanResult result;

                try {
                    if (!empty_library) {
                        result.catalog = bnet::discover(stop);
                    }

                    result.images.resize(std::min<std::size_t>(result.catalog.games.size(), 12));

                    for (std::size_t index = 0; index < result.images.size() && !stop.stop_requested(); ++index) {
                        try {
                            result.images[index] = decode_image(result.catalog.games[index].artwork_path);
                        } catch (...) {
                        }
                    }
                } catch (...) {
                    result.catalog.scan_failed = true;
                    result.catalog.warnings.push_back(L"Could not find your games. Press F5 to try again.");
                }

                if (SUCCEEDED(initialized)) {
                    CoUninitialize();
                }

                if (!stop.stop_requested()) {
                    std::lock_guard lock(results_mutex);
                    scan_result = std::move(result);
                    PostMessageW(window, catalog_ready, 0, 0);
                }
            });
        } catch (...) {
            scanning = false;
            set_status(L"Could not find your games. Press F5 to try again.");
            update_cards();
        }
    }

    void receive_catalog() {
        std::optional<ScanResult> result;

        {
            std::lock_guard lock(results_mutex);
            result.swap(scan_result);
        }

        if (!result) {
            return;
        }

        const auto previous_count = catalog.games.size();
        catalog = std::move(result->catalog);
        game_running.assign(catalog.games.size(), false);
        graphics.upload(result->images);
        scanning = false;
        selected = std::min(selected, catalog.games.empty() ? 0 : catalog.games.size() - 1);
        first = bnet::carousel_start(catalog.games.size(), selected, first);

        if (catalog.games.empty()) {
            set_status(catalog.scan_failed ? L"Could not find your games. Press F5 to try again."
                                           : L"Install a game in Battle.net, then press F5.");
        } else if (catalog.scan_failed) {
            set_status(L"Some games could not be found. Press F5 to try again.");
        } else {
            set_status(L"");
        }

        if (initial_scan || previous_count != catalog.games.size()) {
            center_window(true);
        }

        const bool focus_first = (initial_scan || previous_count == 0) && !settings_open && IsWindowVisible(window) && !IsIconic(window) &&
                                 GetForegroundWindow() == window;
        initial_scan = false;
        refresh_running_games(true);
        update_cards();
        if (focus_first && !catalog.games.empty()) {
            focus_selection();
        }
    }

    void launch(std::size_t index) {
        if (index >= catalog.games.size() || scanning || closing || launch_running || fresh_processes_required || is_running(index)) {
            return;
        }

        const auto ticket = launch_guard.try_begin();

        if (!ticket) {
            return;
        }

        selected = index;
        launch_ticket = *ticket;
        launching_index = index;
        first = bnet::carousel_start(catalog.games.size(), selected, first);
        bnet::LaunchRequest request{catalog.games[index], catalog.client_path};
        launch_running = true;
        set_status(L"");
        update_cards();
        SetTimer(window, 1, 30200, nullptr);
        try {
            launch_worker = std::jthread([this, request = std::move(request)](std::stop_token stop) {
                bnet::LaunchResult result;

                try {
                    bnet::ProcessAdapter adapter;

                    if (dry_run) {
                        adapter = [](const bnet::PreparedLaunch&) {
                            return bnet::LaunchResult{bnet::LaunchStatus::requested, L"Dry run passed. No game was started.", 0};
                        };
                    }

                    result = bnet::launch_game(request, stop, adapter);
                } catch (...) {
                    result.status = bnet::LaunchStatus::process_error;
                }

                if (!stop.stop_requested()) {
                    std::lock_guard lock(results_mutex);
                    launch_result = std::move(result);
                    PostMessageW(window, launch_ready, 0, 0);
                }
            });
        } catch (...) {
            launch_running = false;
            launch_guard.cancel();
            set_status(L"Launch failed. Please try again.");
            update_cards();
        }
    }

    void receive_launch() {
        std::optional<bnet::LaunchResult> result;

        {
            std::lock_guard lock(results_mutex);
            result.swap(launch_result);
        }

        if (result) {
            launch_running = false;
            launch_guard.finish(launch_ticket, result->sent() && !dry_run);

            if (result->status == bnet::LaunchStatus::already_running && launching_index < game_running.size()) {
                game_running[launching_index] = true;
            }

            if (is_running(launching_index)) {
                launch_guard.cancel();
                KillTimer(window, 1);
            }

            if (result->sent() || result->status == bnet::LaunchStatus::already_running) {
                set_status(L"");
            } else {
                set_status(result->message.empty() ? L"Launch failed. Press F5 to rescan and try again." : result->message);
            }

            update_cards();
            refresh_running_games(true);
        }
    }

    void shutdown() {
        if (closing) {
            return;
        }
        closing = true;
        KillTimer(window, 2);
        KillTimer(window, 3);
        hotkey_retry_until = 0;
        refresh_shortcut_state();
        const auto executable = launcher_path();
        if (shortcut_installed) {
            static_cast<void>(bnet::disable_launcher_shortcut(executable));
            shortcut_suspended = true;
        }
        SendMessageW(window, WM_SETHOTKEY, 0, 0);
        UnregisterHotKey(window, active_hotkey_id);
        hotkey_registered = false;
        if (shortcut_suspended) {
            static_cast<void>(bnet::install_launcher_shortcut(executable, settings));
            shortcut_suspended = false;
        }
        scan_worker.request_stop();
        launch_worker.request_stop();
        process_worker.request_stop();
        launch_guard.cancel();
        KillTimer(window, 1);
        remove_tray();

        if (scan_worker.joinable()) {
            scan_worker.join();
        }

        if (launch_worker.joinable()) {
            launch_worker.join();
        }

        if (process_worker.joinable()) {
            process_worker.join();
        }

        DestroyWindow(window);
    }

    void save_shortcut(const bnet::Settings& replacement) {
        if (hotkey_retry_until) {
            set_status(L"Finishing shortcut setup. Try again in a moment.", true);
            return;
        }
        if (!bnet::valid_hotkey(replacement)) {
            set_status(L"Use Ctrl or Alt with a letter, number, or function key except F12.", true);
            return;
        }

        const bool changed = replacement.hotkey_key != settings.hotkey_key || replacement.hotkey_modifiers != settings.hotkey_modifiers;
        refresh_shortcut_state();
        if (!changed && (!shortcut_installed || shortcut_matches) && hotkey_registered && !shortcut_suspended) {
            set_status(L"This shortcut is already saved.", true);
            return;
        }

        const auto executable = launcher_path();
        if (shortcut_installed && executable.empty()) {
            set_status(L"Could not find the better-net application.", true);
            return;
        }

        if (!changed && !hotkey_registered) {
            register_hotkey();
            if (!hotkey_registered) {
                return;
            }
        }
        const int replacement_id = active_hotkey_id == 1 ? 2 : 1;
        if (changed && !RegisterHotKey(window, replacement_id, replacement.hotkey_modifiers | MOD_NOREPEAT, replacement.hotkey_key)) {
            set_status(L"That shortcut is already in use. Try another key combination.", true);
            return;
        }

        if (shortcut_installed) {
            const auto created = bnet::install_launcher_shortcut(executable, replacement);
            if (!created) {
                if (changed) {
                    UnregisterHotKey(window, replacement_id);
                }
                const auto restored = bnet::install_launcher_shortcut(executable, settings);
                if (restored) {
                    shortcut_suspended = false;
                }
                refresh_shortcut_state();
                set_status(created.error(), true);
                return;
            }
            shortcut_suspended = false;
        }

        const auto saved = bnet::save_settings(replacement);
        if (!saved) {
            std::wstring error = saved.error();
            if (shortcut_installed) {
                const auto restored = bnet::install_launcher_shortcut(executable, settings);
                if (!restored) {
                    error += L" Launch when closed could not be restored. " + restored.error();
                } else {
                    shortcut_suspended = false;
                }
            }
            if (changed) {
                UnregisterHotKey(window, replacement_id);
            }
            refresh_shortcut_state();
            set_status(error, true);
            return;
        }
        if (changed) {
            UnregisterHotKey(window, active_hotkey_id);
            active_hotkey_id = replacement_id;
            hotkey_registered = true;
        }
        settings = replacement;
        shortcut_suspended = false;
        SendMessageW(window, WM_SETHOTKEY, 0, 0);
        refresh_shortcut_state();
        set_status(shortcut_installed ? L"Saved. Shortcut can start better-net when it is closed."
                                      : L"Saved. Your shortcut works while better-net is running.",
                   true);
    }

    void toggle_launch_when_closed() {
        if (hotkey_retry_until) {
            set_status(L"Finishing shortcut setup. Try again in a moment.", true);
            return;
        }
        recording = false;
        SetWindowTextW(setting_buttons[1], bnet::hotkey_text(settings).c_str());
        InvalidateRect(window, nullptr, FALSE);
        refresh_shortcut_state();
        const auto executable = launcher_path();
        if (executable.empty()) {
            set_status(L"Could not find the better-net application.", true);
            return;
        }

        if (!shortcut_installed && !hotkey_registered) {
            register_hotkey();
            if (!hotkey_registered) {
                return;
            }
        }

        const auto updated = [&]() -> std::expected<void, std::wstring> {
            if (shortcut_installed) {
                return bnet::disable_launcher_shortcut(executable);
            }
            const auto installed = bnet::install_launcher_shortcut(executable, settings);
            if (!installed) {
                return std::unexpected(installed.error());
            }
            return {};
        }();
        if (updated) {
            shortcut_suspended = false;
            SendMessageW(window, WM_SETHOTKEY, 0, 0);
        }
        refresh_shortcut_state();
        if (updated && !hotkey_registered) {
            begin_hotkey_retry();
        }
        set_status(!updated             ? updated.error()
                   : shortcut_installed ? L"Shortcut can start better-net when it is closed."
                   : hotkey_registered  ? L"Your shortcut works while better-net is running."
                                        : L"Launch when closed is off. This shortcut is currently in use.",
                   true);
    }
};

LRESULT CALLBACK button_proc(HWND control, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto& app = *reinterpret_cast<App*>(data);
    try {
        const int id = GetDlgCtrlID(control);
        const bool card = id >= card_base && id < card_base + 3;
        if (message == WM_MOUSEMOVE) {
            if (card) {
                RECT area{};
                GetClientRect(control, &area);
                const float scale = 96.0F / static_cast<float>(app.graphics.dpi);
                const auto bounds = play_bounds(static_cast<float>(area.right) * scale, static_cast<float>(area.bottom) * scale);
                const float x = static_cast<float>(GET_X_LPARAM(lparam)) * scale;
                const float y = static_cast<float>(GET_Y_LPARAM(lparam)) * scale;
                const auto play = IsWindowEnabled(control) && x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom
                                      ? control
                                      : nullptr;
                if (app.hovered_play != play) {
                    const auto previous = app.hovered_play;
                    app.hovered_play = play;
                    if (previous) {
                        InvalidateRect(previous, nullptr, FALSE);
                    }
                    InvalidateRect(control, nullptr, FALSE);
                }
            }
            if (app.hovered != control) {
                const auto previous = app.hovered;
                app.hovered = control;
                if (previous) {
                    InvalidateRect(previous, nullptr, FALSE);
                }
                InvalidateRect(control, nullptr, FALSE);
                TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0};
                TrackMouseEvent(&tracking);
            }
            if (card && app.dragging && (wparam & MK_LBUTTON)) {
                const int current = GET_X_LPARAM(lparam);
                if (std::abs(current - app.drag_origin) > app.px(48)) {
                    app.select(current < app.drag_origin ? 1 : -1, false);
                    app.drag_origin = current;
                    app.dragged = true;
                }
            }
        } else if (message == WM_MOUSELEAVE) {
            if (app.hovered_play == control) {
                app.hovered_play = nullptr;
            }
            if (app.hovered == control) {
                app.hovered = nullptr;
            }
            InvalidateRect(control, nullptr, FALSE);
        } else if (message == WM_ENABLE && !wparam && app.hovered_play == control) {
            app.hovered_play = nullptr;
            InvalidateRect(control, nullptr, FALSE);
        } else if (message == WM_SETFOCUS && card) {
            const auto index = app.first + static_cast<std::size_t>(id - card_base);
            if (index < app.catalog.games.size()) {
                app.selected = index;
                for (const auto item : app.cards) {
                    InvalidateRect(item, nullptr, FALSE);
                }
            }
        } else if (message == WM_MOUSEWHEEL) {
            app.scroll(GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        } else if (message == WM_LBUTTONDOWN && card && app.catalog.games.size() > 3) {
            app.dragging = true;
            app.dragged = false;
            app.drag_origin = GET_X_LPARAM(lparam);
        } else if (message == WM_LBUTTONUP && card) {
            const bool suppress = app.dragged;
            app.dragging = false;
            app.dragged = false;
            if (suppress) {
                ReleaseCapture();
                SendMessageW(control, BM_SETSTATE, FALSE, 0);
                app.focus_selection();
                return 0;
            }
        } else if (message == WM_CAPTURECHANGED) {
            app.dragging = false;
        }
    } catch (...) {
        app.set_status(L"An operation failed. Press F5 to try again.");
    }
    return DefSubclassProc(control, message, wparam, lparam);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) {
        return DefWindowProcW(window, message, wparam, lparam);
    }
    try {
        if (message == app->taskbar_created) {
            app->add_tray();
            return 0;
        }
        switch (message) {
        case WM_NCCALCSIZE:
            if (wparam && IsZoomed(window)) {
                MONITORINFO monitor{};
                monitor.cbSize = sizeof(monitor);
                GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
                reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0] = monitor.rcWork;
            }
            return 0;
        case WM_NCHITTEST:
            return app->hit_test(lparam);
        case WM_SYSCOMMAND:
            if ((wparam & 0xFFF0) == SC_HOTKEY) {
                app->restore();
                return 0;
            }
            break;
        case WM_PAINT:
            app->paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            if (wparam == SIZE_MINIMIZED) {
                app->refresh_running_games(true);
                return 0;
            }
            if (app->graphics.window_target) {
                app->graphics.window_target->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
            }
            if (app->status_control) {
                app->layout();
                app->refresh_running_games(true);
            }
            return 0;
        case WM_DPICHANGED:
            app->graphics.dpi = HIWORD(wparam);
            app->update_icons();
            if (app->graphics.window_target) {
                app->graphics.window_target->SetDpi(static_cast<float>(app->graphics.dpi), static_cast<float>(app->graphics.dpi));
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
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = POINT{
                app->px(static_cast<float>(std::max(std::size_t{376}, 56 + bnet::visible_game_count(app->catalog.games.size()) * 216))),
                app->px(540)};
            return 0;
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wparam), RGB(153, 149, 159));
            SetBkColor(reinterpret_cast<HDC>(wparam), RGB(17, 17, 19));
            return reinterpret_cast<LRESULT>(app->background);
        case WM_DRAWITEM:
            app->draw_control(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
            return TRUE;
        case WM_COMMAND:
            app->command(LOWORD(wparam));
            return 0;
        case WM_MOUSEWHEEL:
            app->scroll(GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        case WM_TIMER:
            if (wparam == 2) {
                app->retry_hotkey();
            } else if (wparam == 1) {
                KillTimer(window, 1);
                app->update_cards(false);
            } else if (wparam == 3) {
                app->refresh_running_games();
            }
            return 0;
        case catalog_ready:
            app->receive_catalog();
            return 0;
        case launch_ready:
            app->receive_launch();
            return 0;
        case processes_ready:
            app->receive_running_games();
            return 0;
        case restore_message:
        case WM_HOTKEY:
            app->restore();
            return 0;
        case tray_message:
            if (lparam == WM_LBUTTONUP || lparam == WM_LBUTTONDBLCLK) {
                app->restore();
            } else if (lparam == WM_RBUTTONUP) {
                const auto menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"Open better-net");
                AppendMenuW(menu, MF_STRING, 2, L"Exit");
                POINT position{};
                GetCursorPos(&position);
                SetForegroundWindow(window);
                const auto command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, position.x, position.y, 0, window, nullptr);
                DestroyMenu(menu);
                if (command == 1) {
                    app->restore();
                } else if (command == 2) {
                    app->shutdown();
                }
                PostMessageW(window, WM_NULL, 0, 0);
            }
            return 0;
        case WM_CLOSE:
            app->hide();
            return 0;
        case WM_ENDSESSION:
            if (wparam) {
                app->shutdown();
            }
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
    } catch (...) {
        app->set_status(L"An operation failed. Press F5 or restart better-net.");
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int run(HINSTANCE instance) {
    int argument_count = 0;
    auto* arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    App app;
    for (int index = 1; index < argument_count; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (argument == L"--dry-run") {
            app.dry_run = true;
        } else if (argument == L"--empty-library") {
            app.empty_library = true;
        } else if (argument == L"--show") {
            app.force_show = true;
        } else if (argument == L"--background") {
            app.background_start = true;
        }
    }
    LocalFree(arguments);
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\BetterNet.SingleInstance.1");
    if (!singleton) {
        return 1;
    }
    const bool existing_instance = GetLastError() == ERROR_ALREADY_EXISTS;
    const auto mutex = std::unique_ptr<void, decltype(&CloseHandle)>(singleton, &CloseHandle);
    if (existing_instance) {
        if (app.background_start && !app.force_show) {
            return 0;
        }
        if (const auto existing = FindWindowW(window_class, nullptr)) {
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
    definition.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(app_icon_resource));
    definition.hIconSm = definition.hIcon;
    definition.lpszClassName = window_class;
    require(RegisterClassExW(&definition) ? S_OK : E_FAIL);
    app.graphics.dpi = GetDpiForSystem();
    app.window = CreateWindowExW(WS_EX_CONTROLPARENT,
                                 window_class,
                                 L"better-net",
                                 WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                 0,
                                 0,
                                 app.px(376),
                                 app.px(bnet::launcher_height_dip),
                                 nullptr,
                                 nullptr,
                                 instance,
                                 &app);
    if (!app.window) {
        throw std::runtime_error("The launcher window could not be created.");
    }
    app.graphics.dpi = GetDpiForWindow(app.window);
    app.update_icons();
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(app.window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const BOOL disable_transitions = TRUE;
    DwmSetWindowAttribute(app.window, DWMWA_TRANSITIONS_FORCEDISABLED, &disable_transitions, sizeof(disable_transitions));
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    DwmSetWindowAttribute(app.window, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    const COLORREF border = RGB(43, 42, 48);
    DwmSetWindowAttribute(app.window, DWMWA_BORDER_COLOR, &border, sizeof(border));
    const MARGINS margins{1, 1, 1, 1};
    DwmExtendFrameIntoClientArea(app.window, &margins);
    SetWindowPos(app.window, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    app.create_controls();
    const auto loaded = bnet::load_settings();
    app.settings = loaded.value;
    app.register_hotkey();
    app.add_tray();
    app.update_cards();
    app.center_window(true);
    app.scan();
    const bool start_hidden = (app.background_start || app.settings.start_hidden) && app.tray_added && !app.force_show;
    if (!start_hidden) {
        ShowWindow(app.window, SW_SHOW);
        ShowWindow(app.window, SW_SHOW);
        UpdateWindow(app.window);
    }
    if (!loaded.warning.empty()) {
        app.set_status(loaded.warning, true);
    }
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN || message.message == WM_SYSKEYDOWN) {
            const auto key = message.wParam;
            if (key == VK_ESCAPE) {
                if (app.recording) {
                    app.recording = false;
                    SetWindowTextW(app.setting_buttons[1], bnet::hotkey_text(app.settings).c_str());
                    InvalidateRect(app.window, nullptr, TRUE);
                } else if (app.settings_open) {
                    app.open_settings(false);
                } else {
                    app.hide();
                }
                continue;
            }
            if (app.recording) {
                app.record_key(key);
                continue;
            }
            const bool plain_key = message.message == WM_KEYDOWN && GetKeyState(VK_SHIFT) >= 0 && GetKeyState(VK_CONTROL) >= 0 &&
                                   GetKeyState(VK_MENU) >= 0 && GetKeyState(VK_LWIN) >= 0 && GetKeyState(VK_RWIN) >= 0;
            if (!app.settings_open && plain_key && (key == VK_LEFT || key == VK_RIGHT || key == 'A' || key == 'D')) {
                app.select(key == VK_LEFT || key == 'A' ? -1 : 1);
                continue;
            }
            if (key == VK_RETURN) {
                if ((message.lParam & (1LL << 30)) == 0) {
                    const auto focus = GetFocus();
                    if (focus && focus != app.window) {
                        SendMessageW(focus, BM_CLICK, 0, 0);
                    } else if (!app.settings_open) {
                        app.launch(app.selected);
                    }
                }
                continue;
            }
            if (!app.settings_open && key == VK_F5) {
                app.scan();
                continue;
            }
            if (key == VK_TAB && IsDialogMessageW(app.window, &message)) {
                continue;
            }
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);
    int result = 1;
    try {
        result = run(instance);
    } catch (...) {
        MessageBoxW(nullptr,
                    L"better-net could not start. Windows graphics or local settings may be unavailable.",
                    L"better-net",
                    MB_OK | MB_ICONERROR);
    }
    if (SUCCEEDED(initialized)) {
        CoUninitialize();
    }
    return result;
}
