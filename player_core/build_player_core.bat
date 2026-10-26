@echo off
setlocal
cd /d "%~dp0."

set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat

if not exist "%VCVARS%" goto :no_vcvars

call "%VCVARS%" >nul
if errorlevel 1 goto :fail_vcvars

set PARSER_SRC=src\platform_io.cpp src\tag_common.cpp src\tag_id3_mpeg.cpp src\tag_flac_ogg.cpp src\tag_riff_dsd.cpp src\tag_mp4_ape_ncm.cpp src\audio_tag_parser.cpp src\cue_sheet.cpp
set CORE_SRC=%PARSER_SRC% src\music_catalog.cpp src\song_cleaner.cpp src\pinyin_table.cpp src\playlist_manager.cpp src\play_queue_controller.cpp
set P2_SRC=%CORE_SRC% src\fft_visualizer.cpp src\audio_playback_engine.cpp src\audio_ingestion_pipeline.cpp src\scan_cache.cpp src\decryptor.cpp
set CFLAGS=/nologo /utf-8 /std:c++17 /Zc:__cplusplus /permissive- /O2 /EHsc /MD /Gy

echo [1/6] Building test_runner.exe ...
cl.exe %CFLAGS% test\test_runner.cpp %CORE_SRC% /Fe:test\test_runner.exe /link /OPT:REF
if errorlevel 1 goto :fail_build1

echo [2/6] Building test_phase2_runner.exe ...
cl.exe %CFLAGS% test\test_phase2_runner.cpp %P2_SRC% /Fe:test\test_phase2_runner.exe /link /OPT:REF ole32.lib winmm.lib user32.lib
if errorlevel 1 goto :fail_build2

echo [3/6] Building test_hires_runner.exe ...
cl.exe %CFLAGS% test\test_hires_runner.cpp %P2_SRC% /Fe:test\test_hires_runner.exe /link /OPT:REF ole32.lib winmm.lib user32.lib
if errorlevel 1 goto :fail_build3

echo [4/6] Building library_audit.exe ...
cl.exe %CFLAGS% tests\library_audit.cpp %PARSER_SRC% /Fe:test\library_audit.exe /link /OPT:REF
if errorlevel 1 goto :fail_build4

echo [5/6] Building test_lyric_runner.exe ...
cl.exe %CFLAGS% test\test_lyric_runner.cpp src\lyric_manager.cpp src\lyric_provider.cpp src\tinf.cpp %PARSER_SRC% /Fe:test\test_lyric_runner.exe /link /OPT:REF shell32.lib advapi32.lib ole32.lib winhttp.lib
if errorlevel 1 goto :fail_build5

echo [6/6] Building test_kugou_bridge_runner.exe ...
cl.exe %CFLAGS% test\test_kugou_bridge_runner.cpp src\kugou_track_bridge.cpp src\lyric_manager.cpp src\lyric_provider.cpp src\tinf.cpp src\decryptor.cpp %PARSER_SRC% /Fe:test\test_kugou_bridge_runner.exe /link /OPT:REF shell32.lib advapi32.lib ole32.lib winhttp.lib
if errorlevel 1 goto :fail_build6

del /q *.obj test\*.obj 2>nul
echo.
echo [OK] Build succeeded.
goto :end

:no_vcvars
echo [ERROR] Cannot find vcvars64.bat
exit /b 1

:fail_vcvars
echo [ERROR] Failed to initialize MSVC x64 environment
exit /b 1

:fail_build1
echo [ERROR] Failed to build test_runner.exe
exit /b 1

:fail_build2
echo [ERROR] Failed to build test_phase2_runner.exe
exit /b 1

:fail_build3
echo [ERROR] Failed to build test_hires_runner.exe
exit /b 1

:fail_build4
echo [ERROR] Failed to build library_audit.exe
exit /b 1

:fail_build5
echo [ERROR] Failed to build test_lyric_runner.exe
exit /b 1

:fail_build6
echo [ERROR] Failed to build test_kugou_bridge_runner.exe
exit /b 1

:end
endlocal
