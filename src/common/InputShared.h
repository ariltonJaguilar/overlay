#pragma once

#include <windows.h>

#include <string>

constexpr LONG kInputSharedVersion = 5;

struct InputSharedState {
    LONG version;
    volatile LONG blockGameInput;
    volatile LONG controllerConnected;
    volatile LONG controllerButtons;
    volatile LONG controllerThumbLX;
    volatile LONG controllerThumbLY;
    volatile LONG toggleOverlayRequest;
    volatile LONG controllerUpdateTick;
};

inline std::wstring inputSharedMemoryName(DWORD gamePid) {
    return L"Local\\GameOverlayMvp.Input." + std::to_wstring(gamePid);
}
