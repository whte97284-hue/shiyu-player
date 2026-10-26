# 播放器内核静态库:解析 / 入库 / 曲库 / 拼音 / CUE / 增量缓存 / 播放引擎 / 频谱
add_library(player_core STATIC
    src/stb_vorbis_impl.cpp
    src/platform_io.cpp
    src/tag_common.cpp
    src/tag_id3_mpeg.cpp
    src/tag_flac_ogg.cpp
    src/tag_riff_dsd.cpp
    src/tag_mp4_ape_ncm.cpp
    src/audio_tag_parser.cpp
    src/cue_sheet.cpp
    src/music_catalog.cpp
    src/song_cleaner.cpp
    src/pinyin_table.cpp
    src/playlist_manager.cpp
    src/play_queue_controller.cpp
    src/scan_cache.cpp
    src/library_organizer.cpp
    src/library_dedupe.cpp
    src/decryptor.cpp
    src/audio_ingestion_pipeline.cpp
    src/fft_visualizer.cpp
    src/audio_playback_engine.cpp
    src/equalizer.cpp
    src/lyric_manager.cpp
    src/lyric_provider.cpp
    src/playlist_provider.cpp
    src/playlist_matcher.cpp
    src/tinf.cpp
    src/kugou_track_bridge.cpp
    src/cover_fetcher.cpp
    src/cover_writer.cpp
    src/cover_ogg.cpp
)
target_include_directories(player_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(player_core PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(player_core PRIVATE /utf-8 /permissive- /Zc:__cplusplus /W3)
    target_compile_definitions(player_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()
find_package(Threads REQUIRED)
target_link_libraries(player_core PUBLIC Threads::Threads)
if(WIN32)
    target_link_libraries(player_core PUBLIC ole32 winmm user32 shell32 advapi32 winhttp)
else()
    target_link_libraries(player_core PUBLIC ${CMAKE_DL_LIBS} m)
endif()
