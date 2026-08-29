#pragma once

#include <windows.h>

#include <string>

constexpr LONG kInputSharedVersion = 6;

struct InputSharedState {
    LONG version;
    volatile LONG blockGameInput;
    volatile LONG controllerConnected;
    volatile LONG controllerButtons;
    volatile LONG controllerThumbLX;
    volatile LONG controllerThumbLY;
    volatile LONG toggleOverlayRequest;
    volatile LONG controllerUpdateTick;
    volatile LONG xinputCallCount;
    volatile LONG blockedXinputCallCount;
    volatile LONG rawInputCallCount;
    volatile LONG hidParserCallCount;
    volatile LONG hidReadCallCount;
};

inline std::wstring inputSharedMemoryName(DWORD gamePid) {
    return L"Local\\GameOverlayMvp.Input." + std::to_wstring(gamePid);
}
