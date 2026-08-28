#include "InputShared.h"

#include <MinHook.h>
#include <windows.h>
#include <xinput.h>

#include <array>
#include <cstddef>

namespace {
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

constexpr size_t kMaximumHooks = 16;
std::array<XInputGetStateFn, kMaximumHooks> g_originalFunctions{};
std::array<void*, kMaximumHooks> g_hookedTargets{};
size_t g_hookCount = 0;
HANDLE g_sharedMapping = nullptr;
InputSharedState* g_sharedState = nullptr;
bool g_toggleChordHeld = false;

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
