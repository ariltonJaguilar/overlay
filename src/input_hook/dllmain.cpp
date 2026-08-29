#include "InputShared.h"
#include "AchievementShared.h"

#include <MinHook.h>
#include <GameInput.h>
#include <mmsystem.h>
#include <windows.h>
#include <hidsdi.h>
#include <roapi.h>
#include <windows.gaming.input.h>
#include <xinput.h>

#include <array>
#include <algorithm>
#include <cstddef>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputGetKeystrokeFn = DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE);

constexpr size_t kMaximumHooks = 16;
std::array<XInputGetStateFn, kMaximumHooks> g_originalFunctions{};
std::array<void*, kMaximumHooks> g_hookedTargets{};
size_t g_hookCount = 0;
std::array<XInputGetKeystrokeFn, kMaximumHooks> g_originalKeystrokeFunctions{};
std::array<void*, kMaximumHooks> g_hookedKeystrokeTargets{};
size_t g_keystrokeHookCount = 0;
HANDLE g_sharedMapping = nullptr;
InputSharedState* g_sharedState = nullptr;
bool g_toggleChordHeld = false;
using GameInputGetGamepadStateFn = bool(STDMETHODCALLTYPE*)(
    IGameInputReading*, GameInputGamepadState*);
GameInputGetGamepadStateFn g_originalGameInputGetGamepadState = nullptr;
void* g_gameInputGetGamepadStateTarget = nullptr;
HMODULE g_gameInputModule = nullptr;
using GetRawInputDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using GetRawInputBufferFn = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
using JoyGetPosFn = MMRESULT(WINAPI*)(UINT, LPJOYINFO);
using JoyGetPosExFn = MMRESULT(WINAPI*)(UINT, LPJOYINFOEX);
GetRawInputDataFn g_originalGetRawInputData = nullptr;
GetRawInputBufferFn g_originalGetRawInputBuffer = nullptr;
JoyGetPosFn g_originalJoyGetPos = nullptr;
JoyGetPosExFn g_originalJoyGetPosEx = nullptr;
bool g_rawInputHooksInstalled = false;
bool g_winmmHooksInstalled = false;
using HidPGetDataFn = NTSTATUS(WINAPI*)(HIDP_REPORT_TYPE, PHIDP_DATA, PULONG,
                                       PHIDP_PREPARSED_DATA, PCHAR, ULONG);
using ReadFileFn = BOOL(WINAPI*)(HANDLE, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
HidPGetDataFn g_originalHidPGetData = nullptr;
ReadFileFn g_originalReadFile = nullptr;
bool g_directHidHooksInstalled = false;
using WgiGamepad = ABI::Windows::Gaming::Input::IGamepad;
using WgiGamepadReading = ABI::Windows::Gaming::Input::GamepadReading;
using WgiGetCurrentReadingFn = HRESULT(STDMETHODCALLTYPE*)(WgiGamepad*, WgiGamepadReading*);
WgiGetCurrentReadingFn g_originalWgiGetCurrentReading = nullptr;
void* g_wgiGetCurrentReadingTarget = nullptr;

std::wstring registryText(HKEY root, const wchar_t* name, DWORD flags = 0) {
    wchar_t value[32768]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(root, L"Software\\Valve\\Steam", name,
                     RRF_RT_REG_SZ | flags, nullptr, value, &size) == ERROR_SUCCESS) return value;
    return {};
}

std::vector<unsigned char> steamAchievementSchema(unsigned int appId) {
    std::wstring steamPath = registryText(HKEY_CURRENT_USER, L"SteamPath");
    if (steamPath.empty()) steamPath = registryText(HKEY_LOCAL_MACHINE, L"InstallPath", RRF_SUBKEY_WOW6432KEY);
    if (steamPath.empty()) return {};
    const auto path = std::filesystem::path(steamPath) / L"appcache" / L"stats" /
        (L"UserGameStatsSchema_" + std::to_wstring(appId) + L".bin");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return {};
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

std::string localizedSchemaValue(const std::vector<unsigned char>& schema,
                                 const char* englishValue, const std::string& language,
                                 const char* boundaryKey) {
    if (schema.empty() || !englishValue || !*englishValue || language.empty()) return {};
    const std::string englishMarker = std::string("english\0", 8) + englishValue + '\0';
    auto english = std::search(schema.begin(), schema.end(), englishMarker.begin(), englishMarker.end());
    if (english == schema.end()) return {};
    const auto searchStart = english + static_cast<std::ptrdiff_t>(englishMarker.size());
    const std::string boundary = std::string(boundaryKey) + '\0';
    auto searchEnd = std::search(searchStart, schema.end(), boundary.begin(), boundary.end());
    if (searchEnd == schema.end()) searchEnd = schema.end();
    const std::string languageMarker = language + '\0';
    auto localized = std::search(searchStart, searchEnd, languageMarker.begin(), languageMarker.end());
    if (localized == searchEnd) return {};
    localized += static_cast<std::ptrdiff_t>(languageMarker.size());
    auto end = std::find(localized, searchEnd, static_cast<unsigned char>(0));
    return {localized, end};
}

bool schemaProgress(const std::vector<unsigned char>& schema, const char* apiName,
                    std::string& statName, LONG& maximum) {
    if (schema.empty() || !apiName || !*apiName) return false;
    const std::string achievementMarker = std::string("name\0", 5) + apiName + '\0';
    auto achievement = std::search(schema.begin(), schema.end(),
                                   achievementMarker.begin(), achievementMarker.end());
    if (achievement == schema.end()) return false;
    auto limit = achievement + (std::min)(static_cast<size_t>(schema.end() - achievement), size_t{6000});
    const std::string progressMarker("progress\0", 9);
    auto progress = std::search(achievement, limit, progressMarker.begin(), progressMarker.end());
    if (progress == limit) return false;
    const std::string maximumMarker("max_val\0", 8);
    auto maxPosition = std::search(progress, limit, maximumMarker.begin(), maximumMarker.end());
    const std::string statMarker("operand1\0", 9);
    auto statPosition = std::search(progress, limit, statMarker.begin(), statMarker.end());
    if (maxPosition == limit || statPosition == limit) return false;
    maxPosition += static_cast<std::ptrdiff_t>(maximumMarker.size());
    if (limit - maxPosition < 4) return false;
    unsigned int rawMaximum = 0;
    std::memcpy(&rawMaximum, &*maxPosition, sizeof(rawMaximum));
    statPosition += static_cast<std::ptrdiff_t>(statMarker.size());
    auto statEnd = std::find(statPosition, limit, static_cast<unsigned char>(0));
    if (statEnd == limit || statEnd == statPosition || rawMaximum == 0) return false;
    statName.assign(statPosition, statEnd);
    maximum = static_cast<LONG>((std::min)(rawMaximum, static_cast<unsigned int>(LONG_MAX)));
    return true;
}

bool openSharedState() {
    if (g_sharedState) return true;
    const std::wstring name = inputSharedMemoryName(GetCurrentProcessId());
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
    if (!mapping) return false;
    auto* state = static_cast<InputSharedState*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(InputSharedState)));
    if (!state) {
        CloseHandle(mapping);
        return false;
    }
    g_sharedMapping = mapping;
    g_sharedState = state;
    return true;
}

bool inputMustBeBlocked() {
    return openSharedState() && g_sharedState->version == kInputSharedVersion &&
           InterlockedCompareExchange(&g_sharedState->blockGameInput, 0, 0) != 0;
}

bool STDMETHODCALLTYPE hookedGameInputGetGamepadState(
    IGameInputReading* reading, GameInputGamepadState* state) {
    const bool result = g_originalGameInputGetGamepadState &&
                        g_originalGameInputGetGamepadState(reading, state);
    if (result && state && inputMustBeBlocked()) {
        ZeroMemory(state, sizeof(*state));
    }
    return result;
}

void neutralizeRawInput(RAWINPUT* input) {
    if (!input || input->header.dwType != RIM_TYPEHID) return;
    const size_t byteCount = static_cast<size_t>(input->data.hid.dwSizeHid) *
                             input->data.hid.dwCount;
    if (byteCount) ZeroMemory(input->data.hid.bRawData, byteCount);
}

UINT WINAPI hookedGetRawInputData(HRAWINPUT input, UINT command, LPVOID data,
                                  PUINT size, UINT headerSize) {
    const UINT result = g_originalGetRawInputData
        ? g_originalGetRawInputData(input, command, data, size, headerSize)
        : static_cast<UINT>(-1);
    if (result != static_cast<UINT>(-1) && command == RID_INPUT && data &&
        result >= sizeof(RAWINPUTHEADER) && inputMustBeBlocked()) {
        if (openSharedState()) InterlockedIncrement(&g_sharedState->rawInputCallCount);
        auto* rawInput = static_cast<RAWINPUT*>(data);
        if (rawInput->header.dwType == RIM_TYPEHID) {
            // Nao entregue o relatorio ao backend da Unity. Zerar os bytes nao
            // basta: remove tambem o report ID e pode fazer a engine conservar
            // o ultimo estado valido do controle.
            neutralizeRawInput(rawInput);
            return 0;
        }
    }
    return result;
}

NTSTATUS WINAPI hookedHidPGetData(HIDP_REPORT_TYPE reportType, PHIDP_DATA data,
                                  PULONG dataLength, PHIDP_PREPARSED_DATA preparsedData,
                                  PCHAR report, ULONG reportLength) {
    const NTSTATUS result = g_originalHidPGetData
        ? g_originalHidPGetData(reportType, data, dataLength, preparsedData, report, reportLength)
        : static_cast<NTSTATUS>(0xC0110001L);
    if (reportType == HidP_Input && inputMustBeBlocked()) {
        if (openSharedState()) InterlockedIncrement(&g_sharedState->hidParserCallCount);
        if (dataLength) *dataLength = 0;
    }
    return result;
}

BOOL WINAPI hookedReadFile(HANDLE file, LPVOID buffer, DWORD bytesToRead,
                           LPDWORD bytesRead, LPOVERLAPPED overlapped) {
    if (inputMustBeBlocked() && GetFileType(file) == FILE_TYPE_CHAR) {
        if (openSharedState()) InterlockedIncrement(&g_sharedState->hidReadCallCount);
        if (bytesRead) *bytesRead = 0;
        if (overlapped) {
            overlapped->Internal = 0;
            overlapped->InternalHigh = 0;
        }
        return TRUE;
    }
    return g_originalReadFile
        ? g_originalReadFile(file, buffer, bytesToRead, bytesRead, overlapped)
        : FALSE;
}

HRESULT STDMETHODCALLTYPE hookedWgiGetCurrentReading(WgiGamepad* gamepad,
                                                     WgiGamepadReading* reading) {
    const HRESULT result = g_originalWgiGetCurrentReading
        ? g_originalWgiGetCurrentReading(gamepad, reading) : E_FAIL;
    if (SUCCEEDED(result) && reading && inputMustBeBlocked()) {
        const UINT64 timestamp = reading->Timestamp;
        ZeroMemory(reading, sizeof(*reading));
        reading->Timestamp = timestamp;
    }
    return result;
}

UINT WINAPI hookedGetRawInputBuffer(PRAWINPUT data, PUINT size, UINT headerSize) {
    const UINT result = g_originalGetRawInputBuffer
        ? g_originalGetRawInputBuffer(data, size, headerSize)
        : static_cast<UINT>(-1);
    if (result == static_cast<UINT>(-1) || !data || !result || !inputMustBeBlocked()) {
        return result;
    }
    RAWINPUT* current = data;
    bool containsHid = false;
    for (UINT index = 0; index < result; ++index) {
        containsHid = containsHid || current->header.dwType == RIM_TYPEHID;
        neutralizeRawInput(current);
        const auto next = reinterpret_cast<ULONG_PTR>(current) + current->header.dwSize;
        current = reinterpret_cast<RAWINPUT*>(
            (next + sizeof(void*) - 1) & ~(static_cast<ULONG_PTR>(sizeof(void*) - 1)));
    }
    // O buffer pode misturar varios relatorios. Durante o overlay, descarte o
    // lote que contenha HID para impedir que a Unity atualize seu estado por
    // uma segunda rota depois de XInput ter sido neutralizado.
    return containsHid ? 0 : result;
}

MMRESULT WINAPI hookedJoyGetPos(UINT id, LPJOYINFO info) {
    const MMRESULT result = g_originalJoyGetPos ? g_originalJoyGetPos(id, info) : MMSYSERR_ERROR;
    if (result == JOYERR_NOERROR && info && inputMustBeBlocked()) {
        info->wXpos = info->wYpos = info->wZpos = 0x7fff;
        info->wButtons = 0;
    }
    return result;
}

MMRESULT WINAPI hookedJoyGetPosEx(UINT id, LPJOYINFOEX info) {
    const MMRESULT result = g_originalJoyGetPosEx ? g_originalJoyGetPosEx(id, info) : MMSYSERR_ERROR;
    if (result == JOYERR_NOERROR && info && inputMustBeBlocked()) {
        info->dwXpos = info->dwYpos = info->dwZpos = 0x7fff;
        info->dwRpos = info->dwUpos = info->dwVpos = 0x7fff;
        info->dwButtons = info->dwButtonNumber = 0;
        info->dwPOV = JOY_POVCENTERED;
    }
    return result;
}

void publishControllerState(const XINPUT_STATE* state, bool connected) {
    if (!openSharedState()) return;
    if (!connected || !state) {
        InterlockedExchange(&g_sharedState->controllerConnected, FALSE);
        InterlockedExchange(&g_sharedState->controllerButtons, 0);
        InterlockedExchange(&g_sharedState->controllerThumbLX, 0);
        InterlockedExchange(&g_sharedState->controllerThumbLY, 0);
        return;
    }
    InterlockedExchange(&g_sharedState->controllerButtons, state->Gamepad.wButtons);
    InterlockedExchange(&g_sharedState->controllerThumbLX, state->Gamepad.sThumbLX);
    InterlockedExchange(&g_sharedState->controllerThumbLY, state->Gamepad.sThumbLY);
    InterlockedExchange(&g_sharedState->controllerConnected, TRUE);
    InterlockedExchange(&g_sharedState->controllerUpdateTick, static_cast<LONG>(GetTickCount()));
}

template <size_t Index>
DWORD WINAPI hookedXInputGetState(DWORD userIndex, XINPUT_STATE* state) {
    const auto original = g_originalFunctions[Index];
    const DWORD result = original ? original(userIndex, state)
                                  : static_cast<DWORD>(ERROR_DEVICE_NOT_CONNECTED);
    // O controle principal e o indice zero. Nao deixe as sondagens dos slots
    // vazios (1-3) apagarem o estado que o overlay acabou de receber.
    if (userIndex == 0) publishControllerState(state, result == ERROR_SUCCESS);
    if (userIndex == 0 && openSharedState()) {
        InterlockedIncrement(&g_sharedState->xinputCallCount);
    }
    if (userIndex == 0 && result == ERROR_SUCCESS && state) {
        const WORD chord = XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK;
        const bool chordHeld = (state->Gamepad.wButtons & chord) == chord;
        if (chordHeld && !g_toggleChordHeld && openSharedState()) {
            InterlockedExchange(&g_sharedState->toggleOverlayRequest, TRUE);
        }
        g_toggleChordHeld = chordHeld;
        if (chordHeld) state->Gamepad.wButtons &= static_cast<WORD>(~chord);
    }
    if (inputMustBeBlocked()) {
        if (userIndex == 0 && openSharedState()) {
            InterlockedIncrement(&g_sharedState->blockedXinputCallCount);
        }
        // Preserve o estado de conexao percebido pelo jogo. Quando o controle
        // existe, entregamos um quadro neutro em vez de simular a desconexao.
        if (result == ERROR_SUCCESS && state) {
            const DWORD packetNumber = state->dwPacketNumber;
            ZeroMemory(state, sizeof(*state));
            state->dwPacketNumber = packetNumber;
        }
        return result;
    }
    return result;
}

template <size_t... Indices>
constexpr auto makeHookFunctions(std::index_sequence<Indices...>) {
    return std::array<XInputGetStateFn, sizeof...(Indices)>{&hookedXInputGetState<Indices>...};
}

constexpr auto kHookFunctions = makeHookFunctions(std::make_index_sequence<kMaximumHooks>{});

template <size_t Index>
DWORD WINAPI hookedXInputGetKeystroke(DWORD userIndex, DWORD reserved,
                                      PXINPUT_KEYSTROKE keystroke) {
    const auto original = g_originalKeystrokeFunctions[Index];
    const DWORD result = original ? original(userIndex, reserved, keystroke)
                                  : static_cast<DWORD>(ERROR_EMPTY);
    if (!inputMustBeBlocked()) return result;
    if (keystroke) ZeroMemory(keystroke, sizeof(*keystroke));
    return ERROR_EMPTY;
}

template <size_t... Indices>
constexpr auto makeKeystrokeHookFunctions(std::index_sequence<Indices...>) {
    return std::array<XInputGetKeystrokeFn, sizeof...(Indices)>{
        &hookedXInputGetKeystroke<Indices>...};
}

constexpr auto kKeystrokeHookFunctions =
    makeKeystrokeHookFunctions(std::make_index_sequence<kMaximumHooks>{});

bool alreadyHooked(void* target) {
    for (size_t index = 0; index < g_hookCount; ++index) {
        if (g_hookedTargets[index] == target) return true;
    }
    return false;
}

FARPROC newestVersionedExport(HMODULE module, const char* prefix) {
    if (!module || !prefix) return nullptr;
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!directory.VirtualAddress || !directory.Size) return nullptr;
    const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
        base + directory.VirtualAddress);
    const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
    const size_t prefixLength = std::strlen(prefix);
    unsigned long newestVersion = 0;
    const char* newestName = nullptr;
    for (DWORD index = 0; index < exports->NumberOfNames; ++index) {
        const char* name = reinterpret_cast<const char*>(base + names[index]);
        if (std::strncmp(name, prefix, prefixLength) != 0) continue;
        const char* versionText = name + prefixLength;
        if (!*versionText || !std::all_of(versionText, versionText + std::strlen(versionText),
                                           [](unsigned char value) { return std::isdigit(value) != 0; })) {
            continue;
        }
        const unsigned long version = std::strtoul(versionText, nullptr, 10);
        if (!newestName || version > newestVersion) {
            newestVersion = version;
            newestName = name;
        }
    }
    return newestName ? GetProcAddress(module, newestName) : nullptr;
}

void hookTarget(void* target) {
    if (!target || alreadyHooked(target) || g_hookCount >= kMaximumHooks) return;
    const size_t slot = g_hookCount;
    if (MH_CreateHook(target, reinterpret_cast<void*>(kHookFunctions[slot]),
                      reinterpret_cast<void**>(&g_originalFunctions[slot])) != MH_OK) return;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        g_originalFunctions[slot] = nullptr;
        return;
    }
    g_hookedTargets[slot] = target;
    ++g_hookCount;
}

void hookKeystrokeTarget(void* target) {
    if (!target || g_keystrokeHookCount >= kMaximumHooks) return;
    for (size_t index = 0; index < g_keystrokeHookCount; ++index) {
        if (g_hookedKeystrokeTargets[index] == target) return;
    }
    const size_t slot = g_keystrokeHookCount;
    if (MH_CreateHook(target, reinterpret_cast<void*>(kKeystrokeHookFunctions[slot]),
                      reinterpret_cast<void**>(&g_originalKeystrokeFunctions[slot])) != MH_OK) return;
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        g_originalKeystrokeFunctions[slot] = nullptr;
        return;
    }
    g_hookedKeystrokeTargets[slot] = target;
    ++g_keystrokeHookCount;
}

void hookXInputModule(const wchar_t* moduleName) {
    const HMODULE module = GetModuleHandleW(moduleName);
    if (!module) return;
    hookTarget(reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")));
    hookTarget(reinterpret_cast<void*>(GetProcAddress(module, MAKEINTRESOURCEA(100))));
    hookKeystrokeTarget(reinterpret_cast<void*>(GetProcAddress(module, "XInputGetKeystroke")));
}

void collectSteamAchievements() {
    const std::wstring mappingName = achievementSharedMemoryName(GetCurrentProcessId());
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName.c_str());
    if (!mapping) return;
    auto* shared = static_cast<AchievementSharedState*>(MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AchievementSharedState)));
    if (!shared || shared->version != kAchievementSharedVersion) {
        if (shared) UnmapViewOfFile(shared);
        CloseHandle(mapping);
        return;
    }

    using GetInterfaceFn = void*(__cdecl*)();
    using GetCountFn = unsigned int(__cdecl*)(void*);
    using GetNameFn = const char*(__cdecl*)(void*, unsigned int);
    using GetStateFn = bool(__cdecl*)(void*, const char*, bool*, unsigned int*);
    using GetAttributeFn = const char*(__cdecl*)(void*, const char*, const char*);
    using GetIconFn = int(__cdecl*)(void*, const char*);
    using GetImageSizeFn = bool(__cdecl*)(void*, int, unsigned int*, unsigned int*);
    using GetImageRgbaFn = bool(__cdecl*)(void*, int, unsigned char*, int);
    using GetAppIdFn = unsigned int(__cdecl*)(void*);
    using GetStatIntFn = bool(__cdecl*)(void*, const char*, int*);

    // Jogos de 32 bits carregam steam_api.dll; os de 64 bits normalmente usam
    // steam_api64.dll. O hook existe nas duas arquiteturas, portanto nao presuma
    // o nome da DLL aqui.
    for (int attempt = 0; attempt < 120; ++attempt) {
        HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
        if (!steam) steam = GetModuleHandleW(L"steam_api.dll");
        if (!steam) {
            Sleep(1000);
            continue;
        }
        auto getInterface = reinterpret_cast<GetInterfaceFn>(
            newestVersionedExport(steam, "SteamAPI_SteamUserStats_v"));
        if (!getInterface) getInterface = reinterpret_cast<GetInterfaceFn>(
            GetProcAddress(steam, "SteamUserStats"));
        auto getCount = reinterpret_cast<GetCountFn>(
            GetProcAddress(steam, "SteamAPI_ISteamUserStats_GetNumAchievements"));
        auto getName = reinterpret_cast<GetNameFn>(
            GetProcAddress(steam, "SteamAPI_ISteamUserStats_GetAchievementName"));
        auto getState = reinterpret_cast<GetStateFn>(
            GetProcAddress(steam, "SteamAPI_ISteamUserStats_GetAchievementAndUnlockTime"));
        auto getAttribute = reinterpret_cast<GetAttributeFn>(
            GetProcAddress(steam, "SteamAPI_ISteamUserStats_GetAchievementDisplayAttribute"));
        auto getIcon = reinterpret_cast<GetIconFn>(
            GetProcAddress(steam, "SteamAPI_ISteamUserStats_GetAchievementIcon"));
        auto getUtils = reinterpret_cast<GetInterfaceFn>(
            newestVersionedExport(steam, "SteamAPI_SteamUtils_v"));
        if (!getUtils) getUtils = reinterpret_cast<GetInterfaceFn>(
            GetProcAddress(steam, "SteamUtils"));
        auto getImageSize = reinterpret_cast<GetImageSizeFn>(
            GetProcAddress(steam, "SteamAPI_ISteamUtils_GetImageSize"));
        auto getImageRgba = reinterpret_cast<GetImageRgbaFn>(
            GetProcAddress(steam, "SteamAPI_ISteamUtils_GetImageRGBA"));
        auto getAppId = reinterpret_cast<GetAppIdFn>(
            GetProcAddress(steam, "SteamAPI_ISteamUtils_GetAppID"));
        auto getStatInt = reinterpret_cast<GetStatIntFn>(
            GetProcAddress(steam, "SteamAPI_ISteamUserStats_GetStatInt32"));
        void* steamUtils = getUtils ? getUtils() : nullptr;
        const unsigned int appId = steamUtils && getAppId ? getAppId(steamUtils) : 0;
        InterlockedExchange(&shared->appId, static_cast<LONG>(appId));
        if (!getInterface || !getCount || !getName || !getState || !getAttribute) break;

        void* stats = getInterface();
        const unsigned int available = stats ? getCount(stats) : 0;
        if (!stats || available == 0) {
            Sleep(1000);
            continue;
        }

        InterlockedExchange(&shared->status, 0);
        std::string steamLanguage;
        {
            std::wstring language = registryText(HKEY_CURRENT_USER, L"Language");
            if (language.empty()) language = registryText(HKEY_LOCAL_MACHINE, L"Language", RRF_SUBKEY_WOW6432KEY);
            steamLanguage.reserve(language.size());
            for (wchar_t character : language) {
                if (character >= 0 && character <= 0x7f) {
                    steamLanguage.push_back(static_cast<char>(character));
                }
            }
            std::transform(steamLanguage.begin(), steamLanguage.end(), steamLanguage.begin(),
                           [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        }
        const auto schema = appId ? steamAchievementSchema(appId) : std::vector<unsigned char>{};
        auto reloadIcon = [&](SharedAchievement& target) {
            if (!steamUtils || !getIcon || !getImageSize || !getImageRgba) return false;
            const int image = getIcon(stats, target.apiName);
            unsigned int imageWidth = 0, imageHeight = 0;
            if (image <= 0 || !getImageSize(steamUtils, image, &imageWidth, &imageHeight) ||
                !imageWidth || !imageHeight || imageWidth > 512 || imageHeight > 512) return false;
            std::vector<unsigned char> rgba(static_cast<size_t>(imageWidth) * imageHeight * 4);
            if (!getImageRgba(steamUtils, image, rgba.data(), static_cast<int>(rgba.size()))) return false;
            InterlockedExchange(&target.iconWidth, 0);
            for (unsigned int y = 0; y < 64; ++y) {
                for (unsigned int x = 0; x < 64; ++x) {
                    const auto sourceIndex = (static_cast<size_t>(y * imageHeight / 64) * imageWidth +
                                              x * imageWidth / 64) * 4;
                    const BYTE red = rgba[sourceIndex];
                    const BYTE green = rgba[sourceIndex + 1];
                    const BYTE blue = rgba[sourceIndex + 2];
                    const BYTE alpha = rgba[sourceIndex + 3];
                    target.iconPixels[y * 64 + x] =
                        (static_cast<unsigned int>(alpha) << 24) |
                        (static_cast<unsigned int>(red * alpha / 255) << 16) |
                        (static_cast<unsigned int>(green * alpha / 255) << 8) |
                        static_cast<unsigned int>(blue * alpha / 255);
                }
            }
            MemoryBarrier();
            target.iconHeight = 64;
            InterlockedExchange(&target.iconWidth, 64);
            return true;
        };
        const LONG count = static_cast<LONG>((std::min)(available,
            static_cast<unsigned int>(kMaximumSharedAchievements)));
        std::vector<std::string> progressStats(static_cast<size_t>(count));
        for (LONG index = 0; index < count; ++index) {
            auto& target = shared->achievements[index];
            ZeroMemory(&target, sizeof(target));
            const char* apiName = getName(stats, static_cast<unsigned int>(index));
            if (!apiName) continue;
            strncpy_s(target.apiName, apiName, _TRUNCATE);
            const char* displayName = getAttribute(stats, apiName, "name");
            const char* description = getAttribute(stats, apiName, "desc");
            const char* hidden = getAttribute(stats, apiName, "hidden");
            if (displayName) strncpy_s(target.displayName, displayName, _TRUNCATE);
            if (description) strncpy_s(target.description, description, _TRUNCATE);
            const std::string localizedName = localizedSchemaValue(
                schema, displayName, steamLanguage, "desc");
            const std::string localizedDescription = localizedSchemaValue(
                schema, description, steamLanguage, "hidden");
            if (!localizedName.empty()) strncpy_s(target.displayName, localizedName.c_str(), _TRUNCATE);
            if (!localizedDescription.empty()) {
                strncpy_s(target.description, localizedDescription.c_str(), _TRUNCATE);
            }
            target.hidden = hidden && hidden[0] == '1';
            bool unlocked = false;
            unsigned int unlockTime = 0;
            getState(stats, apiName, &unlocked, &unlockTime);
            target.unlocked = unlocked ? TRUE : FALSE;
            target.unlockTime = unlockTime;
            LONG progressMaximum = 0;
            if (schemaProgress(schema, apiName, progressStats[static_cast<size_t>(index)],
                               progressMaximum)) {
                target.hasProgress = TRUE;
                target.progressMaximum = progressMaximum;
                int current = 0;
                if (getStatInt && getStatInt(stats, progressStats[static_cast<size_t>(index)].c_str(), &current)) {
                    target.progressCurrent = current;
                }
            }
            if (getIcon && getUtils && getImageSize && getImageRgba) {
                void* utils = getUtils();
                const int image = getIcon(stats, apiName);
                unsigned int imageWidth = 0, imageHeight = 0;
                if (utils && image > 0 && getImageSize(utils, image, &imageWidth, &imageHeight) &&
                    imageWidth && imageHeight && imageWidth <= 512 && imageHeight <= 512) {
                    std::vector<unsigned char> rgba(static_cast<size_t>(imageWidth) * imageHeight * 4);
                    if (getImageRgba(utils, image, rgba.data(), static_cast<int>(rgba.size()))) {
                        for (unsigned int y = 0; y < 64; ++y) {
                            for (unsigned int x = 0; x < 64; ++x) {
                                const unsigned int sourceX = x * imageWidth / 64;
                                const unsigned int sourceY = y * imageHeight / 64;
                                const auto sourceIndex = (static_cast<size_t>(sourceY) * imageWidth + sourceX) * 4;
                                const BYTE red = rgba[sourceIndex];
                                const BYTE green = rgba[sourceIndex + 1];
                                const BYTE blue = rgba[sourceIndex + 2];
                                const BYTE alpha = rgba[sourceIndex + 3];
                                target.iconPixels[y * 64 + x] =
                                    (static_cast<unsigned int>(alpha) << 24) |
                                    (static_cast<unsigned int>(red * alpha / 255) << 16) |
                                    (static_cast<unsigned int>(green * alpha / 255) << 8) |
                                    static_cast<unsigned int>(blue * alpha / 255);
                            }
                        }
                        MemoryBarrier();
                        target.iconHeight = 64;
                        InterlockedExchange(&target.iconWidth, 64);
                    }
                }
            }
        }
        InterlockedExchange(&shared->count, count);
        InterlockedIncrement(&shared->generation);
        InterlockedExchange(&shared->status, 1);

        // GetAchievementIcon pode retornar 0 enquanto a Steam baixa a imagem.
        // O jogo processa o callback; tentamos novamente sem bloquear a interface.
        if (getIcon && getUtils && getImageSize && getImageRgba) {
            void* utils = getUtils();
            for (int retry = 0; utils && retry < 15; ++retry) {
                Sleep(1000);
                bool changed = false;
                bool missing = false;
                for (LONG index = 0; index < count; ++index) {
                    auto& target = shared->achievements[index];
                    if (target.iconWidth == 64) continue;
                    missing = true;
                    const int image = getIcon(stats, target.apiName);
                    unsigned int imageWidth = 0, imageHeight = 0;
                    if (image <= 0 || !getImageSize(utils, image, &imageWidth, &imageHeight) ||
                        !imageWidth || !imageHeight || imageWidth > 512 || imageHeight > 512) continue;
                    std::vector<unsigned char> rgba(static_cast<size_t>(imageWidth) * imageHeight * 4);
                    if (!getImageRgba(utils, image, rgba.data(), static_cast<int>(rgba.size()))) continue;
                    for (unsigned int y = 0; y < 64; ++y) {
                        for (unsigned int x = 0; x < 64; ++x) {
                            const auto sourceIndex = (static_cast<size_t>(y * imageHeight / 64) * imageWidth +
                                                      x * imageWidth / 64) * 4;
                            const BYTE red = rgba[sourceIndex];
                            const BYTE green = rgba[sourceIndex + 1];
                            const BYTE blue = rgba[sourceIndex + 2];
                            const BYTE alpha = rgba[sourceIndex + 3];
                            target.iconPixels[y * 64 + x] =
                                (static_cast<unsigned int>(alpha) << 24) |
                                (static_cast<unsigned int>(red * alpha / 255) << 16) |
                                (static_cast<unsigned int>(green * alpha / 255) << 8) |
                                static_cast<unsigned int>(blue * alpha / 255);
                        }
                    }
                    MemoryBarrier();
                    target.iconHeight = 64;
                    InterlockedExchange(&target.iconWidth, 64);
                    changed = true;
                }
                if (changed) InterlockedIncrement(&shared->generation);
                if (!missing) break;
            }
        }
        for (;;) {
            Sleep(2000);
            // A coleta inicial ja foi feita. Atualizacoes periodicas so sao
            // necessarias enquanto o usuario esta vendo o overlay.
            if (!inputMustBeBlocked()) continue;
            bool changed = false;
            for (LONG index = 0; index < count; ++index) {
                auto& target = shared->achievements[index];
                bool unlocked = false;
                unsigned int unlockTime = 0;
                if (getState(stats, target.apiName, &unlocked, &unlockTime)) {
                    const LONG unlockedValue = unlocked ? TRUE : FALSE;
                    if (target.unlocked != unlockedValue || target.unlockTime != unlockTime) {
                        InterlockedExchange(&target.unlocked, unlockedValue);
                        target.unlockTime = unlockTime;
                        reloadIcon(target);
                        changed = true;
                    }
                }
                if (target.hasProgress && getStatInt && !progressStats[static_cast<size_t>(index)].empty()) {
                    int current = 0;
                    if (getStatInt(stats, progressStats[static_cast<size_t>(index)].c_str(), &current) &&
                        target.progressCurrent != current) {
                        InterlockedExchange(&target.progressCurrent, current);
                        changed = true;
                    }
                }
            }
            if (changed) InterlockedIncrement(&shared->generation);
        }
    }
    InterlockedExchange(&shared->status, -1);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
}

DWORD WINAPI collectSteamAchievementsThread(void*) {
    collectSteamAchievements();
    return 0;
}

void hookLoadedXInputModules() {
    constexpr const wchar_t* modules[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll",
        L"xinput1_2.dll", L"xinput1_1.dll", L"xinputuap.dll",
    };
    for (const wchar_t* module : modules) hookXInputModule(module);
}

void hookGameInput() {
    if (g_gameInputGetGamepadStateTarget) return;
    if (!g_gameInputModule) g_gameInputModule = LoadLibraryW(L"gameinput.dll");
    if (!g_gameInputModule) return;

    using GameInputCreateFn = HRESULT(WINAPI*)(IGameInput**);
    const auto create = reinterpret_cast<GameInputCreateFn>(
        GetProcAddress(g_gameInputModule, "GameInputCreate"));
    if (!create) return;

    IGameInput* gameInput = nullptr;
    IGameInputReading* reading = nullptr;
    if (FAILED(create(&gameInput)) || !gameInput) return;
    const HRESULT readingResult = gameInput->GetCurrentReading(
        GameInputKindGamepad, nullptr, &reading);
    gameInput->Release();
    if (FAILED(readingResult) || !reading) return;

    // IGameInputReading::GetGamepadState e a entrada 22 da vtable
    // (IUnknown ocupa as tres primeiras entradas).
    void* target = (*reinterpret_cast<void***>(reading))[22];
    reading->Release();
    if (!target) return;
    if (MH_CreateHook(target, reinterpret_cast<void*>(&hookedGameInputGetGamepadState),
                      reinterpret_cast<void**>(&g_originalGameInputGetGamepadState)) != MH_OK) {
        g_originalGameInputGetGamepadState = nullptr;
        return;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        g_originalGameInputGetGamepadState = nullptr;
        return;
    }
    g_gameInputGetGamepadStateTarget = target;
}

bool createAndEnableHook(void* target, void* detour, void** original) {
    if (!target || !detour || !original) return false;
    if (MH_CreateHook(target, detour, original) != MH_OK) return false;
    if (MH_EnableHook(target) == MH_OK) return true;
    MH_RemoveHook(target);
    *original = nullptr;
    return false;
}

void hookRawInput() {
    if (g_rawInputHooksInstalled) return;
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    void* dataTarget = reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputData"));
    void* bufferTarget = reinterpret_cast<void*>(GetProcAddress(user32, "GetRawInputBuffer"));
    const bool dataHooked = g_originalGetRawInputData || createAndEnableHook(
        dataTarget, reinterpret_cast<void*>(&hookedGetRawInputData),
        reinterpret_cast<void**>(&g_originalGetRawInputData));
    const bool bufferHooked = g_originalGetRawInputBuffer || createAndEnableHook(
        bufferTarget, reinterpret_cast<void*>(&hookedGetRawInputBuffer),
        reinterpret_cast<void**>(&g_originalGetRawInputBuffer));
    g_rawInputHooksInstalled = dataHooked && bufferHooked;
}

void hookWinmmJoystick() {
    if (g_winmmHooksInstalled) return;
    HMODULE winmm = GetModuleHandleW(L"winmm.dll");
    if (!winmm) return;
    void* posTarget = reinterpret_cast<void*>(GetProcAddress(winmm, "joyGetPos"));
    void* posExTarget = reinterpret_cast<void*>(GetProcAddress(winmm, "joyGetPosEx"));
    const bool posHooked = g_originalJoyGetPos || createAndEnableHook(
        posTarget, reinterpret_cast<void*>(&hookedJoyGetPos),
        reinterpret_cast<void**>(&g_originalJoyGetPos));
    const bool posExHooked = g_originalJoyGetPosEx || createAndEnableHook(
        posExTarget, reinterpret_cast<void*>(&hookedJoyGetPosEx),
        reinterpret_cast<void**>(&g_originalJoyGetPosEx));
    g_winmmHooksInstalled = posHooked && posExHooked;
}

void hookDirectHid() {
    if (g_directHidHooksInstalled) return;
    HMODULE hid = GetModuleHandleW(L"hid.dll");
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!hid || !kernel32) return;
    void* hidDataTarget = reinterpret_cast<void*>(GetProcAddress(hid, "HidP_GetData"));
    void* readFileTarget = reinterpret_cast<void*>(GetProcAddress(kernel32, "ReadFile"));
    const bool hidDataHooked = g_originalHidPGetData || createAndEnableHook(
        hidDataTarget, reinterpret_cast<void*>(&hookedHidPGetData),
        reinterpret_cast<void**>(&g_originalHidPGetData));
    const bool readFileHooked = g_originalReadFile || createAndEnableHook(
        readFileTarget, reinterpret_cast<void*>(&hookedReadFile),
        reinterpret_cast<void**>(&g_originalReadFile));
    g_directHidHooksInstalled = hidDataHooked && readFileHooked;
}

void hookWindowsGamingInput() {
    if (g_wgiGetCurrentReadingTarget) return;
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) return;

    HSTRING className = nullptr;
    if (FAILED(WindowsCreateString(RuntimeClass_Windows_Gaming_Input_Gamepad,
                                   static_cast<UINT32>(
                                       wcslen(RuntimeClass_Windows_Gaming_Input_Gamepad)),
                                   &className))) return;
    ABI::Windows::Gaming::Input::IGamepadStatics* statics = nullptr;
    const HRESULT factoryResult = RoGetActivationFactory(
        className, __uuidof(ABI::Windows::Gaming::Input::IGamepadStatics),
        reinterpret_cast<void**>(&statics));
    WindowsDeleteString(className);
    if (FAILED(factoryResult) || !statics) return;

    __FIVectorView_1_Windows__CGaming__CInput__CGamepad* gamepads = nullptr;
    const HRESULT listResult = statics->get_Gamepads(&gamepads);
    statics->Release();
    if (FAILED(listResult) || !gamepads) return;
    UINT32 count = 0;
    gamepads->get_Size(&count);
    WgiGamepad* gamepad = nullptr;
    if (count) gamepads->GetAt(0, &gamepad);
    gamepads->Release();
    if (!gamepad) return;

    void* target = (*reinterpret_cast<void***>(gamepad))[8];
    gamepad->Release();
    if (!target) return;
    if (createAndEnableHook(target, reinterpret_cast<void*>(&hookedWgiGetCurrentReading),
                            reinterpret_cast<void**>(&g_originalWgiGetCurrentReading))) {
        g_wgiGetCurrentReadingTarget = target;
    }
}

DWORD WINAPI initializeHook(void*) {
    // O manager ja espera o jogo ganhar foco; damos mais um instante para a engine estabilizar.
    Sleep(1500);
    openSharedState();
    if (MH_Initialize() != MH_OK) return 0;

    hookLoadedXInputModules();
    hookGameInput();
    hookRawInput();
    hookWinmmJoystick();
    hookDirectHid();
    hookWindowsGamingInput();

    // A Steam Input e algumas engines carregam (ou trocam) a implementacao de
    // XInput depois da inicializacao do jogo. A sondagem unica deixava essas
    // chamadas fora do hook: o overlay recebia o controle, mas o jogo tambem.
    // Mantenha a coleta da Steam em outra thread e acompanhe modulos tardios.
    if (HANDLE thread = CreateThread(nullptr, 0, collectSteamAchievementsThread,
                                     nullptr, 0, nullptr)) {
        CloseHandle(thread);
    }
    for (;;) {
        Sleep(1000);
        hookLoadedXInputModules();
        hookGameInput();
        hookRawInput();
        hookWinmmJoystick();
        hookDirectHid();
        hookWindowsGamingInput();
    }
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        if (HANDLE thread = CreateThread(nullptr, 0, initializeHook, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    // Nao removemos hooks no PROCESS_DETACH: o loader lock torna isso inseguro.
    return TRUE;
}
