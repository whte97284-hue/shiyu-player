#pragma once
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

// ============================================================================
//  PlaylistProvider —— 歌单「分享链接 / ID → 明文曲目清单」解析内核 (自包含,零第三方依赖)
//
//  职责边界 (与 LyricProvider 并列,同构):
//    · 本模块只负责一件事:**把用户贴进来的歌单分享链接或 ID,解成一份明文的曲目清单**
//    · 不负责: 播放器识别、资源指纹、查重、去重、下载 —— 那些由上层 (Claude) 完成
//
//  当前实现的平台:
//    · Kugou   —— 酷狗。走网页版 yueku 页面内嵌的 global.data JSON (无需签名)
//    · NetEase —— 网易云。两步式: playlist/detail 取 trackIds → song/detail 批量补全
//    · QQMusic —— QQ音乐。单步式: musicu.fcg 的 uniform_get_Dissinfo 分页取全部曲目
//                 (匿名免签名, 实测 GET 亦可, 无需 POST)
//
//  与 LyricProvider 的关系:
//    · 复用同一套 HttpGetFn 钩子 / WinHTTP 默认实现
//    · 酷狗侧解析出的 hash 可直接喂给 LyricProvider 的 LyricQuery.hash (省一次歌名搜索)
// ============================================================================

namespace playlist_core {

// 单条曲目 (跨平台统一模型)
struct PlaylistTrack {
    std::string source;        // "kugou" / "netease" / "qqmusic"
    std::string id;            // 平台内主键: 酷狗=hash / 网易云=歌曲数字 id / QQ音乐=songmid
    std::wstring title;        // 歌名 (酷狗为 songname,干净无后缀)
    std::wstring artist;       // 歌手 (多人已按 " / " 拼接)
    std::wstring album;        // 专辑名 (可能为空)
    uint32_t duration_ms = 0;  // 时长 (毫秒; 0=未知)

    // ---- 平台扩展字段 (上层做指纹/查重时可选用) ----
    std::string kugou_hash_320;   // 酷狗 320k hash
    std::string kugou_hash_flac;  // 酷狗 flac hash
    std::string kugou_audio_id;   // 酷狗 audio_id
    std::string kugou_album_id;   // 酷狗 album_id
    std::string netease_album_id; // 网易云专辑 id
    std::string qq_media_mid;     // QQ音乐 media_mid (取解密 ekey 的入参, 见 file.media_mid)
    std::string qq_album_mid;     // QQ音乐 album mid
    int         qq_song_type = 0; // QQ音乐 songtype (0=普通, 1=伴唱等)
};

// 歌单元信息
struct PlaylistMeta {
    std::string source;
    std::string id;            // 归一化后的平台歌单 ID
    std::wstring name;         // 歌单名
    std::wstring author;       // 创建者 (可能为空)
    std::wstring description;  // 简介 (可能为空)
    std::string cover_url;     // 封面
    int total = 0;             // 歌单标称曲目总数
};

// 一次完整解析的结果
struct PlaylistResult {
    PlaylistMeta meta;
    std::vector<PlaylistTrack> tracks;   // 明文曲目清单
    std::string error;                   // 失败原因 (成功时为空)
    bool ok = false;

    // 统计
    int with_duration = 0;               // 带时长的曲目数
    int with_album = 0;                  // 带专辑的曲目数
};

// 统一的 HTTP GET 钩子 (与 lyric_provider 保持一致; 默认实现为 WinHTTP)
using HttpGetFn = std::function<bool(const std::string& url, std::string& body)>;

// 平台标识
enum class Platform { Unknown, Kugou, NetEase, QQMusic };

class PlaylistProvider {
public:
    explicit PlaylistProvider(HttpGetFn http = nullptr);

    // ---- 链接/ID 归一化 (静态工具,可单独测试) ----

    // 判断一段输入属于哪个平台; 无法判断返回 Unknown
    static Platform DetectPlatform(const std::string& input);

    // 从各种形态的输入中抽取平台 + 歌单 ID
    //   支持: 纯数字ID / 带id=的URL / /playlist/xxx/1/ 路径 / 酷狗 single/xxx.html
    //         / m.kugou.com/plist/list/xxx  / 短链(需联网跟随重定向,此时走 ResolveInput)
    //   返回 false 表示无法识别
    static bool ParseInput(const std::string& input, Platform& plat, std::string& id);

    // 完整归一化: 对短链会联网跟随重定向后再 ParseInput
    bool ResolveInput(const std::string& input, Platform& plat, std::string& id,
                      std::string* error = nullptr) const;

    // ---- 主入口 ----

    // 解析歌单 (输入可以是 ID 或分享链接/短链)
    PlaylistResult Parse(const std::string& input) const;

    // 指定平台解析
    PlaylistResult ParseAs(Platform plat, const std::string& id) const;

    // 各平台单独入口
    bool FetchKugou(const std::string& id, PlaylistResult& out) const;
    bool FetchNetease(const std::string& id, PlaylistResult& out) const;
    bool FetchQQMusic(const std::string& id, PlaylistResult& out) const;

    // 酷狗新版 global_collection_id 形态 (collection_3_xxx_n_0)
    //   走 mobiles.kugou.com/api/v5/special/{info_v2,song_v2} + H5 签名
    bool FetchKugouGcid(const std::string& gcid, PlaylistResult& out) const;

    // 默认 WinHTTP 实现 (与 LyricProvider::DefaultHttpGet 等价,可共享)
    static bool DefaultHttpGet(const std::string& url, std::string& body);

    // 跟随重定向取最终 URL (最多 max_hops 跳; 用于短链)
    static bool FollowRedirect(const std::string& url, std::string& final_url,
                               std::string* error = nullptr, int max_hops = 5);

    // ---- 工具 (与 LyricProvider 同源实现,便于独立编译测试) ----
    static std::string UrlEncode(const std::string& utf8);
    static std::wstring Utf8ToWide(const std::string& s);
    static std::string WideToUtf8(const std::wstring& s);
    static std::wstring Trim(const std::wstring& s);

private:
    HttpGetFn http_;
};

// 平台名 <-> 枚举
const char* PlatformName(Platform p);
Platform   PlatformFromName(const std::string& name);

} // namespace playlist_core
