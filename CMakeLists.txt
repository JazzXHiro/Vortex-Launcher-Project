cmake_minimum_required(VERSION 3.16)
project(Vortex VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ADD THESE THREE LINES:
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# 1. Find the Qt6 packages used by the C++ entry point and QML files.
find_package(Qt6 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 QuickLayouts QuickDialogs2 QuickEffects)

# 2. Tell the compiler to look in the 'source' folder for your friend's headers (.h)
include_directories(source)

option(VORTEX_DEPLOY_QT "Run windeployqt after building VortexLauncher" ON)

set(VORTEX_ENGINE_SOURCES
    source/app_paths.cpp
    source/game_manager.cpp
    source/idle_tracker.cpp
    source/igdb_manager.cpp
    source/json_text.cpp
    source/metadata_manager.cpp
    source/preference_manager.cpp
    source/secrets.cpp
    source/stats_manager.cpp
    source/steam_manager.cpp
    source/steamgriddb_manager.cpp
    source/vortex_log.cpp
)

# 2b. Both executables carry game names through std::string as UTF-8, so the
# process code page has to be UTF-8 as well or std::filesystem mangles any
# non-ASCII title on the way to disk. MSVC merges this into the manifest it
# already generates. See vortex.manifest for the full story.
if(MSVC)
    set(VORTEX_MANIFEST "${CMAKE_CURRENT_SOURCE_DIR}/vortex.manifest")
else()
    set(VORTEX_MANIFEST "")
endif()

# 3. Create the UI executable.
# Note: We exclude source/main.cpp because ui/main_ui.cpp is the Qt entry point.
qt_add_executable(VortexLauncher
    ui/main_ui.cpp
    ui/vortex_bridge.cpp
    ui/vortex_bridge.h
    # Gamepad navigation. Declares its QObject inline and includes its own .moc,
    # so AUTOMOC has to see this file — it is deliberately not part of
    # VORTEX_ENGINE_SOURCES, which the CLI builds with AUTOMOC off.
    ui/controller_support.cpp
    ${VORTEX_ENGINE_SOURCES}
    ${VORTEX_MANIFEST}
)

# 3b. Create the original console executable.
add_executable(VortexCLI
    source/main.cpp
    ${VORTEX_ENGINE_SOURCES}
    ${VORTEX_MANIFEST}
)
set_target_properties(VortexCLI PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

# 4. Silence policy warnings and set the modern prefix behavior
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)

# 5. Define the QML module with an explicit prefix
qt_add_qml_module(VortexLauncher
    URI Vortex
    VERSION 1.0
    RESOURCE_PREFIX /
    IMPORTS
        QtQuick
        QtQuick.Controls
        QtQuick.Dialogs
        QtQuick.Effects
        QtQuick.Layouts
    QML_FILES
        ui/main.qml
        ui/GameDetails.qml
        ui/FirstRunWizard.qml
        ui/SettingsWindow.qml
        ui/VortexScrollBar.qml
    RESOURCES
        ui/assets/steam.png
)

# 6. Link everything
target_link_libraries(VortexLauncher PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Network
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickDialogs2
    # MultiEffect, for the blurred details-page banner in GameDetails.qml.
    Qt6::QuickEffects
    Qt6::QuickLayouts
)

if(MSVC)
    # /utf-8 makes MSVC read these sources as UTF-8 instead of the system code
    # page. No string literal here is non-ASCII, so this changes no behaviour on
    # its own -- it keeps the toolchain consistent with the UTF-8 manifest above
    # and silences C4819 on the box-drawing characters in the comment banners.
    target_compile_options(VortexLauncher PRIVATE /EHsc /utf-8)
    target_compile_options(VortexCLI PRIVATE /EHsc /utf-8)
endif()

# The launcher runs the analytics scripts from "analytics/" next to the executable
# (see app_paths.h — all runtime data resolves against the exe directory), so keep
# that copy in sync with the sources on every build.
# Note: copy_directory overwrites same-named files but never deletes extras, so
# runtime output such as recommendations.json is refreshed from source here and
# any database created beside the scripts is left alone.
add_custom_command(TARGET VortexLauncher POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/analytics"
            "$<TARGET_FILE_DIR:VortexLauncher>/analytics"
    COMMENT "Copying analytics scripts next to VortexLauncher"
    VERBATIM
)

if(WIN32)
    target_link_libraries(VortexLauncher PRIVATE winhttp)
    target_link_libraries(VortexCLI PRIVATE winhttp)

    # Locate windeployqt next to the Qt installation we actually configured
    # against; QTDIR is only a fallback since it is rarely set.
    get_target_property(VORTEX_QMAKE_EXECUTABLE Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(VORTEX_QT_BIN_DIR "${VORTEX_QMAKE_EXECUTABLE}" DIRECTORY)

    find_program(WINDEPLOYQT_EXECUTABLE
        NAMES windeployqt
        HINTS "${VORTEX_QT_BIN_DIR}" "$ENV{QTDIR}/bin"
    )

    if(VORTEX_DEPLOY_QT AND WINDEPLOYQT_EXECUTABLE)
        # Without this the exe cannot start: a Debug build links the "d"-suffixed
        # Qt DLLs (Qt6Cored.dll and friends) which do not exist beside the binary.
        add_custom_command(TARGET VortexLauncher POST_BUILD
            COMMAND "${WINDEPLOYQT_EXECUTABLE}"
                "$<IF:$<CONFIG:Debug>,--debug,--release>"
                --qmldir "${CMAKE_CURRENT_SOURCE_DIR}/ui"
                --no-translations
                # Copies msvcp140.dll / vcruntime140*.dll beside the binary so a
                # packaged Release build starts on a machine that has never had
                # the VC++ redistributable installed. Harmless for Debug, where
                # the debug CRT it copies is already unshippable -- only the
                # Release package is ever handed to anyone else.
                --compiler-runtime
                "$<TARGET_FILE:VortexLauncher>"
            COMMENT "Deploying Qt runtime files for VortexLauncher"
            VERBATIM
        )
    elseif(VORTEX_DEPLOY_QT)
        message(WARNING "windeployqt not found; VortexLauncher.exe will not start "
                        "unless the Qt bin directory is on PATH.")
    endif()
endif()
