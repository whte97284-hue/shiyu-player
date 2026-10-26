# 曲库扫描链路 — 代码展示

从「用户点扫描」到「曲目进库」的完整实现，共 23 个文件、约 7300 行。

复制自 `F:\kpgui`（Qt 6.12 播放器），保持原样未做任何修改，仅供阅读/评审。

---

## 目录结构

```
scan_showcase/
├── ui/          界面层：触发扫描、读设置、把结果喂给列表模型
├── core/        扫描主流程 + 遍历 + 标签解析 + 曲目构建
└── include/     对应的头文件（数据结构定义都在这里）
```

---

## 调用链

```
ui/library_service.cpp                     触发扫描、读线程数设置
  └ core/audio_ingestion_pipeline.cpp      ★ 核心：枚举 → 分发 → 多线程解析 → 去重 → 入库
      ├ core/platform_io.cpp               递归枚举目录、读文件头、探测磁盘类型
      ├ core/scan_cache.cpp                命中检查（命中就跳过解析）
      ├ core/audio_tag_parser.cpp          格式分发
      │   ├ core/tag_flac_ogg.cpp          FLAC / Ogg Vorbis
      │   ├ core/tag_id3_mpeg.cpp          MP3 / ID3v2
      │   ├ core/tag_mp4_ape_ncm.cpp       MP4 / APE / NCM
      │   ├ core/tag_riff_dsd.cpp          WAV / DSD（RIFF）
      │   ├ core/tag_common.cpp            通用解码 + 封面嗅探
      │   └ core/cue_sheet.cpp             CUE 分轨
      └ core/music_catalog.cpp             BuildTrack：清洗 → 专辑分组 → 拼音 → 搜索 key
          ├ core/song_cleaner.cpp          名字清洗（去 [FLAC] 噪点、从文件名猜歌手/歌名）
          └ core/pinyin_table.cpp          拼音查表
```

---

## 各文件职责

### ui/

| 文件 | 行数 | 说明 |
|---|---|---|
| `library_service.cpp` | 1069 | 触发扫描；`library/threads` 设置（0 = 自动）；扫描完成后重建 AlbumModel / TrackCollectionModel |
| `library_service.h` | — | 对外接口 |

### core/ — 调度与 IO

| 文件 | 行数 | 说明 |
|---|---|---|
| **`audio_ingestion_pipeline.cpp`** | **622** | **整个扫描的中枢**。见下方「关键实现」 |
| `platform_io.cpp` | 468 | `EnumerateFilesRecursive`（`FindFirstFileExW`，宽字符）；`ReadOnlyFile` 随机读；`QuerySeekPenalty` 判断机械盘/SSD |
| `scan_cache.cpp` | 199 | `KPSCACHE` 二进制缓存。格式：`magic(8) + u32 version + u32 count`，然后逐条 `WStr path / u64 size / u64 mtime / u8 ok / WStr target / 11×WStr / ...`；`WStr = u32 长度 + UTF-8` |

### core/ — 标签解析

| 文件 | 行数 | 说明 |
|---|---|---|
| `audio_tag_parser.cpp` | 370 | 入口 `ParseFile` + `Dispatch`（按扩展名/魔数分流） |
| `tag_common.cpp` | — | `DecodeUtf8 / DecodeUtf16 / DecodeLatin1 / DecodeCodePage / DecodeLegacy`（编码回退链）；`SniffImageMime`；`TrimTag` |
| `tag_flac_ogg.cpp` | — | FLAC Vorbis comment、Ogg Vorbis |
| `tag_id3_mpeg.cpp` | — | ID3v2.2/2.3/2.4 + ID3v1；`DecodeId3Strings` 按帧头编码字节解码 |
| `tag_mp4_ape_ncm.cpp` | — | MP4 atoms、APE tag、NCM 加密容器 |
| `tag_riff_dsd.cpp` | — | WAV / DSD。**`ParseWav` 会扫 `LIST/INFO` 和 `id3 ` 两种标签块，ID3 优先** |
| `cue_sheet.cpp` | 123 | CUE 分轨解析 |

### core/ — 曲目构建

| 文件 | 行数 | 说明 |
|---|---|---|
| `music_catalog.cpp` | 383 | `BuildTrack`：调 `SongCleaner::Clean` → 生成 TrackItem → 拼音 → 搜索 key → 专辑分组 |
| `song_cleaner.cpp` | 200 | `Clean`（标题去噪点、缺字段时从文件名/目录名猜）、`GuessFromFileName`、`GuessFromFolder` |
| `pinyin_table.cpp` | 162 | 拼音/罗马音转换，单字符 O(1) 查表 |

> 注：拼音大表 `pinyin_data.inc`（约 2 万行纯数据）没有复制，只在 `pinyin_table.cpp` 里 `#include`。

---

## 关键实现（想改性能看这段）

### 1. 存储感知的并发度 — `audio_ingestion_pipeline.cpp:316-330`

```cpp
unsigned hw = std::max(2u, std::thread::hardware_concurrency());
summary.storage_seek_penalty = platform::QuerySeekPenalty(inputs.front());
size_t numThreads = requestedThreads;
if (numThreads == 0) {
    if (penalty == 1)      numThreads = 4;                      // 机械盘：适度队列深度利用 NCQ
    else if (penalty == 0) numThreads = std::min(hw * 2, 32);   // 固态盘：I/O 并行
    else                   numThreads = std::min(hw, 8);        // 未知
}
```

`library/threads` 设置成非 0 时直接用它，跳过自动判断。

### 2. 任务分发 — `audio_ingestion_pipeline.cpp:333` 起

无锁原子索引 `std::atomic<size_t> nextIndex`，worker 自己抢下标，没有队列锁竞争。

### 3. 缓存命中 — `audio_ingestion_pipeline.cpp:228`

```cpp
const bool useCache = !cachePath.empty() && lazy;
```

**只有懒加载封面时才启用缓存** —— 因为缓存里不存封面数据，只存"封面在文件的哪个偏移、多大"。

### 4. 懒加载封面 — `library_service.cpp:466`

```cpp
job->pipeline.SetLazyCoverExtract(true);
```

扫描时 `load_cover = false`，不读封面图片本身。4900 首歌的封面可能有几个 GB，这一步省掉的 IO 是最多的。

### 5. 去重入库 — `audio_ingestion_pipeline.cpp:544` 起

扫描过程中只往 `collected` 里堆，全部扫完再按「规范化路径 + CUE 起点」去重，然后一次性 `AddPreparedTracks`。避免边扫边入库导致的重复项和列表抖动。

---

## 常见改动指引

| 想改什么 | 看哪个文件 |
|---|---|
| 扫描太慢（线程数、分批入库） | `core/audio_ingestion_pipeline.cpp`，重点 316-330 |
| 某个格式标签读错 | 对应的 `core/tag_*.cpp` |
| 专辑名/歌手名不对 | `core/song_cleaner.cpp` 的 `Clean()`、`core/music_catalog.cpp` 的 `BuildTrack()` |
| 日文/繁体乱码或问号 | `core/tag_common.cpp` 的 `DecodeLegacy()`；WAV 还要看 `core/tag_riff_dsd.cpp` |
| 漏扫某些文件 | `core/platform_io.cpp` 的枚举 + `core/audio_ingestion_pipeline.cpp` 的扩展名过滤 |
| 缓存不生效 / 需要强制重扫 | `include/scan_cache.hpp` 里的 `kParserVersion`，加 1 即让旧缓存全部失效 |
