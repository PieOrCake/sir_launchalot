cmake_minimum_required(VERSION 3.16)
project(sir-launchalot VERSION 1.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Install prefix" FORCE)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network)

set(CORE_SOURCES
    src/core/OverlayManager.cpp
    src/core/AccountManager.cpp
    src/core/AddonScanner.cpp
    src/core/WineManager.cpp
    src/core/InstallDetector.cpp
    src/core/ProcessManager.cpp
    src/core/ProtonResolver.cpp
    src/core/Gw2ApiClient.cpp
    src/core/UpdateChecker.cpp
)

set(CORE_HEADERS
    src/core/OverlayManager.h
    src/core/AccountManager.h
    src/core/AddonScanner.h
    src/core/WineManager.h
    src/core/InstallDetector.h
    src/core/ProcessManager.h
    src/core/ProtonResolver.h
    src/core/Gw2ApiClient.h
    src/core/UpdateChecker.h
)

set(UI_SOURCES
    src/ui/MainWindow.cpp
    src/ui/AccountDialog.cpp
    src/ui/AltSetupWizard.cpp
    src/ui/SettingsDialog.cpp
    src/ui/SetupWizard.cpp
)

set(UI_HEADERS
    src/ui/MainWindow.h
    src/ui/AccountDialog.h
    src/ui/AltSetupWizard.h
    src/ui/SettingsDialog.h
    src/ui/SetupWizard.h
)

# Everything except main() lives in a static library so the test harnesses in
# tests/ can link the real code instead of keeping their own source lists.
add_library(sir-launchalot-lib STATIC
    ${CORE_SOURCES}
    ${CORE_HEADERS}
    ${UI_SOURCES}
    ${UI_HEADERS}
)

target_include_directories(sir-launchalot-lib PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(sir-launchalot-lib PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
)

target_compile_definitions(sir-launchalot-lib PUBLIC
    APP_VERSION="${PROJECT_VERSION}"
)

add_executable(sir-launchalot
    src/main.cpp
)

target_link_libraries(sir-launchalot PRIVATE sir-launchalot-lib)

# Test harnesses. Off by default so the normal build stays a plain app build;
# tests/run-tests.sh turns it on in its own build directory.
option(BUILD_TESTS "Build the test harnesses in tests/" OFF)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# Always package an AppImage after building
add_custom_target(appimage ALL
    COMMAND ${CMAKE_SOURCE_DIR}/scripts/build-appimage.sh --skip-build
    DEPENDS sir-launchalot
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Packaging AppImage..."
)

# Install rules (used by linuxdeploy for AppImage)
install(TARGETS sir-launchalot DESTINATION bin)
install(FILES ${CMAKE_SOURCE_DIR}/resources/sir-launchalot.desktop
        DESTINATION share/applications)
install(FILES ${CMAKE_SOURCE_DIR}/resources/sir-launchalot.svg
        DESTINATION share/icons/hicolor/scalable/apps)
install(FILES ${CMAKE_SOURCE_DIR}/resources/sir-launchalot.png
        DESTINATION share/icons/hicolor/256x256/apps)
