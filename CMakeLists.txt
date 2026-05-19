cmake_minimum_required(VERSION 3.27)

project(hyprexpo
    DESCRIPTION "hyprexpo plugin for Hyprland"
    VERSION 0.1
)

set(CMAKE_CXX_STANDARD 23)

set(HYPREXPO_SRC
    main.cpp
    Dispatchers.cpp
    PluginConfig.cpp
    Overview.cpp
    OverviewInteraction.cpp
    OverviewRender.cpp
    ExpoGesture.cpp
    OverviewPassElement.cpp
    HyprexpoLogic.cpp
)

add_library(hyprexpo SHARED ${HYPREXPO_SRC})

find_package(PkgConfig REQUIRED)
pkg_check_modules(deps REQUIRED IMPORTED_TARGET
    hyprland
    libdrm
    libinput
    libudev
    pangocairo
    pixman-1
    wayland-server
    xkbcommon
    lua5.4
)
target_link_libraries(hyprexpo PRIVATE rt PkgConfig::deps)

enable_testing()
add_executable(HyprexpoLogicTests
    HyprexpoLogic.cpp
    tests/HyprexpoLogicTests.cpp
)
add_test(NAME HyprexpoLogicTests COMMAND HyprexpoLogicTests)

install(TARGETS hyprexpo)
