cmake_minimum_required(VERSION 3.16)
project(tembr VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(TEMBR_BUILD_TESTS "Build unit tests" ON)

find_package(Qt6 6.2 REQUIRED COMPONENTS Core Gui Widgets Concurrent DBus)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
find_package(Threads REQUIRED)

# --- miniaudio: only the playback-device part is used; FFmpeg does decoding --
add_library(miniaudio STATIC src/audio/miniaudio_impl.c)
target_include_directories(miniaudio SYSTEM PUBLIC third_party/miniaudio)
target_compile_definitions(miniaudio PUBLIC
    MA_NO_DECODING MA_NO_ENCODING MA_NO_GENERATION
    MA_NO_RESOURCE_MANAGER MA_NO_NODE_GRAPH MA_NO_ENGINE)
target_link_libraries(miniaudio PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

# --- core: engine, metadata, playlists, search (no widgets) -----------------
add_library(tembr_core STATIC
    src/core/Track.h src/core/Track.cpp
    src/core/MetadataReader.h src/core/MetadataReader.cpp
    src/core/TextDecode.h src/core/TextDecode.cpp
    src/core/PlaylistIO.h src/core/PlaylistIO.cpp
    src/core/MediaCollector.h src/core/MediaCollector.cpp
    src/core/PathResolver.h src/core/PathResolver.cpp
    src/core/Relocator.h src/core/Relocator.cpp
    src/core/TrackQuery.h src/core/TrackQuery.cpp
    src/audio/AudioEngine.h src/audio/AudioEngine.cpp
    src/platform/Mpris.h src/platform/Mpris.cpp
    src/platform/GlobalHotkeys.h src/platform/GlobalHotkeys.cpp
    src/migration/AimpProfiles.h src/migration/AimpProfiles.cpp
    src/migration/PhoneSources.h src/migration/PhoneSources.cpp
    src/migration/PhoneImport.h src/migration/PhoneImport.cpp
)
target_include_directories(tembr_core PUBLIC src)
target_link_libraries(tembr_core PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::DBus PkgConfig::FFMPEG miniaudio)

# --- application -------------------------------------------------------------
add_executable(tembr
    src/main.cpp
    src/ui/MainWindow.h src/ui/MainWindow.cpp
    src/ui/PlaylistModel.h src/ui/PlaylistModel.cpp
    src/ui/PlaylistFilterProxy.h src/ui/PlaylistFilterProxy.cpp
    src/ui/PlaylistPage.h src/ui/PlaylistPage.cpp
    src/ui/TrackInfoDialog.h src/ui/TrackInfoDialog.cpp
    src/ui/Theme.h src/ui/Theme.cpp
    src/ui/Icons.h src/ui/Icons.cpp
    src/ui/NowPlayingPanel.h src/ui/NowPlayingPanel.cpp
    src/ui/SettingsDialog.h src/ui/SettingsDialog.cpp
    src/ui/HotkeyEdit.h src/ui/HotkeyEdit.cpp
)
target_link_libraries(tembr PRIVATE tembr_core Qt6::Widgets)
target_compile_definitions(tembr PRIVATE TEMBR_VERSION="${PROJECT_VERSION}")
install(TARGETS tembr RUNTIME DESTINATION bin)

# --- migration tool ------------------------------------------------------------
add_executable(tembr-migrate tools/tembr-migrate.cpp)
target_link_libraries(tembr-migrate PRIVATE tembr_core)
install(TARGETS tembr-migrate RUNTIME DESTINATION bin)
install(FILES data/tembr.desktop DESTINATION share/applications)
install(FILES data/tembr.svg DESTINATION share/icons/hicolor/scalable/apps)

# --- tests -------------------------------------------------------------------
if(TEMBR_BUILD_TESTS)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    enable_testing()
    foreach(t test_playlistio test_trackquery test_hotkeys test_migration)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE tembr_core Qt6::Test)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    add_executable(test_hotkeyedit tests/test_hotkeyedit.cpp src/ui/HotkeyEdit.cpp src/ui/HotkeyEdit.h)
    target_link_libraries(test_hotkeyedit PRIVATE tembr_core Qt6::Widgets Qt6::Test)
    add_test(NAME test_hotkeyedit COMMAND test_hotkeyedit)
    set_tests_properties(test_hotkeyedit PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    # Manual tools (see the file headers).
    add_executable(playlist_probe tests/playlist_probe.cpp)
    target_link_libraries(playlist_probe PRIVATE tembr_core)

    add_executable(engine_smoke tests/engine_smoke.cpp)
    target_link_libraries(engine_smoke PRIVATE tembr_core)
endif()
