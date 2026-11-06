#include "online_music.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <set>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

namespace online_core {
namespace {

// ===========================================================================
//  极简 JSON
//
//  这几个接口的响应是 data.info[] / result.songs[] 这种多层嵌套，手扫字符串
//  一旦遇到字段顺序变化或数组里夹 null 就会静默取错值，所以建个小树。
//  只实现够用的子集：对象/数组/字符串/数字/布尔/null。
// ===========================================================================

struct JValue {
    enum class T { Null, Bool, Num, Str, Arr, Obj };
    T t = T::Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JValue> arr;
    std::vector<std::pair<std::string, JValue>> obj;

    const JValue* Find(const char* key) const {
        if (t != T::Obj) return nullptr;
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    const JValue* At(size_t i) const {
        if (t != T::Arr || i >= arr.size()) return nullptr;
        return &arr[i];
    }
    size_t Size() const { return t == T::Arr ? arr.size() : 0; }

    std::string S(const char* key) const {
        const JValue* v = Find(key);
        return (v && v->t == T::Str) ? v->str : std::string();
    }
    long long I(const char* key, long long def = 0) const {
        const JValue* v = Find(key);
        if (!v) return def;
        if (v->t == T::Num) return static_cast<long long>(v->num);
        if (v->t == T::Str) {
            try { return std::stoll(v->str); } catch (...) { return def; }
        }
        return def;
    }
};

void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    bool Parse(JValue& out) {
        SkipWs();
        if (!ParseValue(out)) return false;
        SkipWs();
        return true;
    }

private:
    void SkipWs() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i_;
            else break;
        }
    }
    bool ReadHex4(unsigned& v) {
        if (i_ + 4 > s_.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool ParseString(std::string& out) {
        if (i_ >= s_.size() || s_[i_] != '"') return false;
        ++i_;
        out.clear();
        while (i_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[i_++]);
            if (c == '"') return true;
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (i_ >= s_.size()) return false;
            const char e = s_[i_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    unsigned cp = 0;
                    if (!ReadHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 1 < s_.size() &&
                        s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                        i_ += 2;
                        unsigned lo = 0;
                        if (!ReadHex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool ParseNumber(JValue& v) {
        const size_t start = i_;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
                ++i_;
            else break;
        }
        if (i_ == start) return false;
        v.t = JValue::T::Num;
        v.num = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        return true;
    }
    bool ParseArray(JValue& v) {
        if (s_[i_] != '[') return false;
        ++i_;
        v.t = JValue::T::Arr;
        SkipWs();
        if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
        for (;;) {
            JValue item;
            if (!ParseValue(item)) return false;
            v.arr.push_back(std::move(item));
            SkipWs();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { ++i_; SkipWs(); continue; }
            if (s_[i_] == ']') { ++i_; return true; }
            return false;
        }
    }
    bool ParseObject(JValue& v) {
        if (s_[i_] != '{') return false;
        ++i_;
        v.t = JValue::T::Obj;
        SkipWs();
        if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
        for (;;) {
            SkipWs();
            std::string key;
            if (!ParseString(key)) return false;
            SkipWs();
            if (i_ >= s_.size() || s_[i_] != ':') return false;
            ++i_;
            JValue item;
            if (!ParseValue(item)) return false;
            v.obj.emplace_back(std::move(key), std::move(item));
            SkipWs();
            if (i_ >= s_.size()) return false;
            if (s_[i_] == ',') { ++i_; continue; }
            if (s_[i_] == '}') { ++i_; return true; }
            return false;
        }
    }
    bool ParseValue(JValue& v) {
        SkipWs();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        switch (c) {
            case '{': return ParseObject(v);
            case '[': return ParseArray(v);
            case '"': v.t = JValue::T::Str; return ParseString(v.str);
            case 't':
                if (s_.compare(i_, 4, "true") == 0) { i_ += 4; v.t = JValue::T::Bool; v.b = true; return true; }
                return false;
            case 'f':
                if (s_.compare(i_, 5, "false") == 0) { i_ += 5; v.t = JValue::T::Bool; v.b = false; return true; }
                return false;
            case 'n':
                if (s_.compare(i_, 4, "null") == 0) { i_ += 4; v.t = JValue::T::Null; return true; }
                return false;
            default: return ParseNumber(v);
        }
    }

    const std::string& s_;
    size_t i_ = 0;
};

bool ParseJson(const std::string& text, JValue& out) {
    JsonParser p(text);
    return p.Parse(out);
}

// ===========================================================================
//  小工具
// ===========================================================================

std::string UrlEncode(const std::string& utf8) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

using Params = std::vector<std::pair<std::string, std::string>>;

// 按 key 覆盖写（对应 JS 里的 Object.assign）。直接 push_back 会产生重复 key，
// 签名会把两个都拼进去，而服务端只认一个 —— 签名就永远对不上。
void SetParam(Params& p, const std::string& key, const std::string& value) {
    for (auto& kv : p)
        if (kv.first == key) { kv.second = value; return; }
    p.emplace_back(key, value);
}

std::string QueryString(const Params& params) {
    std::string out;
    for (const auto& kv : params) {
        if (!out.empty()) out.push_back('&');
        out += UrlEncode(kv.first);
        out.push_back('=');
        out += UrlEncode(kv.second);
    }
    return out;
}

std::string ToLowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

// 酷狗榜单只给 filename="艺人 - 歌名"，没有 singername 字段。
void SplitKugouFilename(const std::string& filename, const std::string& fallbackTitle,
                        std::string& artist, std::string& title) {
    const std::string sep = " - ";
    const size_t p = filename.find(sep);
    if (p != std::string::npos && p > 0) {
        artist = Trim(filename.substr(0, p));
        title = Trim(filename.substr(p + sep.size()));
        if (!title.empty()) return;
    }
    title = fallbackTitle.empty() ? filename : fallbackTitle;
    artist.clear();
}

std::string KugouCover(const std::string& imgurl) {
    std::string u = imgurl;
    const std::string ph = "{size}";
    const size_t p = u.find(ph);
    if (p != std::string::npos) u.replace(p, ph.size(), "400");
    if (u.rfind("http://", 0) == 0) u.replace(0, 7, "https://");
    return u;
}

// 标准 base64 解码（容忍换行/空白，非法字符跳过）
bool Base64Decode(const std::string& in, std::vector<uint8_t>& out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    out.clear();
    int buf = 0, bits = 0;
    for (char c : in) {
        if (c == '=') break;
        const int v = val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((buf >> bits) & 0xFF));
        }
    }
    return !out.empty();
}

// ===========================================================================
//  MD5（RFC 1321）—— 签名与设备号都要用，不引第三方
// ===========================================================================

void Md5Block(const uint8_t* p, uint32_t h[4]) {
    static const uint32_t S[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};

    uint32_t w[16];
    for (int i = 0; i < 16; ++i)
        w[i] = static_cast<uint32_t>(p[i * 4]) |
               (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(p[i * 4 + 2]) << 16) |
               (static_cast<uint32_t>(p[i * 4 + 3]) << 24);

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; ++i) {
        uint32_t f = 0, g = 0;
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { f = c ^ (b | ~d); g = (7 * i) % 16; }
        const uint32_t tmp = d;
        d = c;
        c = b;
        const uint32_t x = a + f + K[i] + w[g];
        b = b + ((x << S[i]) | (x >> (32 - S[i])));
        a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

// ===========================================================================
//  平台常量
// ===========================================================================

const char* kAndroidSalt = "OIlwieks28dk2k092lksi2UIkp";
const char* kWebSalt = "NVPh5oo715z5DIWAeQlhMDsWXXQV4hwt";
const char* kKeySalt = "57ae12eb6890223e355ccfcb74edf70d";
const char* kAppId = "1005";
const char* kClientVer = "20489";
const char* kSrcAppId = "2919";
const char* kUa = "Android15-1070-11083-46-0-DiscoveryDRADProtocol-wifi";
const char* kPageId = "151369488";
const char* kPPageId = "463467626,350369493,788954147";

std::string g_deviceMid;

long long NowSec() { return static_cast<long long>(time(nullptr)); }

Params BaseParams(const char* appid) {
    return {{"dfid", "-"},
            {"mid", KugouAuth::DeviceMid()},
            {"uuid", "-"},
            {"appid", appid},
            {"clientver", kClientVer},
            {"clienttime", std::to_string(NowSec())}};
}

void AddKugouHeaders(HttpRequest& req) {
    req.headers.push_back({"User-Agent", kUa});
    req.headers.push_back({"dfid", "-"});
    req.headers.push_back({"mid", KugouAuth::DeviceMid()});
    req.headers.push_back({"clienttime", std::to_string(NowSec())});
    req.headers.push_back({"kg-rc", "1"});
    req.headers.push_back({"kg-thash", "5d816a0"});
    req.headers.push_back({"kg-rec", "1"});
    req.headers.push_back({"kg-rf", "B9EDA08A64250DEFFBCADDEE00F8F25F"});
}

} // namespace

// ===========================================================================
//  MD5 / 签名
// ===========================================================================

std::string Md5Hex(const std::string& data) {
    uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    std::string msg = data;
    const uint64_t bitLen = static_cast<uint64_t>(msg.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 0; i < 8; ++i)
        msg.push_back(static_cast<char>((bitLen >> (8 * i)) & 0xFF));

    for (size_t off = 0; off < msg.size(); off += 64)
        Md5Block(reinterpret_cast<const uint8_t*>(msg.data() + off), h);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (uint32_t v : h)
        for (int i = 0; i < 4; ++i) {
            const unsigned char byte = static_cast<unsigned char>((v >> (8 * i)) & 0xFF);
            out.push_back(hex[byte >> 4]);
            out.push_back(hex[byte & 0x0F]);
        }
    return out;
}

namespace {

std::string SignWithSalt(const Params& params, const std::string& body, const char* salt) {
    Params sorted = params;
    std::sort(sorted.begin(), sorted.end(),
              [](const std::pair<std::string, std::string>& a,
                 const std::pair<std::string, std::string>& b) { return a.first < b.first; });
    std::string joined;
    for (const auto& kv : sorted) {
        joined += kv.first;
        joined.push_back('=');
        joined += kv.second;
    }
    return Md5Hex(std::string(salt) + joined + body + salt);
}

} // namespace

std::string SignAndroid(const Params& params, const std::string& body) {
    return SignWithSalt(params, body, kAndroidSalt);
}

std::string SignWeb(const Params& params, const std::string& body) {
    return SignWithSalt(params, body, kWebSalt);
}

const char* SourceName(Source s) { return s == Source::Kugou ? "kugou" : "netease"; }
const char* SourceLabel(Source s) { return s == Source::Kugou ? "酷狗" : "网易云"; }

const char* QualityKey(Quality q) {
    switch (q) {
        case Quality::Flac: return "flac";
        case Quality::High: return "320";
        default: return "128";
    }
}
const char* QualityLabel(Quality q) {
    switch (q) {
        case Quality::Flac: return "无损";
        case Quality::High: return "320K";
        default: return "128K";
    }
}

bool Track::Paywalled() const {
    if (source == Source::Kugou) return payType != 0 || privilege >= 8;
    return payType == 1 || payType == 4;   // 网易 fee: 0/8 免费，1 会员
}

std::string Track::DurationText() const {
    if (durationMs <= 0) return std::string();
    const int total = durationMs / 1000;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", total / 60, total % 60);
    return std::string(buf);
}

// ===========================================================================
//  HTTP（WinHTTP 默认实现）
// ===========================================================================

#ifdef _WIN32
namespace {

std::wstring U8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

bool WinHttpSend(const HttpRequest& req, HttpResponse& resp) {
    resp = HttpResponse();
    const std::wstring wurl = U8ToW(req.url);
    const size_t schemeEnd = wurl.find(L"://");
    if (schemeEnd == std::wstring::npos) return false;

    const std::wstring scheme = wurl.substr(0, schemeEnd);
    const std::wstring rest = wurl.substr(schemeEnd + 3);
    const size_t slash = rest.find(L'/');
    const std::wstring hostport = (slash == std::wstring::npos) ? rest : rest.substr(0, slash);
    const std::wstring object = (slash == std::wstring::npos) ? L"/" : rest.substr(slash);

    std::wstring host = hostport;
    INTERNET_PORT port = 0;
    const size_t colon = hostport.find(L':');
    if (colon != std::wstring::npos) {
        host = hostport.substr(0, colon);
        port = static_cast<INTERNET_PORT>(_wtoi(hostport.c_str() + colon + 1));
    }
    const bool secure = (scheme == L"https");
    if (port == 0) port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    if (host.empty()) return false;

    HINTERNET session = WinHttpOpen(U8ToW(kUa).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    const int t = req.timeoutMs > 0 ? req.timeoutMs : 15000;
    WinHttpSetTimeouts(session, t / 3, t / 3, t / 2, t);

    HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
    if (conn) {
        const std::wstring method = (req.method == "POST") ? L"POST" : L"GET";
        HINTERNET r = WinHttpOpenRequest(conn, method.c_str(), object.c_str(), NULL,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0);
        if (r) {
            std::wstring headers = L"Accept: */*\r\nAccept-Encoding: identity\r\n";
            for (const auto& h : req.headers)
                headers += U8ToW(h.first) + L": " + U8ToW(h.second) + L"\r\n";

            LPVOID bodyPtr = WINHTTP_NO_REQUEST_DATA;
            DWORD bodyLen = 0;
            if (!req.body.empty()) {
                bodyPtr = const_cast<char*>(req.body.data());
                bodyLen = static_cast<DWORD>(req.body.size());
            }
            if (WinHttpSendRequest(r, headers.c_str(), static_cast<DWORD>(-1),
                                   bodyPtr, bodyLen, bodyLen, 0) &&
                WinHttpReceiveResponse(r, NULL)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                    WINHTTP_NO_HEADER_INDEX);
                resp.status = static_cast<int>(status);
                // ok 只表示「拿到了 HTTP 响应」。末尾那次 QueryDataAvailable
                // 在连接关闭后可能返回失败，不能因此把整次请求判死——body 已经读到了。
                resp.ok = true;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(r, &avail)) break;
                    if (avail == 0) break;
                    const size_t old = resp.body.size();
                    resp.body.resize(old + avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(r, &resp.body[old], avail, &got)) {
                        resp.body.resize(old);
                        break;
                    }
                    resp.body.resize(old + got);
                    if (resp.body.size() > 16u * 1024 * 1024) break;
                }
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return resp.ok;
}

} // namespace
#endif

namespace {
HttpFn g_http;
}

void SetHttp(HttpFn fn) { g_http = std::move(fn); }

HttpFn GetHttp() {
    if (g_http) return g_http;
#ifdef _WIN32
    return HttpFn(&WinHttpSend);
#else
    return HttpFn();
#endif
}

namespace {

// HTTPS 失败时降级 http 重试一次：某些网络里 TLS 会被中间人换证书
// （WinHTTP 报 12175），而酷狗这套接口本身大量走明文。
//
// 但「每次先白跑一次 TLS 再降级」是一笔固定开销：一次握手 100~300ms，
// 搜索/榜单每次都要多付这一笔，累计起来就是明显的"慢半拍"。
// 所以按主机记住失败，之后直接走 http —— 与原来的最终行为完全一致（本来就是降级到 http），
// 只是不再重复付那次注定失败的握手。
std::mutex g_httpsMu;
std::set<std::string> g_httpsBroken;

std::string HostOf(const std::string& url) {
    const size_t p = url.find("://");
    if (p == std::string::npos) return {};
    const size_t s = p + 3;
    const size_t e = url.find('/', s);
    return url.substr(s, e == std::string::npos ? std::string::npos : e - s);
}

bool HttpsBrokenFor(const std::string& host) {
    std::lock_guard<std::mutex> lk(g_httpsMu);
    return g_httpsBroken.count(host) > 0;
}

void MarkHttpsBroken(const std::string& host) {
    std::lock_guard<std::mutex> lk(g_httpsMu);
    g_httpsBroken.insert(host);
}

bool DoHttp(HttpRequest req, HttpResponse& resp) {
    const HttpFn fn = GetHttp();
    if (!fn) return false;
    const bool https = req.url.rfind("https://", 0) == 0;
    const std::string host = https ? HostOf(req.url) : std::string();
    if (https && HttpsBrokenFor(host)) {
        req.url = "http://" + req.url.substr(8);
        return fn(req, resp);
    }
    if (fn(req, resp)) return true;
    if (https) {
        MarkHttpsBroken(host);
        req.url = "http://" + req.url.substr(8);
        return fn(req, resp);
    }
    return false;
}

} // namespace

bool FetchBytes(const std::string& url, std::string& out) {
    out.clear();
    if (url.empty()) return false;
    HttpRequest req;
    req.url = url;
    req.headers.push_back({"User-Agent", "Mozilla/5.0"});
    HttpResponse resp;
    if (!DoHttp(req, resp) || resp.status != 200) return false;
    out = std::move(resp.body);
    return !out.empty();
}

std::string KugouAlbumCover(const std::string& albumId) {
    if (albumId.empty() || albumId == "0") return std::string();
    HttpRequest req;
    req.url = "https://mobilecdn.kugou.com/api/v3/album/info?albumid=" + UrlEncode(albumId);
    req.headers.push_back({"User-Agent", kUa});
    HttpResponse resp;
    if (!DoHttp(req, resp) || resp.status != 200) return std::string();
    JValue root;
    if (!ParseJson(resp.body, root)) return std::string();
    const JValue* data = root.Find("data");
    std::string url = data ? data->S("imgurl") : std::string();
    if (url.empty()) return std::string();
    const std::string ph = "{size}";
    const size_t p = url.find(ph);
    if (p != std::string::npos) url.replace(p, ph.size(), "500");
    if (url.rfind("http://", 0) == 0) url.replace(0, 7, "https://");
    return url;
}

// ===========================================================================
//  账号：扫码登录
// ===========================================================================

std::string KugouAuth::DeviceMid() {
    if (!g_deviceMid.empty()) return g_deviceMid;
    std::string seed = "shiyu-player";
#ifdef _WIN32
    char name[256] = {0};
    DWORD n = sizeof(name);
    if (GetComputerNameA(name, &n) && n > 0) seed += std::string("|") + name;
#endif
    g_deviceMid = Md5Hex(seed);
    return g_deviceMid;
}

void KugouAuth::SetDeviceMid(const std::string& mid) {
    if (!mid.empty()) g_deviceMid = mid;
}

bool KugouAuth::CreateQr(QrCode& out, std::string* err) {
    out = QrCode();
    const std::string txt =
        "https://h5.kugou.com/apps/loginQRCode/html/index.html?appid=1005&";
    Params params = BaseParams("1001");
    params.push_back({"type", "1"});
    params.push_back({"plat", "4"});
    params.push_back({"qrcode_txt", txt});
    params.push_back({"srcappid", kSrcAppId});
    params.push_back({"signature", SignWeb(params, "")});

    HttpRequest req;
    req.url = "https://login-user.kugou.com/v2/qrcode?" + QueryString(params);
    AddKugouHeaders(req);
    HttpResponse resp;
    if (!DoHttp(req, resp) || resp.status != 200) {
        if (err) *err = "连不上酷狗登录服务器";
        return false;
    }
    JValue root;
    if (!ParseJson(resp.body, root)) {
        if (err) *err = "登录服务器返回了无法解析的内容";
        return false;
    }
    const JValue* data = root.Find("data");
    out.key = data ? data->S("qrcode") : std::string();
    if (out.key.empty()) {
        if (err) *err = "取二维码失败（服务端没给 key）";
        return false;
    }
    out.scanUrl = txt;
    const std::string img = data->S("qrcode_img");
    const std::string prefix = "base64,";
    const size_t p = img.find(prefix);
    if (p != std::string::npos) {
        out.pngBase64 = img.substr(p + prefix.size());
        Base64Decode(out.pngBase64, out.png);
    }
    return true;
}

KugouAuth::State KugouAuth::CheckQr(const std::string& key, Account& acc, std::string* err) {
    if (key.empty()) {
        if (err) *err = "二维码 key 为空";
        return State::Error;
    }
    Params params = BaseParams(kAppId);
    params.push_back({"plat", "4"});
    params.push_back({"srcappid", kSrcAppId});
    params.push_back({"qrcode", key});
    params.push_back({"signature", SignWeb(params, "")});

    HttpRequest req;
    req.url = "https://login-user.kugou.com/v2/get_userinfo_qrcode?" + QueryString(params);
    AddKugouHeaders(req);
    HttpResponse resp;
    if (!DoHttp(req, resp) || resp.status != 200) {
        if (err) *err = "网络中断";
        return State::Error;
    }
    JValue root;
    if (!ParseJson(resp.body, root)) {
        if (err) *err = "响应无法解析";
        return State::Error;
    }
    const JValue* data = root.Find("data");
    if (!data) return State::Waiting;
    const long long st = data->I("status", 1);
    switch (st) {
        case 0: return State::Expired;
        case 2: return State::Scanned;
        case 4: {
            acc.token = data->S("token");
            acc.userid = std::to_string(data->I("userid", 0));
            acc.nickname = data->S("nickname");
            acc.avatar = data->S("pic");
            if (!acc.Valid()) {
                if (err) *err = "登录成功但没拿到 token";
                return State::Error;
            }
            return State::Confirmed;
        }
        default: return State::Waiting;
    }
}

// ===========================================================================
//  酷狗 Provider
// ===========================================================================

namespace {

class KugouProvider : public Provider {
public:
    Source source() const override { return Source::Kugou; }
    void SetAccount(const Account& a) override { acc_ = a; }
    void SetPreferredQuality(Quality q) override { preferred_ = q; }

    std::vector<SongList> Lists() override {
        std::vector<SongList> out;
        HttpRequest req;
        req.url =
            "https://mobilecdn.kugou.com/api/v3/rank/list?version=9108&plat=0&page=1&pagesize=40";
        req.headers.push_back({"User-Agent", kUa});
        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) return out;
        JValue root;
        if (!ParseJson(resp.body, root)) return out;

        const JValue* data = root.Find("data");
        const JValue* info = data ? data->Find("info") : nullptr;
        if (!info) return out;
        for (size_t i = 0; i < info->Size(); ++i) {
            const JValue* it = info->At(i);
            if (!it) continue;
            SongList l;
            l.source = Source::Kugou;
            // rankid 是数字，用 S() 会取到空
            l.id = std::to_string(it->I("rankid", 0));
            l.name = it->S("rankname");
            if (l.id == "0" || l.name.empty()) continue;
            l.coverUrl = KugouCover(it->S("imgurl"));
            l.note = it->S("update_frequency");
            out.push_back(std::move(l));
        }
        return out;
    }

    std::vector<Track> ListSongs(const std::string& listId, int limit) override {
        std::vector<Track> out;
        if (listId.empty() || limit <= 0) return out;
        HttpRequest req;
        req.url = "https://mobilecdn.kugou.com/api/v3/rank/song?rankid=" + UrlEncode(listId) +
                  "&page=1&pagesize=" + std::to_string(std::min(limit, 200)) + "&version=9108";
        req.headers.push_back({"User-Agent", kUa});
        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) return out;
        JValue root;
        if (!ParseJson(resp.body, root)) return out;
        const JValue* data = root.Find("data");
        const JValue* info = data ? data->Find("info") : nullptr;
        if (!info) return out;
        for (size_t i = 0; i < info->Size(); ++i) {
            const JValue* it = info->At(i);
            if (!it) continue;
            Track t = FromNode(*it);
            if (!t.id.empty()) out.push_back(std::move(t));
        }
        return out;
    }

    std::vector<Track> Search(const std::string& keyword, int limit, int offset) override {
        std::vector<Track> out;
        if (keyword.empty() || limit <= 0) return out;
        const int pageSize = std::min(limit, 100);
        const int page = offset / pageSize + 1;
        HttpRequest req;
        req.url = "https://mobilecdn.kugou.com/api/v3/search/song?format=json&keyword=" +
                  UrlEncode(keyword) + "&page=" + std::to_string(page) + "&pagesize=" +
                  std::to_string(pageSize) + "&showtype=1";
        req.headers.push_back({"User-Agent", kUa});
        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) return out;
        JValue root;
        if (!ParseJson(resp.body, root)) return out;
        const JValue* data = root.Find("data");
        const JValue* info = data ? data->Find("info") : nullptr;
        if (!info) return out;
        for (size_t i = 0; i < info->Size(); ++i) {
            const JValue* it = info->At(i);
            if (!it) continue;
            Track t = FromNode(*it);
            if (!t.id.empty()) out.push_back(std::move(t));
        }
        return out;
    }

    // ── 按实体浏览 ────────────────────────────────────────────────────
    //
    // 酷狗的曲目搜索**不返回 singerid**（只有 singername），所以「点歌手」必须
    // 先拿名字反查一次 search/singer。专辑不用查 —— 曲目里本来就带 album_id。

    std::vector<ArtistInfo> SearchArtists(const std::string& kw, int limit) override {
        std::vector<ArtistInfo> out;
        if (kw.empty() || limit <= 0) return out;
        HttpRequest req;
        req.url = "https://mobilecdn.kugou.com/api/v3/search/singer?format=json&keyword=" +
                  UrlEncode(kw) + "&page=1&pagesize=" + std::to_string(std::min(limit, 30));
        req.headers.push_back({"User-Agent", kUa});
        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) return out;
        JValue root;
        if (!ParseJson(resp.body, root)) return out;
        // 这个接口的 data 直接是数组，不是 { info: [...] }
        const JValue* data = root.Find("data");
        if (!data) return out;
        for (size_t i = 0; i < data->Size(); ++i) {
            const JValue* it = data->At(i);
            if (!it) continue;
            ArtistInfo a;
            a.id = std::to_string(it->I("singerid", 0));
            a.name = it->S("singername");
            if (a.id == "0" || a.name.empty()) continue;
            out.push_back(std::move(a));
        }
        return out;
    }

    std::vector<AlbumInfo> SearchAlbums(const std::string& kw, int limit) override {
        std::vector<AlbumInfo> out;
        if (kw.empty() || limit <= 0) return out;
        HttpRequest req;
        req.url = "https://mobilecdn.kugou.com/api/v3/search/album?format=json&keyword=" +
                  UrlEncode(kw) + "&page=1&pagesize=" + std::to_string(std::min(limit, 30));
        req.headers.push_back({"User-Agent", kUa});
        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) return out;
        JValue root;
        if (!ParseJson(resp.body, root)) return out;
        const JValue* data = root.Find("data");
        const JValue* info = data ? data->Find("info") : nullptr;
        if (!info) return out;
        for (size_t i = 0; i < info->Size(); ++i) {
            const JValue* it = info->At(i);
            if (!it) continue;
            AlbumInfo a;
            a.id = std::to_string(it->I("albumid", 0));
            a.name = it->S("albumname");
            a.artist = it->S("singername");
            a.cover = KugouCover(it->S("imgurl"));
            a.publishTime = it->S("publishtime");
            a.songCount = static_cast<int>(it->I("songcount", 0));
            if (a.id == "0" || a.name.empty()) continue;
            out.push_back(std::move(a));
        }
        return out;
    }

    ArtistInfo Artist(const std::string& id) override {
        ArtistInfo a;
        if (id.empty()) return a;
        HttpRequest req;
        req.url = "https://mobilecdn.kugou.com/api/v3/singer/info?singerid=" + UrlEncode(id);
        req.headers.push_back({"User-Agent", kUa});
        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) return a;
        JValue root;
        if (!ParseJson(resp.body, root)) return a;
        const JValue* data = root.Find("data");
        if (!data) return a;
        a.id = std::to_string(data->I("singerid", 0));
        a.name = data->S("singername");
        a.avatar = KugouCover(data->S("imgurl"));
        a.songCount = static_cast<int>(data->I("songcount", 0));
        a.albumCount = static_cast<int>(data->I("albumcount", 0));
        return a;
    }

    std::vector<Track> ArtistSongs(const std::string& id, int limit, int offset) override {
        std::vector<Track> out;
        if (id.empty() || limit <= 0) return out;
        const int pageSize = std::min(limit, 100);
        const int page = offset / pageSize + 1;
        HttpRequest req;
        req.url = "https://mobilecdn.kugou.com/api/v3/singer/song?singerid=" + UrlEncode(id) +
                  "&page=" + std::to_string(page) + "&pagesize=" + std::to_string(pageSize) +
                  "&sort=1&version=9108";
        req.headers.push_back({"User-Agent", kUa});
        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) return out;
        JValue root;
        if (!ParseJson(resp.body, root)) return out;
        const JValue* data = root.Find("data");
        const JValue* info = data ? data->Find("info") : nullptr;
        if (!info) return out;
        for (size_t i = 0; i < info->Size(); ++i) {
            const JValue* it = info->At(i);
            if (!it) continue;
            Track t = FromNode(*it);
            t.artistId = id;
            if (!t.id.empty()) out.push_back(std::move(t));
        }
        return out;
    }

    AlbumInfo Album(const std::string& id) override {
        AlbumInfo a;
        if (id.empty()) return a;
        HttpRequest req;
        req.url = "https://mobilecdn.kugou.com/api/v3/album/info?albumid=" + UrlEncode(id);
        req.headers.push_back({"User-Agent", kUa});
        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) return a;
        JValue root;
        if (!ParseJson(resp.body, root)) return a;
        const JValue* data = root.Find("data");
        if (!data) return a;
        a.id = std::to_string(data->I("albumid", 0));
        a.name = data->S("albumname");
        a.artist = data->S("singername");
        a.cover = KugouCover(data->S("imgurl"));
        a.publishTime = data->S("publishtime");
        a.songCount = static_cast<int>(data->I("songcount", 0));
        return a;
    }

    std::vector<Track> AlbumSongs(const std::string& id, int limit) override {
        std::vector<Track> out;
        if (id.empty() || limit <= 0) return out;
        HttpRequest req;
        req.url = "https://mobilecdn.kugou.com/api/v3/album/song?albumid=" + UrlEncode(id) +
                  "&page=1&pagesize=" + std::to_string(std::min(limit, 200)) + "&version=9108";
        req.headers.push_back({"User-Agent", kUa});
        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) return out;
        JValue root;
        if (!ParseJson(resp.body, root)) return out;
        const JValue* data = root.Find("data");
        const JValue* info = data ? data->Find("info") : nullptr;
        if (!info) return out;
        for (size_t i = 0; i < info->Size(); ++i) {
            const JValue* it = info->At(i);
            if (!it) continue;
            Track t = FromNode(*it);
            if (t.albumId.empty()) t.albumId = id;
            if (!t.id.empty()) out.push_back(std::move(t));
        }
        return out;
    }

    Resolved Resolve(const Track& track) override {
        if (track.id.empty()) {
            Resolved r;
            r.message = "这首歌没有 hash，无法解析";
            return r;
        }
        const std::string hash = ToLowerAscii(track.id);

        // ① 有账号：v5/url，从期望音质向下降级
        Resolved v5Last;
        bool triedV5 = false;
        if (acc_.Valid()) {
            triedV5 = true;
            for (Quality q = preferred_;;) {
                Resolved r = ResolveV5(track, hash, q);
                if (r.ok) return r;
                if (r.need_login) return r;   // token 失效，降级没意义
                v5Last = r;
                if (q == Quality::Standard) break;
                q = static_cast<Quality>(static_cast<int>(q) + 1);
            }
        }

        // ② 回落 i/v2（不接收账号，只对 privilege<10 有效）
        Resolved r = ResolveLegacy(hash);
        if (r.ok) return r;

        if (!acc_.Valid() && (r.paywalled || track.Paywalled())) {
            r.need_login = true;
            r.message = "这首需要酷狗会员，扫码登录后可播";
            return r;
        }
        if (triedV5 && !v5Last.message.empty()) return v5Last;
        return r;
    }

private:
    Account acc_;
    Quality preferred_ = Quality::Flac;

    static Track FromNode(const JValue& it) {
        Track t;
        t.source = Source::Kugou;
        t.id = ToLowerAscii(it.S("hash"));
        t.title = it.S("songname");
        t.artist = it.S("singername");
        if (t.artist.empty()) {
            std::string a, ti;
            SplitKugouFilename(it.S("filename"), it.S("songname"), a, ti);
            t.artist = a;
            if (t.title.empty()) t.title = ti;
        }
        t.album = it.S("album_name");
        t.albumId = std::to_string(it.I("album_id", 0));
        t.albumAudioId = std::to_string(it.I("album_audio_id", 0));
        t.durationMs = static_cast<int>(it.I("duration", 0)) * 1000;
        t.bitrate = static_cast<int>(it.I("bitrate", 0)) * 1000;
        t.fileSize = it.I("filesize", 0);
        t.payType = static_cast<int>(it.I("pay_type", 0));
        t.privilege = static_cast<int>(it.I("privilege", 0));
        return t;
    }

    // 已登录路径：trackercdn /v5/url（android 签名 + key）
    Resolved ResolveV5(const Track& track, const std::string& hash, Quality q) {
        Resolved r;
        r.quality = q;

        Params params = BaseParams(kAppId);
        params.push_back({"token", acc_.token});
        params.push_back({"userid", acc_.userid});
        params.push_back({"album_id", track.albumId});
        params.push_back({"area_code", "1"});
        params.push_back({"hash", hash});
        params.push_back({"ssa_flag", "is_fromtrack"});
        params.push_back({"version", "11430"});
        params.push_back({"page_id", kPageId});
        params.push_back({"quality", QualityKey(q)});
        params.push_back({"album_audio_id", track.albumAudioId});
        params.push_back({"behavior", "play"});
        params.push_back({"pid", "2"});
        params.push_back({"cmd", "26"});
        params.push_back({"pidversion", "3001"});
        params.push_back({"IsFreePart", "0"});
        params.push_back({"ppage_id", kPPageId});
        params.push_back({"cdnBackup", "1"});
        params.push_back({"module", ""});
        SetParam(params, "clientver", "11430");   // 覆盖，不能重复
        params.push_back(
            {"key", Md5Hex(hash + kKeySalt + kAppId + KugouAuth::DeviceMid() + acc_.userid)});
        params.push_back({"signature", SignAndroid(params, "")});

        HttpRequest req;
        req.url = "https://trackercdn.kugou.com/v5/url?" + QueryString(params);
        AddKugouHeaders(req);
        req.headers.push_back({"x-router", "trackercdn.kugou.com"});
        req.timeoutMs = 20000;

        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) {
            r.message = "连不上酷狗服务器";
            return r;
        }
        JValue root;
        if (!ParseJson(resp.body, root)) {
            r.message = "酷狗返回了无法解析的内容";
            return r;
        }
        const long long err = root.I("error_code", 0);
        if (err == 20017) {
            r.need_login = true;
            r.paywalled = true;
            r.message = "登录已失效，请重新扫码";
            return r;
        }
        const long long status = root.I("status", 0);
        std::string url;
        const JValue* u = root.Find("url");
        if (u && u->Size() > 0) {
            const JValue* u0 = u->At(0);
            if (u0 && u0->t == JValue::T::Str) url = u0->str;
        }
        if (status == 1 && !url.empty()) {
            r.ok = true;
            r.url = url;
            r.ext = ToLowerAscii(root.S("extName"));
            if (r.ext.empty()) r.ext = (q == Quality::Flac ? "flac" : "mp3");
            r.bitrate = static_cast<int>(root.I("bitrate", 0));
            r.fileSize = root.I("fileSize", 0);
            return r;
        }
        r.paywalled = true;
        r.message = std::string("这首拿不到") + QualityLabel(q) + "（可能无此音质或需要购买）";
        return r;
    }

    // 未登录路径：trackercdn /i/v2，不接收账号
    Resolved ResolveLegacy(const std::string& hash) {
        Resolved r;
        HttpRequest req;
        req.url = "https://trackercdn.kugou.com/i/v2/?key=" + Md5Hex(hash + "kgcloudv2") +
                  "&hash=" + hash + "&br=hq&appid=1005&pid=2&cmd=25&behavior=play";
        req.headers.push_back({"User-Agent", kUa});
        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) {
            r.message = "连不上酷狗服务器";
            return r;
        }
        JValue root;
        if (!ParseJson(resp.body, root)) {
            r.message = "酷狗返回了无法解析的内容";
            return r;
        }
        const long long status = root.I("status", -1);
        std::string first;
        const JValue* u = root.Find("url");
        if (u && u->Size() > 0) {
            const JValue* u0 = u->At(0);
            if (u0 && u0->t == JValue::T::Str) first = u0->str;
        }
        if (status == 1 && !first.empty()) {
            r.ok = true;
            r.url = first;
            r.ext = ToLowerAscii(root.S("extName"));
            if (r.ext.empty()) r.ext = "mp3";
            r.bitrate = static_cast<int>(root.I("bitRate", 0));
            r.quality = r.bitrate >= 300000 ? Quality::High : Quality::Standard;
            return r;
        }
        if (status == 2) {
            r.paywalled = true;
            r.message = "酷狗付费曲：CDN 直接拒绝";
            return r;
        }
        r.message = "酷狗没有给出播放地址（status=" + std::to_string(status) + "）";
        return r;
    }
};

// ===========================================================================
//  网易云 Provider
// ===========================================================================

// 网易的专辑里 artist 常常是个空壳（name=""），真正的艺人在 artists[0]
std::string NeteaseAlbumArtist(const JValue& al) {
    const JValue* arr = al.Find("artists");
    if (arr && arr->Size() > 0) {
        const JValue* a0 = arr->At(0);
        if (a0) {
            const std::string n = a0->S("name");
            if (!n.empty()) return n;
        }
    }
    const JValue* one = al.Find("artist");
    return one ? one->S("name") : std::string();
}

class NeteaseProvider : public Provider {
public:
    Source source() const override { return Source::Netease; }

    std::vector<SongList> Lists() override {
        std::vector<SongList> out;
        JValue root;
        if (!Fetch("https://music.163.com/api/toplist", root)) return out;
        const JValue* list = root.Find("list");
        if (!list) return out;
        for (size_t i = 0; i < list->Size(); ++i) {
            const JValue* it = list->At(i);
            if (!it) continue;
            SongList l;
            l.source = Source::Netease;
            l.id = std::to_string(it->I("id", 0));
            l.name = it->S("name");
            if (l.id == "0" || l.name.empty()) continue;
            l.coverUrl = it->S("coverImgUrl");
            l.note = it->S("updateFrequency");
            out.push_back(std::move(l));
        }
        return out;
    }

    std::vector<Track> ListSongs(const std::string& listId, int limit) override {
        std::vector<Track> out;
        if (listId.empty() || limit <= 0) return out;
        JValue root;
        if (!Fetch("https://music.163.com/api/playlist/detail?id=" + UrlEncode(listId), root))
            return out;
        const JValue* result = root.Find("result");
        const JValue* tracks = result ? result->Find("tracks") : nullptr;
        if (!tracks) return out;
        const size_t n = std::min<size_t>(tracks->Size(), static_cast<size_t>(limit));
        for (size_t i = 0; i < n; ++i) {
            const JValue* it = tracks->At(i);
            if (!it) continue;
            Track t = FromNode(*it);
            if (!t.id.empty()) out.push_back(std::move(t));
        }
        return out;
    }

    std::vector<Track> Search(const std::string& keyword, int limit, int offset) override {
        std::vector<Track> out;
        if (keyword.empty() || limit <= 0) return out;
        JValue root;
        if (!Fetch("https://music.163.com/api/search/get?s=" + UrlEncode(keyword) +
                       "&type=1&limit=" + std::to_string(std::min(limit, 100)) +
                       "&offset=" + std::to_string(offset),
                   root))
            return out;
        const JValue* result = root.Find("result");
        const JValue* songs = result ? result->Find("songs") : nullptr;
        if (!songs) return out;
        for (size_t i = 0; i < songs->Size(); ++i) {
            const JValue* it = songs->At(i);
            if (!it) continue;
            Track t = FromNode(*it);
            if (!t.id.empty()) out.push_back(std::move(t));
        }
        return out;
    }

    // ── 按实体浏览 ────────────────────────────────────────────────────
    // 网易的曲目搜索不返回 artists/albums，得单独用 type=100 / type=10 查一次。

    std::vector<ArtistInfo> SearchArtists(const std::string& kw, int limit) override {
        std::vector<ArtistInfo> out;
        if (kw.empty() || limit <= 0) return out;
        JValue root;
        if (!Fetch("https://music.163.com/api/search/get?s=" + UrlEncode(kw) +
                       "&type=100&limit=" + std::to_string(std::min(limit, 30)) + "&offset=0",
                   root))
            return out;
        const JValue* result = root.Find("result");
        const JValue* arr = result ? result->Find("artists") : nullptr;
        if (!arr) return out;
        for (size_t i = 0; i < arr->Size(); ++i) {
            const JValue* it = arr->At(i);
            if (!it) continue;
            ArtistInfo a;
            a.id = std::to_string(it->I("id", 0));
            a.name = it->S("name");
            a.avatar = it->S("picUrl");
            a.songCount = static_cast<int>(it->I("musicSize", 0));
            a.albumCount = static_cast<int>(it->I("albumSize", 0));
            if (a.id == "0" || a.name.empty()) continue;
            out.push_back(std::move(a));
        }
        return out;
    }

    std::vector<AlbumInfo> SearchAlbums(const std::string& kw, int limit) override {
        std::vector<AlbumInfo> out;
        if (kw.empty() || limit <= 0) return out;
        JValue root;
        if (!Fetch("https://music.163.com/api/search/get?s=" + UrlEncode(kw) +
                       "&type=10&limit=" + std::to_string(std::min(limit, 30)) + "&offset=0",
                   root))
            return out;
        const JValue* result = root.Find("result");
        const JValue* arr = result ? result->Find("albums") : nullptr;
        if (!arr) return out;
        for (size_t i = 0; i < arr->Size(); ++i) {
            const JValue* it = arr->At(i);
            if (!it) continue;
            AlbumInfo a;
            a.id = std::to_string(it->I("id", 0));
            a.name = it->S("name");
            a.artist = NeteaseAlbumArtist(*it);
            a.cover = it->S("picUrl");
            a.publishTime = std::to_string(it->I("publishTime", 0));
            a.songCount = static_cast<int>(it->I("size", 0));
            if (a.id == "0" || a.name.empty()) continue;
            out.push_back(std::move(a));
        }
        return out;
    }

    ArtistInfo Artist(const std::string& id) override {
        ArtistInfo a;
        if (id.empty()) return a;
        JValue root;
        if (!Fetch("https://music.163.com/api/artist/" + UrlEncode(id), root)) return a;
        const JValue* ar = root.Find("artist");
        if (!ar) return a;
        a.id = std::to_string(ar->I("id", 0));
        a.name = ar->S("name");
        a.avatar = ar->S("picUrl");
        a.songCount = static_cast<int>(ar->I("musicSize", 0));
        a.albumCount = static_cast<int>(ar->I("albumSize", 0));
        return a;
    }

    // 网易的公开接口最多给 50 首热门（要全部得走加密的 weapi），
    // 所以这里只做首页，offset > 0 直接返回空 —— 宁可少给，也不假装有下一页。
    std::vector<Track> ArtistSongs(const std::string& id, int limit, int offset) override {
        std::vector<Track> out;
        if (id.empty() || limit <= 0 || offset > 0) return out;
        JValue root;
        if (!Fetch("https://music.163.com/api/artist/" + UrlEncode(id), root)) return out;
        const JValue* hs = root.Find("hotSongs");
        if (!hs) return out;
        for (size_t i = 0; i < hs->Size() && static_cast<int>(out.size()) < limit; ++i) {
            const JValue* it = hs->At(i);
            if (!it) continue;
            Track t = FromNode(*it);
            t.artistId = id;
            if (!t.id.empty()) out.push_back(std::move(t));
        }
        return out;
    }

    AlbumInfo Album(const std::string& id) override {
        AlbumInfo a;
        if (id.empty()) return a;
        JValue root;
        if (!Fetch("https://music.163.com/api/v1/album/" + UrlEncode(id), root)) return a;
        const JValue* al = root.Find("album");
        if (!al) return a;
        a.id = std::to_string(al->I("id", 0));
        a.name = al->S("name");
        a.artist = NeteaseAlbumArtist(*al);
        a.cover = al->S("picUrl");
        a.publishTime = std::to_string(al->I("publishTime", 0));
        a.songCount = static_cast<int>(al->I("size", 0));
        return a;
    }

    std::vector<Track> AlbumSongs(const std::string& id, int limit) override {
        std::vector<Track> out;
        if (id.empty() || limit <= 0) return out;
        JValue root;
        if (!Fetch("https://music.163.com/api/v1/album/" + UrlEncode(id), root)) return out;
        const JValue* songs = root.Find("songs");
        if (!songs) return out;
        for (size_t i = 0; i < songs->Size() && static_cast<int>(out.size()) < limit; ++i) {
            const JValue* it = songs->At(i);
            if (!it) continue;
            Track t = FromNode(*it);
            if (t.albumId.empty()) t.albumId = id;
            if (!t.id.empty()) out.push_back(std::move(t));
        }
        return out;
    }

    Resolved Resolve(const Track& track) override {
        Resolved r;
        if (track.id.empty()) {
            r.message = "这首歌没有 id，无法解析";
            return r;
        }
        JValue root;
        if (!Fetch("https://music.163.com/api/song/enhance/player/url?ids=[" + track.id +
                       "]&br=320000",
                   root)) {
            r.message = "连不上网易云服务器";
            return r;
        }
        const JValue* data = root.Find("data");
        const JValue* first = data ? data->At(0) : nullptr;
        if (!first) {
            r.message = "网易云没有返回歌曲信息";
            return r;
        }
        const long long code = first->I("code", -1);
        const std::string url = first->S("url");
        if (code == 200 && !url.empty()) {
            r.ok = true;
            r.url = url;
            r.ext = ToLowerAscii(first->S("type"));
            if (r.ext.empty()) r.ext = "mp3";
            r.bitrate = static_cast<int>(first->I("br", 0));
            r.fileSize = first->I("size", 0);
            r.quality = r.bitrate >= 300000 ? Quality::High : Quality::Standard;
            return r;
        }
        if (code == -110) {
            r.paywalled = true;
            r.message = "网易云会员曲（需要登录会员账号）";
            return r;
        }
        r.message = "网易云没有给出播放地址（code=" + std::to_string(code) + "）";
        return r;
    }

private:
    static bool Fetch(const std::string& url, JValue& out) {
        HttpRequest req;
        req.url = url;
        req.headers.push_back({"User-Agent", kUa});
        req.headers.push_back({"Referer", "https://music.163.com/"});
        HttpResponse resp;
        if (!DoHttp(req, resp) || resp.status != 200) return false;
        return ParseJson(resp.body, out);
    }

    // 网易有两套字段名：旧接口用 artists/album/duration，新接口（v1/album、
    // artist/top/song）用 ar/al/dt。两套都得认，否则「专辑里的歌」会全是空标题。
    static Track FromNode(const JValue& it) {
        Track t;
        t.source = Source::Netease;
        t.id = std::to_string(it.I("id", 0));
        if (t.id == "0") t.id.clear();
        t.title = it.S("name");

        const JValue* artists = it.Find("artists");
        if (!artists) artists = it.Find("ar");
        if (artists) {
            for (size_t i = 0; i < artists->Size(); ++i) {
                const JValue* a = artists->At(i);
                if (!a) continue;
                const std::string nm = a->S("name");
                if (nm.empty()) continue;
                if (t.artistId.empty()) t.artistId = std::to_string(a->I("id", 0));
                if (!t.artist.empty()) t.artist += " / ";
                t.artist += nm;
            }
        }

        const JValue* album = it.Find("album");
        if (!album) album = it.Find("al");
        if (album) {
            t.album = album->S("name");
            t.coverUrl = album->S("picUrl");
            const long long aid = album->I("id", 0);
            if (aid > 0) t.albumId = std::to_string(aid);
        }

        long long dur = it.I("duration", 0);
        if (dur == 0) dur = it.I("dt", 0);
        t.durationMs = static_cast<int>(dur);
        t.payType = static_cast<int>(it.I("fee", 0));
        t.fileSize = it.I("size", 0);
        return t;
    }
};

} // namespace

std::unique_ptr<Provider> MakeKugou() { return std::unique_ptr<Provider>(new KugouProvider()); }
std::unique_ptr<Provider> MakeNetease() { return std::unique_ptr<Provider>(new NeteaseProvider()); }

} // namespace online_core
