find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Quick QuickControls2 ShaderTools Svg)
qt_standard_project_setup()

qt_add_executable(KugouPlayer
    src/main.cpp
    src/library_service.h src/library_service.cpp
    src/models.h src/models.cpp
    src/cover_provider.h src/cover_provider.cpp
    src/player_controller.h src/player_controller.cpp
    src/window_helper.h src/window_helper.cpp
    src/playlist_service.h src/playlist_service.cpp
    src/playlist_importer.h src/playlist_importer.cpp
    src/decrypt_service.h src/decrypt_service.cpp
    src/cover_service.h src/cover_service.cpp
    src/folder_watcher.h src/folder_watcher.cpp
    src/settings_service.h src/settings_service.cpp
    src/library_tools.h src/library_tools.cpp
    src/taskbar_helper.h src/taskbar_helper.cpp
    src/frame_diag.h src/frame_diag.cpp
    src/render_guard.h src/render_guard.cpp
)

set_source_files_properties(qml/Theme.qml qml/Nav.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(KugouPlayer
    URI PlayerUi
    VERSION 1.0
    RESOURCE_PREFIX /qt/qml
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/TitleBar.qml
        qml/SideNav.qml
        qml/AlbumWall.qml
        qml/CoverBurst.qml
        qml/SmoothPan.qml
        qml/AlbumCard.qml
        qml/AlbumDetail.qml
        qml/TrackRow.qml
        qml/PlayIsland.qml
        qml/SignalChain.qml
        qml/Spectrum.qml
        qml/SeekBar.qml
        qml/IconButton.qml
        qml/Icon.qml
        qml/QualityBadge.qml
        qml/RoundedImage.qml
        qml/Aurora.qml
        qml/Vinyl.qml
        qml/SoftShadow.qml
        qml/EdgeProgress.qml
        qml/VolumeControl.qml
        qml/NowPlaying.qml
        qml/HeroBanner.qml
        qml/ScanToast.qml
        qml/Onboarding.qml
        qml/LibraryPanel.qml
        qml/Nav.qml
        qml/PlayingBars.qml
        qml/SmoothScroll.qml
        qml/TrackMenu.qml
        qml/TrackInfo.qml
        qml/Toast.qml
        qml/LetterRail.qml
        qml/SongsPage.qml
        qml/ArtistsPage.qml
        qml/ArtistDetail.qml
        qml/Mosaic.qml
        qml/PlaylistsPage.qml
        qml/PlaylistDetail.qml
        qml/SearchPage.qml
        qml/ToolsPage.qml
        qml/CrossfadeCover.qml
        qml/Backdrop.qml
        qml/QueuePanel.qml
        qml/Capsule.qml
        qml/SettingsPage.qml
        qml/DecryptPrompt.qml
        qml/DecryptToast.qml
        qml/OrganizePage.qml
        qml/DedupePage.qml
        qml/DesktopLyrics.qml
        qml/AlbumStrip.qml
        qml/CoverViewer.qml
        qml/CoverPanel.qml
        qml/HoverTip.qml
        qml/SortPopup.qml
        qml/LyricSourcePanel.qml
        qml/EqualizerPanel.qml
        qml/PlaylistImport.qml
        qml/SortTab.qml
        qml/DetailBar.qml
)

qt_add_shaders(KugouPlayer "shaders"
    PREFIX "/"
    FILES
        shaders/grain.frag
        shaders/rounded.frag
        shaders/frosted.frag
        shaders/aurora.frag
        shaders/vinyl.frag
        shaders/sheen.frag
        shaders/glow.frag
        shaders/ring.frag
        shaders/tint.frag
        shaders/wipe.frag
        shaders/backdrop.frag
        shaders/fade.frag
)

target_link_libraries(KugouPlayer PRIVATE player_core Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2 Qt6::Svg)
target_include_directories(KugouPlayer PRIVATE src)

if(MSVC)
    target_compile_options(KugouPlayer PRIVATE /utf-8 /permissive- /Zc:__cplusplus)
    target_compile_definitions(KugouPlayer PRIVATE NOMINMAX)
endif()
if(WIN32)
    target_link_libraries(KugouPlayer PRIVATE dwmapi ole32 dbghelp)
    # 程序图标 (资源管理器、任务栏固定图标)
    target_sources(KugouPlayer PRIVATE res/app.rc)
    set_target_properties(KugouPlayer PROPERTIES WIN32_EXECUTABLE ON)
endif()

file(GLOB PLAYER_ICONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} icons/*.svg)
qt_add_resources(KugouPlayer "icons" PREFIX "/" FILES ${PLAYER_ICONS})
qt_add_resources(KugouPlayer "images" PREFIX "/" FILES images/shadow.png images/app_icon_256.png)
