#include "InputShared.h"
#include "AchievementShared.h"

#include <MinHook.h>
#include <windows.h>
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

constexpr size_t kMaximumHooks = 16;
std::array<XInputGetStateFn, kMaximumHooks> g_originalFunctions{};
std::array<void*, kMaximumHooks> g_hookedTargets{};
size_t g_hookCount = 0;
HANDLE g_sharedMapping = nullptr;
InputSharedState* g_sharedState = nullptr;
bool g_toggleChordHeld = false;

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

void hookXInputModule(const wchar_t* moduleName) {
    const HMODULE module = GetModuleHandleW(moduleName);
    if (!module) return;
    hookTarget(reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")));
    hookTarget(reinterpret_cast<void*>(GetProcAddress(module, MAKEINTRESOURCEA(100))));
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
        void* steamUtils = getUtils ? getUtils() : nullptr;
        const unsigned int appId = steamUtils && getAppId ? getAppId(steamUtils) : 0;
        InterlockedExchange(&shared->appId, static_cast<LONG>(appId));
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

DWORD WINAPI initializeHook(void*) {
    // O manager ja espera o jogo ganhar foco; damos mais um instante para a engine estabilizar.
    Sleep(1500);
    openSharedState();
    if (MH_Initialize() != MH_OK) return 0;

    constexpr const wchar_t* modules[] = {
        L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll",
        L"xinput1_2.dll", L"xinput1_1.dll", L"xinputuap.dll",
    };
    for (const wchar_t* module : modules) hookXInputModule(module);
    collectSteamAchievements();
    return 0;
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
