#include "SteamDiscovery.h"

#include <windows.h>
#include <tlhelp32.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
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

bool injectInputHook(const std::filesystem::path& dllPath, DWORD gamePid) {
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
} // namespace

int wmain(int argc, wchar_t* argv[]) {
    HANDLE managerMutex = CreateMutexW(nullptr, TRUE, L"Local\\GameOverlayMvp.Manager");
    if (!managerMutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (managerMutex) CloseHandle(managerMutex);
        std::wcerr << L"O GameOverlayManager ja esta em execucao.\n";
        return 1;
    }

    const auto manualTarget = configuredProcess(argc, argv);
    const std::filesystem::path overlayPath = executableDirectory() / L"Overlay.exe";
    const std::filesystem::path hookPath = executableDirectory() / L"OverlayInputHook.dll";
    if (!std::filesystem::exists(overlayPath)) {
        std::wcerr << L"Overlay.exe nao foi encontrado ao lado do manager: " << overlayPath << L"\n";
        CloseHandle(managerMutex);
        return 1;
    }
    if (!std::filesystem::exists(hookPath)) {
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
                if (HANDLE process = launchOverlay(overlayPath, pid)) {
                    overlays.emplace(pid, process);
                    std::wcout << L"Overlay iniciado para o PID " << pid << L".\n";
                    std::wcout << L"Aguardando estabilizacao do jogo antes de ativar o hook...\n";
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    if (injectInputHook(hookPath, pid)) {
                        std::wcout << L"Hook de XInput injetado no PID " << pid << L".\n";
                    }
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}
