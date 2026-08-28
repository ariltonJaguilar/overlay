#include <windows.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
bool injectInputHook(const std::filesystem::path& dllPath, DWORD gamePid) {
    HANDLE game = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, gamePid);
    if (!game) return false;

    const std::wstring path = dllPath.wstring();
    const SIZE_T byteCount = (path.size() + 1) * sizeof(wchar_t);
    void* remotePath = VirtualAllocEx(game, nullptr, byteCount, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remotePath || !WriteProcessMemory(game, remotePath, path.c_str(), byteCount, nullptr)) {
        if (remotePath) VirtualFreeEx(game, remotePath, 0, MEM_RELEASE);
        CloseHandle(game);
        return false;
    }

    const auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = loadLibrary ? CreateRemoteThread(game, nullptr, 0, loadLibrary, remotePath, 0, nullptr) : nullptr;
    bool loaded = false;
    if (thread) {
        if (WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0) {
            DWORD result = 0;
            loaded = GetExitCodeThread(thread, &result) && result != 0;
        }
        CloseHandle(thread);
    }
    VirtualFreeEx(game, remotePath, 0, MEM_RELEASE);
    CloseHandle(game);
    return loaded;
}
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 5 || _wcsicmp(argv[1], L"--pid") != 0 || _wcsicmp(argv[3], L"--dll") != 0) {
        std::wcerr << L"Uso: GameOverlayInjector32.exe --pid <PID> --dll <caminho>\n";
        return 2;
    }
    const DWORD pid = wcstoul(argv[2], nullptr, 10);
    return pid != 0 && injectInputHook(argv[4], pid) ? 0 : 1;
}
