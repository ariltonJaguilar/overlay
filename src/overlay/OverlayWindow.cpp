#include "OverlayWindow.h"

#include <dwmapi.h>
#include <windowsx.h>
#include <wincodec.h>
#include <mmdeviceapi.h>
#include <powrprof.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <cwctype>

namespace {
constexpr wchar_t kWindowClass[] = L"GameOverlayMvp.Window";
constexpr int kOverlayWidth = 620;
constexpr int kBarHeight = 280;
constexpr BYTE kGradientBottomOpacity = 245;
constexpr BYTE kPanelRed = 24;
constexpr BYTE kPanelGreen = 24;
constexpr BYTE kPanelBlue = 28;
constexpr int kVolumePanelWidth = 620;
constexpr int kVolumePanelRadius = 28;
constexpr int kSliderHalfWidth = 210;
constexpr int kAchievementPanelWidth = 920;
constexpr int kAchievementCardHalfWidth = 450;
constexpr int kAchievementListTop = 92;
constexpr int kAchievementRowHeight = 112;
constexpr int kAchievementCardHeight = 104;

struct WindowSearch {
    DWORD pid;
    HWND result;
};

BOOL CALLBACK findGameWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<WindowSearch*>(parameter);
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == search->pid && IsWindowVisible(window) && GetWindow(window, GW_OWNER) == nullptr) {
        search->result = window;
        return FALSE;
    }
    return TRUE;
}

bool isSteamWindowProcess(HWND window) {
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;

    wchar_t path[32768]{};
    DWORD length = static_cast<DWORD>(std::size(path));
    const BOOL queried = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    if (!queried) return false;

    std::wstring executable = std::filesystem::path(std::wstring(path, length)).filename().wstring();
    std::transform(executable.begin(), executable.end(), executable.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return executable == L"steam.exe" || executable == L"steamwebhelper.exe";
}

struct SteamWindowSearch {
    HWND bigPicture = nullptr;
    HWND fallback = nullptr;
};

BOOL CALLBACK findSteamWindow(HWND window, LPARAM parameter) {
    auto* search = reinterpret_cast<SteamWindowSearch*>(parameter);
    if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr ||
        !isSteamWindowProcess(window)) return TRUE;

    wchar_t title[512]{};
    GetWindowTextW(window, title, static_cast<int>(std::size(title)));
    std::wstring normalizedTitle(title);
    std::transform(normalizedTitle.begin(), normalizedTitle.end(), normalizedTitle.begin(),
                   [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    if (normalizedTitle.find(L"big picture") != std::wstring::npos) {
        search->bigPicture = window;
        return FALSE;
    }
    if (!search->fallback && !normalizedTitle.empty()) search->fallback = window;
    return TRUE;
}

bool focusSteamBigPicture() {
    SteamWindowSearch search{};
    EnumWindows(findSteamWindow, reinterpret_cast<LPARAM>(&search));
    const HWND steamWindow = search.bigPicture ? search.bigPicture : search.fallback;
    if (!steamWindow) return false;

    if (IsIconic(steamWindow)) ShowWindow(steamWindow, SW_RESTORE);
    const HWND foreground = GetForegroundWindow();
    const DWORD currentThread = GetCurrentThreadId();
    const DWORD foregroundThread = foreground ? GetWindowThreadProcessId(foreground, nullptr) : 0;
    if (foregroundThread && foregroundThread != currentThread)
        AttachThreadInput(currentThread, foregroundThread, TRUE);
    BringWindowToTop(steamWindow);
    const bool focused = SetForegroundWindow(steamWindow) != FALSE;
    if (foregroundThread && foregroundThread != currentThread)
        AttachThreadInput(currentThread, foregroundThread, FALSE);
    return focused;
}

void drawAchievementIcon(HDC dc, int x, int y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 3, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, x - 15, y - 17, x + 15, y + 5);
    Arc(dc, x - 28, y - 15, x - 7, y + 10, x - 8, y + 5, x - 12, y - 13);
    Arc(dc, x + 7, y - 15, x + 28, y + 10, x + 12, y - 13, x + 8, y + 5);
    MoveToEx(dc, x, y + 5, nullptr); LineTo(dc, x, y + 18);
    MoveToEx(dc, x - 13, y + 18, nullptr); LineTo(dc, x + 13, y + 18);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
}

void drawVolumeIcon(HDC dc, int x, int y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 3, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    POINT speaker[]{{x - 23, y - 8}, {x - 12, y - 8}, {x + 1, y - 20},
                    {x + 1, y + 20}, {x - 12, y + 8}, {x - 23, y + 8}};
    Polygon(dc, speaker, 6);
    Arc(dc, x - 8, y - 16, x + 25, y + 16, x + 7, y - 12, x + 7, y + 12);
    Arc(dc, x - 5, y - 25, x + 40, y + 25, x + 17, y - 20, x + 17, y + 20);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
}

void drawSettingsIcon(HDC dc, int x, int y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 3, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x - 18, y - 18, x + 18, y + 18);
    Ellipse(dc, x - 6, y - 6, x + 6, y + 6);
    for (int i = 0; i < 8; ++i) {
        const double angle = i * 3.141592653589793 / 4.0;
        MoveToEx(dc, x + static_cast<int>(18 * std::cos(angle)),
                 y + static_cast<int>(18 * std::sin(angle)), nullptr);
        LineTo(dc, x + static_cast<int>(26 * std::cos(angle)),
               y + static_cast<int>(26 * std::sin(angle)));
    }
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
}

void boxBlur(std::vector<std::uint32_t>& pixels, int width, int height, int radius) {
    std::vector<std::uint32_t> temporary(pixels.size());
    auto blurPass = [&](const std::vector<std::uint32_t>& source,
                        std::vector<std::uint32_t>& destination, bool horizontal) {
        const int major = horizontal ? height : width;
        const int minor = horizontal ? width : height;
        for (int line = 0; line < major; ++line) {
            int blue = 0, green = 0, red = 0;
            auto pixelAt = [&](int position) {
                position = (std::max)(0, (std::min)(minor - 1, position));
                const int index = horizontal ? line * width + position : position * width + line;
                return source[static_cast<size_t>(index)];
            };
            for (int offset = -radius; offset <= radius; ++offset) {
                const auto color = pixelAt(offset);
                blue += color & 0xff; green += (color >> 8) & 0xff; red += (color >> 16) & 0xff;
            }
            const int divisor = radius * 2 + 1;
            for (int position = 0; position < minor; ++position) {
                const int index = horizontal ? line * width + position : position * width + line;
                destination[static_cast<size_t>(index)] = static_cast<std::uint32_t>(blue / divisor) |
                    (static_cast<std::uint32_t>(green / divisor) << 8) |
                    (static_cast<std::uint32_t>(red / divisor) << 16);
                const auto leaving = pixelAt(position - radius);
                const auto entering = pixelAt(position + radius + 1);
                blue += static_cast<int>(entering & 0xff) - static_cast<int>(leaving & 0xff);
                green += static_cast<int>((entering >> 8) & 0xff) - static_cast<int>((leaving >> 8) & 0xff);
                red += static_cast<int>((entering >> 16) & 0xff) - static_cast<int>((leaving >> 16) & 0xff);
            }
        }
    };
    blurPass(pixels, temporary, true);
    blurPass(temporary, pixels, false);
}

bool insideRoundedRectangle(int x, int y, int left, int top, int right, int bottom, int radius) {
    if (x < left || x >= right || y < top || y >= bottom) return false;
    const int nearestX = std::clamp(x, left + radius, right - radius - 1);
    const int nearestY = std::clamp(y, top + radius, bottom - radius - 1);
    const int dx = x - nearestX;
    const int dy = y - nearestY;
    return dx * dx + dy * dy <= radius * radius;
}

float roundedRectangleCoverage(int x, int y, int left, int top, int right, int bottom, int radius) {
    const float px = x + 0.5f;
    const float py = y + 0.5f;
    const float nearestX = std::clamp(px, static_cast<float>(left + radius),
                                     static_cast<float>(right - radius));
    const float nearestY = std::clamp(py, static_cast<float>(top + radius),
                                     static_cast<float>(bottom - radius));
    const float dx = px - nearestX;
    const float dy = py - nearestY;
    const float distance = std::sqrt(dx * dx + dy * dy);
    return std::clamp(radius + 0.5f - distance, 0.0f, 1.0f);
}

std::wstring fromUtf8(const char* text) {
    if (!text || !*text) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (length <= 1) return {};
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), length);
    result.resize(static_cast<size_t>(length - 1));
    return result;
}

void fillRoundedRectangleCorners(HDC dc, const RECT& rect,
                                 int topLeft, int topRight, int bottomRight, int bottomLeft) {
    constexpr double kappa = 0.5522847498;
    auto curve = [&](int x1, int y1, int x2, int y2, int x3, int y3) {
        POINT points[]{{x1, y1}, {x2, y2}, {x3, y3}};
        PolyBezierTo(dc, points, 3);
    };
    BeginPath(dc);
    MoveToEx(dc, rect.left + topLeft, rect.top, nullptr);
    LineTo(dc, rect.right - topRight, rect.top);
    curve(rect.right - topRight + static_cast<int>(topRight * kappa), rect.top,
          rect.right, rect.top + topRight - static_cast<int>(topRight * kappa),
          rect.right, rect.top + topRight);
    LineTo(dc, rect.right, rect.bottom - bottomRight);
    curve(rect.right, rect.bottom - bottomRight + static_cast<int>(bottomRight * kappa),
          rect.right - bottomRight + static_cast<int>(bottomRight * kappa), rect.bottom,
          rect.right - bottomRight, rect.bottom);
    LineTo(dc, rect.left + bottomLeft, rect.bottom);
    curve(rect.left + bottomLeft - static_cast<int>(bottomLeft * kappa), rect.bottom,
          rect.left, rect.bottom - bottomLeft + static_cast<int>(bottomLeft * kappa),
          rect.left, rect.bottom - bottomLeft);
    LineTo(dc, rect.left, rect.top + topLeft);
    curve(rect.left, rect.top + topLeft - static_cast<int>(topLeft * kappa),
          rect.left + topLeft - static_cast<int>(topLeft * kappa), rect.top,
          rect.left + topLeft, rect.top);
    CloseFigure(dc);
    EndPath(dc);
    FillPath(dc);
}
} // namespace

OverlayWindow* OverlayWindow::activeInstance_ = nullptr;

int OverlayWindow::barTop() const { return frameHeight_ - kBarHeight; }
int OverlayWindow::menuOffset() const { return barTop() - 200; }
int OverlayWindow::sliderY() const { return menuOffset() + 112; }
int OverlayWindow::achievementVisibleCount() const {
    return (std::max)(1, (barTop() - 70 - kAchievementListTop) / kAchievementRowHeight);
}
std::vector<int> OverlayWindow::filteredAchievementIndices() const {
    std::vector<int> result;
    if (!achievementState_ || achievementState_->status != 1) return result;
    const int count = achievementState_->count;
    result.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        const bool unlocked = achievementState_->achievements[index].unlocked != FALSE;
        if (achievementFilter_ == AchievementFilter::All ||
            (achievementFilter_ == AchievementFilter::Locked && !unlocked) ||
            (achievementFilter_ == AchievementFilter::Unlocked && unlocked)) {
            result.push_back(index);
        }
    }
    return result;
}
void OverlayWindow::cycleAchievementFilter() {
    if (screen_ != Screen::Achievements) return;
    achievementFilter_ = achievementFilter_ == AchievementFilter::All
        ? AchievementFilter::Locked
        : achievementFilter_ == AchievementFilter::Locked
            ? AchievementFilter::Unlocked : AchievementFilter::All;
    achievementSelection_ = 0;
    renderAndPresent();
}
void OverlayWindow::toggleHiddenAchievementDetails() {
    if (screen_ != Screen::Achievements) return;
    revealHiddenAchievements_ = !revealHiddenAchievements_;
    renderAndPresent();
}

bool OverlayWindow::create(HINSTANCE instance, DWORD gamePid) {
    gamePid_ = gamePid;
    gameProcess_ = OpenProcess(SYNCHRONIZE, FALSE, gamePid);
    if (!gameProcess_) return false;

    const std::wstring sharedName = inputSharedMemoryName(gamePid);
    sharedMapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(InputSharedState), sharedName.c_str());
    if (!sharedMapping_) return false;
    sharedState_ = static_cast<InputSharedState*>(
        MapViewOfFile(sharedMapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(InputSharedState)));
    if (!sharedState_) return false;
    sharedState_->version = kInputSharedVersion;
    InterlockedExchange(&sharedState_->blockGameInput, FALSE);
    InterlockedExchange(&sharedState_->controllerConnected, FALSE);
    InterlockedExchange(&sharedState_->controllerButtons, 0);
    InterlockedExchange(&sharedState_->controllerThumbLX, 0);
    InterlockedExchange(&sharedState_->controllerThumbLY, 0);
    InterlockedExchange(&sharedState_->toggleOverlayRequest, FALSE);
    InterlockedExchange(&sharedState_->controllerUpdateTick, 0);

    const std::wstring achievementName = achievementSharedMemoryName(gamePid);
    achievementMapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              sizeof(AchievementSharedState), achievementName.c_str());
    if (achievementMapping_) {
        achievementState_ = static_cast<AchievementSharedState*>(MapViewOfFile(
            achievementMapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AchievementSharedState)));
        if (achievementState_) {
            ZeroMemory(achievementState_, sizeof(*achievementState_));
            achievementState_->version = kAchievementSharedVersion;
        }
    }
    loadIcons();
    GameInputCreate(&gameInput_);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.hbrBackground = nullptr;
    RegisterClassExW(&windowClass);

    const DWORD extendedStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    window_ = CreateWindowExW(extendedStyle, kWindowClass, L"Game Overlay MVP", WS_POPUP,
                              0, 0, kOverlayWidth, frameHeight_, nullptr, nullptr, instance, this);
    if (!window_) return false;

    activeInstance_ = this;
    keyboardHook_ = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, instance, 0);

    applyVisualStyle();
    centerOnGameMonitor();

    if (!RegisterHotKey(window_, kHotkeyId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, 'O')) {
        MessageBoxW(nullptr, L"Nao foi possivel registrar Ctrl+Shift+O.", L"Overlay MVP", MB_OK | MB_ICONWARNING);
    }
    SetTimer(window_, kProcessTimerId, 16, nullptr);
    syncVisibilityWithGame();
    return true;
}

void OverlayWindow::applyVisualStyle() {
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_DONOTROUND;
    DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));

    const BOOL darkMode = TRUE;
    DwmSetWindowAttribute(window_, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    const COLORREF noBorder = DWMWA_COLOR_NONE;
    DwmSetWindowAttribute(window_, DWMWA_BORDER_COLOR, &noBorder, sizeof(noBorder));
    const LONG_PTR styles = GetWindowLongPtrW(window_, GWL_EXSTYLE);
    SetWindowLongPtrW(window_, GWL_EXSTYLE, styles | WS_EX_LAYERED);
    systemBackdrop_ = false;
}

void OverlayWindow::centerOnGameMonitor() {
    WindowSearch search{gamePid_, nullptr};
    EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&search));
    HMONITOR monitor = search.result ? MonitorFromWindow(search.result, MONITOR_DEFAULTTONEAREST)
                                     : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int monitorHeight = info.rcMonitor.bottom - info.rcMonitor.top;
    frameHeight_ = (std::max)(480, monitorHeight - 56);
    const int y = info.rcMonitor.bottom - frameHeight_;
    SetWindowPos(window_, HWND_TOPMOST, info.rcMonitor.left, y, width, frameHeight_, SWP_NOACTIVATE);
}

void OverlayWindow::toggleVisibility() {
    enabled_ = !enabled_;
    if (!enabled_) {
        screen_ = Screen::MainBar;
        draggingVolume_ = false;
    }
    syncVisibilityWithGame();
}

void OverlayWindow::setGameInputBlocked(bool blocked) {
    if (sharedState_) InterlockedExchange(&sharedState_->blockGameInput, blocked ? TRUE : FALSE);
}

void OverlayWindow::syncVisibilityWithGame() {
    DWORD foregroundPid = 0;
    const HWND foregroundWindow = GetForegroundWindow();
    if (foregroundWindow) {
        GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
    }

    const bool shouldShow = enabled_ && (foregroundPid == gamePid_ || foregroundWindow == window_);
    if (shouldShow == shown_) return;

    shown_ = shouldShow;
    setGameInputBlocked(shown_);
    if (shown_) {
        centerOnGameMonitor();
        if (!captureAndBlurBackground()) {
            shown_ = false;
            setGameInputBlocked(false);
            return;
        }
        renderAndPresent();
        ShowWindow(window_, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(window_, SW_HIDE);
        blurredBackground_.clear();
        if (!enabled_) {
            WindowSearch search{gamePid_, nullptr};
            EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&search));
            if (search.result) SetForegroundWindow(search.result);
        }
    }
}

LRESULT CALLBACK OverlayWindow::keyboardHookProc(int code, WPARAM wParam, LPARAM lParam) {
    static bool controlDown = false;
    static bool shiftDown = false;
    static bool altDown = false;
    if (code >= 0) {
        const auto* key = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
        const bool up = wParam == WM_KEYUP || wParam == WM_SYSKEYUP;
        if (key->vkCode == VK_CONTROL || key->vkCode == VK_LCONTROL || key->vkCode == VK_RCONTROL) {
            if (down) controlDown = true;
            if (up) controlDown = false;
        }
        if (key->vkCode == VK_SHIFT || key->vkCode == VK_LSHIFT || key->vkCode == VK_RSHIFT) {
            if (down) shiftDown = true;
            if (up) shiftDown = false;
        }
        if (key->vkCode == VK_MENU || key->vkCode == VK_LMENU || key->vkCode == VK_RMENU) {
            if (down) altDown = true;
            if (up) altDown = false;
            // O shell precisa receber o Alt inicial para reconhecer Alt+Tab.
            return CallNextHookEx(nullptr, code, wParam, lParam);
        }

        if (activeInstance_ && activeInstance_->shown_) {
            if (key->vkCode == VK_TAB && (altDown || (key->flags & LLKHF_ALTDOWN))) {
                return CallNextHookEx(nullptr, code, wParam, lParam);
            }
            // Nunca bloqueie uma liberacao de tecla. Isso evita modificadores
            // presos caso o jogo ou o overlay sejam encerrados neste instante.
            if (up) return CallNextHookEx(nullptr, code, wParam, lParam);
            if (down) {
                if (key->vkCode == 'O' && controlDown && shiftDown) {
                    PostMessageW(activeInstance_->window_, WM_HOTKEY, kHotkeyId, 0);
                } else if (key->vkCode == VK_LEFT || key->vkCode == VK_RIGHT ||
                           key->vkCode == VK_UP || key->vkCode == VK_DOWN ||
                           key->vkCode == VK_RETURN || key->vkCode == VK_SPACE ||
                           key->vkCode == 'X' || key->vkCode == 'Y' ||
                           key->vkCode == VK_ESCAPE) {
                    PostMessageW(activeInstance_->window_, WM_KEYDOWN, key->vkCode, 0);
                }
            }
            return 1; // Nenhuma tecla chega ao jogo enquanto a barra está aberta.
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void OverlayWindow::moveSelection(int direction) {
    if (screen_ == Screen::Volume) {
        adjustVolume(direction);
        return;
    }
    if (screen_ == Screen::Settings) {
        settingsSelection_ = (settingsSelection_ + direction + 3) % 3;
        renderAndPresent();
        return;
    }
    if (screen_ == Screen::Achievements) {
        const int count = static_cast<int>(filteredAchievementIndices().size());
        if (count > 0) achievementSelection_ = std::clamp(achievementSelection_ + direction, 0, count - 1);
        renderAndPresent();
        return;
    }
    if (screen_ == Screen::Confirmation) {
        confirmationSelection_ = (confirmationSelection_ + direction + 2) % 2;
        renderAndPresent();
        return;
    }
    selectedItem_ = (selectedItem_ + direction + 3) % 3;
    renderAndPresent();
}

void OverlayWindow::activateSelection() {
    if (screen_ == Screen::MainBar && selectedItem_ == 0) {
        achievementSelection_ = 0;
        achievementFilter_ = AchievementFilter::All;
        revealHiddenAchievements_ = false;
        screen_ = Screen::Achievements;
        renderAndPresent();
        return;
    }
    if (screen_ == Screen::MainBar && selectedItem_ == 1) {
        // O dispositivo padrao pode mudar enquanto o overlay esta executando
        // (headset, HDMI, Bluetooth). Reabra o endpoint ao entrar nesta tela.
        if (endpointVolume_) {
            endpointVolume_->Release();
            endpointVolume_ = nullptr;
        }
        if (initializeSystemVolume()) {
            screen_ = Screen::Volume;
            renderAndPresent();
        }
        return;
    }
    if (screen_ == Screen::MainBar && selectedItem_ == 2) {
        settingsSelection_ = 0;
        screen_ = Screen::Settings;
        renderAndPresent();
        return;
    }
    if (screen_ == Screen::Settings) {
        confirmationSelection_ = 0;
        screen_ = Screen::Confirmation;
        renderAndPresent();
        return;
    }
    if (screen_ == Screen::Confirmation) {
        if (confirmationSelection_ == 1) executeSettingsAction();
        else screen_ = Screen::Settings;
        renderAndPresent();
        return;
    }
    MessageBeep(MB_OK);
}

void OverlayWindow::goBack() {
    if (screen_ == Screen::Confirmation) {
        screen_ = Screen::Settings;
        renderAndPresent();
    } else if (screen_ == Screen::Volume || screen_ == Screen::Achievements ||
               screen_ == Screen::Settings) {
        draggingVolume_ = false;
        screen_ = Screen::MainBar;
        renderAndPresent();
    } else {
        toggleVisibility();
    }
}

void OverlayWindow::executeSettingsAction() {
    if (settingsSelection_ == 0) {
        SetSuspendState(FALSE, FALSE, FALSE);
    } else if (settingsSelection_ == 1) {
        HANDLE token = nullptr;
        TOKEN_PRIVILEGES privileges{};
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
            privileges.PrivilegeCount = 1;
            LookupPrivilegeValueW(nullptr, L"SeShutdownPrivilege", &privileges.Privileges[0].Luid);
            privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
            CloseHandle(token);
            ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG,
                          SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED);
        }
    } else {
        prepareForGameExit();
        WindowSearch search{gamePid_, nullptr};
        EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&search));
        if (search.result) PostMessageW(search.result, WM_CLOSE, 0, 0);
    }
}

void OverlayWindow::prepareForGameExit() {
    closingGame_ = true;
    enabled_ = false;
    shown_ = false;
    draggingVolume_ = false;
    ReleaseCapture();
    setGameInputBlocked(false);
    ShowWindow(window_, SW_HIDE);
    blurredBackground_.clear();

    // O hook deixa de ser necessario neste ponto. Remove-lo imediatamente
    // garante que teclado e atalhos do shell nao dependam do fim do jogo.
    if (keyboardHook_) {
        UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    // Deixe a propria Steam restaurar/ativar o Big Picture. A interface atual
    // vive em steamwebhelper.exe e nem sempre expoe um HWND principal enumeravel.
    ShellExecuteW(window_, L"open", L"steam://open/bigpicture", nullptr, nullptr, SW_SHOWNORMAL);
    focusSteamBigPicture();
    nextSteamFocusAttempt_ = GetTickCount64() + 100;
}

bool OverlayWindow::initializeSystemVolume() {
    if (!endpointVolume_) {
        IMMDeviceEnumerator* enumerator = nullptr;
        IMMDevice* device = nullptr;
        HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&enumerator));
        if (SUCCEEDED(result)) {
            // Este e o mesmo papel usado pelo mixer principal para jogos e midia.
            result = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
        }
        if (SUCCEEDED(result)) {
            result = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER,
                                      nullptr, reinterpret_cast<void**>(&endpointVolume_));
        }
        if (device) device->Release();
        if (enumerator) enumerator->Release();
        if (FAILED(result)) return false;
    }
    return SUCCEEDED(endpointVolume_->GetMasterVolumeLevelScalar(&volumeLevel_));
}

void OverlayWindow::adjustVolume(int direction) {
    if (!initializeSystemVolume()) return;
    volumeLevel_ = std::clamp(volumeLevel_ + direction * 0.01f, 0.0f, 1.0f);
    endpointVolume_->SetMasterVolumeLevelScalar(volumeLevel_, nullptr);
    endpointVolume_->SetMute(volumeLevel_ <= 0.0001f ? TRUE : FALSE, nullptr);
    renderAndPresent();
}

void OverlayWindow::setVolumeFromMouse(int x) {
    if (!initializeSystemVolume()) return;
    const int center = frameWidth_ / 2;
    volumeLevel_ = std::clamp(static_cast<float>(x - (center - kSliderHalfWidth)) /
                                  static_cast<float>(kSliderHalfWidth * 2),
                              0.0f, 1.0f);
    endpointVolume_->SetMasterVolumeLevelScalar(volumeLevel_, nullptr);
    endpointVolume_->SetMute(volumeLevel_ <= 0.0001f ? TRUE : FALSE, nullptr);
    renderAndPresent();
}

void OverlayWindow::pollController() {
    if (sharedState_ &&
        InterlockedExchange(&sharedState_->toggleOverlayRequest, FALSE) != FALSE) {
        toggleVisibility();
    }
    if (achievementState_ && screen_ == Screen::Achievements) {
        const LONG generation = InterlockedCompareExchange(&achievementState_->generation, 0, 0);
        if (generation != achievementGeneration_) {
            achievementGeneration_ = generation;
            renderAndPresent();
        }
    }
    if (!shown_) {
        ZeroMemory(previousControllerButtons_, sizeof(previousControllerButtons_));
        previousGameInputButtons_ = GameInputGamepadNone;
        previousSharedControllerButtons_ = 0;
        heldNavigationDirection_ = 0;
        nextNavigationRepeat_ = 0;
        return;
    }

    auto navigate = [&](int direction) {
        const ULONGLONG now = GetTickCount64();
        if (direction != heldNavigationDirection_) {
            heldNavigationDirection_ = direction;
            if (direction != 0) {
                moveSelection(direction);
                nextNavigationRepeat_ = now + (screen_ == Screen::Volume ? 160 : 350);
            }
        } else if (direction != 0 && now >= nextNavigationRepeat_) {
            moveSelection(direction);
            nextNavigationRepeat_ = now + (screen_ == Screen::Volume ? 32 : 140);
        }
    };

    // Fonte preferencial: estado capturado dentro do próprio processo do jogo.
    // Isso também funciona quando a Steam expõe o controle apenas ao jogo.
    if (sharedState_ &&
        InterlockedCompareExchange(&sharedState_->controllerConnected, 0, 0) != FALSE &&
        static_cast<DWORD>(GetTickCount() - static_cast<DWORD>(
            InterlockedCompareExchange(&sharedState_->controllerUpdateTick, 0, 0))) < 250) {
        const WORD buttons = static_cast<WORD>(
            InterlockedCompareExchange(&sharedState_->controllerButtons, 0, 0));
        const SHORT thumbLX = static_cast<SHORT>(
            InterlockedCompareExchange(&sharedState_->controllerThumbLX, 0, 0));
        const SHORT thumbLY = static_cast<SHORT>(
            InterlockedCompareExchange(&sharedState_->controllerThumbLY, 0, 0));
        const WORD pressed = static_cast<WORD>(buttons & ~previousSharedControllerButtons_);
        int direction = 0;
        if (screen_ == Screen::Settings || screen_ == Screen::Achievements) {
            if ((buttons & XINPUT_GAMEPAD_DPAD_UP) || thumbLY > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) direction = -1;
            else if ((buttons & XINPUT_GAMEPAD_DPAD_DOWN) || thumbLY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) direction = 1;
        } else {
            if ((buttons & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_LEFT_SHOULDER)) ||
                thumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) direction = -1;
            else if ((buttons & (XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_RIGHT_SHOULDER)) ||
                     thumbLX > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) direction = 1;
        }
        navigate(direction);
        if (pressed & XINPUT_GAMEPAD_A) activateSelection();
        if (pressed & XINPUT_GAMEPAD_Y) cycleAchievementFilter();
        if (pressed & XINPUT_GAMEPAD_X) toggleHiddenAchievementDetails();
        if (pressed & XINPUT_GAMEPAD_B) goBack();
        previousSharedControllerButtons_ = buttons;
        return;
    }

    if (gameInput_) {
        IGameInputReading* reading = nullptr;
        if (SUCCEEDED(gameInput_->GetCurrentReading(GameInputKindGamepad, nullptr, &reading))) {
            GameInputGamepadState state{};
            if (reading->GetGamepadState(&state)) {
                const auto buttons = state.buttons;
                const auto pressed = static_cast<GameInputGamepadButtons>(
                    static_cast<unsigned int>(buttons) &
                    ~static_cast<unsigned int>(previousGameInputButtons_));
                int direction = 0;
                if (screen_ == Screen::Settings || screen_ == Screen::Achievements) {
                    if ((buttons & GameInputGamepadDPadUp) || state.leftThumbstickY > 0.35f) direction = -1;
                    else if ((buttons & GameInputGamepadDPadDown) || state.leftThumbstickY < -0.35f) direction = 1;
                } else {
                    if ((buttons & (GameInputGamepadDPadLeft | GameInputGamepadLeftShoulder)) ||
                        state.leftThumbstickX < -0.35f) direction = -1;
                    else if ((buttons & (GameInputGamepadDPadRight | GameInputGamepadRightShoulder)) ||
                             state.leftThumbstickX > 0.35f) direction = 1;
                }
                navigate(direction);
                if (pressed & GameInputGamepadA) activateSelection();
                if (pressed & GameInputGamepadY) cycleAchievementFilter();
                if (pressed & GameInputGamepadX) toggleHiddenAchievementDetails();
                if (pressed & GameInputGamepadB) goBack();
                previousGameInputButtons_ = buttons;
                reading->Release();
                return;
            }
            reading->Release();
        }
    }

    for (DWORD userIndex = 0; userIndex < XUSER_MAX_COUNT; ++userIndex) {
        XINPUT_STATE state{};
        if (XInputGetState(userIndex, &state) != ERROR_SUCCESS) {
            previousControllerButtons_[userIndex] = 0;
            continue;
        }

        const WORD buttons = state.Gamepad.wButtons;
        const WORD pressed = static_cast<WORD>(buttons & ~previousControllerButtons_[userIndex]);
        int direction = 0;
        if (screen_ == Screen::Settings || screen_ == Screen::Achievements) {
            if ((buttons & XINPUT_GAMEPAD_DPAD_UP) || state.Gamepad.sThumbLY > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) direction = -1;
            else if ((buttons & XINPUT_GAMEPAD_DPAD_DOWN) || state.Gamepad.sThumbLY < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) direction = 1;
        } else {
            if ((buttons & (XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_LEFT_SHOULDER)) ||
                state.Gamepad.sThumbLX < -XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) direction = -1;
            else if ((buttons & (XINPUT_GAMEPAD_DPAD_RIGHT | XINPUT_GAMEPAD_RIGHT_SHOULDER)) ||
                     state.Gamepad.sThumbLX > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) direction = 1;
        }

        navigate(direction);

        if (pressed & XINPUT_GAMEPAD_A) activateSelection();
        if (pressed & XINPUT_GAMEPAD_Y) cycleAchievementFilter();
        if (pressed & XINPUT_GAMEPAD_X) toggleHiddenAchievementDetails();
        if (pressed & XINPUT_GAMEPAD_B) goBack();
        previousControllerButtons_[userIndex] = buttons;
        return; // O primeiro controle conectado assume a navegação da barra.
    }

    heldNavigationDirection_ = 0;
    nextNavigationRepeat_ = 0;
}

void OverlayWindow::draw(HDC dc) const {
    RECT client{};
    GetClientRect(window_, &client);
    const int width = client.right - client.left;
    SetBkMode(dc, TRANSPARENT);
    if (screen_ == Screen::Volume || screen_ == Screen::Achievements ||
        screen_ == Screen::Settings) {
        HFONT titleFont = CreateFontW(44, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldTitleFont = SelectObject(dc, titleFont);
        SetTextColor(dc, RGB(248, 249, 252));
        RECT titleRect{};
        const wchar_t* title = nullptr;
        if (screen_ == Screen::Volume) {
            title = L"Volume";
            titleRect = RECT{0, sliderY() - 76, width, sliderY() - 30};
        } else if (screen_ == Screen::Achievements) {
            title = L"Conquistas";
            titleRect = RECT{0, 27, width, 76};
        } else {
            title = L"Configura\u00e7\u00f5" L"es";
            titleRect = RECT{0, menuOffset() - 23, width, menuOffset() + 22};
        }
        DrawTextW(dc, title, -1, &titleRect, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, oldTitleFont);
        DeleteObject(titleFont);
    }
    if (screen_ == Screen::Settings || screen_ == Screen::Confirmation) {
        HFONT font = CreateFontW(32, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetTextColor(dc, RGB(245, 246, 250));

        if (screen_ == Screen::Settings) {
            const wchar_t* labels[] = {L"Suspender", L"Desligar", L"Fechar jogo"};
            for (int index = 0; index < 3; ++index) {
                RECT row{width / 2 - 245, menuOffset() + 31 + index * 50,
                         width / 2 + 245, menuOffset() + 73 + index * 50};
                if (index == settingsSelection_) {
                    HBRUSH selected = CreateSolidBrush(RGB(92, 95, 104));
                    SelectObject(dc, selected);
                    if (index == 0) fillRoundedRectangleCorners(dc, row, 7, 7, 7, 7);
                    else if (index == 2) fillRoundedRectangleCorners(dc, row, 7, 7, 23, 23);
                    else fillRoundedRectangleCorners(dc, row, 7, 7, 7, 7);
                    SelectObject(dc, oldBrush);
                    DeleteObject(selected);
                }
                DrawTextW(dc, labels[index], -1, &row, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            }
        } else {
            const wchar_t* actions[] = {L"Suspender o computador?", L"Desligar o computador?",
                                        L"Fechar o jogo?"};
            RECT question{width / 2 - 205, menuOffset() + 39,
                          width / 2 + 205, menuOffset() + 91};
            DrawTextW(dc, actions[settingsSelection_], -1, &question,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            const wchar_t* labels[] = {L"Cancelar", L"Confirmar"};
            for (int index = 0; index < 2; ++index) {
                RECT button{width / 2 - 210 + index * 220, menuOffset() + 108,
                            width / 2 - 10 + index * 220, menuOffset() + 154};
                COLORREF buttonColor{};
                if (index == 1) {
                    buttonColor = index == confirmationSelection_ ? RGB(145, 91, 94)
                                                                   : RGB(92, 63, 67);
                } else {
                    buttonColor = index == confirmationSelection_ ? RGB(115, 118, 127)
                                                                   : RGB(65, 68, 76);
                }
                HBRUSH brush = CreateSolidBrush(buttonColor);
                SelectObject(dc, brush);
                if (index == 0) fillRoundedRectangleCorners(dc, button, 7, 7, 7, 23);
                else fillRoundedRectangleCorners(dc, button, 7, 7, 23, 7);
                DrawTextW(dc, labels[index], -1, &button, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                SelectObject(dc, oldBrush);
                DeleteObject(brush);
            }
        }
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); SelectObject(dc, oldFont);
        DeleteObject(font);
    }
    if (screen_ == Screen::Achievements) {
        const LONG status = achievementState_
            ? InterlockedCompareExchange(&achievementState_->status, 0, 0) : -1;
        const auto filtered = filteredAchievementIndices();
        const int count = static_cast<int>(filtered.size());
        HFONT nameFont = CreateFontW(30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HFONT descriptionFont = CreateFontW(25, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                            ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, nameFont);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        if (status != 1 || count == 0) {
            SetTextColor(dc, RGB(220, 222, 228));
            RECT message{0, 220, width, 360};
            const wchar_t* text = status < 0 ? L"Conquistas da Steam indisponiveis" :
                                  status == 0 ? L"Carregando conquistas da Steam..." :
                                                L"Nenhuma conquista neste filtro";
            DrawTextW(dc, text, -1, &message, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
        } else {
            const int visibleCount = achievementVisibleCount();
            const int first = std::clamp(achievementSelection_ - visibleCount / 2, 0,
                                         (std::max)(0, count - visibleCount));
            for (int visible = 0; visible < visibleCount && first + visible < count; ++visible) {
                const int position = first + visible;
                const auto& achievement = achievementState_->achievements[filtered[position]];
                RECT row{width / 2 - kAchievementCardHalfWidth,
                         kAchievementListTop + visible * kAchievementRowHeight,
                         width / 2 + kAchievementCardHalfWidth,
                         kAchievementListTop + kAchievementCardHeight +
                             visible * kAchievementRowHeight};
                SetTextColor(dc, achievement.unlocked ? RGB(250, 250, 252) : RGB(145, 147, 154));
                if (achievement.iconWidth <= 0) {
                    RECT marker{row.left + 18, row.top, row.left + 92, row.bottom};
                    DrawTextW(dc, achievement.unlocked ? L"\x2713" : L"\x25CB", -1, &marker,
                              DT_CENTER | DT_SINGLELINE | DT_VCENTER);
                }
                const bool conceal = achievement.hidden && !achievement.unlocked &&
                                     !revealHiddenAchievements_;
                std::wstring name = conceal ? L"Conquista oculta"
                                            : fromUtf8(achievement.displayName);
                std::wstring description = conceal
                    ? L"Os detalhes desta conquista est\u00e3o ocultos"
                    : fromUtf8(achievement.description);
                RECT nameRect{row.left + 105, row.top + 11, row.right - 18, row.top + 47};
                SelectObject(dc, nameFont);
                DrawTextW(dc, name.c_str(), -1, &nameRect,
                          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                SetTextColor(dc, achievement.unlocked ? RGB(195, 197, 204) : RGB(115, 117, 124));
                RECT descRect{row.left + 105, row.top + 48, row.right - 18,
                              row.top + 82};
                SelectObject(dc, descriptionFont);
                DrawTextW(dc, description.c_str(), -1, &descRect,
                          DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
                if (achievement.hasProgress && achievement.progressMaximum > 0) {
                    const std::wstring progressText =
                        std::to_wstring(achievement.progressCurrent) + L"/" +
                        std::to_wstring(achievement.progressMaximum);
                    SelectObject(dc, descriptionFont);
                    SetTextColor(dc, achievement.unlocked ? RGB(195, 197, 204)
                                                           : RGB(145, 147, 154));
                    SIZE progressTextSize{};
                    GetTextExtentPoint32W(dc, progressText.c_str(),
                                          static_cast<int>(progressText.size()),
                                          &progressTextSize);
                    const int progressLabelLeft = row.left + 105;
                    const int progressLeft = progressLabelLeft + progressTextSize.cx + 10;
                    const int progressRight = row.right - 18;
                    const int progressTop = row.bottom - 15;
                    RECT progressLabel{progressLabelLeft, progressTop - 10,
                                       progressLeft - 10, progressTop + 17};
                    DrawTextW(dc, progressText.c_str(), -1, &progressLabel,
                              DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
                    HBRUSH track = CreateSolidBrush(RGB(82, 84, 91));
                    SelectObject(dc, track);
                    RoundRect(dc, progressLeft, progressTop, progressRight, progressTop + 7, 7, 7);
                    const double ratio = std::clamp(
                        static_cast<double>(achievement.progressCurrent) /
                            achievement.progressMaximum, 0.0, 1.0);
                    const int activeRight = progressLeft +
                        static_cast<int>((progressRight - progressLeft) * ratio);
                    if (activeRight > progressLeft) {
                        HBRUSH active = CreateSolidBrush(RGB(195, 197, 203));
                        SelectObject(dc, active);
                        RoundRect(dc, progressLeft, progressTop, activeRight, progressTop + 7, 7, 7);
                        SelectObject(dc, track);
                        DeleteObject(active);
                    }
                    SelectObject(dc, oldBrush);
                    DeleteObject(track);
                }
            }
        }
        HFONT legendFont = CreateFontW(22, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                       ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        SelectObject(dc, legendFont);
        SetTextColor(dc, RGB(190, 192, 200));
        const wchar_t* filterName = achievementFilter_ == AchievementFilter::All ? L"Todas" :
                                    achievementFilter_ == AchievementFilter::Locked ? L"Bloqueadas" :
                                                                                      L"Desbloqueadas";
        const wchar_t* hiddenAction = revealHiddenAchievements_ ? L"Ocultar secretas"
                                                                 : L"Revelar secretas";
        const int center = width / 2;
        const std::wstring hiddenText = (buttonIcons_[0].pixels.empty() ? L"X  " : L"") +
                                        std::wstring(hiddenAction);
        const std::wstring filterText = (buttonIcons_[1].pixels.empty() ? L"Y  " : L"") +
                                        std::wstring(L"Filtrar: ") + filterName;
        RECT hiddenRect{center - 320, barTop() - 58, center - 105, barTop() - 10};
        RECT filterRect{center - 55, barTop() - 58, center + 180, barTop() - 10};
        RECT backRect{center + 215, barTop() - 58, center + 360, barTop() - 10};
        DrawTextW(dc, hiddenText.c_str(), -1, &hiddenRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        DrawTextW(dc, filterText.c_str(), -1, &filterRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        const wchar_t* backText = buttonIcons_[2].pixels.empty() ? L"B  Voltar" : L"Voltar";
        DrawTextW(dc, backText, -1, &backRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        SelectObject(dc, oldFont);
        DeleteObject(legendFont);
        SelectObject(dc, oldBrush); SelectObject(dc, oldPen); SelectObject(dc, oldFont);
        DeleteObject(descriptionFont); DeleteObject(nameFont);
    }
    const int centers[] = {width / 2 - 190, width / 2, width / 2 + 190};

    for (int i = 0; i < 3; ++i) {
        const COLORREF color = RGB(245, 246, 250);
        if (icons_[i].pixels.empty()) {
            if (i == 0) drawAchievementIcon(dc, centers[i], barTop() + 190, color);
            if (i == 1) drawVolumeIcon(dc, centers[i], barTop() + 190, color);
            if (i == 2) drawSettingsIcon(dc, centers[i], barTop() + 190, color);
        }
    }
}

bool OverlayWindow::loadIcons() {
    wchar_t executablePath[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath,
                                            static_cast<DWORD>(std::size(executablePath)));
    const auto directory = std::filesystem::path(std::wstring(executablePath, length)).parent_path();
    const wchar_t* filenames[] = {
        L"trophy.png", L"volume.png", L"settings.png", L"x.png", L"y.png", L"b.png"
    };
    bool allLoaded = true;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return false;

    for (int index = 0; index < 6; ++index) {
        IconImage& destination = index < 3 ? icons_[index] : buttonIcons_[index - 3];
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* converter = nullptr;
        const auto path = directory / L"assets" / (index < 3 ? L"icons" : L"buttons") / filenames[index];
        HRESULT result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
        UINT sourceWidth = 0, sourceHeight = 0;
        if (SUCCEEDED(result)) result = frame->GetSize(&sourceWidth, &sourceHeight);
        UINT width = 1, height = 1;
        if (SUCCEEDED(result) && sourceWidth && sourceHeight) {
            const double maximumSize = index < 3 ? 60.0 : 30.0;
            const double scale = (std::min)(maximumSize / sourceWidth, maximumSize / sourceHeight);
            width = (std::max)(1u, static_cast<UINT>(sourceWidth * scale));
            height = (std::max)(1u, static_cast<UINT>(sourceHeight * scale));
        }
        if (SUCCEEDED(result)) result = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(result)) result = scaler->Initialize(frame, width, height,
                                                            WICBitmapInterpolationModeFant);
        if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(result)) result = converter->Initialize(
            scaler, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
            WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(result)) {
            destination.width = static_cast<int>(width);
            destination.height = static_cast<int>(height);
            destination.pixels.resize(static_cast<size_t>(width) * height);
            result = converter->CopyPixels(nullptr, width * 4,
                                           static_cast<UINT>(destination.pixels.size() * 4),
                                           reinterpret_cast<BYTE*>(destination.pixels.data()));
        }
        if (FAILED(result)) {
            destination = {};
            allLoaded = false;
        }
        if (converter) converter->Release();
        if (scaler) scaler->Release();
        if (frame) frame->Release();
        if (decoder) decoder->Release();
    }
    factory->Release();
    return allLoaded;
}

void OverlayWindow::blendIcon(std::uint32_t* destination, int destinationWidth,
                              const IconImage& icon, int centerX, int centerY, BYTE opacity) const {
    if (icon.pixels.empty()) return;
    const int left = centerX - icon.width / 2;
    const int top = centerY - icon.height / 2;
    for (int y = 0; y < icon.height; ++y) {
        if (top + y < 0 || top + y >= frameHeight_) continue;
        for (int x = 0; x < icon.width; ++x) {
            if (left + x < 0 || left + x >= destinationWidth) continue;
            const auto source = icon.pixels[static_cast<size_t>(y) * icon.width + x];
            const BYTE originalAlpha = static_cast<BYTE>(source >> 24);
            const BYTE alpha = static_cast<BYTE>(originalAlpha * opacity / 255);
            if (alpha == 0) continue;
            const BYTE inverse = static_cast<BYTE>(255 - alpha);
            const auto targetIndex = static_cast<size_t>(top + y) * destinationWidth + left + x;
            const auto target = destination[targetIndex];
            const BYTE blue = static_cast<BYTE>(((source & 0xff) * opacity / 255) +
                                                 (target & 0xff) * inverse / 255);
            const BYTE green = static_cast<BYTE>((((source >> 8) & 0xff) * opacity / 255) +
                                                  ((target >> 8) & 0xff) * inverse / 255);
            const BYTE red = static_cast<BYTE>((((source >> 16) & 0xff) * opacity / 255) +
                                                ((target >> 16) & 0xff) * inverse / 255);
            const BYTE outAlpha = static_cast<BYTE>(alpha + ((target >> 24) & 0xff) * inverse / 255);
            destination[targetIndex] = (static_cast<std::uint32_t>(outAlpha) << 24) |
                (static_cast<std::uint32_t>(red) << 16) |
                (static_cast<std::uint32_t>(green) << 8) | blue;
        }
    }
}

void OverlayWindow::drawVolumeSlider(std::uint32_t* destination, int destinationWidth) const {
    const float left = static_cast<float>(destinationWidth / 2 - kSliderHalfWidth);
    const float right = static_cast<float>(destinationWidth / 2 + kSliderHalfWidth);
    const float knobX = left + (right - left) * volumeLevel_;

    auto blendPixel = [&](int x, int y, BYTE gray, float coverage, BYTE opacity = 255) {
        if (x < 0 || x >= destinationWidth || y < 0 || y >= frameHeight_) return;
        const BYTE sourceAlpha = static_cast<BYTE>(std::clamp(coverage, 0.0f, 1.0f) * opacity);
        if (!sourceAlpha) return;
        const BYTE inverse = static_cast<BYTE>(255 - sourceAlpha);
        auto& pixel = destination[static_cast<size_t>(y) * destinationWidth + x];
        const BYTE blue = static_cast<BYTE>(gray * sourceAlpha / 255 + (pixel & 0xff) * inverse / 255);
        const BYTE green = static_cast<BYTE>(gray * sourceAlpha / 255 + ((pixel >> 8) & 0xff) * inverse / 255);
        const BYTE red = static_cast<BYTE>(gray * sourceAlpha / 255 + ((pixel >> 16) & 0xff) * inverse / 255);
        const BYTE alpha = static_cast<BYTE>(sourceAlpha + ((pixel >> 24) & 0xff) * inverse / 255);
        pixel = (static_cast<std::uint32_t>(alpha) << 24) |
                (static_cast<std::uint32_t>(red) << 16) |
                (static_cast<std::uint32_t>(green) << 8) | blue;
    };

    auto capsule = [&](float from, float to, float radius, BYTE gray) {
        const int minX = static_cast<int>(std::floor(from - radius - 1));
        const int maxX = static_cast<int>(std::ceil(to + radius + 1));
        const int minY = static_cast<int>(std::floor(sliderY() - radius - 1));
        const int maxY = static_cast<int>(std::ceil(sliderY() + radius + 1));
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float nearestX = std::clamp(x + 0.5f, from, to);
                const float dx = x + 0.5f - nearestX;
                const float dy = y + 0.5f - sliderY();
                const float coverage = std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                blendPixel(x, y, gray, coverage);
            }
        }
    };

    auto circle = [&](float radius, BYTE gray, BYTE opacity) {
        const int minX = static_cast<int>(std::floor(knobX - radius - 1));
        const int maxX = static_cast<int>(std::ceil(knobX + radius + 1));
        const int minY = static_cast<int>(std::floor(sliderY() - radius - 1));
        const int maxY = static_cast<int>(std::ceil(sliderY() + radius + 1));
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float dx = x + 0.5f - knobX;
                const float dy = y + 0.5f - sliderY();
                const float coverage = std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                blendPixel(x, y, gray, coverage, opacity);
            }
        }
    };

    capsule(left, right, 6.0f, 82);
    capsule(left, (std::max)(left, knobX), 6.0f, 190);
    circle(17.0f, 35, 105);
    circle(13.0f, 238, 255);
    circle(8.0f, 205, 255);
}

void OverlayWindow::drawAchievementImages(std::uint32_t* destination, int destinationWidth) const {
    if (!achievementState_ || achievementState_->status != 1) return;
    const auto filtered = filteredAchievementIndices();
    const int count = static_cast<int>(filtered.size());
    const int visibleCount = achievementVisibleCount();
    const int first = std::clamp(achievementSelection_ - visibleCount / 2, 0,
                                 (std::max)(0, count - visibleCount));
    for (int visible = 0; visible < visibleCount && first + visible < count; ++visible) {
        const auto& achievement = achievementState_->achievements[filtered[first + visible]];
        if (achievement.iconWidth != 64 || achievement.iconHeight != 64) continue;
        const int left = destinationWidth / 2 - kAchievementCardHalfWidth + 18;
        const int top = kAchievementListTop + 20 + visible * kAchievementRowHeight;
        // A Steam ja devolve a variante correta (conquistada ou bloqueada).
        // Nao altere cor, brilho ou opacidade da imagem recebida.
        constexpr BYTE opacity = 255;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const auto source = achievement.iconPixels[y * 64 + x];
                const float cornerX = (std::min)(x + 0.5f, 64.0f - (x + 0.5f));
                const float cornerY = (std::min)(y + 0.5f, 64.0f - (y + 0.5f));
                float mask = 1.0f;
                if (cornerX < 8.0f && cornerY < 8.0f) {
                    const float dx = 8.0f - cornerX;
                    const float dy = 8.0f - cornerY;
                    mask = std::clamp(8.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                }
                const float effectiveOpacity = opacity * mask;
                const BYTE sourceAlpha = static_cast<BYTE>((source >> 24) * effectiveOpacity / 255);
                if (!sourceAlpha) continue;
                const BYTE inverse = static_cast<BYTE>(255 - sourceAlpha);
                auto& target = destination[static_cast<size_t>(top + y) * destinationWidth + left + x];
                const BYTE blue = static_cast<BYTE>((source & 0xff) * effectiveOpacity / 255 +
                                                     (target & 0xff) * inverse / 255);
                const BYTE green = static_cast<BYTE>(((source >> 8) & 0xff) * effectiveOpacity / 255 +
                                                      ((target >> 8) & 0xff) * inverse / 255);
                const BYTE red = static_cast<BYTE>(((source >> 16) & 0xff) * effectiveOpacity / 255 +
                                                    ((target >> 16) & 0xff) * inverse / 255);
                const BYTE alpha = static_cast<BYTE>(sourceAlpha + ((target >> 24) & 0xff) * inverse / 255);
                target = (static_cast<std::uint32_t>(alpha) << 24) |
                         (static_cast<std::uint32_t>(red) << 16) |
                         (static_cast<std::uint32_t>(green) << 8) | blue;
            }
        }
    }
}

void OverlayWindow::releaseRenderer() {
    if (frameDc_) {
        DeleteDC(frameDc_);
        frameDc_ = nullptr;
    }
    if (frameBitmap_) {
        DeleteObject(frameBitmap_);
        frameBitmap_ = nullptr;
    }
    framePixels_ = nullptr;
    frameWidth_ = 0;
}

bool OverlayWindow::captureAndBlurBackground() {
    RECT windowRect{};
    GetWindowRect(window_, &windowRect);
    const int width = windowRect.right - windowRect.left;
    if (width <= 0) return false;

    releaseRenderer();
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -frameHeight_;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    frameDc_ = CreateCompatibleDC(screen);
    frameBitmap_ = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &framePixels_, nullptr, 0);
    if (!frameDc_ || !frameBitmap_ || !framePixels_) {
        if (screen) ReleaseDC(nullptr, screen);
        releaseRenderer();
        return false;
    }
    SelectObject(frameDc_, frameBitmap_);
    BitBlt(frameDc_, 0, 0, width, frameHeight_, screen, windowRect.left, windowRect.top,
           SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);

    frameWidth_ = width;
    const auto* captured = static_cast<const std::uint32_t*>(framePixels_);
    blurredBackground_.assign(captured, captured + static_cast<size_t>(width) * frameHeight_);
    // Quatro passagens formam uma aproximação gaussiana mais forte e removem
    // artefatos de alta frequência da captura do jogo.
    for (int pass = 0; pass < 4; ++pass) {
        boxBlur(blurredBackground_, width, frameHeight_, 18);
    }
    return true;
}

void OverlayWindow::renderAndPresent() {
    if (!framePixels_ || blurredBackground_.empty() || frameWidth_ <= 0) return;
    auto* output = static_cast<std::uint32_t*>(framePixels_);
    if (screen_ == Screen::Volume) initializeSystemVolume();
    const int panelWidth = screen_ == Screen::Volume ? 464 :
                           screen_ == Screen::Settings ? 500 :
                           screen_ == Screen::Achievements ? kAchievementPanelWidth :
                           screen_ == Screen::Confirmation ? 430 : kVolumePanelWidth;
    const int panelTop = screen_ == Screen::Volume ? sliderY() - 84 :
                         screen_ == Screen::Achievements ? 18 :
                         screen_ == Screen::Settings ? menuOffset() - 28 :
                         screen_ == Screen::Confirmation ? menuOffset() + 34 : sliderY() - 22;
    const int panelBottom = screen_ == Screen::Volume ? sliderY() + 22 :
                            screen_ == Screen::Achievements ? barTop() - 6 :
                            screen_ == Screen::Settings ? menuOffset() + 178 :
                            screen_ == Screen::Confirmation ? menuOffset() + 159 : sliderY() + 22;
    const int panelLeft = (frameWidth_ - panelWidth) / 2;
    const int panelRight = panelLeft + panelWidth;
    const int panelRadius = (std::min)(kVolumePanelRadius, (panelBottom - panelTop) / 2);
    for (int y = 0; y < frameHeight_; ++y) {
        const double progress = y >= barTop()
            ? static_cast<double>(y - barTop()) / (kBarHeight - 1) : 0.0;
        const double fadeProgress = y >= barTop() ? (std::min)(1.0, progress * 2.0) : 0.0;
        const double eased = fadeProgress * fadeProgress * (3.0 - 2.0 * fadeProgress);
        BYTE alpha = static_cast<BYTE>(kGradientBottomOpacity * eased);
        if (alpha < 6) alpha = 0; // Evita quantização cromática quase invisível no topo.
        const double sourceWeight = 1.0 - 0.42 * eased;
        const double tintWeight = 1.0 - sourceWeight;
        for (int x = 0; x < frameWidth_; ++x) {
            const size_t index = static_cast<size_t>(y) * frameWidth_ + x;
            const auto source = blurredBackground_[index];
            const double blueTinted = (source & 0xff) * sourceWeight + kPanelBlue * tintWeight;
            const double greenTinted = ((source >> 8) & 0xff) * sourceWeight + kPanelGreen * tintWeight;
            const double redTinted = ((source >> 16) & 0xff) * sourceWeight + kPanelRed * tintWeight;
            const BYTE blue = static_cast<BYTE>(blueTinted * alpha / 255.0);
            const BYTE green = static_cast<BYTE>(greenTinted * alpha / 255.0);
            const BYTE red = static_cast<BYTE>(redTinted * alpha / 255.0);
            output[index] =
                (static_cast<std::uint32_t>(alpha) << 24) |
                (static_cast<std::uint32_t>(red) << 16) |
                (static_cast<std::uint32_t>(green) << 8) | blue;

            if (screen_ != Screen::MainBar &&
                roundedRectangleCoverage(x, y, panelLeft, panelTop, panelRight,
                                         panelBottom, panelRadius) > 0.0f) {
                constexpr BYTE panelAlpha = 244;
                constexpr double panelSourceWeight = 0.52;
                const float coverage = roundedRectangleCoverage(
                    x, y, panelLeft, panelTop, panelRight, panelBottom, panelRadius);
                const BYTE coveredAlpha = static_cast<BYTE>(panelAlpha * coverage);
                const BYTE panelBlue = static_cast<BYTE>(((source & 0xff) * panelSourceWeight +
                    kPanelBlue * (1.0 - panelSourceWeight)) * coveredAlpha / 255.0);
                const BYTE panelGreen = static_cast<BYTE>((((source >> 8) & 0xff) * panelSourceWeight +
                    kPanelGreen * (1.0 - panelSourceWeight)) * coveredAlpha / 255.0);
                const BYTE panelRed = static_cast<BYTE>((((source >> 16) & 0xff) * panelSourceWeight +
                    kPanelRed * (1.0 - panelSourceWeight)) * coveredAlpha / 255.0);
                output[index] = (static_cast<std::uint32_t>(coveredAlpha) << 24) |
                    (static_cast<std::uint32_t>(panelRed) << 16) |
                    (static_cast<std::uint32_t>(panelGreen) << 8) | panelBlue;
            }
        }
    }

    if (screen_ == Screen::Achievements && achievementState_ && achievementState_->status == 1 &&
        !filteredAchievementIndices().empty()) {
        const int count = static_cast<int>(filteredAchievementIndices().size());
        const int visibleCount = achievementVisibleCount();
        const int first = std::clamp(achievementSelection_ - visibleCount / 2, 0,
                                     (std::max)(0, count - visibleCount));
        const int visible = achievementSelection_ - first;
        const int left = frameWidth_ / 2 - kAchievementCardHalfWidth;
        const int right = frameWidth_ / 2 + kAchievementCardHalfWidth;
        const int top = kAchievementListTop + visible * kAchievementRowHeight;
        const int bottom = kAchievementListTop + kAchievementCardHeight +
                           visible * kAchievementRowHeight;
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                const float coverage = roundedRectangleCoverage(x, y, left, top, right, bottom, 8);
                if (coverage <= 0.0f) continue;
                auto& pixel = output[static_cast<size_t>(y) * frameWidth_ + x];
                const float multiplier = 1.0f - 0.30f * coverage;
                const BYTE blue = static_cast<BYTE>((pixel & 0xff) * multiplier);
                const BYTE green = static_cast<BYTE>(((pixel >> 8) & 0xff) * multiplier);
                const BYTE red = static_cast<BYTE>(((pixel >> 16) & 0xff) * multiplier);
                pixel = (pixel & 0xff000000u) | (static_cast<std::uint32_t>(red) << 16) |
                        (static_cast<std::uint32_t>(green) << 8) | blue;
            }
        }
    }

    BITMAPINFO uiInfo{};
    constexpr int uiScale = 3;
    uiInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    uiInfo.bmiHeader.biWidth = frameWidth_ * uiScale;
    uiInfo.bmiHeader.biHeight = -frameHeight_ * uiScale;
    uiInfo.bmiHeader.biPlanes = 1;
    uiInfo.bmiHeader.biBitCount = 32;
    uiInfo.bmiHeader.biCompression = BI_RGB;
    void* uiPixelsRaw = nullptr;
    HBITMAP uiBitmap = CreateDIBSection(frameDc_, &uiInfo, DIB_RGB_COLORS, &uiPixelsRaw, nullptr, 0);
    HDC uiDc = CreateCompatibleDC(frameDc_);
    HGDIOBJ oldUiBitmap = SelectObject(uiDc, uiBitmap);
    ZeroMemory(uiPixelsRaw, static_cast<size_t>(frameWidth_ * uiScale) *
                              (frameHeight_ * uiScale) * sizeof(std::uint32_t));
    SetGraphicsMode(uiDc, GM_ADVANCED);
    XFORM transform{static_cast<FLOAT>(uiScale), 0.0f, 0.0f,
                    static_cast<FLOAT>(uiScale), 0.0f, 0.0f};
    SetWorldTransform(uiDc, &transform);
    draw(uiDc);

    const auto* uiPixels = static_cast<const std::uint32_t*>(uiPixelsRaw);
    const int uiWidth = frameWidth_ * uiScale;
    for (int y = 0; y < frameHeight_; ++y) {
      for (int x = 0; x < frameWidth_; ++x) {
        unsigned blueSum = 0, greenSum = 0, redSum = 0;
        for (int sampleY = 0; sampleY < uiScale; ++sampleY) {
            for (int sampleX = 0; sampleX < uiScale; ++sampleX) {
                const auto sample = uiPixels[static_cast<size_t>(y * uiScale + sampleY) * uiWidth +
                                             x * uiScale + sampleX];
                blueSum += sample & 0xff;
                greenSum += (sample >> 8) & 0xff;
                redSum += (sample >> 16) & 0xff;
            }
        }
        constexpr unsigned sampleCount = uiScale * uiScale;
        const std::uint32_t ui = (redSum / sampleCount << 16) |
                                 (greenSum / sampleCount << 8) | blueSum / sampleCount;
        const auto sourceAlphaValue = (std::max)({ui & 0xffu, (ui >> 8) & 0xffu,
                                                   (ui >> 16) & 0xffu});
        const BYTE sourceAlpha = static_cast<BYTE>(sourceAlphaValue);
        if (sourceAlpha == 0) continue;
        const BYTE inverse = static_cast<BYTE>(255 - sourceAlpha);
        const size_t i = static_cast<size_t>(y) * frameWidth_ + x;
        const std::uint32_t destination = output[i];
        const BYTE blue = static_cast<BYTE>((ui & 0xff) + (destination & 0xff) * inverse / 255);
        const BYTE green = static_cast<BYTE>(((ui >> 8) & 0xff) + ((destination >> 8) & 0xff) * inverse / 255);
        const BYTE red = static_cast<BYTE>(((ui >> 16) & 0xff) + ((destination >> 16) & 0xff) * inverse / 255);
        const BYTE alpha = static_cast<BYTE>(sourceAlpha + ((destination >> 24) & 0xff) * inverse / 255);
        output[i] = (static_cast<std::uint32_t>(alpha) << 24) |
                    (static_cast<std::uint32_t>(red) << 16) |
                    (static_cast<std::uint32_t>(green) << 8) | blue;
      }
    }

    if (screen_ == Screen::Volume) drawVolumeSlider(output, frameWidth_);
    if (screen_ == Screen::Achievements) {
        drawAchievementImages(output, frameWidth_);
        blendIcon(output, frameWidth_, buttonIcons_[0], frameWidth_ / 2 - 345, barTop() - 34, 255);
        blendIcon(output, frameWidth_, buttonIcons_[1], frameWidth_ / 2 - 80, barTop() - 34, 255);
        blendIcon(output, frameWidth_, buttonIcons_[2], frameWidth_ / 2 + 195, barTop() - 34, 255);
    }

    const int iconCenters[] = {frameWidth_ / 2 - 190, frameWidth_ / 2, frameWidth_ / 2 + 190};
    for (int index = 0; index < 3; ++index) {
        blendIcon(output, frameWidth_, icons_[index], iconCenters[index], barTop() + 190,
                  index == selectedItem_ ? 255 : 82);
    }
    SelectObject(uiDc, oldUiBitmap);
    DeleteDC(uiDc);
    DeleteObject(uiBitmap);

    RECT windowRect{};
    GetWindowRect(window_, &windowRect);
    POINT destination{windowRect.left, windowRect.top};
    SIZE size{frameWidth_, frameHeight_};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(window_, screen, &destination, &size, frameDc_, &source, 0, &blend, ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

LRESULT CALLBACK OverlayWindow::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    OverlayWindow* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<OverlayWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }

    switch (message) {
    case WM_NCHITTEST: {
        if (!self) return HTCLIENT;
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(window, &point);
        if (point.y >= self->barTop()) return HTCLIENT;
        if (self->screen_ != Screen::MainBar) {
            const int panelWidth = self->screen_ == Screen::Volume ? 464 :
                                   self->screen_ == Screen::Settings ? 500 :
                                   self->screen_ == Screen::Achievements ? kAchievementPanelWidth :
                                   self->screen_ == Screen::Confirmation ? 430 : kVolumePanelWidth;
            const int panelTop = self->screen_ == Screen::Volume ? self->sliderY() - 84 :
                                 self->screen_ == Screen::Achievements ? 18 :
                                 self->screen_ == Screen::Settings ? self->menuOffset() - 28 :
                                 self->screen_ == Screen::Confirmation ? self->menuOffset() + 34 : self->sliderY() - 22;
            const int panelBottom = self->screen_ == Screen::Volume ? self->sliderY() + 22 :
                                    self->screen_ == Screen::Achievements ? self->barTop() - 6 :
                                    self->screen_ == Screen::Settings ? self->menuOffset() + 178 :
                                    self->screen_ == Screen::Confirmation ? self->menuOffset() + 159 : self->sliderY() + 22;
            const int left = (self->frameWidth_ - panelWidth) / 2;
            const int radius = (std::min)(kVolumePanelRadius, (panelBottom - panelTop) / 2);
            if (insideRoundedRectangle(point.x, point.y, left, panelTop,
                                       left + panelWidth, panelBottom,
                                       radius)) return HTCLIENT;
        }
        return HTTRANSPARENT;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_HOTKEY:
        if (self && wParam == kHotkeyId) self->toggleVisibility();
        return 0;
    case WM_TIMER:
        if (self && wParam == kProcessTimerId) {
            if (WaitForSingleObject(self->gameProcess_, 0) == WAIT_OBJECT_0) {
                if (self->closingGame_) focusSteamBigPicture();
                DestroyWindow(window);
            } else if (self->closingGame_) {
                const ULONGLONG now = GetTickCount64();
                if (now >= self->nextSteamFocusAttempt_) {
                    focusSteamBigPicture();
                    self->nextSteamFocusAttempt_ = now + 250;
                }
            } else {
                self->syncVisibilityWithGame();
                self->pollController();
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (!self) return 0;
        if (wParam == VK_LEFT) self->moveSelection(-1);
        else if (wParam == VK_RIGHT) self->moveSelection(1);
        else if (wParam == VK_UP && (self->screen_ == Screen::Settings ||
                                     self->screen_ == Screen::Achievements)) self->moveSelection(-1);
        else if (wParam == VK_DOWN && (self->screen_ == Screen::Settings ||
                                       self->screen_ == Screen::Achievements)) self->moveSelection(1);
        else if (wParam == 'Y') self->cycleAchievementFilter();
        else if (wParam == 'X') self->toggleHiddenAchievementDetails();
        else if (wParam == VK_RETURN || wParam == VK_SPACE) self->activateSelection();
        else if (wParam == VK_ESCAPE) self->goBack();
        return 0;
    case WM_LBUTTONDOWN:
        if (self && self->screen_ == Screen::Volume) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (self->screen_ == Screen::Achievements) {
                const int count = static_cast<int>(self->filteredAchievementIndices().size());
                const int visibleCount = self->achievementVisibleCount();
                if (count > 0 && y >= kAchievementListTop &&
                    y < kAchievementListTop + visibleCount * kAchievementRowHeight) {
                    const int first = std::clamp(
                        self->achievementSelection_ - visibleCount / 2, 0,
                        (std::max)(0, count - visibleCount));
                    const int selected = first +
                        (y - kAchievementListTop) / kAchievementRowHeight;
                    if (selected < count) {
                        self->achievementSelection_ = selected;
                        self->renderAndPresent();
                    }
                }
                return 0;
            }
            const int center = self->frameWidth_ / 2;
            if (y >= self->sliderY() - 22 && y <= self->sliderY() + 22 &&
                x >= center - kSliderHalfWidth - 12 && x <= center + kSliderHalfWidth + 12) {
                self->draggingVolume_ = true;
                SetCapture(window);
                self->setVolumeFromMouse(x);
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (self && self->screen_ == Screen::Achievements) {
            self->moveSelection(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (self && self->draggingVolume_) self->setVolumeFromMouse(GET_X_LPARAM(lParam));
        return 0;
    case WM_LBUTTONUP:
        if (self) {
            const int x = GET_X_LPARAM(lParam);
            if (self->screen_ == Screen::Volume) {
                if (self->draggingVolume_) {
                    self->draggingVolume_ = false;
                    ReleaseCapture();
                    self->setVolumeFromMouse(x);
                }
                return 0;
            }
            const int y = GET_Y_LPARAM(lParam);
            if (self->screen_ == Screen::Settings) {
                if (y >= self->menuOffset() + 31 && y < self->menuOffset() + 181) {
                    self->settingsSelection_ = std::clamp((y - (self->menuOffset() + 31)) / 50, 0, 2);
                    self->renderAndPresent();
                    self->activateSelection();
                }
                return 0;
            }
            if (self->screen_ == Screen::Confirmation) {
                if (y >= self->menuOffset() + 108 && y <= self->menuOffset() + 154) {
                    const int center = self->frameWidth_ / 2;
                    if (x >= center - 210 && x <= center - 10) self->confirmationSelection_ = 0;
                    else if (x >= center + 10 && x <= center + 210) self->confirmationSelection_ = 1;
                    else return 0;
                    self->renderAndPresent();
                    self->activateSelection();
                }
                return 0;
            }
            if (y < self->barTop()) return 0;
            RECT client{};
            GetClientRect(window, &client);
            const int center = client.right / 2;
            if (x >= center - 270 && x <= center + 270) {
                self->selectedItem_ = (x < center - 95) ? 0 : (x < center + 95 ? 1 : 2);
                self->renderAndPresent();
                self->activateSelection();
            }
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(window, &paint);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        if (self) {
            self->setGameInputBlocked(false);
            self->releaseRenderer();
            if (self->keyboardHook_) UnhookWindowsHookEx(self->keyboardHook_);
            activeInstance_ = nullptr;
            KillTimer(window, kProcessTimerId);
            UnregisterHotKey(window, kHotkeyId);
        }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

int OverlayWindow::run(HINSTANCE instance, DWORD gamePid) {
    const std::wstring mutexName = L"Local\\GameOverlayMvp.Overlay." + std::to_wstring(gamePid);
    HANDLE mutex = CreateMutexW(nullptr, TRUE, mutexName.c_str());
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 2;
    }
    if (!create(instance, gamePid)) {
        if (gameInput_) gameInput_->Release();
        if (sharedState_) UnmapViewOfFile(sharedState_);
        if (sharedMapping_) CloseHandle(sharedMapping_);
        if (gameProcess_) CloseHandle(gameProcess_);
        CloseHandle(mutex);
        return 1;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    setGameInputBlocked(false);
    if (endpointVolume_) endpointVolume_->Release();
    if (gameInput_) gameInput_->Release();
    UnmapViewOfFile(sharedState_);
    if (achievementState_) UnmapViewOfFile(achievementState_);
    if (achievementMapping_) CloseHandle(achievementMapping_);
    CloseHandle(sharedMapping_);
    CloseHandle(gameProcess_);
    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}
