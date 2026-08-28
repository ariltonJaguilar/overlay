#pragma once

#include <windows.h>
#include <xinput.h>
#include <GameInput.h>
#include <endpointvolume.h>

#include "InputShared.h"
#include "AchievementShared.h"

#include <cstdint>
#include <vector>

class OverlayWindow {
public:
    int run(HINSTANCE instance, DWORD gamePid);

private:
    struct IconImage {
        int width = 0;
        int height = 0;
        std::vector<std::uint32_t> pixels;
    };

    static constexpr int kHotkeyId = 1;
    static constexpr UINT_PTR kProcessTimerId = 1;

    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK keyboardHookProc(int code, WPARAM wParam, LPARAM lParam);
    bool create(HINSTANCE instance, DWORD gamePid);
    void applyVisualStyle();
    void toggleVisibility();
    void syncVisibilityWithGame();
    void centerOnGameMonitor();
    void setGameInputBlocked(bool blocked);
    void pollController();
    void moveSelection(int direction);
    void activateSelection();
    void goBack();
    bool initializeSystemVolume();
    void adjustVolume(int direction);
    void setVolumeFromMouse(int x);
    void executeSettingsAction();
    void prepareForGameExit();
    void draw(HDC dc) const;
    bool captureAndBlurBackground();
    void renderAndPresent();
    void releaseRenderer();
    bool loadIcons();
    void blendIcon(std::uint32_t* destination, int destinationWidth,
                   const IconImage& icon, int centerX, int centerY, BYTE opacity) const;
    void drawVolumeSlider(std::uint32_t* destination, int destinationWidth) const;
    void drawAchievementImages(std::uint32_t* destination, int destinationWidth) const;
    int barTop() const;
    int menuOffset() const;
    int sliderY() const;
    int achievementVisibleCount() const;
    std::vector<int> filteredAchievementIndices() const;
    void cycleAchievementFilter();
    void toggleHiddenAchievementDetails();

    HWND window_ = nullptr;
    HHOOK keyboardHook_ = nullptr;
    HANDLE gameProcess_ = nullptr;
    HANDLE sharedMapping_ = nullptr;
    InputSharedState* sharedState_ = nullptr;
    HANDLE achievementMapping_ = nullptr;
    AchievementSharedState* achievementState_ = nullptr;
    DWORD gamePid_ = 0;
    bool enabled_ = false;
    bool shown_ = false;
    bool systemBackdrop_ = false;
    int selectedItem_ = 0;
    enum class Screen { MainBar, Volume, Achievements, Settings, Confirmation } screen_ = Screen::MainBar;
    int achievementSelection_ = 0;
    enum class AchievementFilter { All, Locked, Unlocked } achievementFilter_ = AchievementFilter::All;
    bool revealHiddenAchievements_ = false;
    LONG achievementGeneration_ = -1;
    int settingsSelection_ = 0;
    int confirmationSelection_ = 0;
    IAudioEndpointVolume* endpointVolume_ = nullptr;
    float volumeLevel_ = 0.0f;
    bool draggingVolume_ = false;
    bool closingGame_ = false;
    ULONGLONG nextSteamFocusAttempt_ = 0;
    WORD previousControllerButtons_[XUSER_MAX_COUNT]{};
    IGameInput* gameInput_ = nullptr;
    GameInputGamepadButtons previousGameInputButtons_ = GameInputGamepadNone;
    WORD previousSharedControllerButtons_ = 0;
    int heldNavigationDirection_ = 0;
    ULONGLONG nextNavigationRepeat_ = 0;
    HDC frameDc_ = nullptr;
    HBITMAP frameBitmap_ = nullptr;
    void* framePixels_ = nullptr;
    int frameWidth_ = 0;
    int frameHeight_ = 480;
    std::vector<std::uint32_t> blurredBackground_;
    IconImage icons_[3];
    IconImage buttonIcons_[3];
    static OverlayWindow* activeInstance_;
};
