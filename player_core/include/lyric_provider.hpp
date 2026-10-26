#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>
#include <atomic>

// ============================================================================
//  LyricProvider —— 多源在线歌词聚合内核 (自包含,零第三方依赖)
//
//  设计参照 LDDC / MusicPlayer2 的成熟协议,但代码全部自研:
//    · 统一「候选」模型   —— 每个源都归一成 LyricCandidate,前端只认这一种
//    · 统一「文档」模型   —— 各源原始载荷 (LRC / 网易云 lrc+tlrc / 酷狗 krc) 归一成 RawLyricDoc
//    · 多源聚合          —— 依次查询各源,合并候选并按匹配度排序 (串行:WinHTTP 并发不可靠)
//
//  当前实现的源:
//    · LRCLIB   —— 开放 REST,无需签名/加密,响应即为标准 LRC       (最省事,国外曲目强)
//    · KuGou    —— 酷狗,两段式: songsearch 取 hash → krcs 取歌词    (中文覆盖好,带逐字 KRC)
//    · NetEase  —— 网易云,search 走明文 api,歌词 lrc+tlyric(翻译)  (中文覆盖好)
//
//  ⚠ 关于网易云:它的 /api/search/get 无需登录即可用,行为完全正常 ——
//    搜不到某首歌通常是因为「该曲在网易云没有版权」(如周杰伦全系已下架),
//    此时返回的会是翻唱/伴奏。这不是接口故障,任何源都会遇到版权缺曲,
//    排序层已通过歌名/歌手/时长匹配把翻唱压到后面,不要为此误判为「搜索降级」。
//
//  注意:网易云的「逐字」歌词 (yrc / eapi 加密载荷) 有意不做 —— 需要复刻
//  客户端 EAPI 加密算法,收益(逐字)与维护成本不成比例。LRC 层已够用。
// ============================================================================

namespace lyric_core {

// 单条在线歌词候选 (跨源统一)
struct LyricCandidate {
    std::string source;        // "lrclib" / "netease" / "kugou"
    std::string id;            // 源内主键 (酷狗还需要 accesskey)
    std::string accesskey;     // 酷狗用;其余源留空
    std::wstring title;        // 源侧歌名
    std::wstring artist;       // 源侧歌手
    std::wstring album;        // 源侧专辑
    uint32_t duration_ms = 0;  // 源侧时长 (毫秒,0 表示未知)
    int match_score = 0;       // 与查询的匹配度 (由 RankCandidates 算,越大越好)
    bool has_synced = true;    // 是否带时间轴 (false = 纯文本歌词)
    bool title_match = false;  // 歌名与查询对得上 (由 RankCandidates 判定;自动挑选只认对得上的)
};

// 从某个源拉回的原始歌词文档 (已归一,但未解析成 ParsedLyrics)
struct RawLyricDoc {
    std::string source;          // 来源标识
    std::string lrc_text;        // 标准 LRC 文本 (UTF-8);可能为空
    std::string translation_text;// 翻译 LRC 文本 (UTF-8);可能为空 (网易云 tlyric)
    std::string romaji_text;     // 音译 LRC 文本 (UTF-8);可能为空
    std::string raw_payload;     // Krc: 酷狗 krc 二进制 (Base64 解码后);Plain: 无时间轴的纯文本歌词
    enum class Payload { Lrc, Krc, Plain } kind = Payload::Lrc;
};

// 一次完整查询的结果 (供 UI 展示 + 选择)
struct LyricSearchResult {
    std::vector<LyricCandidate> candidates; // 按 match_score 降序
    std::vector<std::string> source_errors; // 失败源及原因 (调试/前端提示用)
    int answered = 0;                       // 有正常应答的源个数 (含「查无结果」);0 多半是断网
};

// 统一的 HTTP GET 钩子 (默认实现为 WinHTTP);测试时可注入假实现
using HttpGetFn = std::function<bool(const std::string& url, std::string& body)>;

// 查询参数 (前端从当前曲目构造)
struct LyricQuery {
    std::wstring title;
    std::wstring artist;
    std::wstring album;       // 可选
    uint32_t duration_ms = 0;
    std::string hash;         // 酷狗 hash,可选
    bool loose = false;       // 手动搜索:关键词不一定是歌名,不按歌名 / 歌手 / 时长扣分,也不丢弃低分候选

    bool Empty() const { return title.empty(); }
    std::wstring Keyword() const; // "歌手 歌名" 拼装
};

// 多源聚合器。查询方法只读成员,可多线程同时调用;源开关请在查询开始前设好 (每次查询各建一个实例最简单)。
class LyricProvider {
public:
    explicit LyricProvider(HttpGetFn http = nullptr);

    // 依次查询所有已启用的源,合并候选并按匹配度排序
    LyricSearchResult SearchAll(const LyricQuery& q) const;

    // 只查某一个源 (前端做「换来源」「聚合链接手动搜索」时用)
    bool Search(const std::string& source, const LyricQuery& q, std::vector<LyricCandidate>& out,
                std::string* error = nullptr) const;

    // 按候选拉取歌词载荷 (归一为 RawLyricDoc)
    bool Fetch(const LyricCandidate& c, RawLyricDoc& out, std::string* error = nullptr) const;

    // 便捷组合:搜索 → 自动挑最匹配 → 拉取
    bool SearchAndFetch(const LyricQuery& q, RawLyricDoc& out, LyricCandidate* picked = nullptr,
                        LyricSearchResult* all = nullptr) const;

    // 候选择优:按 [时长命中 → 歌名/歌手规范化全等 → 来源权重] 打分,写入 match_score 并降序排序
    static void RankCandidates(const LyricQuery& q, std::vector<LyricCandidate>& list);
    static int PickBest(const LyricQuery& q, const std::vector<LyricCandidate>& list);

    // 启用/禁用某个源 (默认全开)
    void SetSourceEnabled(const std::string& source, bool on);
    bool SourceEnabled(const std::string& source) const;
    static std::vector<std::string> KnownSources();

    // 默认 WinHTTP 实现 (非 Windows 返回 false);可作为其他组件的共享 http 钩子
    static bool DefaultHttpGet(const std::string& url, std::string& body);

    // ---- 以下工具函数公开,便于其它模块/测试复用 (原 lyric_manager.cpp 里是匿名实现) ----
    static std::string UrlEncode(const std::string& utf8);
    static bool Base64Decode(const std::string& in, std::string& out);
    static std::string Md5Hex(const std::string& in);           // 酷狗签名/参数用
    static std::wstring Normalize(const std::wstring& s);       // 去修饰、统一大小写与全半角
    static bool LooksLikeLrc(const std::string& text);          // 是否含 [mm:ss] 时间轴

private:
    bool SearchLrclib(const LyricQuery& q, std::vector<LyricCandidate>& out, std::string* error) const;
    bool FetchLrclib(const LyricCandidate& c, RawLyricDoc& out, std::string* error) const;

    bool SearchNetease(const LyricQuery& q, std::vector<LyricCandidate>& out, std::string* error) const;
    bool FetchNetease(const LyricCandidate& c, RawLyricDoc& out, std::string* error) const;

    bool SearchKugou(const LyricQuery& q, std::vector<LyricCandidate>& out, std::string* error) const;
    bool FetchKugou(const LyricCandidate& c, RawLyricDoc& out, std::string* error) const;

    HttpGetFn http_;
    bool lrclib_on_ = true;
    bool netease_on_ = true;
    bool kugou_on_ = true;
};

} // namespace lyric_core
