#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include <memory>
#include "lyric_provider.hpp"
#include <mutex>
#include <functional>
#include <unordered_set>

namespace lyric_core {

// 逐字歌词片元 (KRC 独有的逐字卡拉OK时间轴)
struct LyricSyllable {
    std::wstring text;          // 字词文本
    uint32_t start_ms = 0;      // 绝对开始时间 (毫秒)
    uint32_t duration_ms = 0;   // 演唱持续时间 (毫秒)
};

// 单行歌词
struct LyricLine {
    uint32_t start_ms = 0;      // 行绝对开始时间 (毫秒)
    uint32_t duration_ms = 0;   // 行总持续时间 (毫秒)
    std::wstring full_text;     // 纯文本歌词
    std::vector<LyricSyllable> syllables; // 逐字染色信息 (仅 KRC 有, 普通 LRC 为空)
    std::wstring translation;   // 中文翻译 (KRC [language:] 中 type 1;双语 LRC 中同时间戳的第二行)
    std::wstring romaji;        // 音译 / 罗马音 (KRC [language:] 中 type 0)
};

// 完整解析后的歌词文档
struct ParsedLyrics {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring by;
    int32_t offset_ms = 0;
    std::vector<LyricLine> lines;

    bool IsEmpty() const { return lines.empty(); }

    bool HasTranslation() const;

    // 导出为国际标准通用 LRC 文本 (去除逐字标签, 输出 [mm:ss.xx] 纯文本时间轴)
    // withTranslation: 有翻译时在原文行后追加同时间戳的翻译行 (双语 LRC)
    std::string ToStandardLrc(bool withTranslation = false) const;

    // 根据当前音频播放位置 (秒), 二分查找当前活跃歌词行索引 (-1 表示未开始)
    int FindLineIndexAt(double seconds) const;

    // 获取某一行当前正在唱的字索引 (-1 表示未唱或该行无逐字信息)
    int FindSyllableIndexAt(size_t lineIndex, double seconds) const;
};

// 酷狗本地歌词路径智能探测器 (不硬编码, 多级注册表/INI/盘符全域嗅探)
class KuGouPathFinder {
public:
    // 执行全系统多级智能嗅探, 返回所有真实存在的酷狗歌词库路径
    static std::vector<std::wstring> DiscoverLyricDirectories();

    // 手动追加自定义歌词候选目录
    static void AddCustomDirectory(const std::wstring& dir);

private:
    static bool ProbeFromKuGouIni(std::vector<std::wstring>& outDirs);
    static bool ProbeFromRegistry(std::vector<std::wstring>& outDirs);
    static bool ProbeFromLogicalDrives(std::vector<std::wstring>& outDirs);
    static bool ProbeFromSpecialFolders(std::vector<std::wstring>& outDirs);

    static std::vector<std::wstring> custom_dirs_;
    static std::mutex mutex_;
};

// KRC (Kugou ResourCe) 解密与解析器
class KrcDecoder {
public:
    // 从文件解码:以 krc1 开头按酷狗 KRC 解密,否则按 LRC 文本解析 (在线缓存的 .lrc)
    static bool DecodeFile(const std::wstring& krcFilePath, ParsedLyrics& outLyrics);

    // 从二进制内存流解码 (用于流式或者包内提取)
    static bool DecodeMemory(const uint8_t* data, size_t size, ParsedLyrics& outLyrics);

    // 将已解密的原始 KRC 文本解析为结构化歌词 (含 [language:] 中文翻译 / 音译)
    static bool ParseRawKrcText(const std::string& rawUtf8Text, ParsedLyrics& outLyrics);

    // 解析普通 LRC 文本 (酷狗在线接口 fmt=lrc 的返回), 同一时间戳的第二行视为翻译
    static bool ParseLrcText(const std::string& utf8Text, ParsedLyrics& outLyrics);

private:
    // 酷狗 16 字节异或密钥
    static const uint8_t KRC_KEY[16];
};

// 酷狗在线歌词接口:
//   1. GET http://krcs.kugou.com/search?ver=1&man=yes&client=pc&keyword=歌手 - 歌名&duration=毫秒&hash=
//      -> {"status":200,"candidates":[{"id","accesskey","song","singer","duration"}...]}
//   2. GET http://krcs.kugou.com/download?ver=1&client=pc&id=&accesskey=&fmt=krc|lrc&charset=utf8
//      -> {"status":200,"content":"<Base64>","fmt":"krc"}; krc 解码后是 krc1 加密流, lrc 解码后是 UTF-8 文本
//   备用域名 lyrics.kugou.com。hash 是酷狗曲库里原始音频的 hash, 转换后的文件算不出来, 通常留空
struct KuGouLyricCandidate {
    std::string id;
    std::string accesskey;
    std::wstring song;
    std::wstring singer;
    uint32_t duration_ms = 0;
};

class KuGouOnlineLyrics {
public:
    // HTTP GET: 成功 (HTTP 200) 时把响应体写入 body。默认实现为 WinHTTP (非 Windows 平台返回 false)
    using HttpGetFn = std::function<bool(const std::string& url, std::string& body)>;

    explicit KuGouOnlineLyrics(HttpGetFn http = nullptr);

    bool Search(const std::wstring& keyword, uint32_t durationMs, const std::string& hash,
                std::vector<KuGouLyricCandidate>& out) const;

    // fmt 为 "krc" 或 "lrc"; 返回 Base64 解码后的原始字节
    bool Download(const KuGouLyricCandidate& c, const std::string& fmt, std::string& outBytes) const;

    // 从前几项候选中挑选时长最接近的一项 (相差超过 8 秒视为别的版本, 返回 -1)
    static int PickCandidate(const std::vector<KuGouLyricCandidate>& list, uint32_t durationMs);

    static bool DefaultHttpGet(const std::string& url, std::string& body);

private:
    bool GetWithFallback(const std::string& pathAndQuery, std::string& body) const;
    HttpGetFn http_;
};

// 统一歌词管理器 (严格匹配、内存缓存、实时定位、LRC 导出)。所有公开方法线程安全。
class LyricManager {
public:
    LyricManager();
    ~LyricManager() = default;

    // 刷新并重新探测本地酷狗歌词库 (同时清空文件索引)
    void RefreshDirectories();

    // 获取当前探测到的所有有效歌词目录
    std::vector<std::wstring> GetLyricDirectories() const;

    // 为歌曲查找并载入酷狗 .krc 歌词:先用标签里的歌名 / 歌手,再用文件名 (「歌手 - 歌名.flac」)。
    // 歌名须规范化后完全一致;双方都有歌手时歌手也须相符,宁缺毋错。
    bool LoadLyricsForTrack(const std::wstring& audioFilePath, const std::wstring& title, const std::wstring& artist, ParsedLyrics& outLyrics);

    // 本地酷狗歌词库没有时走酷狗在线接口 (优先 KRC 以保留逐字时间轴与中文翻译, 失败再取 LRC)。
    // 拉到的 KRC 原样写入缓存目录「歌手 - 歌名.krc」, 之后由 LoadLyricsForTrack 直接命中, 不再联网
    bool FetchOnline(const std::wstring& audioFilePath, const std::wstring& title, const std::wstring& artist,
                     uint32_t durationMs, ParsedLyrics& outLyrics);

    // 多源聚合拉取:依次查询 LRCLIB / 网易云 / 酷狗,统一排名后按名次拉取,只采用歌名对得上的带时间轴歌词。
    // 结果写入缓存目录「歌手 - 歌名」(酷狗 KRC 写 .krc,其他源写带翻译的 .lrc),之后 LoadLyricsForTrack 直接命中;
    // 确实查不到的曲目本次运行内不再重复请求。picked 非空时返回采用的候选 (用于界面显示来源)
    bool FetchOnlineMulti(const std::wstring& audioFilePath, const std::wstring& title, const std::wstring& artist,
                          uint32_t durationMs, ParsedLyrics& outLyrics, LyricCandidate* picked = nullptr);

    // 把任一歌词源拉到的载荷转成结构化歌词:KRC 解密、LRC 解析 (另附的翻译按时间戳并入),纯文本按行切分 (无时间轴)
    static bool ParseDoc(const RawLyricDoc& doc, ParsedLyrics& outLyrics);

    // 外语歌词 (假名 / 谚文占 CJK 两成以上,或以拉丁字母为主、几乎没有汉字);中文歌夹几句日文不算
    static bool IsForeignLyrics(const ParsedLyrics& lyrics);
    // 外语且没有翻译:值得在线找一份带翻译的
    static bool NeedsTranslation(const ParsedLyrics& lyrics);
    // 时间轴一致程度:local 的正文行里,能在 online 中前后 1 秒内找到同一句的比例 (0~1)。
    // 各家断句不同 (酷狗常把一行拆成两行),按二字组重合判同一句
    static double TimelineAgreement(const ParsedLyrics& local, const ParsedLyrics& online);

    // 本地歌词 (同名 .lrc / 内嵌) 是外语又没有翻译时,在线找一份带翻译、且时间轴与本地一致 (>= 60% 行对得上) 的版本。
    // 找到过的存在翻译缓存里,下次不联网;确认没有的 30 天内不再查;对不上的版本宁可不用 (本地兜底)。
    // allowNetwork=false 时只查缓存 (几个小文件,可在界面线程里调用,载入时直接显示,不先闪一下本地版)
    bool FindTranslatedVersion(const ParsedLyrics& local, const std::wstring& audioFilePath, const std::wstring& title,
                               const std::wstring& artist, uint32_t durationMs, ParsedLyrics& outLyrics,
                               LyricCandidate* picked = nullptr, bool allowNetwork = true);

    // 在线歌词缓存目录 (同时加入本地歌词库索引)
    void SetCacheDirectory(const std::wstring& dir);

    // 替换在线接口的 HTTP 实现 (测试用)
    void SetHttpGetter(KuGouOnlineLyrics::HttpGetFn http);

    // 把歌词存为带 BOM 的标准 .lrc (含翻译行),先写临时文件再替换,不会留下半截文件
    bool ExportLrcFile(const ParsedLyrics& lyrics, const std::wstring& targetLrcPath);

    // 在已探测的歌词库中搜索最匹配的 .krc / .lrc 文件 (找不到返回空)
    std::wstring FindMatchingKrcFile(const std::wstring& audioFilePath, const std::wstring& title, const std::wstring& artist);

private:
    struct KrcEntry {
        std::wstring title;   // 规范化后的歌名
        std::wstring artist;  // 规范化后的歌手
        std::wstring path;
        uint64_t mtime = 0;
    };
    bool EnsureIndexLocked(bool onMiss); // 返回是否重建了索引
    std::vector<uint64_t> DirStampsLocked() const;
    std::wstring MatchLocked(const std::wstring& title, const std::wstring& artist) const;
    std::wstring TranslationDirLocked() const;
    void LoadTranslationMissesLocked();
    void SaveTranslationMissesLocked();
    std::unordered_map<std::wstring, int64_t> tr_misses_; // 确认没有翻译的曲目 -> 记录时间 (秒)
    bool tr_misses_loaded_ = false;

    std::vector<std::wstring> search_dirs_;
    std::wstring cache_dir_;
    KuGouOnlineLyrics::HttpGetFn http_;
    std::unordered_set<std::wstring> online_misses_; // 本次运行中在线确实查不到的曲目, 不重复请求
    std::vector<KrcEntry> index_;
    std::vector<uint64_t> dir_stamps_; // 建索引时各歌词目录的修改时间
    int64_t index_built_at_ = 0;   // 秒 (steady clock),0 表示尚未建立
    std::unordered_map<std::wstring, ParsedLyrics> cache_; // key: .krc 路径,容量有上限
    mutable std::mutex mutex_;
};

} // namespace lyric_core
