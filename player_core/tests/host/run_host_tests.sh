#!/usr/bin/env bash
# 跨平台 (Linux/macOS/WSL) 内核自测:真实样本解析对照 + 入库/检索集成测试 + Sanitizer 模糊测试
# 依赖: g++ (C++17), ffmpeg/ffprobe, python3 + pip install mutagen pillow cryptography
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=${1:-build/host}
mkdir -p "$OUT"
CORE="src/platform_io.cpp src/tag_common.cpp src/tag_id3_mpeg.cpp src/tag_flac_ogg.cpp src/tag_riff_dsd.cpp
      src/tag_mp4_ape_ncm.cpp src/audio_tag_parser.cpp src/cue_sheet.cpp src/music_catalog.cpp src/song_cleaner.cpp
      src/pinyin_table.cpp src/audio_ingestion_pipeline.cpp src/scan_cache.cpp src/decryptor.cpp"
SAN="-O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all"

echo "== 生成真实样本 (ffmpeg + mutagen)"
python3 tests/host/make_fixtures.py "$OUT/fx"

echo "== 编译 (ASan + UBSan)"
g++ -std=c++17 $SAN -pthread tests/host/tag_probe.cpp $CORE -o "$OUT/tag_probe"
g++ -std=c++17 $SAN -pthread tests/host/test_core_host.cpp $CORE -o "$OUT/core_host"
g++ -std=c++17 $SAN -pthread tests/host/fuzz_parser.cpp $CORE -o "$OUT/fuzz"
g++ -std=c++17 -O2 -pthread tests/host/test_core_host.cpp $CORE -o "$OUT/core_host_o2"
# 播放引擎:miniaudio 只启用 null 虚拟声卡后端 (无需真实音频设备)
g++ -std=c++17 $SAN -pthread -DMA_ENABLE_ONLY_SPECIFIC_BACKENDS -DMA_ENABLE_NULL tests/host/test_engine_host.cpp \
    src/audio_playback_engine.cpp src/equalizer.cpp src/fft_visualizer.cpp $CORE -o "$OUT/engine_host" -ldl -lm
# 均衡器:miniaudio 的实现在引擎源文件里,一起链接
g++ -std=c++17 $SAN -pthread -DMA_ENABLE_ONLY_SPECIFIC_BACKENDS -DMA_ENABLE_NULL tests/host/test_eq_host.cpp \
    src/audio_playback_engine.cpp src/equalizer.cpp src/fft_visualizer.cpp $CORE -o "$OUT/eq_host" -ldl -lm
g++ -std=c++17 $SAN -pthread tests/host/test_lyric_host.cpp src/lyric_manager.cpp src/lyric_provider.cpp src/tinf.cpp $CORE -o "$OUT/lyric_host"
g++ -std=c++17 $SAN -pthread tests/host/test_lyric_online_host.cpp src/lyric_manager.cpp src/lyric_provider.cpp src/tinf.cpp $CORE -o "$OUT/lyric_online_host"
g++ -std=c++17 $SAN -pthread tests/host/test_kugou_bridge_host.cpp src/kugou_track_bridge.cpp src/lyric_manager.cpp src/lyric_provider.cpp src/tinf.cpp $CORE -o "$OUT/kugou_bridge_host"
g++ -std=c++17 $SAN -pthread tests/host/test_organizer_host.cpp src/library_organizer.cpp $CORE -o "$OUT/organizer_host"
g++ -std=c++17 $SAN -pthread tests/host/test_dedupe_host.cpp src/library_dedupe.cpp $CORE -o "$OUT/dedupe_host"
g++ -std=c++17 $SAN -pthread tests/host/test_playlist_host.cpp src/playlist_provider.cpp src/playlist_matcher.cpp src/library_dedupe.cpp $CORE -o "$OUT/playlist_host"

echo "== 1. 解析对照 (ffprobe 为基准)"
python3 tests/host/check_fixtures.py "$OUT/tag_probe" "$OUT/fx"
echo "== 2. 入库 / 曲库 / 检索 / CUE / KGM 集成测试"
"$OUT/core_host" "$OUT/fx" "$OUT/work"
echo "== 3. 吞吐压测 (O2)"
"$OUT/core_host_o2" "$OUT/fx" "$OUT/work" 165 | sed -n '/\[5\]/,$p'
echo "== 4. 播放引擎 (null 虚拟声卡)"
"$OUT/engine_host" "$OUT/fx"
echo "== 4b. 均衡器 (旁路逐位不变 / 各段响应 / 前级 / 平滑)"
"$OUT/eq_host"
echo "== 5. 歌词内核 (KRC 解密 / 匹配 / 模糊测试)"
"$OUT/lyric_host" "$OUT/lyric_work"
"$OUT/lyric_online_host" "$OUT/lyric_online_work"
echo "== 5b. 酷狗桥接 (缓存 / 同名 / 并发解密)"
"$OUT/kugou_bridge_host" "$OUT/kugou_bridge_work"
echo "== 5c. 整理曲库 (模板 / 冲突 / 交换改名 / 撤销)"
"$OUT/organizer_host" "$OUT/organizer_work"
echo "== 5d. 查找重复歌曲 (五种匹配 / 自动勾选 / 隔离 / 废纸篓 / 硬链接 / 撤销)"
"$OUT/dedupe_host" "$OUT/dedupe_work"
echo "== 5e. 歌单导入 (链接归一化 / 酷狗、网易云与 QQ音乐解析 / 匹配本地曲库)"
"$OUT/playlist_host"
echo "== 6. 变异模糊测试"
"$OUT/fuzz" "${FUZZ_ITERS:-500}" $(ls "$OUT"/fx/* | grep -v manifest)
