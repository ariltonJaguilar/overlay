#pragma once

#include <windows.h>
#include <xinput.h>
#include <GameInput.h>
#include <endpointvolume.h>

#include "InputShared.h"
#include "AchievementShared.h"

#include <cstdint>
#include <filesystem>
#include <future>
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
    void showStartupCover();
    void updateStartupCover(BYTE opacity);
    void releaseStartupCover();
    bool gameWindowCoversMonitor() const;
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
    std::vector<std::uint32_t> captureLiveBlurFrame() const;
    void renderAndPresent();
    void releaseRenderer();
    bool loadIcons();
    bool loadGameIcon();
    bool loadSteamGameLogo(unsigned int appId);
    bool loadImageFile(const std::filesystem::path& path, int maximumWidth,
                       int maximumHeight, IconImage& destination) const;
    void loadScreenshots();
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
    bool startupCoverActive_ = true;
    ULONGLONG startupCoverDeadline_ = 0;
    ULONGLONG startupFadeInStart_ = 0;
    ULONGLONG startupFadeStart_ = 0;
    BYTE startupCoverOpacity_ = 0;
    BYTE startupFadeOutOpacity_ = 255;
    HDC startupCoverDc_ = nullptr;
    HBITMAP startupCoverBitmap_ = nullptr;
    HGDIOBJ startupCoverOldBitmap_ = nullptr;
    bool systemBackdrop_ = false;
    bool liveCaptureSupported_ = false;
    ULONGLONG nextLiveBlurUpdate_ = 0;
    bool liveBlurInFlight_ = false;
    std::future<std::vector<std::uint32_t>> liveBlurFuture_;
    int selectedItem_ = 0;
    enum class Screen { MainBar, Volume, Achievements, Screenshots, ScreenshotViewer,
                        Settings, Confirmation } screen_ = Screen::MainBar;
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
    unsigned int exitGameAppId_ = 0;
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
    IconImage icons_[4];
    IconImage buttonIcons_[3];
    IconImage gameIcon_;
    unsigned int loadedGameLogoAppId_ = 0;
    int displayedClockMinute_ = -1;
    std::vector<std::filesystem::path> screenshotPaths_;
    std::vector<IconImage> screenshotThumbnails_;
    IconImage screenshotViewerImage_;
    int screenshotSelection_ = 0;
    static OverlayWindow* activeInstance_;
};
