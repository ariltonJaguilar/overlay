#include "SteamDiscovery.h"

#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <tlhelp32.h>

#include <chrono>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
constexpr int kManagerIconId = 101;
std::wstring lower(std::wstring value);
std::wstring normalizedGameName(const std::wstring& value);
std::filesystem::path executableDirectory();
int managerLoop(int argc, wchar_t* argv[]);

struct Blacklist {
    std::unordered_set<std::wstring> appIds;
    std::unordered_set<std::wstring> executables;
    std::unordered_set<std::wstring> gameNames;

    bool blocks(const std::optional<SteamGame>& game, const std::filesystem::path& executable,
                std::wstring& reason) const {
        const std::wstring filename = lower(executable.filename().wstring());
        if (executables.contains(filename)) {
            reason = L"executavel bloqueado: " + filename;
            return true;
        }
        if (game && appIds.contains(game->appId)) {
            reason = L"AppID Steam bloqueado: " + game->appId + L" (" + game->name + L")";
            return true;
        }
        if (game && gameNames.contains(normalizedGameName(game->name))) {
            reason = L"jogo com anti-cheat identificado: " + game->name;
            return true;
        }
        return false;
    }
};

std::wstring trim(std::wstring value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring readUtf8Text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    if (bytes.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), size);
    if (!text.empty() && text.front() == 0xfeff) text.erase(text.begin());
    return text;
}

void writeUtf8Text(const std::filesystem::path& path, const std::wstring& text, bool append) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), size, nullptr, nullptr);
    std::ofstream output(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

namespace {
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr int kListId = 100;
constexpr int kNameId = 101;
constexpr int kAddId = 102;
constexpr int kRemoveId = 103;
constexpr int kSearchId = 104;
constexpr COLORREF kBackground = RGB(20, 20, 23);
constexpr COLORREF kCard = RGB(31, 31, 35);
constexpr COLORREF kSurface = RGB(43, 43, 48);
constexpr COLORREF kText = RGB(245, 245, 247);
constexpr COLORREF kMuted = RGB(166, 166, 176);
constexpr COLORREF kAccent = RGB(96, 104, 255);
HFONT gUiFont = nullptr;
HFONT gTitleFont = nullptr;
HBRUSH gBackgroundBrush = nullptr;
HBRUSH gSurfaceBrush = nullptr;

std::filesystem::path uiBlacklistPath() { return executableDirectory() / L"overlay-blacklist.txt"; }

void refreshBlacklistList(HWND window, int topIndex = -1) {
    const HWND list = GetDlgItem(window, kListId);
    wchar_t searchBuffer[512]{};
    GetWindowTextW(GetDlgItem(window, kSearchId), searchBuffer, static_cast<int>(std::size(searchBuffer)));
    const std::wstring search = lower(trim(searchBuffer));
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    std::wistringstream input(readUtf8Text(uiBlacklistPath()));
    std::wstring line;
    while (std::getline(input, line)) {
        const std::wstring entry = trim(line);
        if (!entry.empty() && !entry.starts_with(L"#")) {
            const std::wstring display = entry.starts_with(L"game=") ? entry.substr(5) : entry;
            if (search.empty() || lower(display).find(search) != std::wstring::npos) {
                SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
            }
        }
    }
    if (topIndex >= 0) {
        const LRESULT count = SendMessageW(list, LB_GETCOUNT, 0, 0);
        if (count > 0) {
            const int restoredIndex = static_cast<int>(std::min<LRESULT>(topIndex, count - 1));
            SendMessageW(list, LB_SETTOPINDEX, restoredIndex, 0);
        }
    }
}

void appendGameToBlacklist(HWND window) {
    wchar_t buffer[512]{};
    GetWindowTextW(GetDlgItem(window, kNameId), buffer, static_cast<int>(std::size(buffer)));
    const std::wstring game = trim(buffer);
    if (game.empty()) return;
    writeUtf8Text(uiBlacklistPath(), L"game=" + game + L"\n", true);
    SetWindowTextW(GetDlgItem(window, kNameId), L"");
    refreshBlacklistList(window);
}

void removeSelectedBlacklistEntry(HWND window) {
    const HWND list = GetDlgItem(window, kListId);
    const LRESULT topIndex = SendMessageW(list, LB_GETTOPINDEX, 0, 0);
    const LRESULT selection = SendMessageW(list, LB_GETCURSEL, 0, 0);
    if (selection == LB_ERR) return;
    const LRESULT length = SendMessageW(list, LB_GETTEXTLEN, selection, 0);
    std::wstring selected(static_cast<size_t>(length), L'\0');
    SendMessageW(list, LB_GETTEXT, selection, reinterpret_cast<LPARAM>(selected.data()));

    std::wistringstream input(readUtf8Text(uiBlacklistPath()));
    std::vector<std::wstring> lines;
    std::wstring line;
    bool removed = false;
    while (std::getline(input, line)) {
        const std::wstring candidate = trim(line);
        if (!removed && (candidate == selected || candidate == L"game=" + selected)) { removed = true; continue; }
        lines.push_back(line);
    }
    std::wstring updated;
    for (const auto& savedLine : lines) updated += savedLine + L"\n";
    writeUtf8Text(uiBlacklistPath(), updated, false);
    refreshBlacklistList(window, topIndex == LB_ERR ? -1 : static_cast<int>(topIndex));
}

void addTrayIcon(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = 1;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(kManagerIconId));
    wcscpy_s(data.szTip, L"Game Overlay Manager");
    Shell_NotifyIconW(NIM_ADD, &data);
}

void removeTrayIcon(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

LRESULT CALLBACK managerWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_CREATE: {
        gBackgroundBrush = CreateSolidBrush(kBackground);
        gSurfaceBrush = CreateSolidBrush(kSurface);
        gUiFont = CreateFontW(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Text");
        gTitleFont = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Display");
        HWND list = CreateWindowW(L"LISTBOX", nullptr, WS_CHILD | WS_VISIBLE | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | WS_VSCROLL,
                      32, 144, 616, 298, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kListId)), nullptr, nullptr);
        HWND search = CreateWindowW(L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                      32, 108, 616, 28, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSearchId)), nullptr, nullptr);
        SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Buscar na lista"));
        SendMessageW(search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
        HWND edit = CreateWindowW(L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                      44, 482, 410, 24, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kNameId)), nullptr, nullptr);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Nome do jogo"));
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(2, 2));
        SetWindowTheme(list, L"DarkMode_Explorer", nullptr);
        SetWindowTheme(edit, L"DarkMode_CFD", nullptr);
        CreateWindowW(L"BUTTON", L"Adicionar", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      474, 474, 174, 40, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddId)), nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Remover", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                      32, 530, 132, 40, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRemoveId)), nullptr, nullptr);
        EnumChildWindows(window, [](HWND child, LPARAM) -> BOOL { SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(gUiFont), TRUE); return TRUE; }, 0);
        refreshBlacklistList(window);
        addTrayIcon(window);
        return 0;
    }
    case WM_MEASUREITEM:
        reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = 42;
        return TRUE;
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (draw->CtlType == ODT_LISTBOX) {
            HBRUSH brush = CreateSolidBrush((draw->itemState & ODS_SELECTED) ? RGB(55, 55, 68) : kSurface);
            FillRect(draw->hDC, &draw->rcItem, brush); DeleteObject(brush);
            if (draw->itemID != static_cast<UINT>(-1)) {
                wchar_t text[512]{}; SendMessageW(draw->hwndItem, LB_GETTEXT, draw->itemID, reinterpret_cast<LPARAM>(text));
                SetBkMode(draw->hDC, TRANSPARENT); SetTextColor(draw->hDC, kText); SelectObject(draw->hDC, gUiFont);
                RECT rect = draw->rcItem; rect.left += 18; DrawTextW(draw->hDC, text, -1, &rect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            }
            return TRUE;
        }
        if (draw->CtlType == ODT_BUTTON) {
            const bool primary = draw->CtlID == kAddId;
            FillRect(draw->hDC, &draw->rcItem, gBackgroundBrush);
            HBRUSH brush = CreateSolidBrush(primary ? kAccent : RGB(58, 58, 64));
            HPEN pen = CreatePen(PS_SOLID, 1, primary ? kAccent : RGB(75, 75, 82));
            SelectObject(draw->hDC, brush); SelectObject(draw->hDC, pen);
            RoundRect(draw->hDC, draw->rcItem.left, draw->rcItem.top, draw->rcItem.right, draw->rcItem.bottom, 12, 12);
            wchar_t text[64]{}; GetWindowTextW(draw->hwndItem, text, 64);
            SetBkMode(draw->hDC, TRANSPARENT); SetTextColor(draw->hDC, kText); SelectObject(draw->hDC, gUiFont);
            RECT rect = draw->rcItem; DrawTextW(draw->hDC, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            DeleteObject(brush); DeleteObject(pen); return TRUE;
        }
        break;
    }
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
        SetTextColor(reinterpret_cast<HDC>(wParam), kText); SetBkColor(reinterpret_cast<HDC>(wParam), kSurface);
        return reinterpret_cast<LRESULT>(gSurfaceBrush);
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); FillRect(dc, &paint.rcPaint, gBackgroundBrush);
        SetBkMode(dc, TRANSPARENT); SetTextColor(dc, kText); SelectObject(dc, gTitleFont);
        TextOutW(dc, 32, 28, L"Blacklist", 9);
        SetTextColor(dc, kMuted); SelectObject(dc, gUiFont);
        const wchar_t* subtitle = L"Jogos protegidos não recebem overlay nem injeção.";
        TextOutW(dc, 32, 72, subtitle, lstrlenW(subtitle));
        RECT card{24, 100, 656, 450}; HBRUSH cardBrush = CreateSolidBrush(kCard); HPEN cardPen = CreatePen(PS_SOLID, 1, RGB(52,52,58));
        SelectObject(dc, cardBrush); SelectObject(dc, cardPen); RoundRect(dc, card.left, card.top, card.right, card.bottom, 16, 16);
        HBRUSH inputBrush = CreateSolidBrush(kSurface); HPEN inputPen = CreatePen(PS_SOLID, 1, RGB(67,67,74));
        SelectObject(dc, inputBrush); SelectObject(dc, inputPen); RoundRect(dc, 32, 474, 466, 514, 12, 12);
        RoundRect(dc, 32, 108, 648, 136, 10, 10);
        DeleteObject(cardBrush); DeleteObject(cardPen); DeleteObject(inputBrush); DeleteObject(inputPen);
        EndPaint(window, &paint); return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == kAddId) appendGameToBlacklist(window);
        else if (LOWORD(wParam) == kRemoveId) removeSelectedBlacklistEntry(window);
        else if (LOWORD(wParam) == kSearchId && HIWORD(wParam) == EN_CHANGE) refreshBlacklistList(window);
        return 0;
    case kTrayMessage:
        if (lParam == WM_LBUTTONUP) {
            ShowWindow(window, SW_SHOW);
            SetForegroundWindow(window);
        } else if (lParam == WM_RBUTTONUP) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, 1, L"Abrir");
            AppendMenuW(menu, MF_STRING, 2, L"Sair");
            POINT point{}; GetCursorPos(&point);
            SetForegroundWindow(window);
            const UINT action = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, point.x, point.y, 0, window, nullptr);
            DestroyMenu(menu);
            if (action == 1) ShowWindow(window, SW_SHOW);
            if (action == 2) DestroyWindow(window);
        }
        return 0;
    case WM_CLOSE:
        ShowWindow(window, SW_HIDE);
        return 0;
    case WM_DESTROY:
        removeTrayIcon(window);
        DeleteObject(gUiFont); DeleteObject(gTitleFont); DeleteObject(gBackgroundBrush); DeleteObject(gSurfaceBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

int uiMessageLoop(HINSTANCE instance) {
    std::thread([] { managerLoop(__argc, __wargv); }).detach();
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    if (const HMODULE theme = GetModuleHandleW(L"uxtheme.dll")) {
        if (const auto setMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(theme, MAKEINTRESOURCEA(135)))) {
            setMode(2);
        }
    }
    const wchar_t* className = L"GameOverlayManagerWindow";
    WNDCLASSW windowClass{};
    windowClass.hInstance = instance;
    windowClass.lpszClassName = className;
    windowClass.lpfnWndProc = managerWindowProc;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kManagerIconId));
    windowClass.hbrBackground = CreateSolidBrush(kBackground);
    RegisterClassW(&windowClass);
    const HWND window = CreateWindowExW(0, className, L"Game Overlay Manager",
                                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                        CW_USEDEFAULT, CW_USEDEFAULT, 700, 640, nullptr, nullptr, instance, nullptr);
    if (!window) return 1;
    const DWORD darkMode = 1;
    const DWORD roundedCorners = 2;
    DwmSetWindowAttribute(window, 20, &darkMode, sizeof(darkMode));
    DwmSetWindowAttribute(window, 33, &roundedCorners, sizeof(roundedCorners));
    const DWORD backdrop = 2;
    DwmSetWindowAttribute(window, 38, &backdrop, sizeof(backdrop));
    ShowWindow(window, SW_HIDE);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

std::wstring lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring normalizedGameName(const std::wstring& value) {
    std::wstring result;
    for (const wchar_t character : lower(value)) {
        if (std::iswalnum(character)) result.push_back(character);
    }
    return result;
}

Blacklist loadBlacklist(const std::filesystem::path& path) {
    Blacklist blacklist;
    std::wistringstream input(readUtf8Text(path));
    if (!input) {
        std::wcerr << L"Blacklist nao encontrada: " << path << L". Nenhum jogo sera bloqueado.\n";
        return blacklist;
    }

    std::wstring line;
    while (std::getline(input, line)) {
        line = trim(line);
        if (line.empty() || line.starts_with(L"#")) continue;
        const size_t separator = line.find(L'=');
        if (separator == std::wstring::npos) {
            std::wcerr << L"Entrada de blacklist ignorada: " << line << L"\n";
            continue;
        }
        const std::wstring type = lower(trim(line.substr(0, separator)));
        const std::wstring value = trim(line.substr(separator + 1));
        if (value.empty()) continue;
        if (type == L"appid") blacklist.appIds.insert(value);
        else if (type == L"exe") blacklist.executables.insert(lower(std::filesystem::path(value).filename().wstring()));
        else if (type == L"game") blacklist.gameNames.insert(normalizedGameName(value));
        else std::wcerr << L"Tipo de blacklist desconhecido: " << type << L"\n";
    }
    return blacklist;
}

std::optional<std::wstring> configuredProcess(int argc, wchar_t* argv[]) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], L"--process") == 0) {
            return std::filesystem::path(argv[i + 1]).filename().wstring();
        }
    }
    return std::nullopt;
}

std::unordered_set<DWORD> findProcesses(const std::wstring& executableName) {
    std::unordered_set<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, executableName.c_str()) == 0) result.insert(entry.th32ProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::optional<std::filesystem::path> processExecutable(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return std::nullopt;
    std::vector<wchar_t> buffer(32768);
    DWORD size = static_cast<DWORD>(buffer.size());
    const BOOL success = QueryFullProcessImageNameW(process, 0, buffer.data(), &size);
    CloseHandle(process);
    if (!success) return std::nullopt;
    return std::filesystem::path(std::wstring(buffer.data(), size));
}

DWORD foregroundProcessId() {
    DWORD pid = 0;
    if (const HWND window = GetForegroundWindow()) GetWindowThreadProcessId(window, &pid);
    return pid;
}

std::filesystem::path executableDirectory() {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
}

HANDLE launchOverlay(const std::filesystem::path& overlayPath, DWORD gamePid) {
    std::wstring commandLine = L"\"" + overlayPath.wstring() + L"\" --pid " + std::to_wstring(gamePid);
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(overlayPath.c_str(), mutableCommand.data(), nullptr, nullptr,
                                        FALSE, 0, nullptr, overlayPath.parent_path().c_str(),
                                        &startup, &process);
    if (!created) {
        std::wcerr << L"Falha ao iniciar Overlay.exe para o PID " << gamePid
                   << L" (erro " << GetLastError() << L").\n";
        return nullptr;
    }
    CloseHandle(process.hThread);
    return process.hProcess;
}

bool injectInputHook64(const std::filesystem::path& dllPath, DWORD gamePid) {
    HANDLE game = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, gamePid);
    if (!game) {
        std::wcerr << L"Nao foi possivel abrir o jogo para injecao (erro " << GetLastError() << L").\n";
        return false;
    }

    const std::wstring path = dllPath.wstring();
    const SIZE_T byteCount = (path.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(game, nullptr, byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath || !WriteProcessMemory(game, remotePath, path.c_str(), byteCount, nullptr)) {
        if (remotePath) VirtualFreeEx(game, remotePath, 0, MEM_RELEASE);
        CloseHandle(game);
        return false;
    }

    const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(kernel32, "LoadLibraryW"));
    HANDLE thread = loadLibrary
        ? CreateRemoteThread(game, nullptr, 0, loadLibrary, remotePath, 0, nullptr)
        : nullptr;
    bool loaded = false;
    if (thread) {
        WaitForSingleObject(thread, 5000);
        DWORD moduleResult = 0;
        GetExitCodeThread(thread, &moduleResult);
        loaded = moduleResult != 0;
        CloseHandle(thread);
    }

    VirtualFreeEx(game, remotePath, 0, MEM_RELEASE);
    CloseHandle(game);
    if (!loaded) {
        std::wcerr << L"Falha ao carregar OverlayInputHook.dll no PID " << gamePid << L".\n";
    }
    return loaded;
}

bool is32BitProcess(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;

    using IsWow64Process2Fn = BOOL(WINAPI*)(HANDLE, USHORT*, USHORT*);
    const auto isWow64Process2 = reinterpret_cast<IsWow64Process2Fn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "IsWow64Process2"));
    bool result = false;
    if (isWow64Process2) {
        USHORT processMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        USHORT nativeMachine = IMAGE_FILE_MACHINE_UNKNOWN;
        result = isWow64Process2(process, &processMachine, &nativeMachine) &&
                 processMachine == IMAGE_FILE_MACHINE_I386;
    } else {
        BOOL wow64 = FALSE;
        result = IsWow64Process(process, &wow64) && wow64;
    }
    CloseHandle(process);
    return result;
}

bool injectInputHook32(const std::filesystem::path& injectorPath,
                       const std::filesystem::path& dllPath, DWORD gamePid) {
    std::wstring commandLine = L"\"" + injectorPath.wstring() + L"\" --pid " +
                               std::to_wstring(gamePid) + L" --dll \"" + dllPath.wstring() + L"\"";
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(injectorPath.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0,
                        nullptr, injectorPath.parent_path().c_str(), &startup, &process)) {
        std::wcerr << L"Nao foi possivel iniciar o injetor 32-bit (erro " << GetLastError() << L").\n";
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 7000);
    DWORD exitCode = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hProcess);
    return wait == WAIT_OBJECT_0 && exitCode == 0;
}
int managerLoop(int argc, wchar_t* argv[]) {
    HANDLE managerMutex = CreateMutexW(nullptr, TRUE, L"Local\\GameOverlayMvp.Manager");
    if (!managerMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (managerMutex) CloseHandle(managerMutex);
        std::wcerr << L"O GameOverlayManager ja esta em execucao.\n";
        return 1;
    }

    const auto manualTarget = configuredProcess(argc, argv);
    const std::filesystem::path overlayPath = executableDirectory() / L"Overlay.exe";
    const std::filesystem::path hook64Path = executableDirectory() / L"OverlayInputHook.dll";
    const std::filesystem::path hook32Path = executableDirectory() / L"OverlayInputHook32.dll";
    const std::filesystem::path injector32Path = executableDirectory() / L"GameOverlayInjector32.exe";
    const std::filesystem::path blacklistPath = executableDirectory() / L"overlay-blacklist.txt";
    Blacklist blacklist = loadBlacklist(blacklistPath);
    std::error_code blacklistTimeError;
    auto blacklistWriteTime = std::filesystem::last_write_time(blacklistPath, blacklistTimeError);
    if (!std::filesystem::exists(overlayPath)) {
        std::wcerr << L"Overlay.exe nao foi encontrado ao lado do manager: " << overlayPath << L"\n";
        CloseHandle(managerMutex);
        return 1;
    }
    if (!std::filesystem::exists(hook64Path)) {
        std::wcerr << L"OverlayInputHook.dll nao foi encontrada ao lado do manager.\n";
        CloseHandle(managerMutex);
        return 1;
    }

    SteamDiscovery steam;
    if (manualTarget) {
        std::wcout << L"Modo manual: aguardando " << *manualTarget << L"...\n";
    } else if (!steam.refresh()) {
        std::wcerr << L"Falha na deteccao Steam: " << steam.lastError() << L"\n";
        CloseHandle(managerMutex);
        return 1;
    } else {
        std::wcout << L"Modo Steam: " << steam.games().size()
                   << L" instalacoes encontradas. Aguardando um jogo em primeiro plano...\n";
    }

    std::unordered_map<DWORD, HANDLE> overlays;
    auto nextSteamRefresh = std::chrono::steady_clock::now() + std::chrono::minutes(1);

    for (;;) {
        std::error_code currentBlacklistTimeError;
        const auto currentBlacklistWriteTime =
            std::filesystem::last_write_time(blacklistPath, currentBlacklistTimeError);
        if (!currentBlacklistTimeError &&
            (blacklistTimeError || currentBlacklistWriteTime != blacklistWriteTime)) {
            blacklist = loadBlacklist(blacklistPath);
            blacklistWriteTime = currentBlacklistWriteTime;
            blacklistTimeError.clear();
            std::wcout << L"Blacklist atualizada.\n";
        }
        for (auto it = overlays.begin(); it != overlays.end();) {
            if (WaitForSingleObject(it->second, 0) == WAIT_OBJECT_0) {
                CloseHandle(it->second);
                it = overlays.erase(it);
            } else {
                ++it;
            }
        }

        std::unordered_set<DWORD> candidates;
        if (manualTarget) {
            candidates = findProcesses(*manualTarget);
        } else {
            if (std::chrono::steady_clock::now() >= nextSteamRefresh) {
                steam.refresh();
                nextSteamRefresh = std::chrono::steady_clock::now() + std::chrono::minutes(1);
            }
            const DWORD pid = foregroundProcessId();
            if (pid != 0 && !overlays.contains(pid)) {
                if (const auto executable = processExecutable(pid)) {
                    if (const auto game = steam.gameForExecutable(*executable)) {
                        candidates.insert(pid);
                        std::wcout << L"Jogo Steam detectado: " << game->name
                                   << L" (AppID " << game->appId << L", PID " << pid << L").\n";
                    }
                }
            }
        }

        for (const DWORD pid : candidates) {
            if (!overlays.contains(pid)) {
                const auto executable = processExecutable(pid);
                const auto game = (!manualTarget && executable) ? steam.gameForExecutable(*executable)
                                                                 : std::nullopt;
                std::wstring blockReason;
                if (executable && blacklist.blocks(game, *executable, blockReason)) {
                    std::wcout << L"Overlay nao ativado para o PID " << pid << L": "
                               << blockReason << L".\n";
                    continue;
                }
                if (HANDLE process = launchOverlay(overlayPath, pid)) {
                    overlays.emplace(pid, process);
                    std::wcout << L"Overlay iniciado para o PID " << pid << L".\n";
                    std::wcout << L"Aguardando estabilizacao do jogo antes de ativar o hook...\n";
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    const bool gameIs32Bit = is32BitProcess(pid);
                    if (gameIs32Bit && (!std::filesystem::exists(hook32Path) ||
                                        !std::filesystem::exists(injector32Path))) {
                        std::wcerr << L"Componentes 32-bit ausentes. Execute build-all.ps1 para gera-los.\n";
                    } else if ((gameIs32Bit && injectInputHook32(injector32Path, hook32Path, pid)) ||
                               (!gameIs32Bit && injectInputHook64(hook64Path, pid))) {
                        std::wcout << L"Hook de XInput injetado no PID " << pid << L".\n";
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    return uiMessageLoop(instance);
}
