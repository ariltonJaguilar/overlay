cmake_minimum_required(VERSION 3.20)

project(GameOverlayMvp LANGUAGES CXX)

add_subdirectory(third_party/minhook EXCLUDE_FROM_ALL)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(GameOverlayManager
    src/manager/main.cpp
    src/manager/SteamDiscovery.cpp
    src/manager/SteamDiscovery.h
)
add_executable(Overlay WIN32
    src/overlay/main.cpp
    src/overlay/OverlayWindow.cpp
    src/overlay/OverlayWindow.h
)
add_library(OverlayInputHook SHARED
    src/input_hook/dllmain.cpp
    src/common/InputShared.h
)

target_include_directories(Overlay PRIVATE src/common)
target_include_directories(OverlayInputHook PRIVATE src/common)
target_link_libraries(Overlay PRIVATE shell32 dwmapi xinput9_1_0 gameinput msimg32 windowscodecs ole32 PowrProf Advapi32)
target_link_libraries(OverlayInputHook PRIVATE minhook)

add_custom_command(TARGET Overlay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets/icons"
            "$<TARGET_FILE_DIR:Overlay>/assets/icons"
)

target_compile_definitions(GameOverlayManager PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(Overlay PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_definitions(OverlayInputHook PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)

if(MSVC)
    target_compile_options(GameOverlayManager PRIVATE /W4 /permissive-)
    target_compile_options(Overlay PRIVATE /W4 /permissive-)
    target_compile_options(OverlayInputHook PRIVATE /W4 /permissive-)
endif()
