# LyricProvider —— 多源在线歌词聚合内核（初版交付）

## 一、这是什么

一份**自包含、零第三方依赖**的 C++17 歌词多源聚合内核，参照 LDDC（Python）与
MusicPlayer2（MFC）的成熟协议思路自研，**未引用它们任何代码**。

- 头文件：`player_core/include/lyric_provider.hpp`
- 实现：  `player_core/src/lyric_provider.cpp`
- 测试：  `player_core/test/test_lyric_provider.cpp`
- 构建：  `player_core/CMakeLists.txt` 已加入 `src/lyric_provider.cpp`；
           `build_player_core.bat` 第 5/6 步也已加入。

## 二、设计（三层模型）

1. **`LyricCandidate`** —— 跨源统一候选：`source / id / accesskey / title / artist /
   album / duration_ms / match_score / has_synced`。前端只认这一种。
2. **`RawLyricDoc`** —— 跨源统一载荷：`lrc_text / translation_text / romaji_text /
   raw_payload / kind{Lrc|Krc|Plain}`。各源格式差异全部在这里归一。
3. **`LyricProvider`** —— 聚合器：
   - `SearchAll(q)` → 依次查各源，合并 + 去重 + 打分 + 过滤低分，返回 `LyricSearchResult`
   - `Search(source, q, out)` → 单源查询（供「换来源 / 手动搜索」用）
   - `Fetch(candidate, doc)` → 按候选取载荷
   - `SearchAndFetch(q, doc, picked, all)` → 一步到位

打分公式（`RankCandidates`）：歌名命中 +100 / 包含 +40 / 不符 -40；歌手 +60/+25/-20；
时长 ≤2s +50 / ≤5s +30 / ≤8s +10 / 其他 -60；来源酷狗 +6、其余 +4；无时间轴 -10。
**负分候选一律丢弃**（避免无版权曲目返回的噪声）。

## 三、三个源（已实测全部打通）

| 源 | 协议 | 特点 | 实测 |
|---|---|---|---|
| **LRCLIB** | `https://lrclib.net/api/search` + `/api/get/{id}` | 开放 REST，无签名/加密，直接返回 LRC | ✅ 稳定 |
| **KuGou** | 两段式：`songsearch.kugou.com/song_search_v2` 取 hash → `krcs.kugou.com/search` 取 id/accesskey → `/download?fmt=krc` | 返回 KRC，**带逐字卡拉OK时间轴** | ✅ 稳定 |
| **NetEase** | `https://music.163.com/api/search/get` + `/api/song/lyric?lv=-1&tv=-1&rv=-1` | 明文 api 无需加密，返回 lrc + tlyric(翻译) + romalrc | ✅ 稳定 |

### ⚠ 三条踩坑记录（重要）

1. **酷狗旧接口已废弃**。`krcs.kugou.com/search` 直接传 keyword 会返回
   `status=200 但 candidates:[]`。**必须先从 `songsearch.kugou.com/song_search_v2`
   拿 `FileHash`**，再用 hash 调 krcs/search。
2. **酷狗候选的 `id` 是字符串**（`"136759220"`），不是数字。按数字解析会得到 0，
   导致 download 失败（`base64 failed`）。
3. **WinHTTP 不能并发**。曾用 `std::thread` 并行打三个源，结果请求全部失败
   （串行时三源全正常，Python 同 URL 也正常）。**已改为串行**，总耗时 1~2 秒。
   如果以后要并行，每个线程需独立的 session 且要处理连接池。

## 四、实测结果（2026-10-16）

| 曲目 | 酷狗 | LRCLIB | 网易云 |
|---|---|---|---|
| 富士山下 / 陈奕迅 | ✅ 166分 KRC 73行(逐字) | ✅ 164分 41行 | ✅ 63行(含翻译) |
| 起风了 / 买辣椒也用券 | ✅ 166分 | ✅ 164分 45行 | ✅ 70行 |
| Yesterday / The Beatles | ✅ 166分 | ✅ 164分 20行 | ✅ 20行 + 翻译753B |
| 大鱼 / 周深 | ✅ 166分 | ✅ 164分 | ✅ |
| 稻香 / 周杰伦 | ✅ 166分 | ✅ 164分 | ❌ **无版权**(噪声已被过滤) |

**关于网易云**：`/api/search/get` 无需登录即可用，行为完全正常。
搜不到某首歌通常是**该曲在网易云没有版权**（如周杰伦全系已下架），此时返回的
是翻唱/伴奏 —— **这不是接口故障，不要误判为「搜索降级」**。排序层已通过
歌名/歌手/时长把翻唱压到后面，负分候选直接丢弃。

**有意不做**：网易云的逐字歌词（yrc）与 eapi 加密载荷 —— 需复刻客户端 EAPI
加密算法，收益（逐字）与维护成本不成比例。LRC 层已够用；逐字需求由酷狗 KRC 覆盖。

## 五、已接入 LyricManager

新增 `LyricManager::FetchOnlineMulti(audioFilePath, title, artist, durationMs, out)`：
- 复用与旧 `FetchOnline` 相同的搜索词补齐策略（标签缺失时用「歌手 - 歌名」文件名、
  丢占位歌手、去 track number）
- 调 `LyricProvider::SearchAndFetch` 择优
- 载荷按 kind 分发：KRC → `KrcDecoder::DecodeMemory`；LRC/Plain → `KrcDecoder::ParseLrcText`
- 落缓存：酷狗 KRC 存 `.krc`（走索引，下次 `LoadLyricsForTrack` 直接命中）；
  其他源用 `ExportLrcFile` 存 `.lrc`
- **旧 `FetchOnline` 保留未动**，可平滑切换

## 六、可复用的现成能力（支撑「歌词来源选择」UI）

- **保存到本地**：`LyricManager::ExportLrcFile()`（写 UTF-8 BOM，原子替换）
- **候选列表搜索**：`LyricProvider::Search(source, q, out)` —— 已含 song/singer/duration，
  可直接喂给弹层做「聚合搜索选择」
- **统一工具**（原在 `lyric_manager.cpp` 匿名 namespace，现已公开可跨模块复用）：
  `LyricProvider::UrlEncode / Base64Decode / Md5Hex / Normalize / LooksLikeLrc`

## 七、待办 / 建议 Claude 补充

1. **网易云搜索精度**：目前拿的是 `name` 字段（含"(原唱:xx)""(Cover xx)"等后缀），
   可考虑用 `/api/search/get/web` 的 `simpleSongData` 或加 `alia` 字段清洗。
2. **源开关持久化**：`SetSourceEnabled` 已有，但未接设置项。
3. **`applyLyricsText` 的 JSON 行 bug**（`player_controller.cpp:1106` 附近）仍存在：
   `metaRe = ^\[[a-zA-Z]+:.*\]$` 不匹配网易云的 JSON 行，会掉进 plain。内核
   `KrcDecoder::ParseLrcText` 处理是对的，建议对齐。
4. **歌词后缀规则三处不一致**：`loadLyrics()` 只认 `.lrc/.LRC`，
   `LyricsOf()` 认 `.zh.lrc`，`IsCompanionExt()` 认 `.lrc/.krc/.qrc/.trc`，建议统一。
