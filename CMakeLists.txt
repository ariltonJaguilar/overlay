cmake_minimum_required(VERSION 3.20)

project(GameOverlayMvp LANGUAGES CXX)

add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(OverlayInputHook SHARED
    src/input_hook/dllmain.cpp
    src/common/InputShared.h
    src/common/AchievementShared.h
)

target_include_directories(OverlayInputHook PRIVATE src/common)
target_link_libraries(OverlayInputHook PRIVATE minhook)

target_compile_definitions(OverlayInputHook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_executable(WandDiagnostic src/wand_diagnostic/main.cpp)
    target_link_libraries(WandDiagnostic PRIVATE iphlpapi ws2_32 ole32 oleaut32 uiautomationcore advapi32)
    target_compile_definitions(WandDiagnostic PRIVATE UNICODE _UNICODE NOMINMAX)
    add_executable(WandControlProbe src/wand_control_probe/main.cpp)
    target_link_libraries(WandControlProbe PRIVATE ole32 oleaut32 uiautomationcore)
    target_compile_definitions(WandControlProbe PRIVATE UNICODE _UNICODE NOMINMAX)
    add_executable(GameOverlayManager WIN32
        src/manager/main.cpp
        src/manager/SteamDiscovery.cpp
        src/manager/SteamDiscovery.h
        src/manager/Manager.rc
    )
    target_link_libraries(GameOverlayManager PRIVATE shell32 dwmapi uxtheme)
    target_link_options(GameOverlayManager PRIVATE
        "/MANIFEST:EMBED"
        "/MANIFESTINPUT:${CMAKE_SOURCE_DIR}/src/manager/Manager.manifest"
    )
    add_executable(Overlay WIN32
        src/overlay/main.cpp
        src/overlay/OverlayWindow.cpp
        src/overlay/OverlayWindow.h
        src/overlay/WandIntegration.cpp
        src/overlay/WandIntegration.h
    )
    target_include_directories(Overlay PRIVATE src/common)
    target_link_libraries(Overlay PRIVATE shell32 dwmapi xinput9_1_0 gameinput msimg32 windowscodecs ole32 oleaut32 uiautomationcore iphlpapi ws2_32 PowrProf Advapi32)
    add_custom_command(TARGET Overlay POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/icons" "$<TARGET_FILE_DIR:Overlay>/assets/icons"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/assets/buttons" "$<TARGET_FILE_DIR:Overlay>/assets/buttons"
    )
    add_custom_command(TARGET GameOverlayManager POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/overlay-blacklist.txt" "$<TARGET_FILE_DIR:GameOverlayManager>/overlay-blacklist.txt"
    )
    target_compile_definitions(GameOverlayManager PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(WandDiagnostic PRIVATE /W4 /permissive- /utf-8)
        target_compile_options(WandControlProbe PRIVATE /W4 /permissive- /utf-8)
        target_compile_options(GameOverlayManager PRIVATE /W4 /permissive- /utf-8)
        target_compile_options(Overlay PRIVATE /W4 /permissive- /utf-8)
    endif()
else()
    set_target_properties(OverlayInputHook PROPERTIES OUTPUT_NAME OverlayInputHook32)
    add_executable(GameOverlayInjector32 src/manager/injector_main.cpp)
    target_compile_definitions(GameOverlayInjector32 PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(GameOverlayInjector32 PRIVATE /W4 /permissive-)
    endif()
endif()

if(MSVC)
    target_compile_options(OverlayInputHook PRIVATE /W4 /permissive-)
endif()
