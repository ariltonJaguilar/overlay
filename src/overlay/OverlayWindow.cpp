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

namespace {
constexpr wchar_t kWindowClass[] = L"GameOverlayMvp.Window";
constexpr int kOverlayWidth = 620;
constexpr int kBarHeight = 280;
constexpr int kOverlayHeight = 480;
constexpr int kBarTop = kOverlayHeight - kBarHeight;
constexpr BYTE kGradientBottomOpacity = 235;
constexpr BYTE kPanelRed = 24;
constexpr BYTE kPanelGreen = 24;
constexpr BYTE kPanelBlue = 28;
constexpr int kVolumePanelWidth = 620;
constexpr int kVolumePanelTop = 18;
constexpr int kVolumePanelBottom = 194;
constexpr int kVolumePanelRadius = 28;
constexpr int kSliderHalfWidth = 210;
constexpr int kSliderY = 112;

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
    loadIcons();
    GameInputCreate(&gameInput_);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = windowProc;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    RegisterClassExW(&windowClass);

    const DWORD extendedStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    window_ = CreateWindowExW(extendedStyle, kWindowClass, L"Game Overlay MVP", WS_POPUP,
                              0, 0, kOverlayWidth, kOverlayHeight, nullptr, nullptr, instance, this);
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

void OverlayWindow::centerOnGameMonitor() const {
    WindowSearch search{gamePid_, nullptr};
    EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&search));
    HMONITOR monitor = search.result ? MonitorFromWindow(search.result, MONITOR_DEFAULTTONEAREST)
                                     : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    GetMonitorInfoW(monitor, &info);
    const int width = info.rcMonitor.right - info.rcMonitor.left;
    const int y = info.rcMonitor.bottom - kOverlayHeight;
    SetWindowPos(window_, HWND_TOPMOST, info.rcMonitor.left, y, width, kOverlayHeight, SWP_NOACTIVATE);
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
    if (screen_ == Screen::Confirmation) {
        confirmationSelection_ = (confirmationSelection_ + direction + 2) % 2;
        renderAndPresent();
        return;
    }
    selectedItem_ = (selectedItem_ + direction + 3) % 3;
    renderAndPresent();
}

void OverlayWindow::activateSelection() {
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
    } else if (screen_ == Screen::Volume || screen_ == Screen::Settings) {
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
            LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &privileges.Privileges[0].Luid);
            privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr);
            CloseHandle(token);
            ExitWindowsEx(EWX_POWEROFF | EWX_FORCEIFHUNG,
                          SHTDN_REASON_MAJOR_APPLICATION | SHTDN_REASON_FLAG_PLANNED);
        }
    } else {
        WindowSearch search{gamePid_, nullptr};
        EnumWindows(findGameWindow, reinterpret_cast<LPARAM>(&search));
        if (search.result) PostMessageW(search.result, WM_CLOSE, 0, 0);
    }
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
        InterlockedCompareExchange(&sharedState_->controllerConnected, 0, 0) != FALSE) {
        const WORD buttons = static_cast<WORD>(
            InterlockedCompareExchange(&sharedState_->controllerButtons, 0, 0));
        const SHORT thumbLX = static_cast<SHORT>(
            InterlockedCompareExchange(&sharedState_->controllerThumbLX, 0, 0));
        const SHORT thumbLY = static_cast<SHORT>(
            InterlockedCompareExchange(&sharedState_->controllerThumbLY, 0, 0));
        const WORD pressed = static_cast<WORD>(buttons & ~previousSharedControllerButtons_);
        int direction = 0;
        if (screen_ == Screen::Settings) {
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
                if (screen_ == Screen::Settings) {
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
        if (screen_ == Screen::Settings) {
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
    if (screen_ == Screen::Settings || screen_ == Screen::Confirmation) {
        HFONT font = CreateFontW(28, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        HGDIOBJ oldFont = SelectObject(dc, font);
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetTextColor(dc, RGB(245, 246, 250));

        if (screen_ == Screen::Settings) {
            const wchar_t* labels[] = {L"Suspender", L"Desligar", L"Fechar jogo"};
            for (int index = 0; index < 3; ++index) {
                RECT row{width / 2 - 245, 31 + index * 50, width / 2 + 245, 73 + index * 50};
                if (index == settingsSelection_) {
                    HBRUSH selected = CreateSolidBrush(RGB(92, 95, 104));
                    SelectObject(dc, selected);
                    if (index == 0) fillRoundedRectangleCorners(dc, row, 23, 23, 7, 7);
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
            RECT question{width / 2 - 205, 39, width / 2 + 205, 91};
            DrawTextW(dc, actions[settingsSelection_], -1, &question,
                      DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            const wchar_t* labels[] = {L"Cancelar", L"Confirmar"};
            for (int index = 0; index < 2; ++index) {
                RECT button{width / 2 - 210 + index * 220, 108,
                            width / 2 - 10 + index * 220, 154};
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
    const int centers[] = {width / 2 - 190, width / 2, width / 2 + 190};

    for (int i = 0; i < 3; ++i) {
        const COLORREF color = RGB(245, 246, 250);
        if (icons_[i].pixels.empty()) {
            if (i == 0) drawAchievementIcon(dc, centers[i], kBarTop + 190, color);
            if (i == 1) drawVolumeIcon(dc, centers[i], kBarTop + 190, color);
            if (i == 2) drawSettingsIcon(dc, centers[i], kBarTop + 190, color);
        }
    }
}

bool OverlayWindow::loadIcons() {
    wchar_t executablePath[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, executablePath,
                                            static_cast<DWORD>(std::size(executablePath)));
    const auto directory = std::filesystem::path(std::wstring(executablePath, length)).parent_path();
    const wchar_t* filenames[] = {L"trophy.png", L"volume.png", L"settings.png"};
    bool allLoaded = true;

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) return false;

    for (int index = 0; index < 3; ++index) {
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICBitmapScaler* scaler = nullptr;
        IWICFormatConverter* converter = nullptr;
        const auto path = directory / L"assets" / L"icons" / filenames[index];
        HRESULT result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
        UINT sourceWidth = 0, sourceHeight = 0;
        if (SUCCEEDED(result)) result = frame->GetSize(&sourceWidth, &sourceHeight);
        UINT width = 1, height = 1;
        if (SUCCEEDED(result) && sourceWidth && sourceHeight) {
            const double scale = (std::min)(60.0 / sourceWidth, 60.0 / sourceHeight);
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
            icons_[index].width = static_cast<int>(width);
            icons_[index].height = static_cast<int>(height);
            icons_[index].pixels.resize(static_cast<size_t>(width) * height);
            result = converter->CopyPixels(nullptr, width * 4,
                                           static_cast<UINT>(icons_[index].pixels.size() * 4),
                                           reinterpret_cast<BYTE*>(icons_[index].pixels.data()));
        }
        if (FAILED(result)) {
            icons_[index] = {};
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
        if (top + y < 0 || top + y >= kOverlayHeight) continue;
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
        if (x < 0 || x >= destinationWidth || y < 0 || y >= kOverlayHeight) return;
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
        const int minY = static_cast<int>(std::floor(kSliderY - radius - 1));
        const int maxY = static_cast<int>(std::ceil(kSliderY + radius + 1));
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float nearestX = std::clamp(x + 0.5f, from, to);
                const float dx = x + 0.5f - nearestX;
                const float dy = y + 0.5f - kSliderY;
                const float coverage = std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                blendPixel(x, y, gray, coverage);
            }
        }
    };

    auto circle = [&](float radius, BYTE gray, BYTE opacity) {
        const int minX = static_cast<int>(std::floor(knobX - radius - 1));
        const int maxX = static_cast<int>(std::ceil(knobX + radius + 1));
        const int minY = static_cast<int>(std::floor(kSliderY - radius - 1));
        const int maxY = static_cast<int>(std::ceil(kSliderY + radius + 1));
        for (int y = minY; y <= maxY; ++y) {
            for (int x = minX; x <= maxX; ++x) {
                const float dx = x + 0.5f - knobX;
                const float dy = y + 0.5f - kSliderY;
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
    info.bmiHeader.biHeight = -kOverlayHeight;
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
    BitBlt(frameDc_, 0, 0, width, kOverlayHeight, screen, windowRect.left, windowRect.top,
           SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);

    frameWidth_ = width;
    const auto* captured = static_cast<const std::uint32_t*>(framePixels_);
    blurredBackground_.assign(captured, captured + static_cast<size_t>(width) * kOverlayHeight);
    // Quatro passagens formam uma aproximação gaussiana mais forte e removem
    // artefatos de alta frequência da captura do jogo.
    for (int pass = 0; pass < 4; ++pass) {
        boxBlur(blurredBackground_, width, kOverlayHeight, 18);
    }
    return true;
}

void OverlayWindow::renderAndPresent() {
    if (!framePixels_ || blurredBackground_.empty() || frameWidth_ <= 0) return;
    auto* output = static_cast<std::uint32_t*>(framePixels_);
    if (screen_ == Screen::Volume) initializeSystemVolume();
    const int panelWidth = screen_ == Screen::Settings ? 500 :
                           screen_ == Screen::Confirmation ? 430 : kVolumePanelWidth;
    const int panelTop = screen_ == Screen::Settings ? 26 :
                         screen_ == Screen::Confirmation ? 34 : kVolumePanelTop;
    const int panelBottom = screen_ == Screen::Settings ? 178 :
                            screen_ == Screen::Confirmation ? 159 : kVolumePanelBottom;
    const int panelLeft = (frameWidth_ - panelWidth) / 2;
    const int panelRight = panelLeft + panelWidth;
    for (int y = 0; y < kOverlayHeight; ++y) {
        const double progress = y >= kBarTop
            ? static_cast<double>(y - kBarTop) / (kBarHeight - 1) : 0.0;
        const double fadeProgress = y >= kBarTop ? (std::min)(1.0, progress * 2.0) : 0.0;
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
                                         panelBottom, kVolumePanelRadius) > 0.0f) {
                constexpr BYTE panelAlpha = 232;
                constexpr double panelSourceWeight = 0.62;
                const float coverage = roundedRectangleCoverage(
                    x, y, panelLeft, panelTop, panelRight, panelBottom, kVolumePanelRadius);
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

    BITMAPINFO uiInfo{};
    constexpr int uiScale = 3;
    uiInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    uiInfo.bmiHeader.biWidth = frameWidth_ * uiScale;
    uiInfo.bmiHeader.biHeight = -kOverlayHeight * uiScale;
    uiInfo.bmiHeader.biPlanes = 1;
    uiInfo.bmiHeader.biBitCount = 32;
    uiInfo.bmiHeader.biCompression = BI_RGB;
    void* uiPixelsRaw = nullptr;
    HBITMAP uiBitmap = CreateDIBSection(frameDc_, &uiInfo, DIB_RGB_COLORS, &uiPixelsRaw, nullptr, 0);
    HDC uiDc = CreateCompatibleDC(frameDc_);
    HGDIOBJ oldUiBitmap = SelectObject(uiDc, uiBitmap);
    ZeroMemory(uiPixelsRaw, static_cast<size_t>(frameWidth_ * uiScale) *
                              (kOverlayHeight * uiScale) * sizeof(std::uint32_t));
    SetGraphicsMode(uiDc, GM_ADVANCED);
    XFORM transform{static_cast<FLOAT>(uiScale), 0.0f, 0.0f,
                    static_cast<FLOAT>(uiScale), 0.0f, 0.0f};
    SetWorldTransform(uiDc, &transform);
    draw(uiDc);

    const auto* uiPixels = static_cast<const std::uint32_t*>(uiPixelsRaw);
    const int uiWidth = frameWidth_ * uiScale;
    for (int y = 0; y < kOverlayHeight; ++y) {
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

    const int iconCenters[] = {frameWidth_ / 2 - 190, frameWidth_ / 2, frameWidth_ / 2 + 190};
    for (int index = 0; index < 3; ++index) {
        blendIcon(output, frameWidth_, icons_[index], iconCenters[index], kBarTop + 190,
                  index == selectedItem_ ? 255 : 82);
    }
    SelectObject(uiDc, oldUiBitmap);
    DeleteDC(uiDc);
    DeleteObject(uiBitmap);

    RECT windowRect{};
    GetWindowRect(window_, &windowRect);
    POINT destination{windowRect.left, windowRect.top};
    SIZE size{frameWidth_, kOverlayHeight};
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
        if (point.y >= kBarTop) return HTCLIENT;
        if (self->screen_ != Screen::MainBar) {
            const int panelWidth = self->screen_ == Screen::Settings ? 500 :
                                   self->screen_ == Screen::Confirmation ? 430 : kVolumePanelWidth;
            const int panelTop = self->screen_ == Screen::Settings ? 26 :
                                 self->screen_ == Screen::Confirmation ? 34 : kVolumePanelTop;
            const int panelBottom = self->screen_ == Screen::Settings ? 178 :
                                    self->screen_ == Screen::Confirmation ? 159 : kVolumePanelBottom;
            const int left = (self->frameWidth_ - panelWidth) / 2;
            if (insideRoundedRectangle(point.x, point.y, left, panelTop,
                                       left + panelWidth, panelBottom,
                                       kVolumePanelRadius)) return HTCLIENT;
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
                DestroyWindow(window);
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
        else if (wParam == VK_UP && self->screen_ == Screen::Settings) self->moveSelection(-1);
        else if (wParam == VK_DOWN && self->screen_ == Screen::Settings) self->moveSelection(1);
        else if (wParam == VK_RETURN || wParam == VK_SPACE) self->activateSelection();
        else if (wParam == VK_ESCAPE) self->goBack();
        return 0;
    case WM_LBUTTONDOWN:
        if (self && self->screen_ == Screen::Volume) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            const int center = self->frameWidth_ / 2;
            if (y >= kSliderY - 22 && y <= kSliderY + 22 &&
                x >= center - kSliderHalfWidth - 12 && x <= center + kSliderHalfWidth + 12) {
                self->draggingVolume_ = true;
                SetCapture(window);
                self->setVolumeFromMouse(x);
            }
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
                if (y >= 31 && y < 181) {
                    self->settingsSelection_ = std::clamp((y - 31) / 50, 0, 2);
                    self->renderAndPresent();
                    self->activateSelection();
                }
                return 0;
            }
            if (self->screen_ == Screen::Confirmation) {
                if (y >= 108 && y <= 154) {
                    const int center = self->frameWidth_ / 2;
                    if (x >= center - 210 && x <= center - 10) self->confirmationSelection_ = 0;
                    else if (x >= center + 10 && x <= center + 210) self->confirmationSelection_ = 1;
                    else return 0;
                    self->renderAndPresent();
                    self->activateSelection();
                }
                return 0;
            }
            if (y < kBarTop) return 0;
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
    CloseHandle(sharedMapping_);
    CloseHandle(gameProcess_);
    CloseHandle(mutex);
    return static_cast<int>(message.wParam);
}
