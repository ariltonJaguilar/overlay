#pragma once

#include <windows.h>

#include <string>

constexpr LONG kAchievementSharedVersion = 2;
constexpr LONG kMaximumSharedAchievements = 256;

struct SharedAchievement {
    char apiName[128];
    char displayName[160];
    char description[320];
    LONG unlocked;
    LONG hidden;
    unsigned long unlockTime;
    LONG hasProgress;
    LONG progressCurrent;
    LONG progressMaximum;
    LONG iconWidth;
    LONG iconHeight;
    unsigned int iconPixels[64 * 64];
};

struct AchievementSharedState {
    LONG version;
    volatile LONG appId;
    volatile LONG status; // 0: carregando, 1: pronto, -1: indisponivel
    volatile LONG count;
    volatile LONG generation;
    SharedAchievement achievements[kMaximumSharedAchievements];
};

inline std::wstring achievementSharedMemoryName(DWORD gamePid) {
    return L"Local\\GameOverlayMvp.Achievements." + std::to_wstring(gamePid);
}
