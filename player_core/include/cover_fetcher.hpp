#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ============================================================================
//  CoverFetcher —— 给缺失封面的 FLAC 补内嵌封面 (自包含,零第三方依赖)
//
//  背景:酷狗 KGG 解密出的 FLAC 只有 TITLE/ALBUM/ARTIST 三个文本标签,**没有 PICTURE 块**
//  (kgg-dec.exe 只做纯字节流解密,不认识 FLAC 元数据 —— 反编译已证实)。
//  本模块在解密完成后补这一步,让产物换任何播放器都能显示封面。
//
//  定位思路(实测可行,见 KUGOU_COVER_API_FEASIBILITY.md):
//    md5(整个 FLAC 文件) == 酷狗的 sqhash,大小 == sqfilesize
//    → 用「歌手 歌名」搜索酷狗,拿候选列表
//    → 用 sqhash / sqfilesize 精确校验挑出同一版文件(避免拿到单曲EP/remix的封面)
//    → album_id → album/info 的 imgurl(替换 {size}=1080) → 下载 JPEG
//    → 以 PICTURE 块(type=3, Cover front) 写入 FLAC
//  酷狗失败或无法校验时回退网易云(同样要校验专辑名,否则容易拿到单曲EP封面)。
//
//  设计:同步阻塞调用,失败只返回 false + 原因,不做重试/后台队列(调用方在解密线程里,
//  本来就是后台)。HTTP 可注入,便于单测。
// ============================================================================

namespace cover_core {

// ---- HTTP 钩子:默认实现复用 LyricProvider::DefaultHttpGet(WinHTTP) ----
// 返回 true 表示 HTTP 200 且 body 非空
using HttpGetFn = std::function<bool(const std::string& url, std::string& body)>;
void SetHttpGet(HttpGetFn fn);
HttpGetFn GetHttp();

// ---- FLAC 元数据 ----
struct FlacTags {
    std::string title;   // UTF-8
    std::string artist;  // UTF-8 (优先 ARTIST,回落 ALBUMARTIST)
    std::string album;   // UTF-8
    // 备用标题:文件名的后段(「艺人 - 标题」取标题)。标签被翻成罗马字时
    // (Sakurairo Diary)文件名往往还留着原文(桜色ダイアリー),用它才搜得到。
    std::string alt_title;
};

// 是否 FLAC(看 4 字节魔数)
bool IsFlac(const std::wstring& path);
// 是否已有内嵌封面(PICTURE 块)
bool FlacHasCover(const std::wstring& path);
// 读 Vorbis Comment 里的 TITLE/ARTIST/ALBUM
bool ReadFlacTags(const std::wstring& path, FlacTags& out);

// 流式计算整个文件的 MD5(小写 hex)。酷狗的 sqhash 就是它。
std::string Md5File(const std::wstring& path);

// 把 JPEG 作为 PICTURE 块插入 FLAC(已有封面则不动,返回 false)
// 先写 path + ".tmp" 再原子替换,失败不会损坏原文件。
// width/height 未知时传 0(标准允许)。
// mime 传空 = 自动嗅探
bool EmbedFlacPicture(const std::wstring& path, const std::string& jpeg,
                      int width = 0, int height = 0, std::string* error = nullptr,
                      const std::string& mime = std::string());

// ---- 在线取封面 ----
struct CoverResult {
    std::string jpeg;    // 图片字节 (字段名沿用,实际可能是 PNG/WEBP)
    std::string mime;    // "image/jpeg" / "image/png" / "image/webp"
    std::string source;  // "kugou" / "netease"
    std::string detail;  // 调试信息(album_id 等)
};

// 在线取封面。flac_path 用于"内容校验"(size/md5),可传空则只用 tag 搜索。
// 返回 true 表示 out.jpeg 是可用的 JPEG。
bool FetchOnline(const FlacTags& tags, const std::wstring& flac_path,
                 CoverResult& out, std::string* error = nullptr);

// ---- 一站式:没封面就补 ----
// 返回 true 表示"现在有封面了"(原本就有,或本次成功嵌入)。
// 已经有的直接返回 true 且不联网。
bool EnsureCover(const std::wstring& flac_path, std::string* error = nullptr,
                 std::string* source = nullptr);

// ---- 工具(公开便于测试) ----
std::string UrlEncode(const std::string& utf8);
// 识别图片格式,返回 MIME;不认识返回 nullptr
const char* SniffImage(const std::string& data);
// 前 8 字节的 hex (错误信息里显示,便于定位返回的到底是什么)
std::string HexHead(const std::string& data);
// 从 JSON 文本里取字符串字段(轻量,只够用不追求完备;找不到返回 false)
bool JsonGetString(const std::string& json, const std::string& key, std::string& out, size_t from = 0);
bool JsonGetInt(const std::string& json, const std::string& key, long long& out, size_t from = 0);

} // namespace cover_core
