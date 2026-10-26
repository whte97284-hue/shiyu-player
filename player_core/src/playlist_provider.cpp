// ============================================================================
//  PlaylistProvider 实现 —— 歌单分享链接/ID → 明文曲目清单
//
//  协议来源 (2026-10-16 实测验证):
//
//   酷狗 新版 global_collection_id (需 H5 签名, 2026-10-16 实测验证):
//     分享短链 t1.kugou.com/xxxx → 302 → wwwapi.kugou.com/share/zlist.html
//       ?global_collection_id=collection_3_{uid}_{n}_0&listid=3&...
//     ⚠ 该形态走不了 www2 yueku (页面没有 global.data)。
//     正确路径 (两步, 均需 signature):
//       1) GET https://mobiles.kugou.com/api/v5/special/info_v2
//              ?appid=1058&specialid=0&global_specialid={gcid}&format=json
//          → data.specialname / nickname / songcount / imgurl
//       2) GET https://mobiles.kugou.com/api/v5/special/song_v2
//              ?appid=1058&global_specialid={gcid}&specialid=0&plat=0&version=8000
//              &page=N&pagesize=300
//          → data.info[] 每首含 filename / hash / 320hash / sqhash / duration(秒)
//     签名: signature = MD5( KEY + "k1=v1k2=v2..."(key升序) + KEY )
//       KEY = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt"  (酷狗 web 前端 infSign.min.js 公开字面量)
//     公共参数: srcappid=2919 clientver=20000 clienttime|mid|uuid=毫秒时间戳 dfid=-
//
//   酷狗短链: https://t1.kugou.com/xxxxxxxxxxx → 302 → 分享页 (见上)
//     ⚠ 分享页返回的 HTML 里也内嵌 dataFromSmarty 数组 (仅前 100 首), 可作兜底;
//        要拿全量必须走 info_v2 / song_v2。
//     ⚠ 酷狗码 "#12345678#" 是 App 内私有索引, 无公开 web 接口。
//
//   酷狗 (无需签名, 老版 specialid):
//     GET https://www2.kugou.kugou.com/yueku/v9/special/single/{specialId}-5-9999.html
//     页面内嵌  global.data = [ {...}, ... ];   ← 整个曲目数组
//     另有      global = { ... name: "歌单名" ... pic: "封面" ... };
//
//   网易云 (无需登录/无需加密, 两步式):
//     1) POST http://music.163.com/api/v6/playlist/detail   body: s=0&id={pid}&n=1000&t=0
//        → playlist.name / trackCount / trackIds[].id   (注意: trackIds 元素是对象)
//     2) GET  http://music.163.com/api/song/detail/?id={csv}&ids=%5B{csv}%5D
//        → songs[].name / ar[].name / dt / id / al.id
//
//   QQ音乐 (匿名, 无需签名 / 无需登录 / 无需 ekey):
//     GET https://u.y.qq.com/cgi-bin/musicu.fcg?format=json&data={URL编码的JSON}
//       data = {"comm":{"ct":24,"cv":0},
//               "req_1":{"module":"music.srfDissInfo.aiDissInfo",
//                        "method":"uniform_get_Dissinfo",
//                        "param":{"disstid":{歌单ID},"song_begin":N,"song_num":300,
//                                 "enc_host_uin":"","tag":1,"userinfo":1}}}
//     → req_1.data.songlist[]  每首含 name / singer[].name / mid(songmid) /
//                               interval(【秒】!) / album.{name,mid} / file.media_mid
//       req_1.data.dirinfo.{title,desc,picurl,listennum}  + total_song_num
//     ⚠ 实测 GET 亦可 (官方虽走 POST, 但 GET+query 同样返回 200 与完整数据),
//       因此本内核可复用现有 GET-only 钩子, 无需新增 POST 通道。
//     ⚠ interval 单位是【秒】(网易云 dt 是毫秒, 勿混)。
//     ⚠ 分页用 song_begin 递增 (步长=上次实际返回条数), 并以 mid 去重。
//     短链: c6.y.qq.com/base/fcgi-bin/u?__=xxx → 302 ×3 →
//              i.y.qq.com/n2/m/share/details/taoge.html?...&id={id} →
//              y.qq.com/n/ryqq_v2/playlist/{id}
//       ⚠ *.y.qq.com 各子域共用证书, 不会触发 WinHTTP 12175。
// ============================================================================
#include "../include/playlist_provider.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#endif
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <regex>
#include <map>
#include <memory>
#include <utility>
#include <set>
#include <chrono>

// windows.h 会把 min/max 定义成宏, 与 std::min/std::max 冲突
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#ifdef _WIN32
#pragma comment(lib, "winhttp.lib")
#endif

namespace playlist_core {

// ============================================================================
//  轻量 JSON 解析 (与 lyric_provider 同思路: 够用即可,不做通用库)
// ============================================================================
namespace {

struct JVal;
using JValPtr = std::shared_ptr<JVal>;   // 简易共享指针, 避免手写引用计数

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool b = false;
    double num = 0;
    std::string str;                                   // 原始 UTF-8 字节
    std::vector<JValPtr> arr;
    std::map<std::string, JValPtr> obj;

    const JVal* Get(const char* k) const {
        auto it = obj.find(k);
        return it == obj.end() ? nullptr : it->second.get();
    }
    const JVal* At(size_t i) const {
        return i < arr.size() ? arr[i].get() : nullptr;
    }
    size_t Size() const { return type == Arr ? arr.size() : 0; }
    bool IsStr() const { return type == Str; }
    bool IsNum() const { return type == Num; }
    bool IsArr() const { return type == Arr; }
    bool IsObj() const { return type == Obj; }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}

    JValPtr Parse() {
        SkipWs();
        JValPtr v = ParseValue();
        return v;
    }

private:
    const std::string& s_;
    size_t i_ = 0;

    void SkipWs() {
        while (i_ < s_.size()) {
            char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { ++i_; continue; }
            break;
        }
    }

    bool Literal(const char* lit) {
        size_t n = strlen(lit);
        if (s_.compare(i_, n, lit) == 0) { i_ += n; return true; }
        return false;
    }

    JValPtr ParseValue() {
        SkipWs();
        if (i_ >= s_.size()) return nullptr;
        char c = s_[i_];
        if (c == '{') return ParseObject();
        if (c == '[') return ParseArray();
        if (c == '"') return ParseString();
        if (c == 't') { if (Literal("true")) { auto v = std::make_shared<JVal>(); v->type = JVal::Bool; v->b = true; return v; } }
        if (c == 'f') { if (Literal("false")) { auto v = std::make_shared<JVal>(); v->type = JVal::Bool; v->b = false; return v; } }
        if (c == 'n') { if (Literal("null")) { return std::make_shared<JVal>(); } }
        return ParseNumber();
    }

    JValPtr ParseString() {
        auto v = std::make_shared<JVal>();
        v->type = JVal::Str;
        ++i_; // skip "
        std::string out;
        while (i_ < s_.size()) {
            char c = s_[i_++];
            if (c == '"') break;
            if (c == '\\' && i_ < s_.size()) {
                char e = s_[i_++];
                switch (e) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case 'u': {
                        // \uXXXX (含代理对)
                        unsigned cp = 0;
                        if (i_ + 4 <= s_.size()) {
                            cp = (unsigned)strtoul(s_.substr(i_, 4).c_str(), nullptr, 16);
                            i_ += 4;
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() &&
                            s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                            unsigned lo = (unsigned)strtoul(s_.substr(i_ + 2, 4).c_str(), nullptr, 16);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                i_ += 6;
                            }
                        }
                        AppendUtf8(out, cp);
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += c;
            }
        }
        v->str = std::move(out);
        return v;
    }

    static void AppendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    JValPtr ParseNumber() {
        auto v = std::make_shared<JVal>();
        v->type = JVal::Num;
        size_t start = i_;
        while (i_ < s_.size()) {
            char c = s_[i_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E') ++i_;
            else break;
        }
        v->num = strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return v;
    }

    JValPtr ParseArray() {
        auto v = std::make_shared<JVal>();
        v->type = JVal::Arr;
        ++i_; // [
        SkipWs();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return v; }
        while (i_ < s_.size()) {
            JValPtr e = ParseValue();
            if (!e) break;
            v->arr.push_back(e);
            SkipWs();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == ']') { ++i_; break; }
            break;
        }
        return v;
    }

    JValPtr ParseObject() {
        auto v = std::make_shared<JVal>();
        v->type = JVal::Obj;
        ++i_; // {
        SkipWs();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return v; }
        while (i_ < s_.size()) {
            SkipWs();
            if (i_ >= s_.size() || s_[i_] != '"') break;
            JValPtr key = ParseString();
            SkipWs();
            if (i_ >= s_.size() || s_[i_] != ':') break;
            ++i_; // :
            JValPtr val = ParseValue();
            if (!val) break;
            v->obj[key->str] = val;
            SkipWs();
            if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
            if (i_ < s_.size() && s_[i_] == '}') { ++i_; break; }
            break;
        }
        return v;
    }
};

// ---- 宽窄字符串转换 ----
#ifdef _WIN32
std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n);
    return out;
}
#else
// 非 Windows (宿主测试 / Linux 版界面): wchar_t 为 UTF-32
std::string ToUtf8(const std::wstring& w) {
    std::string out;
    out.reserve(w.size() * 3);
    for (wchar_t wc : w) {
        uint32_t c = static_cast<uint32_t>(wc);
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0x800) { out += static_cast<char>(0xC0 | (c >> 6)); out += static_cast<char>(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12)); out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18)); out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)); out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::wstring ToWide(const std::string& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (len == 0 || i + len > s.size()) { out += L'\xFFFD'; ++i; continue; }
        uint32_t cp = len == 1 ? c : len == 2 ? (c & 0x1F) : len == 3 ? (c & 0x0F) : (c & 0x07);
        bool bad = false;
        for (int k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { bad = true; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (bad) { out += L'\xFFFD'; ++i; continue; }
        out += static_cast<wchar_t>(cp);
        i += len;
    }
    return out;
}
#endif

std::wstring TrimW(const std::wstring& s) {
    size_t b = 0, e = s.size();
    auto sp = [](wchar_t c) {
        return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' ||
               c == 0x3000 || c == L'"' || c == 0xA0;
    };
    while (b < e && sp(s[b])) ++b;
    while (e > b && sp(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// 该 JSON 字符串值按 UTF-8 转宽 (网易云/酷狗返回均为 UTF-8)
std::wstring JStr(const JVal* v) {
    if (!v || !v->IsStr()) return {};
    return TrimW(ToWide(v->str));
}

std::string JStrRaw(const JVal* v) {
    return (v && v->IsStr()) ? v->str : std::string();
}

// 数值字段 (可能是 number 也可能是 string)
double JNum(const JVal* v) {
    if (!v) return 0;
    if (v->IsNum()) return v->num;
    if (v->IsStr()) return strtod(v->str.c_str(), nullptr);
    return 0;
}

// 剥掉 HTML 里的 <em> 之类标签 (网易云歌名里可能带高亮标签)
std::wstring StripHtml(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    bool in = false;
    for (wchar_t c : s) {
        if (c == L'<') { in = true; continue; }
        if (c == L'>') { in = false; continue; }
        if (!in) out += c;
    }
    return out;
}

} // anonymous namespace

// ============================================================================
//  平台识别 & 输入归一化
// ============================================================================

const char* PlatformName(Platform p) {
    switch (p) {
        case Platform::Kugou:   return "kugou";
        case Platform::NetEase: return "netease";
        case Platform::QQMusic: return "qqmusic";
        default:                return "unknown";
    }
}

Platform PlatformFromName(const std::string& name) {
    if (name == "kugou" || name == "kg")     return Platform::Kugou;
    if (name == "netease" || name == "wyy" || name == "163") return Platform::NetEase;
    if (name == "qq" || name == "qqmusic" || name == "qqm")  return Platform::QQMusic;
    return Platform::Unknown;
}

Platform PlaylistProvider::DetectPlatform(const std::string& input) {
    std::string low = input;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return (char)tolower(c); });

    // 纯数字 → 无法从内容判断平台 (调用方需指定或由链接上下文决定)
    bool all_digit = !low.empty() &&
        std::all_of(low.begin(), low.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (all_digit) return Platform::Unknown;

    if (low.find("kugou.com") != std::string::npos) return Platform::Kugou;
    if (low.find("kgl") != std::string::npos)       return Platform::Kugou;
    if (low.find("music.163.com") != std::string::npos) return Platform::NetEase;
    if (low.find("163cn.tv") != std::string::npos)      return Platform::NetEase;
    if (low.find("y.music.163") != std::string::npos)   return Platform::NetEase;
    if (low.find("netease") != std::string::npos)       return Platform::NetEase;
    // QQ音乐: 域名众多, 统一用 "qq.com" 兜底
    //   y.qq.com(网页版) / c6.y.qq.com(短链) / i.y.qq.com · i2.y.qq.com(H5) / u.y.qq.com(API)
    if (low.find("y.qq.com") != std::string::npos)      return Platform::QQMusic;
    if (low.find("qq.com") != std::string::npos)        return Platform::QQMusic;
    if (low.find("qqmusic") != std::string::npos)       return Platform::QQMusic;
    return Platform::Unknown;
}

// 从 URL 中提取指定参数值
static std::string ExtractQuery(const std::string& url, const std::string& key) {
    std::string pat1 = key + "=";
    size_t p = url.find("?" + pat1);
    if (p == std::string::npos) {
        if (url.compare(0, pat1.size(), pat1) == 0) p = 0;
        else {
            p = url.find("&" + pat1);
            if (p != std::string::npos) p += 1;
        }
    } else {
        p += 1;
    }
    if (p == std::string::npos) return {};
    size_t v = p + pat1.size();
    size_t e = url.find_first_of("&#", v);
    return url.substr(v, e == std::string::npos ? std::string::npos : e - v);
}

bool PlaylistProvider::ParseInput(const std::string& raw, Platform& plat, std::string& id) {
    std::string input = raw;
    // 去掉首尾空白
    size_t b = input.find_first_not_of(" \t\r\n");
    size_t e = input.find_last_not_of(" \t\r\n");
    if (b == std::string::npos) return false;
    input = input.substr(b, e - b + 1);

    // 用户常把 App 的整段分享文案原样贴进来:
    //   "https://t1.kugou.com/3RB9v52G6V2  复制整段信息，去酷狗搜索此码 #40717681# ..."
    // 先抠出第一个链接 (到空白或非 ASCII 字符为止) 再识别; 否则后面的「兜底抓长数字」会把酷狗码
    // 40717681 当成老版歌单号, 去请求老页面报「未找到 global.data」
    {
        size_t h = input.find("http://");
        size_t hs = input.find("https://");
        if (hs != std::string::npos && (h == std::string::npos || hs < h)) h = hs;
        if (h != std::string::npos) {
            size_t t = h;
            while (t < input.size()) {
                const unsigned char c = static_cast<unsigned char>(input[t]);
                if (c <= 0x20 || c >= 0x80 || c == '"' || c == '\'' || c == '<' || c == '>') break;
                ++t;
            }
            std::string url = input.substr(h, t - h);
            while (!url.empty() && (url.back() == ')' || url.back() == ',' || url.back() == '.')) url.pop_back();
            if (url != input) return ParseInput(url, plat, id);
        }
    }

    std::string low = input;
    std::transform(low.begin(), low.end(), low.begin(),
                   [](unsigned char c) { return (char)tolower(c); });

    // 1) 纯数字 ID
    bool all_digit = !input.empty() &&
        std::all_of(input.begin(), input.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (all_digit) {
        // 纯数字无法判断平台,交给调用方 (这里给 Unknown + 填入 id)
        plat = Platform::Unknown;
        id = input;
        return true;
    }

    // 2) 酷狗形态
    if (low.find("kugou.com") != std::string::npos || low.find("kgl") != std::string::npos) {
        plat = Platform::Kugou;
        std::smatch m;

        // ---- 2a) 优先级最高: global_collection_id (新版歌单标识) ----
        //   形如 collection_3_1016644002_3_0
        //   ⚠ 必须【先于】listid 判断: 酷狗分享 URL 里 listid 常是短数字(如 listid=3),
        //     它只是「列表类型」而【不是】歌单 ID, 先取它会得到完全错误的歌单。
        {
            std::string gcid = ExtractQuery(input, "global_collection_id");
            if (!gcid.empty()) { id = "gcid:" + gcid; return true; }
        }

        // ---- 2b) 短链: t1.kugou.com/xxxx / kgl.kugou.com/xxxx ----
        //   短链本身不含 ID, 需要联网跟随 302 拿真实分享 URL。
        //   这里把「待跟随」的信息编码进 id (前缀 "short:"), 由 ResolveInput 处理。
        {
            std::regex re_short(R"(^https?://(?:t1|t2|t3|t4|t5|t6|t7|t8|t9|t10|kgl|kugou)\.kugou\.com/([A-Za-z0-9_-]+)/?(?:[?#].*)?$)",
                                std::regex::icase);
            if (std::regex_search(input, m, re_short)) {
                std::string code = m[1].str();
                // 排除明显是页面路径的关键字
                std::string lc = code;
                std::transform(lc.begin(), lc.end(), lc.begin(),
                               [](unsigned char c) { return (char)tolower(c); });
                if (lc != "share" && lc != "plist" && lc != "index" && lc != "plist.html") {
                    id = "short:" + code;
                    return true;
                }
            }
        }

        // ---- 2c) 老版式路径 /yy/special/single/1067062.html ----
        std::regex re_single(R"(special/single/(\d+))");
        if (std::regex_search(input, m, re_single)) { id = m[1].str(); return true; }
        // m.kugou.com/plist/list/123456
        std::regex re_plist(R"(plist/list/(\d+))");
        if (std::regex_search(input, m, re_plist)) { id = m[1].str(); return true; }

        // ---- 2d) 分享页其它参数 ----
        //   specialid 优先于 listid: 前者是真歌单号, 后者多用于「列表类型」
        for (const char* key : { "specialid", "special_id", "listid", "list_id" }) {
            std::string v = ExtractQuery(input, key);
            if (!v.empty()) {
                std::regex re_num(R"((\d{4,}))");
                if (std::regex_search(v, m, re_num)) { id = m[1].str(); return true; }
            }
        }
        // 兜底: 抓最后一段较长数字
        std::regex re_last(R"((\d{5,}))");
        if (std::regex_search(input, m, re_last)) { id = m[1].str(); return true; }
        return false;
    }

    // 3) 网易云形态
    if (low.find("163.com") != std::string::npos || low.find("163cn.tv") != std::string::npos) {
        plat = Platform::NetEase;
        // ?id=xxx
        std::string qid = ExtractQuery(input, "id");
        if (!qid.empty() && std::all_of(qid.begin(), qid.end(),
                [](char c){ return isdigit((unsigned char)c); })) {
            id = qid;
            return true;
        }
        // /playlist/1234567890/1/ (旧版)
        std::regex re_pl(R"(/playlist/(\d+))");
        std::smatch m;
        if (std::regex_search(input, m, re_pl)) { id = m[1].str(); return true; }
        // /m/playlist?id=
        std::regex re_digits(R"((\d{5,}))");
        if (std::regex_search(input, m, re_digits)) { id = m[1].str(); return true; }
        return false;
    }

    // 4) QQ音乐形态
    if (low.find("qq.com") != std::string::npos || low.find("qqmusic") != std::string::npos) {
        plat = Platform::QQMusic;
        std::smatch m;

        // ---- 4a) 短链: c6.y.qq.com/base/fcgi-bin/u?__=xxx ----
        //   短链里没有歌单号, 必须联网跟随 302 (实测 3 跳) 拿最终 URL:
        //     ... -> i.y.qq.com/n2/m/share/details/taoge.html?...&id=8962418095
        //     ... -> y.qq.com/n/ryqq_v2/playlist/8962418095
        //   把完整 URL 编码进 id (前缀 "shorturl:"), 由 ResolveInput 处理。
        if (low.find("/base/fcgi-bin/u") != std::string::npos ||
            low.find("/fcgi-bin/u") != std::string::npos) {
            id = "shorturl:" + input;
            return true;
        }

        // ---- 4b) 新版网页版路径 /n/ryqq_v2/playlist/8962418095 ----
        std::regex re_pl(R"(/playlist/(\d+))");
        if (std::regex_search(input, m, re_pl)) { id = m[1].str(); return true; }

        // ---- 4c) H5 分享页 taoge.html?...&id=8962418095 ----
        for (const char* key : { "id", "disstid", "dissid" }) {
            std::string v = ExtractQuery(input, key);
            if (!v.empty()) {
                std::regex re_num(R"(^(\d+)$)");
                if (std::regex_match(v, m, re_num)) { id = m[1].str(); return true; }
            }
        }

        // ---- 4d) 兜底: 抓较长的纯数字段 ----
        std::regex re_last(R"((\d{6,}))");
        if (std::regex_search(input, m, re_last)) { id = m[1].str(); return true; }
        return false;
    }

    // 5) 自定义 scheme: "kugou:1067062" / "netease:7480897649" / "qq:8962418095"
    size_t colon = input.find(':');
    if (colon != std::string::npos && colon < 10) {
        std::string pfx = low.substr(0, colon);
        if (pfx == "kugou" || pfx == "kg") {
            plat = Platform::Kugou; id = input.substr(colon + 1); return !id.empty();
        }
        if (pfx == "netease" || pfx == "wyy" || pfx == "163") {
            plat = Platform::NetEase; id = input.substr(colon + 1); return !id.empty();
        }
        if (pfx == "qq" || pfx == "qqmusic" || pfx == "qqm") {
            plat = Platform::QQMusic; id = input.substr(colon + 1); return !id.empty();
        }
    }

    return false;
}

bool PlaylistProvider::FollowRedirect(const std::string& url, std::string& final_url,
                                      std::string* error, int max_hops) {
#ifdef _WIN32
    std::string cur = url;
    for (int hop = 0; hop < max_hops; ++hop) {
        std::wstring wurl = ToWide(cur);
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof(uc);
        const size_t cap = wurl.size() + 64;
        std::wstring hostbuf(256, L'\0'), pathbuf(cap, L'\0');
        uc.lpszHostName = &hostbuf[0];   uc.dwHostNameLength = 255;
        uc.lpszUrlPath = &pathbuf[0];    uc.dwUrlPathLength = (DWORD)(cap - 1);
        if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) {
            if (error) *error = "URL 解析失败";
            return false;
        }
        hostbuf.resize(uc.dwHostNameLength);
        pathbuf.resize(uc.dwUrlPathLength);
        bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
        HINTERNET ses = WinHttpOpen(L"KugouPlayer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) { if (error) *error = "WinHttpOpen 失败"; return false; }
        WinHttpSetTimeouts(ses, 8000, 8000, 8000, 8000);
        HINTERNET con = WinHttpConnect(ses, hostbuf.c_str(), uc.nPort, 0);
        if (!con) { WinHttpCloseHandle(ses); if (error) *error = "连接失败"; return false; }
        HINTERNET req = WinHttpOpenRequest(con, L"GET", pathbuf.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
        if (!req) { WinHttpCloseHandle(con); WinHttpCloseHandle(ses);
            if (error) *error = "请求创建失败"; return false; }
        // 不自动跟随, 手动读 Location
        DWORD opt = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));
        WinHttpAddRequestHeaders(req, L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64)",
                                 (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

        bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(req, nullptr);
        DWORD status = 0, len = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

        std::string location;
        if (ok && status >= 300 && status < 400) {
            DWORD llen = 0;
            WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                nullptr, &llen, WINHTTP_NO_HEADER_INDEX);
            if (llen > 0) {
                std::wstring wloc(llen / sizeof(wchar_t), L'\0');
                if (WinHttpQueryHeaders(req, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                        &wloc[0], &llen, WINHTTP_NO_HEADER_INDEX))
                    location = ToUtf8(TrimW(wloc));
            }
        }
        WinHttpCloseHandle(req);
        WinHttpCloseHandle(con);
        WinHttpCloseHandle(ses);

        if (!ok) { if (error) *error = "重定向请求失败"; return false; }
        if (location.empty()) { final_url = cur; return true; }

        // 相对跳转补全
        if (location.compare(0, 2, "//") == 0) {
            location = std::string(https ? "https:" : "http:") + location;
        } else if (location[0] == '/') {
            char scheme[16]{};
            _snprintf_s(scheme, sizeof(scheme), _TRUNCATE, "%s://", https ? "https" : "http");
            location = std::string(scheme) + ToUtf8(hostbuf) + location;
        }
        cur = location;
    }
    final_url = cur;
    return true;
#else
    (void)max_hops;
    final_url = url;
    if (error) *error = "当前平台不支持跟随短链";
    return false;
#endif
}

bool PlaylistProvider::ResolveInput(const std::string& input, Platform& plat, std::string& id,
                                    std::string* error) const {
    if (!ParseInput(input, plat, id)) {
        // 纯酷狗码 (形如 "#40717681#" 或 用户只贴了数字串但带 # 包裹)
        //   酷狗码是酷狗 App 内的私有索引, 无公开 web 接口, 这里给明确提示。
        {
            std::string s = input;
            size_t b = s.find_first_not_of(" \t\r\n");
            size_t e = s.find_last_not_of(" \t\r\n");
            if (b != std::string::npos) s = s.substr(b, e - b + 1);
            // 只贴了酷狗码, 或贴的分享文案里没有链接、只有 "#数字#"
            std::smatch km;
            static const std::regex re_code(R"(#\d{5,}#)");
            if (std::regex_search(s, km, re_code)) {
                if (error) *error =
                    "酷狗码 (#xxxxxxxx#) 是酷狗 App 内私有索引, 没有公开网页接口, 无法解析。"
                    "请在酷狗 App 里打开该歌单, 用「分享 → 复制链接」把分享链接贴进来。";
                return false;
            }
        }
        if (error) *error = "无法识别的链接或 ID";
        return false;
    }

    // ---- 短链: 联网跟随重定向, 从最终 URL 里取真实标识 ----

    // QQ音乐短链 (c6.y.qq.com/base/fcgi-bin/u?__=xxx): 实测 3 跳
    //   -> i.y.qq.com/n2/m/share/details/taoge.html?...&id=8962418095
    //   -> y.qq.com/n/ryqq_v2/playlist/8962418095
    // ⚠ *.y.qq.com 各子域共用证书, 不会触发 WinHTTP 12175
    if (id.compare(0, 9, "shorturl:") == 0) {
        std::string url = id.substr(9);
        std::string fin, rerr;
        if (!FollowRedirect(url, fin, &rerr, 8)) {
            if (error) *error = "QQ音乐短链解析失败: " + rerr;
            return false;
        }
        Platform p2 = Platform::Unknown; std::string i2;
        if (!ParseInput(fin, p2, i2) || i2.empty() || i2.compare(0, 9, "shorturl:") == 0) {
            if (error) *error = "QQ音乐短链跳转后仍无法识别歌单 ID";
            return false;
        }
        plat = p2; id = i2;
        return true;
    }

    if (id.compare(0, 6, "short:") == 0) {
        std::string code = id.substr(6);
        std::string url = "https://t1.kugou.com/" + code;
        std::string fin;
        std::string rerr;
        if (!FollowRedirect(url, fin, &rerr)) {
            if (error) *error = "短链解析失败: " + rerr;
            return false;
        }
        // 最终 URL 应是分享页: .../share/zlist.html?...&global_collection_id=xxx&...
        Platform p2 = Platform::Unknown; std::string i2;
        if (!ParseInput(fin, p2, i2) || p2 != Platform::Kugou) {
            if (error) *error = "短链跳转后仍无法识别歌单 ID";
            return false;
        }
        plat = p2; id = i2;
        // i2 此时可能是 "gcid:collection_..." 或纯数字, 交由 FetchKugou/ParseAs 处理
        return true;
    }

    // 平台未知 + 是纯数字 → 失败 (必须能判断平台)
    if (plat == Platform::Unknown) {
        // 可能输入是短链, 试着跟随重定向再判断一次
        if (input.find("://") != std::string::npos) {
            std::string fin;
            if (FollowRedirect(input, fin, error)) {
                Platform p2 = Platform::Unknown; std::string i2;
                if (ParseInput(fin, p2, i2) && p2 != Platform::Unknown) {
                    plat = p2; id = i2; return true;
                }
            }
        }
        if (error) *error = "无法判断所属平台 (纯数字 ID 需要指定平台)";
        return false;
    }
    return true;
}

// ============================================================================
//  酷狗 H5 签名 (infSign) —— 用于 mobiles.kugou.com 的 API 网关
//  算法: MD5( KEY + 按 key 升序拼接的 "k=v" + KEY )
//  密钥来自酷狗 web 前端 staticssl.kugou.com/common/js/min/infSign.min.js
//  (该 js 里的字面量 'NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt')
//  这是【公开】的 H5 端签名, 非 App 私有密钥, 用于公开歌单数据。
// ============================================================================
namespace {

const char* kKgSignKey = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt";

// ---------------------------------------------------------------------------
// 自包含 MD5 (RFC 1321)。仅实现在此文件内部, 不引第三方库。
// ---------------------------------------------------------------------------
struct Md5Ctx {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t total = 0;
    unsigned char buf[64];
    size_t buflen = 0;
};

static inline uint32_t md5_rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

static const uint32_t kMd5K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const int kMd5S[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

static void md5_block(Md5Ctx& ctx, const unsigned char* p) {
    uint32_t M[16];
    for (int i = 0; i < 16; ++i)
        M[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) |
               ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
    uint32_t A = ctx.a, B = ctx.b, C = ctx.c, D = ctx.d;
    for (int i = 0; i < 64; ++i) {
        uint32_t F; int g;
        if (i < 16)      { F = (B & C) | (~B & D);        g = i; }
        else if (i < 32) { F = (D & B) | (~D & C);        g = (5*i + 1) % 16; }
        else if (i < 48) { F = B ^ C ^ D;                 g = (3*i + 5) % 16; }
        else             { F = C ^ (B | ~D);              g = (7*i) % 16; }
        F = F + A + kMd5K[i] + M[g];
        A = D; D = C; C = B;
        B = B + md5_rotl(F, kMd5S[i]);
    }
    ctx.a += A; ctx.b += B; ctx.c += C; ctx.d += D;
}

static void md5_update(Md5Ctx& ctx, const void* data, size_t len) {
    const unsigned char* p = (const unsigned char*)data;
    ctx.total += len;
    if (ctx.buflen) {
        size_t need = 64 - ctx.buflen;
        size_t take = len < need ? len : need;
        memcpy(ctx.buf + ctx.buflen, p, take);
        ctx.buflen += take; p += take; len -= take;
        if (ctx.buflen == 64) { md5_block(ctx, ctx.buf); ctx.buflen = 0; }
    }
    while (len >= 64) { md5_block(ctx, p); p += 64; len -= 64; }
    if (len) { memcpy(ctx.buf, p, len); ctx.buflen = len; }
}

static std::string md5_hex(const std::string& in) {
    Md5Ctx ctx;
    md5_update(ctx, in.data(), in.size());
    uint64_t bits = ctx.total * 8;
    unsigned char pad = 0x80;
    md5_update(ctx, &pad, 1);
    unsigned char zero = 0;
    while (ctx.buflen != 56) md5_update(ctx, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; ++i) lenb[i] = (unsigned char)((bits >> (8*i)) & 0xFF);
    md5_update(ctx, lenb, 8);
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    uint32_t v[4] = { ctx.a, ctx.b, ctx.c, ctx.d };
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            unsigned char byte = (unsigned char)((v[i] >> (8*j)) & 0xFF);
            out += hex[byte >> 4];
            out += hex[byte & 0xF];
        }
    return out;
}

// 酷狗 H5 签名: 参数按 key 升序, 拼 "k=v", 前后各包一次密钥, 取 MD5
//   params: 有序的 (key,value) 列表 (含空值也要参与, 顺序无所谓, 函数内会排序)
static std::string KgSign(const std::vector<std::pair<std::string, std::string>>& params) {
    std::vector<std::pair<std::string, std::string>> sorted = params;
    std::sort(sorted.begin(), sorted.end(),
              [](const std::pair<std::string, std::string>& x,
                 const std::pair<std::string, std::string>& y) { return x.first < y.first; });
    std::string s = kKgSignKey;
    for (const auto& kv : sorted) { s += kv.first; s += '='; s += kv.second; }
    s += kKgSignKey;
    return md5_hex(s);
}

} // namespace

// ============================================================================
//  酷狗: 两种形态
// ============================================================================

bool PlaylistProvider::FetchKugouGcid(const std::string& gcid, PlaylistResult& out) const {
    out.meta.source = "kugou";
    out.meta.id = gcid;

    if (gcid.rfind("collection_", 0) != 0) {
        out.error = "global_collection_id 格式不正确";
        return false;
    }

    // ---- 时间戳 (毫秒) ----
    long long now_ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string ts = std::to_string(now_ms);

    auto build_url = [&](const std::string& base,
                         std::vector<std::pair<std::string, std::string>> params) {
        params.push_back({ "srcappid", "2919" });
        params.push_back({ "clientver", "20000" });
        params.push_back({ "clienttime", ts });
        params.push_back({ "mid", ts });
        params.push_back({ "uuid", ts });
        params.push_back({ "dfid", "-" });
        std::string sig = KgSign(params);
        std::string url = base + "?";
        for (const auto& kv : params) url += kv.first + "=" + kv.second + "&";
        url += "signature=" + sig;
        return url;
    };

    // ---- 第一步: 元信息 (歌单名 / 作者 / 总数) ----
    {
        std::vector<std::pair<std::string, std::string>> p = {
            { "appid", "1058" },
            { "specialid", "0" },
            { "global_specialid", gcid },
            { "format", "jsonp" },
        };
        std::string url = build_url("https://mobiles.kugou.com/api/v5/special/info_v2", p);
        std::string body;
        if (!http_(url, body) || body.empty()) {
            out.error = "酷狗歌单元信息请求失败 (网络或接口变更)";
#ifdef PLAYLIST_DEBUG
            out.error += "\n[DEBUG] url=" + url + "\n[DEBUG] body=" + body.substr(0, 300);
#endif
            return false;
        }
        JsonParser jp(body);
        JValPtr root = jp.Parse();
        const JVal* data = root && root->IsObj() ? root->Get("data") : nullptr;
        if (!data || !data->IsObj()) {
            out.error = "酷狗歌单元信息解析失败 (歌单可能不存在或已删除)";
#ifdef PLAYLIST_DEBUG
            out.error += "\n[DEBUG] url=" + url + "\n[DEBUG] resp=" + body.substr(0, 300);
#endif
            return false;
        }
        out.meta.name = StripHtml(JStr(data->Get("specialname")));
        out.meta.author = JStr(data->Get("nickname"));
        out.meta.description = JStr(data->Get("intro"));
        std::string img = JStrRaw(data->Get("imgurl"));
        // 封面 {size} 占位替换
        size_t sp = img.find("{size}");
        if (sp != std::string::npos) img.replace(sp, 6, "240");
        out.meta.cover_url = img;
        out.meta.total = (int)JNum(data->Get("songcount"));
    }

    // ---- 第二步: 分页拉全部曲目 (每页最多 300) ----
    const int kPageSize = 300;
    int total = out.meta.total;
    int page = 1;
    int fetched = 0;
    // 上限保护: 最多 100 页 (3 万首), 防止异常歌单死循环
    for (; page <= 100; ++page) {
        int limit = kPageSize;
        if (total > 0) {
            int remain = total - fetched;
            if (remain <= 0) break;
            if (remain < limit) limit = remain;
        }
        std::vector<std::pair<std::string, std::string>> p = {
            { "appid", "1058" },
            { "global_specialid", gcid },
            { "specialid", "0" },
            { "plat", "0" },
            { "version", "8000" },
            { "page", std::to_string(page) },
            { "pagesize", std::to_string(limit) },
        };
        std::string url = build_url("https://mobiles.kugou.com/api/v5/special/song_v2", p);
        std::string body;
        if (!http_(url, body) || body.empty()) {
            if (page == 1) { out.error = "酷狗歌单曲目请求失败 (网络或接口变更)"; return false; }
            break;   // 部分成功: 保留已拿到的
        }
        JsonParser jp(body);
        JValPtr root = jp.Parse();
        const JVal* data = root && root->IsObj() ? root->Get("data") : nullptr;
        const JVal* info = data && data->IsObj() ? data->Get("info") : nullptr;
        if (!info || !info->IsArr() || info->Size() == 0) break;

        for (size_t k = 0; k < info->Size(); ++k) {
            const JVal* s = info->At(k);
            if (!s || !s->IsObj()) continue;
            PlaylistTrack t;
            t.source = "kugou";
            // filename 形如 "Artist - Title"; 另用 trans_param? 此处优先拆 filename
            std::wstring fn = StripHtml(JStr(s->Get("filename")));
            std::wstring sep = L" - ";
            size_t d = fn.find(sep);
            if (d != std::wstring::npos) {
                t.artist = fn.substr(0, d);
                t.title  = fn.substr(d + sep.size());
            } else {
                t.title = fn;
            }
            // 兜底: 若 filename 拆不出, 用 songname/author_name (部分接口有)
            if (t.title.empty())  t.title  = StripHtml(JStr(s->Get("songname")));
            if (t.artist.empty()) t.artist = StripHtml(JStr(s->Get("author_name")));
            if (t.album.empty())  t.album  = JStr(s->Get("album_name"));

            // duration 单位: 秒 (老接口是毫秒, 这里统一换算)
            double dur = JNum(s->Get("duration"));
            if (dur > 0) t.duration_ms = (uint32_t)((long long)(dur * 1000.0 + 0.5));

            t.id              = JStrRaw(s->Get("hash"));
            t.kugou_hash_320  = JStrRaw(s->Get("320hash"));
            t.kugou_hash_flac = JStrRaw(s->Get("sqhash"));
            double aid        = JNum(s->Get("audio_id"));
            if (aid > 0) t.kugou_audio_id = std::to_string((long long)aid);
            t.kugou_album_id  = JStrRaw(s->Get("album_id"));

            if (t.title.empty()) continue;
            if (t.duration_ms > 0) ++out.with_duration;
            if (!t.album.empty())  ++out.with_album;
            out.tracks.push_back(std::move(t));
        }
        fetched = (int)out.tracks.size();
    }

    if (out.tracks.empty()) { out.error = "歌单为空或全部条目无效"; return false; }
    if (out.meta.total <= 0) out.meta.total = (int)out.tracks.size();
    out.ok = true;
    return true;
}

bool PlaylistProvider::FetchKugou(const std::string& id, PlaylistResult& out) const {
    // 新版 global_collection_id 形态
    if (id.rfind("gcid:", 0) == 0)
        return FetchKugouGcid(id.substr(5), out);
    if (id.rfind("collection_", 0) == 0)
        return FetchKugouGcid(id, out);

    out.meta.source = "kugou";
    out.meta.id = id;

    // ⚠ 必须用 http 明文, 不能用 https:
    //   www2.kugou.kugou.com 是 CDN 节点, 其 TLS 证书 hostname 与域名不匹配
    //   (python ssl 严格校验报 Hostname mismatch; WinHTTP 报 12175
    //    ERROR_WINHTTP_SECURE_FAILURE, 且 SECURITY_FLAGS 对发送阶段失败无效)
    //   实测该页面 http 明文返回 200 且数据完整 —— 公开歌单数据无需加密传输。
    std::string url = "http://www2.kugou.kugou.com/yueku/v9/special/single/" + id + "-5-9999.html";
    std::string body;
    if (!http_(url, body)) {
        out.error = "酷狗页面请求失败 (网络或接口变更)";
        return false;
    }
    // 兼容: 若返回内容里没有 JSON, 可能是页面结构变了
    if (body.find("global.data") == std::string::npos) {
        out.error = "酷狗页面结构变更: 未找到 global.data";
        return false;
    }

    // ---- 抠曲目数组 ----
    size_t sp = body.find("global.data = [");
    if (sp == std::string::npos) { out.error = "未匹配到 global.data 数组"; return false; }
    sp += strlen("global.data = ");
    // 从 [ 开始做括号配对, 找匹配的 ]
    int depth = 0;
    bool instr = false, esc = false;
    size_t se = std::string::npos;
    for (size_t k = sp; k < body.size(); ++k) {
        char c = body[k];
        if (instr) {
            if (esc) { esc = false; }
            else if (c == '\\') { esc = true; }
            else if (c == '"') { instr = false; }
        } else {
            if (c == '"') instr = true;
            else if (c == '[') ++depth;
            else if (c == ']') { if (--depth == 0) { se = k + 1; break; } }
        }
    }
    if (se == std::string::npos) { out.error = "global.data 数组不完整"; return false; }

    std::string arr_str = body.substr(sp, se - sp);
    JsonParser jp(arr_str);
    JValPtr arr = jp.Parse();
    if (!arr || !arr->IsArr()) { out.error = "global.data 解析失败"; return false; }

    // ---- 抠歌单名/封面 ----
    {
        size_t gp = body.find("global = {");
        if (gp != std::string::npos) {
            size_t ge = body.find("};", gp);
            if (ge != std::string::npos) {
                std::string meta_str = body.substr(gp + strlen("global = "), ge - gp);
                std::regex re_name(R"(name:\s*"([^"]*)\")");
                std::regex re_pic(R"(pic:\s*"([^"]*)\")");
                std::smatch m;
                if (std::regex_search(meta_str, m, re_name)) out.meta.name = ToWide(m[1].str());
                if (std::regex_search(meta_str, m, re_pic))  out.meta.cover_url = m[1].str();
            }
        }
    }

    // ---- 逐条曲目 ----
    for (size_t k = 0; k < arr->Size(); ++k) {
        const JVal* s = arr->At(k);
        if (!s || !s->IsObj()) continue;
        PlaylistTrack t;
        t.source = "kugou";
        t.title  = StripHtml(JStr(s->Get("songname")));
        t.artist = StripHtml(JStr(s->Get("singername")));
        t.album  = JStr(s->Get("album_name"));
        // 时长: duration 为毫秒 (酷狗部分字段是秒 *1000 的整值)
        double dur = JNum(s->Get("duration"));
        t.duration_ms = (uint32_t)(dur > 0 ? (long long)(dur + 0.5) : 0);

        t.id = JStrRaw(s->Get("hash"));
        t.kugou_hash_320  = JStrRaw(s->Get("hash_320"));
        t.kugou_hash_flac = JStrRaw(s->Get("hash_flac"));
        t.kugou_audio_id  = JStrRaw(s->Get("audio_id"));
        t.kugou_album_id  = JStrRaw(s->Get("album_id"));

        if (t.title.empty()) continue;   // 无歌名视为无效条目
        if (t.duration_ms > 0) ++out.with_duration;
        if (!t.album.empty())  ++out.with_album;
        out.tracks.push_back(std::move(t));
    }

    out.meta.total = (int)out.tracks.size();
    if (out.tracks.empty()) { out.error = "歌单为空或全部条目无效"; return false; }
    out.ok = true;
    return true;
}

// ============================================================================
//  网易云: playlist/detail + song/detail 两步式
// ============================================================================

bool PlaylistProvider::FetchNetease(const std::string& id, PlaylistResult& out) const {
    out.meta.source = "netease";
    out.meta.id = id;

    // ---- 第一步: 取歌单元信息 + trackIds ----
    std::string url = "http://music.163.com/api/v6/playlist/detail?id=" + UrlEncode(id) +
                      "&n=1000&s=0&t=0";
    std::string body;
    if (!http_(url, body)) { out.error = "网易云歌单请求失败 (网络或接口变更)"; return false; }

    JsonParser jp(body);
    JValPtr root = jp.Parse();
    if (!root || !root->IsObj()) { out.error = "网易云歌单响应解析失败"; return false; }
    const JVal* pl = root->Get("playlist");
    if (!pl || !pl->IsObj()) {
        double code = JNum(root->Get("code"));
        out.error = (code == 404) ? "歌单不存在或已删除"
                                   : "网易云未返回歌单数据 (可能需要登录或接口变更)";
        return false;
    }

    out.meta.name = StripHtml(JStr(pl->Get("name")));
    out.meta.cover_url = JStrRaw(pl->Get("coverImgUrl"));
    out.meta.description = JStr(pl->Get("description"));
    if (const JVal* cr = pl->Get("creator"))
        out.meta.author = JStr(cr->Get("nickname"));
    out.meta.total = (int)JNum(pl->Get("trackCount"));

    // trackIds: 元素是对象 {"id":123,...} (也可能是裸数字, 两种都兼容)
    const JVal* tids = pl->Get("trackIds");
    std::vector<std::string> ids;
    if (tids && tids->IsArr()) {
        for (size_t k = 0; k < tids->Size(); ++k) {
            const JVal* e = tids->At(k);
            if (!e) continue;
            if (e->IsObj()) {
                double v = JNum(e->Get("id"));
                if (v > 0) ids.push_back(std::to_string((long long)v));
            } else if (e->IsNum()) {
                ids.push_back(std::to_string((long long)e->num));
            } else if (e->IsStr() && !e->str.empty()) {
                ids.push_back(e->str);
            }
        }
    }

    // 若 playlist 里直接带了 tracks (新版小节歌单可能只给前几首), 先收下
    if (const JVal* tr = pl->Get("tracks"); tr && tr->IsArr() && tr->Size() > 0) {
        for (size_t k = 0; k < tr->Size(); ++k) {
            const JVal* s = tr->At(k);
            if (!s || !s->IsObj()) continue;
            PlaylistTrack t;
            t.source = "netease";
            t.title  = StripHtml(JStr(s->Get("name")));
            const JVal* ar = s->Get("ar") ? s->Get("ar") : s->Get("artists");
            if (ar && ar->IsArr()) {
                std::wstring a;
                for (size_t j = 0; j < ar->Size(); ++j) {
                    const JVal* one = ar->At(j);
                    if (!one) continue;
                    if (!a.empty()) a += L" / ";
                    a += JStr(one->Get("name"));
                }
                t.artist = a;
            }
            const JVal* al = s->Get("al") ? s->Get("al") : s->Get("album");
            if (al) {
                t.album = JStr(al->Get("name"));
                t.netease_album_id = std::to_string((long long)JNum(al->Get("id")));
            }
            double dt = JNum(s->Get("dt"));
            if (dt <= 0) dt = JNum(s->Get("duration"));
            t.duration_ms = (uint32_t)(dt > 0 ? (long long)(dt + 0.5) : 0);
            t.id = std::to_string((long long)JNum(s->Get("id")));
            if (!t.title.empty()) out.tracks.push_back(std::move(t));
        }
    }

    // ---- 第二步: 分批补全 (每批 200, 实测安全) ----
    // 若第一步已把 tracks 全给全了, 可跳过; 这里以 trackIds 为准做完整补全
    if (!ids.empty()) {
        std::vector<PlaylistTrack> full;
        const size_t kBatch = 200;
        for (size_t start = 0; start < ids.size(); start += kBatch) {
            size_t end = std::min(start + kBatch, ids.size());
            std::string csv;
            for (size_t k = start; k < end; ++k) {
                if (!csv.empty()) csv += ",";
                csv += ids[k];
            }
            std::string u2 = "http://music.163.com/api/song/detail/?id=" + csv +
                             "&ids=" + UrlEncode("[" + csv + "]");
            std::string b2;
            if (!http_(u2, b2)) continue;   // 单批失败不致命, 继续下一批
            JsonParser jp2(b2);
            JValPtr r2 = jp2.Parse();
            if (!r2 || !r2->IsObj()) continue;
            const JVal* songs = r2->Get("songs");
            if (!songs || !songs->IsArr()) continue;
            for (size_t k = 0; k < songs->Size(); ++k) {
                const JVal* s = songs->At(k);
                if (!s || !s->IsObj()) continue;
                PlaylistTrack t;
                t.source = "netease";
                t.title  = StripHtml(JStr(s->Get("name")));
                const JVal* ar = s->Get("ar") ? s->Get("ar") : s->Get("artists");
                if (ar && ar->IsArr()) {
                    std::wstring a;
                    for (size_t j = 0; j < ar->Size(); ++j) {
                        const JVal* one = ar->At(j);
                        if (!one) continue;
                        if (!a.empty()) a += L" / ";
                        a += JStr(one->Get("name"));
                    }
                    t.artist = a;
                }
                const JVal* al = s->Get("al") ? s->Get("al") : s->Get("album");
                if (al) {
                    t.album = JStr(al->Get("name"));
                    t.netease_album_id = std::to_string((long long)JNum(al->Get("id")));
                }
                double dt = JNum(s->Get("dt"));
                if (dt <= 0) dt = JNum(s->Get("duration"));
                t.duration_ms = (uint32_t)(dt > 0 ? (long long)(dt + 0.5) : 0);
                t.id = std::to_string((long long)JNum(s->Get("id")));
                if (!t.title.empty()) full.push_back(std::move(t));
            }
        }
        // 补全结果优先 (字段更全); 若补全失败则保留第一步的 tracks
        if (!full.empty()) out.tracks.swap(full);
    }

    if (out.tracks.empty()) { out.error = "歌单为空或曲目全部无效"; return false; }

    out.with_duration = 0; out.with_album = 0;
    for (const auto& t : out.tracks) {
        if (t.duration_ms > 0) ++out.with_duration;
        if (!t.album.empty())  ++out.with_album;
    }
    // 元信息里的 total 若与实际不符 (歌单有下架曲目), 以实际为准但保留标称值
    out.ok = true;
    return true;
}

// ============================================================================
//  QQ音乐: musicu.fcg 单步式, 匿名免签名, song_begin 分页
// ============================================================================

bool PlaylistProvider::FetchQQMusic(const std::string& id, PlaylistResult& out) const {
    out.meta.source = "qqmusic";
    out.meta.id = id;

    bool all_digit = !id.empty() &&
        std::all_of(id.begin(), id.end(), [](char c) { return c >= '0' && c <= '9'; });
    if (!all_digit) { out.error = "QQ音乐歌单 ID 非法 (应为纯数字)"; return false; }

    std::vector<PlaylistTrack> tracks;
    std::set<std::string> seen;
    const int kPage = 300;      // 实测 300 稳定; 1000 亦可但会被服务端自行截到 ~997
    int begin = 0;
    int total = 0;
    bool first = true;
    bool got_any_page = false;
    int guard = 0;

    while (guard++ < 50) {
        // 组装 musicu.fcg 的 data JSON (紧凑无空格, 减少 URL 长度)
        std::string req = "{\"comm\":{\"ct\":24,\"cv\":0},\"req_1\":{"
            "\"module\":\"music.srfDissInfo.aiDissInfo\","
            "\"method\":\"uniform_get_Dissinfo\","
            "\"param\":{\"disstid\":" + id + ",\"enc_host_uin\":\"\","
            "\"tag\":1,\"userinfo\":1,"
            "\"song_begin\":" + std::to_string(begin) + ","
            "\"song_num\":" + std::to_string(kPage) + "}}}";

        std::string url = "https://u.y.qq.com/cgi-bin/musicu.fcg?format=json&data=" +
                          UrlEncode(req);
        std::string body;
        if (!http_(url, body)) {
            if (first) { out.error = "QQ音乐歌单请求失败 (网络或接口变更)"; return false; }
            break;   // 后续页失败不致命, 保留已拿到的部分
        }
        got_any_page = true;

        JsonParser jp(body);
        JValPtr root = jp.Parse();
        if (!root || !root->IsObj()) {
            if (first) { out.error = "QQ音乐歌单响应解析失败"; return false; }
            break;
        }
        const JVal* r1 = root->Get("req_1");
        if (!r1 || !r1->IsObj()) {
            if (first) { out.error = "QQ音乐响应缺少 req_1 (接口变更?)"; return false; }
            break;
        }
        double code = JNum(r1->Get("code"));
        const JVal* data = r1->Get("data");
        if (!data || !data->IsObj()) {
            if (first) {
                out.error = (code == 0)
                    ? "QQ音乐未返回歌单数据 (可能无版权或接口变更)"
                    : "QQ音乐歌单不可访问 (code=" + std::to_string((long long)code) + ")";
                return false;
            }
            break;
        }

        if (first) {
            const JVal* di = data->Get("dirinfo");
            if (di && di->IsObj()) {
                out.meta.name        = StripHtml(JStr(di->Get("title")));
                out.meta.description = StripHtml(JStr(di->Get("desc")));
                out.meta.cover_url   = JStrRaw(di->Get("picurl"));
            }
            total = (int)JNum(data->Get("total_song_num"));
            out.meta.total = total;
        }

        const JVal* sl = data->Get("songlist");
        if (!sl || !sl->IsArr() || sl->Size() == 0) break;

        int new_cnt = 0;
        for (size_t k = 0; k < sl->Size(); ++k) {
            const JVal* s = sl->At(k);
            if (!s || !s->IsObj()) continue;

            // mid 即 songmid, 是 QQ音乐侧的主键
            std::string mid = JStrRaw(s->Get("mid"));
            if (mid.empty()) continue;
            if (seen.count(mid)) continue;
            seen.insert(mid);
            ++new_cnt;

            PlaylistTrack t;
            t.source = "qqmusic";
            t.id     = mid;

            std::wstring nm = JStr(s->Get("name"));
            if (nm.empty()) nm = JStr(s->Get("title"));
            t.title = StripHtml(nm);

            const JVal* sg = s->Get("singer");
            if (sg && sg->IsArr()) {
                std::wstring a;
                for (size_t j = 0; j < sg->Size(); ++j) {
                    const JVal* one = sg->At(j);
                    if (!one) continue;
                    std::wstring xn = JStr(one->Get("name"));
                    if (xn.empty()) continue;
                    if (!a.empty()) a += L" / ";
                    a += xn;
                }
                t.artist = a;
            }

            const JVal* al = s->Get("album");
            if (al && al->IsObj()) {
                t.album        = JStr(al->Get("name"));
                t.qq_album_mid = JStrRaw(al->Get("mid"));
            }

            // ⚠ interval 单位是【秒】(勿与网易云的毫秒 dt 混淆)
            double sec = JNum(s->Get("interval"));
            if (sec > 0) t.duration_ms = (uint32_t)(long long)(sec * 1000.0 + 0.5);

            const JVal* fl = s->Get("file");
            if (fl && fl->IsObj()) t.qq_media_mid = JStrRaw(fl->Get("media_mid"));

            if (s->Get("songtype")) t.qq_song_type = (int)JNum(s->Get("songtype"));

            if (!t.title.empty()) tracks.push_back(std::move(t));
        }

        if (new_cnt == 0) break;                  // 无新增 → 已到尾部
        begin += (int)sl->Size();                 // 按【实际返回条数】推进, 不用 kPage
        if (total > 0 && begin >= total) break;
    }

    if (!got_any_page) { out.error = "QQ音乐歌单请求失败 (网络或接口变更)"; return false; }
    if (tracks.empty()) { out.error = "歌单为空或全部条目无效"; return false; }

    out.tracks.swap(tracks);
    out.with_duration = 0; out.with_album = 0;
    for (const auto& t : out.tracks) {
        if (t.duration_ms > 0) ++out.with_duration;
        if (!t.album.empty())  ++out.with_album;
    }
    out.ok = true;
    return true;
}

// ============================================================================
//  主入口
// ============================================================================

PlaylistResult PlaylistProvider::Parse(const std::string& input) const {
    Platform plat = Platform::Unknown;
    std::string id;
    std::string err;
    if (!ResolveInput(input, plat, id, &err)) {
        PlaylistResult r;
        r.error = err;
        return r;
    }
    return ParseAs(plat, id);
}

PlaylistResult PlaylistProvider::ParseAs(Platform plat, const std::string& id) const {
    PlaylistResult r;
    switch (plat) {
        case Platform::Kugou:   FetchKugou(id, r); break;
        case Platform::NetEase: FetchNetease(id, r); break;
        case Platform::QQMusic: FetchQQMusic(id, r); break;
        default:
            r.error = "平台未指定或不支持";
            break;
    }
    return r;
}

// ============================================================================
//  工具函数
// ============================================================================

PlaylistProvider::PlaylistProvider(HttpGetFn http)
    : http_(http ? http : &PlaylistProvider::DefaultHttpGet) {}

std::string PlaylistProvider::UrlEncode(const std::string& utf8) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::wstring PlaylistProvider::Utf8ToWide(const std::string& s) { return ToWide(s); }
std::string PlaylistProvider::WideToUtf8(const std::wstring& s) { return ToUtf8(s); }
std::wstring PlaylistProvider::Trim(const std::wstring& s) { return TrimW(s); }

bool PlaylistProvider::DefaultHttpGet(const std::string& url, std::string& body) {
#ifdef _WIN32
    std::wstring wurl = ToWide(url);
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    // ⚠ 缓冲区必须够大: 网易云 song/detail 一次拼 200 个 ID 时 URL 可达 ~5KB.
    //   本机旧实现用 path[4096]/extra[2048] → 长 URL 被静默截断 → 请求参数丢失.
    //   这里直接按 URL 长度动态分配。
    const size_t cap = wurl.size() + 64;
    std::wstring hostbuf(256, L'\0');
    std::wstring pathbuf(cap, L'\0');
    std::wstring extrabuf(cap, L'\0');
    uc.lpszHostName = &hostbuf[0];        uc.dwHostNameLength = 255;
    uc.lpszUrlPath = &pathbuf[0];         uc.dwUrlPathLength = (DWORD)(cap - 1);
    uc.lpszExtraInfo = &extrabuf[0];      uc.dwExtraInfoLength = (DWORD)(cap - 1);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &uc)) return false;
    hostbuf.resize(uc.dwHostNameLength);
    pathbuf.resize(uc.dwUrlPathLength);
    extrabuf.resize(uc.dwExtraInfoLength);

    bool https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    std::wstring full_path = pathbuf + extrabuf;

    HINTERNET ses = WinHttpOpen(L"KugouPlayer/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) return false;
    WinHttpSetTimeouts(ses, 8000, 8000, 10000, 10000);

    HINTERNET con = WinHttpConnect(ses, hostbuf.c_str(), uc.nPort, 0);
    if (!con) { WinHttpCloseHandle(ses); return false; }

    // ⚠ Referer 必须在 WinHttpOpenRequest 时传入:
    //   若在此传 WINHTTP_NO_REFERER, 后续 WinHttpAddRequestHeaders("Referer: ...")
    //   【不会生效】(WinHTTP 认为 Referer 槽位已占)。
    const wchar_t* ref = WINHTTP_NO_REFERER;
    if (url.find("163.com") != std::string::npos) {
        ref = L"https://music.163.com/";
    } else if (url.find("y.qq.com") != std::string::npos ||
               url.find("qq.com") != std::string::npos) {
        ref = L"https://y.qq.com/";
    }

    HINTERNET req = WinHttpOpenRequest(con, L"GET", full_path.c_str(), nullptr,
        ref, WINHTTP_DEFAULT_ACCEPT_TYPES, https ? WINHTTP_FLAG_SECURE : 0);
    if (!req) { WinHttpCloseHandle(con); WinHttpCloseHandle(ses); return false; }

    WinHttpAddRequestHeaders(req, L"User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                                 L"AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36",
                             (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);

    // ⚠ 酷狗 mobiles.kugou.com API 网关【强制】要求客户端标识【请求头】:
    //     mid / dfid / clienttime  —— 缺任意一个一律回 {"errcode":1001,"status":0}
    //   (实测: 只带 UA 不行、只带 Referer 不行; 这三个头与 query 里的
    //    mid=uuid=clienttime 保持一致即可。UA 只要非空就通过。)
    if (url.find("kugou.com") != std::string::npos &&
        url.find("mobiles.kugou.com") != std::string::npos) {
        std::string ts = std::to_string((long long)
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        std::wstring wmid = L"mid: " + ToWide(ts);
        std::wstring wct  = L"clienttime: " + ToWide(ts);
        WinHttpAddRequestHeaders(req, wmid.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        WinHttpAddRequestHeaders(req, wct.c_str(),  (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
        WinHttpAddRequestHeaders(req, L"dfid: -",   (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }

    bool ok = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(req, nullptr);
    if (ok) {
        DWORD status = 0, slen = sizeof(status);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &slen, WINHTTP_NO_HEADER_INDEX);
        ok = (status >= 200 && status < 300);
    }
    if (ok) {
        body.clear();
        DWORD avail = 0;
        do {
            avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
            if (avail == 0) break;
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(req, &chunk[0], avail, &read) || read == 0) { ok = false; break; }
            chunk.resize(read);
            body += chunk;
        } while (avail > 0);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return ok;
#else
    (void)url;
    body.clear();
    return false;
#endif
}

} // namespace playlist_core
