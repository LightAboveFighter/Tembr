cmake_minimum_required(VERSION 3.16)
project(tembr VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(TEMBR_BUILD_TESTS "Build unit tests" ON)
set(TEMBR_SANITIZE "" CACHE STRING "Sanitizers for a checking build: address,undefined or thread (empty: none)")

# Our C++ only: miniaudio is C and is included as a system header.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options($<$<COMPILE_LANGUAGE:CXX>:-Wall> $<$<COMPILE_LANGUAGE:CXX>:-Wextra>)
endif()
if(TEMBR_SANITIZE)
    add_compile_options(-fsanitize=${TEMBR_SANITIZE} -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${TEMBR_SANITIZE})
endif()

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
    src/core/LocalFiles.h src/core/LocalFiles.cpp
    src/core/FileTrash.h src/core/FileTrash.cpp
    src/core/TrackQuery.h src/core/TrackQuery.cpp
    src/core/MediaOpen.h src/core/MediaOpen.cpp
    src/core/TrackSort.h src/core/TrackSort.cpp
    src/core/ColumnFit.h src/core/ColumnFit.cpp
    src/core/FileTimes.h src/core/FileTimes.cpp
    src/core/SessionFiles.h src/core/SessionFiles.cpp
    src/audio/AudioEngine.h src/audio/AudioEngine.cpp
    src/platform/Mpris.h src/platform/Mpris.cpp
    src/platform/GlobalHotkeys.h src/platform/GlobalHotkeys.cpp
    src/migration/AimpProfiles.h src/migration/AimpProfiles.cpp
    src/migration/PhoneSources.h src/migration/PhoneSources.cpp
    src/migration/PhoneImport.h src/migration/PhoneImport.cpp
)
target_include_directories(tembr_core PUBLIC src)
target_link_libraries(tembr_core PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::DBus PkgConfig::FFMPEG miniaudio)

# --- user interface (a library: the tests build windows from it) -------------
add_library(tembr_ui STATIC
    src/ui/MainWindow.h src/ui/MainWindow.cpp
    src/ui/PlaylistModel.h src/ui/PlaylistModel.cpp
    src/ui/PlaylistFilterProxy.h src/ui/PlaylistFilterProxy.cpp
    src/ui/PlaylistPage.h src/ui/PlaylistPage.cpp
    src/ui/TrackInfoDialog.h src/ui/TrackInfoDialog.cpp
    src/ui/Theme.h src/ui/Theme.cpp
    src/ui/DialogFrame.h
    src/ui/Icons.h src/ui/Icons.cpp
    src/ui/NowPlayingPanel.h src/ui/NowPlayingPanel.cpp
    src/ui/SettingsDialog.h src/ui/SettingsDialog.cpp
    src/ui/HotkeyEdit.h src/ui/HotkeyEdit.cpp
    src/ui/TrackOsd.h src/ui/TrackOsd.cpp
    src/ui/DeleteFilesDialog.h src/ui/DeleteFilesDialog.cpp
    src/ui/LocalFilesDialog.h src/ui/LocalFilesDialog.cpp
    src/ui/MigrationDialog.h src/ui/MigrationDialog.cpp
    src/ui/PlaybackController.h src/ui/PlaybackController.cpp
    src/ui/TransportBar.h src/ui/TransportBar.cpp
)
target_link_libraries(tembr_ui PUBLIC tembr_core Qt6::Widgets)

# --- application -------------------------------------------------------------
add_executable(tembr src/main.cpp)
target_link_libraries(tembr PRIVATE tembr_ui)
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
    foreach(t test_playlistio test_trackquery test_hotkeys test_localfiles test_migration test_mediaopen test_filetrash
            test_columnfit test_audioengine test_sessionfiles)
        add_executable(${t} tests/${t}.cpp)
        target_link_libraries(${t} PRIVATE tembr_core Qt6::Test)
        add_test(NAME ${t} COMMAND ${t})
    endforeach()

    add_executable(test_hotkeyedit tests/test_hotkeyedit.cpp src/ui/HotkeyEdit.cpp src/ui/HotkeyEdit.h)
    target_link_libraries(test_hotkeyedit PRIVATE tembr_core Qt6::Widgets Qt6::Test)
    add_test(NAME test_hotkeyedit COMMAND test_hotkeyedit)
    set_tests_properties(test_hotkeyedit PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(test_osd tests/test_osd.cpp src/ui/TrackOsd.cpp src/ui/TrackOsd.h src/ui/Theme.cpp src/ui/Theme.h)
    target_link_libraries(test_osd PRIVATE tembr_core Qt6::Widgets Qt6::Test)
    add_test(NAME test_osd COMMAND test_osd)
    set_tests_properties(test_osd PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(test_playlistmodel tests/test_playlistmodel.cpp src/ui/PlaylistModel.cpp src/ui/PlaylistModel.h
                   src/ui/Icons.cpp src/ui/Theme.cpp)
    target_link_libraries(test_playlistmodel PRIVATE tembr_core Qt6::Widgets Qt6::Test)
    add_test(NAME test_playlistmodel COMMAND test_playlistmodel)
    set_tests_properties(test_playlistmodel PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_deletedialog tests/test_deletedialog.cpp src/ui/DeleteFilesDialog.cpp src/ui/DeleteFilesDialog.h)
    target_link_libraries(test_deletedialog PRIVATE tembr_core Qt6::Widgets Qt6::Test)
    add_test(NAME test_deletedialog COMMAND test_deletedialog)
    set_tests_properties(test_deletedialog PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_playlistpage tests/test_playlistpage.cpp src/ui/PlaylistPage.cpp src/ui/PlaylistPage.h
                   src/ui/PlaylistModel.cpp src/ui/PlaylistModel.h src/ui/PlaylistFilterProxy.cpp
                   src/ui/PlaylistFilterProxy.h src/ui/Icons.cpp src/ui/Theme.cpp)
    target_link_libraries(test_playlistpage PRIVATE tembr_core Qt6::Widgets Qt6::Test)
    add_test(NAME test_playlistpage COMMAND test_playlistpage)
    set_tests_properties(test_playlistpage PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_executable(test_playbackcontroller tests/test_playbackcontroller.cpp tests/TestAudio.h)
    target_link_libraries(test_playbackcontroller PRIVATE tembr_ui Qt6::Test)
    add_test(NAME test_playbackcontroller COMMAND test_playbackcontroller)
    set_tests_properties(test_playbackcontroller PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;TEMBR_AUDIO_BACKEND=null")

    add_executable(test_transportbar tests/test_transportbar.cpp tests/TestAudio.h)
    target_link_libraries(test_transportbar PRIVATE tembr_ui Qt6::Test)
    add_test(NAME test_transportbar COMMAND test_transportbar)
    set_tests_properties(test_transportbar PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;TEMBR_AUDIO_BACKEND=null")

    add_executable(test_mainwindow tests/test_mainwindow.cpp tests/TestAudio.h)
    target_link_libraries(test_mainwindow PRIVATE tembr_ui Qt6::Test)
    add_test(NAME test_mainwindow COMMAND test_mainwindow)
    set_tests_properties(test_mainwindow PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;TEMBR_AUDIO_BACKEND=null")

    # Manual tools (see the file headers).
    add_executable(playlist_probe tests/playlist_probe.cpp)
    target_link_libraries(playlist_probe PRIVATE tembr_core)

    add_executable(engine_smoke tests/engine_smoke.cpp)
    target_link_libraries(engine_smoke PRIVATE tembr_core)
endif()
