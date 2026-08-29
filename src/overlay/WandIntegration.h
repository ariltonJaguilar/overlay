#pragma once

#include <windows.h>

#include <string>
#include <vector>

struct WandMod {
    enum class Kind { Switch, Integer, Slider } kind = Kind::Switch;
    std::wstring name;
    bool enabled = false;
    bool numeric = false;
    double value = 0;
    double minimum = 0;
    double maximum = 0;
    double step = 1;
};

struct WandLoadResult {
    bool success = false;
    std::wstring error;
    std::vector<WandMod> mods;
};

class WandIntegration {
public:
    static bool isRunning();
    static bool isTrainerActive(DWORD gamePid);
    static bool startHidden(std::wstring& error);
    static WandLoadResult selectGameAndLoad(const std::wstring& gameName, DWORD gamePid = 0);
    static bool toggle(const std::wstring& modName, bool& enabled, std::wstring& error);
    static bool setValue(const std::wstring& modName, double requested, double& actual,
                         std::wstring& error);
};
