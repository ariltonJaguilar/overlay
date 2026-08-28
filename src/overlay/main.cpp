#include "OverlayWindow.h"

#include <windows.h>
#include <shellapi.h>
#include <objbase.h>

#include <cwchar>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    DWORD pid = 0;
    for (int i = 1; argv && i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], L"--pid") == 0) {
            wchar_t* end = nullptr;
            const unsigned long value = wcstoul(argv[i + 1], &end, 10);
            if (end && *end == L'\0') pid = static_cast<DWORD>(value);
        }
    }
    if (argv) LocalFree(argv);

    if (pid == 0) {
        MessageBoxW(nullptr, L"Uso: Overlay.exe --pid <PID>", L"Overlay MVP", MB_OK | MB_ICONERROR);
        return 1;
    }

    OverlayWindow overlay;
    const int result = overlay.run(instance, pid);
    if (SUCCEEDED(comResult)) CoUninitialize();
    return result;
}
