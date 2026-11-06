#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// ============================================================================
//  OnlineMusic —— 在线曲库内核（酷狗 / 网易云）
//
//  职责：列榜单、拉歌曲、搜索、账号（扫码登录/刷新）、把一首歌换成可播放直链。
//  零 Qt、零第三方依赖（自带极简 JSON 与 MD5），HTTP 可注入便于单测。
//
//  ── 两条解析路径（2026-11-05 实测）────────────────────────────────────────
//  · 未登录：走 trackercdn `i/v2`，不接收账号信息。privilege>=10 的曲子
//    服务端直接回 "The Resource Needs to be Paid"，**换任何凭证都一样**。
//  · 已登录：走 `v5/url`（带 android 签名 + token/userid），能拿到 VIP 曲的
//    真实直链，实测《稻香》flac 26,012,257 字节、文件头 fLaC。
//  所以 Resolve() 有账号就优先 v5/url，并按 flac→320→128 逐档降级；
//  没账号或全部失败才回落到 i/v2。
//
//  ── 签名（盐是唯一关键，猜不出来，来自 MakcRe/KuGouMusicApi）──────────────
//    signature = md5( SALT + 按 key 排序的 "k=v" 直接拼接 + body + SALT )
//    android SALT = OIlwieks28dk2k092lksi2UIkp      （取直链、登录校验）
//    web     SALT = NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt（扫码取码/轮询）
//  POST 时 body 整串参与签名，GET 时为空串；clienttime 是**秒**。
//
//  ── 凭证 ─────────────────────────────────────────────────────────────────
//  只认扫码登录拿到的 token（PC 客户端的 TK/Stoken 属于 appid 1001，
//  1005 接口一律回 20017，不可用）。token 由调用方持久化，本模块不落盘。
// ============================================================================

namespace online_core {

enum class Source { Kugou = 0, Netease = 1 };

const char* SourceName(Source s);   // "kugou" / "netease"
const char* SourceLabel(Source s);  // "酷狗" / "网易云"

// 音质档位。数值越小越好。
enum class Quality { Flac = 0, High = 1, Standard = 2 };
const char* QualityKey(Quality q);    // "flac" / "320" / "128"
const char* QualityLabel(Quality q);  // "无损" / "320K" / "128K"

struct Track {
    Source source = Source::Kugou;
    std::string id;           // 酷狗=hash(小写)，网易=数字 id
    std::string title;
    std::string artist;
    std::string artistId;     // 网易云搜索直接给；酷狗没有，靠歌手名反查
    std::string album;
    std::string albumId;
    std::string albumAudioId;
    int durationMs = 0;
    int bitrate = 0;
    long long fileSize = 0;
    int payType = 0;          // 酷狗 pay_type；网易 fee
    int privilege = 0;        // 酷狗 privilege，>=10 为会员专属
    std::string coverUrl;

    bool Paywalled() const;
    std::string DurationText() const;  // "03:21"
};

struct SongList {
    Source source = Source::Kugou;
    std::string id;
    std::string name;
    std::string coverUrl;
    std::string note;
};

// 歌手 / 专辑：搜索结果里的「实体」，点进去能看它的全部歌曲。
// 和 Track 里的 artist/album 字符串不是一回事 —— 这里带 id，才能二次请求。
struct ArtistInfo {
    std::string id;
    std::string name;
    std::string avatar;     // 可能含 {size} 模板
    int songCount = 0;
    int albumCount = 0;
};

struct AlbumInfo {
    std::string id;
    std::string name;
    std::string artist;
    std::string cover;      // 可能含 {size} 模板
    std::string publishTime;
    int songCount = 0;
};

struct Resolved {
    bool ok = false;
    std::string url;
    std::string ext;          // "flac" / "mp3" / "m4a"
    int bitrate = 0;
    long long fileSize = 0;
    Quality quality = Quality::Standard;
    bool paywalled = false;   // 明确因会员/购买被拒（区别于网络失败）
    bool need_login = false;  // 需要登录才能拿（调用方据此提示扫码）
    std::string message;      // 已中文化，可直接显示
};

// ── 账号 ──────────────────────────────────────────────────────────────────
struct Account {
    std::string token;
    std::string userid;
    std::string nickname;
    std::string avatar;
    bool Valid() const { return !token.empty() && !userid.empty(); }
    void Clear() { token.clear(); userid.clear(); nickname.clear(); avatar.clear(); }
};

// ── HTTP ──────────────────────────────────────────────────────────────────
struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeoutMs = 15000;
};

struct HttpResponse {
    bool ok = false;      // 传输层成功（拿到响应）
    int status = 0;       // HTTP 状态码
    std::string body;
};

using HttpFn = std::function<bool(const HttpRequest&, HttpResponse&)>;
void SetHttp(HttpFn fn);   // 进程级注入（测试用），传 nullptr 恢复默认 WinHTTP
HttpFn GetHttp();

// ── 签名（暴露出来便于单测）───────────────────────────────────────────────
std::string Md5Hex(const std::string& data);
std::string SignAndroid(const std::vector<std::pair<std::string, std::string>>& params,
                        const std::string& body);
std::string SignWeb(const std::vector<std::pair<std::string, std::string>>& params,
                    const std::string& body);

// 取任意 URL 的字节（内部带 https→http 降级）。失败返回 false。
// 用于下载封面这类小文件；大文件请用调用方自己的流式实现。
bool FetchBytes(const std::string& url, std::string& out);

// 酷狗专辑封面地址（把 {size} 换成 500，http 升 https）。失败返回空串。
std::string KugouAlbumCover(const std::string& albumId);

// ── 酷狗账号：扫码登录 ────────────────────────────────────────────────────
class KugouAuth {
public:
    enum class State {
        Error = -1,
        Expired = 0,
        Waiting = 1,
        Scanned = 2,     // 已扫码，待手机端确认
        Confirmed = 4,   // 成功，account 已填充
    };

    struct QrCode {
        std::string key;              // 轮询用
        std::string pngBase64;        // data:image/png;base64,xxx 里的纯 base64
        std::vector<uint8_t> png;     // 已解码的 PNG 字节，调用方直接存盘显示
        std::string scanUrl;          // H5 地址（备用：可自行生成二维码）
    };

    // 取二维码。失败返回 false 并填 err。
    bool CreateQr(QrCode& out, std::string* err = nullptr);

    // 轮询状态。Confirmed 时把账号写进 acc。
    // 网络失败返回 State::Error（可重试），不是致命错误。
    State CheckQr(const std::string& key, Account& acc, std::string* err = nullptr);

    // 注：酷狗的 token 续期接口（login_by_token）要求 RSA+AES 加密的请求体，
    // 本内核不实现——续期失败时重新扫码即可（3 秒）。token 失效的表现是
    // 解析接口回 20017，Resolve() 会置 need_login 让上层提示扫码。

    // 设备标识（mid）。同一台机器应保持稳定，否则 token 会失效。
    static std::string DeviceMid();
    static void SetDeviceMid(const std::string& mid);
};

// ── Provider ──────────────────────────────────────────────────────────────
class Provider {
public:
    virtual ~Provider() = default;
    virtual Source source() const = 0;
    virtual std::vector<SongList> Lists() = 0;
    virtual std::vector<Track> ListSongs(const std::string& listId, int limit) = 0;
    // offset 用于「加载更多」：酷狗按 page 翻页，网易按 offset
    virtual std::vector<Track> Search(const std::string& keyword, int limit, int offset = 0) = 0;
    virtual Resolved Resolve(const Track& track) = 0;

    // ── 按实体浏览 ────────────────────────────────────────────────────
    // 默认返回空：不支持该能力的源不会假装支持，调用方据空结果降级。
    virtual std::vector<ArtistInfo> SearchArtists(const std::string& /*kw*/, int /*limit*/) {
        return {};
    }
    virtual std::vector<AlbumInfo> SearchAlbums(const std::string& /*kw*/, int /*limit*/) {
        return {};
    }
    virtual ArtistInfo Artist(const std::string& /*id*/) { return {}; }
    virtual std::vector<Track> ArtistSongs(const std::string& /*id*/, int /*limit*/, int /*offset*/) {
        return {};
    }
    virtual AlbumInfo Album(const std::string& /*id*/) { return {}; }
    virtual std::vector<Track> AlbumSongs(const std::string& /*id*/, int /*limit*/) { return {}; }

    // 设置账号后，酷狗侧才会走 v5/url 拿 VIP 直链。
    virtual void SetAccount(const Account&) {}
    // 期望音质；解析时从该档开始向下降级。
    virtual void SetPreferredQuality(Quality) {}
};

std::unique_ptr<Provider> MakeKugou();
std::unique_ptr<Provider> MakeNetease();

} // namespace online_core
