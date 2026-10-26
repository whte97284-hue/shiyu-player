#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../include/lyric_provider.hpp"
#include "../include/audio_tag_parser.hpp"   // 复用 Utf8ToWide / WideToUtf8

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <sstream>

namespace lyric_core {

// ============================================================================
//  一、自包含工具层 (JSON / Base64 / MD5 / URL / HTTP / 规范化)
//  这些原本散落在 lyric_manager.cpp 的匿名 namespace 里,这里重新落一份,
//  与旧实现互不影响,新内核可以独立单测。
// ============================================================================

namespace {

// ---- 最小 JSON 解析 (对象/数组/字符串/数字/布尔/null),够解析各源响应 ----
struct JVal {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;

    const JVal* Get(const char* key) const {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double AsNumber() const { return num; }
    std::string AsString() const { return str; }

    class Parser {
    public:
        explicit Parser(const std::string& s) : s_(s) {}
        bool Parse(JVal& out) {
            SkipWs();
            return ParseValue(out, 0);
        }

    private:
        const std::string& s_;
        size_t i_ = 0;

        void SkipWs() {
            while (i_ < s_.size()) {
                char c = s_[i_];
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') ++i_;
                else break;
            }
        }
        bool Match(const char* lit) {
            size_t n = std::strlen(lit);
            if (s_.compare(i_, n, lit) == 0) { i_ += n; return true; }
            return false;
        }
        bool ParseHex4(unsigned& out) {
            if (i_ + 4 > s_.size()) return false;
            out = 0;
            for (int k = 0; k < 4; ++k) {
                char c = s_[i_ + k];
                unsigned d;
                if (c >= '0' && c <= '9') d = c - '0';
                else if (c >= 'a' && c <= 'f') d = 10 + c - 'a';
                else if (c >= 'A' && c <= 'F') d = 10 + c - 'A';
                else return false;
                out = (out << 4) | d;
            }
            i_ += 4;
            return true;
        }
        static void AppendUtf8(std::string& out, unsigned cp) {
            if (cp < 0x80) out.push_back(static_cast<char>(cp));
            else if (cp < 0x800) {
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
        bool ParseString(std::string& out) {
            if (i_ >= s_.size() || s_[i_] != '"') return false;
            ++i_;
            while (i_ < s_.size()) {
                char c = s_[i_++];
                if (c == '"') return true;
                if (c == '\\') {
                    if (i_ >= s_.size()) return false;
                    char e = s_[i_++];
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
                            if (!ParseHex4(cp)) return false;
                            // 代理对 (D800-DBFF + DC00-DFFF) 合并
                            if (cp >= 0xD800 && cp <= 0xDBFF && s_.compare(i_, 2, "\\u") == 0) {
                                size_t save = i_;
                                i_ += 2;
                                unsigned lo = 0;
                                if (ParseHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                } else {
                                    i_ = save;
                                }
                            }
                            AppendUtf8(out, cp);
                            break;
                        }
                        default: out.push_back(e); break;
                    }
                } else {
                    out.push_back(c);
                }
            }
            return false;
        }
        bool ParseValue(JVal& v, int depth) {
            if (depth > 64) return false;
            SkipWs();
            if (i_ >= s_.size()) return false;
            char c = s_[i_];
            if (c == '{') {
                ++i_;
                v.type = JVal::Object;
                SkipWs();
                if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                for (;;) {
                    SkipWs();
                    std::string key;
                    if (!ParseString(key)) return false;
                    SkipWs();
                    if (i_ >= s_.size() || s_[i_] != ':') return false;
                    ++i_;
                    JVal child;
                    if (!ParseValue(child, depth + 1)) return false;
                    v.obj.emplace_back(std::move(key), std::move(child));
                    SkipWs();
                    if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                    if (i_ < s_.size() && s_[i_] == '}') { ++i_; return true; }
                    return false;
                }
            }
            if (c == '[') {
                ++i_;
                v.type = JVal::Array;
                SkipWs();
                if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                for (;;) {
                    JVal child;
                    if (!ParseValue(child, depth + 1)) return false;
                    v.arr.push_back(std::move(child));
                    SkipWs();
                    if (i_ < s_.size() && s_[i_] == ',') { ++i_; continue; }
                    if (i_ < s_.size() && s_[i_] == ']') { ++i_; return true; }
                    return false;
                }
            }
            if (c == '"') { v.type = JVal::String; return ParseString(v.str); }
            if (Match("true")) { v.type = JVal::Bool; v.b = true; return true; }
            if (Match("false")) { v.type = JVal::Bool; v.b = false; return true; }
            if (Match("null")) { v.type = JVal::Null; return true; }
            // 数字
            {
                size_t start = i_;
                if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) ++i_;
                bool any = false;
                while (i_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[i_])) || s_[i_] == '.' ||
                                          s_[i_] == 'e' || s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) {
                    ++i_;
                    any = true;
                }
                if (!any) return false;
                v.type = JVal::Number;
                try { v.num = std::stod(s_.substr(start, i_ - start)); } catch (...) { v.num = 0; }
                return true;
            }
        }
    };
};

bool ParseJson(const std::string& text, JVal& out) {
    JVal::Parser p(text);
    return p.Parse(out);
}

// ---- URL 百分号编码 (仅编码非 unreserved 字符) ----
std::string UrlEncodeImpl(const std::string& utf8) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
            c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        }
    }
    return out;
}

// ---- Base64 解码 (标准字母表, 忽略空白, 容忍缺失填充) ----
bool Base64DecodeImpl(const std::string& in, std::string& out) {
    static int8_t tbl[256];
    static std::once_flag once;
    std::call_once(once, []() {
        for (int i = 0; i < 256; ++i) tbl[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) tbl[static_cast<unsigned char>(a[i])] = static_cast<int8_t>(i);
    });
    out.clear();
    uint32_t val = 0;
    int bits = -8;
    for (unsigned char c : in) {
        if (c == '=') break;
        if (c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        int8_t d = tbl[c];
        if (d < 0) return false;
        val = ((val << 6) | static_cast<uint32_t>(d)) & 0xFFFFFFu; // 只留 24 位,不会溢出
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<char>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return true;
}

// ---- MD5 (酷狗部分接口需要签名, 此处提供最小实现) ----
struct Md5 {
    uint32_t a = 0x67452301, b = 0xefcdab89, c = 0x98badcfe, d = 0x10325476;
    uint64_t len = 0;
    unsigned char buf[64];
    size_t bufLen = 0;

    void Init() { a = 0x67452301; b = 0xefcdab89; c = 0x98badcfe; d = 0x10325476; len = 0; bufLen = 0; }

    static uint32_t Rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
    static uint32_t F(uint32_t x, uint32_t y, uint32_t z) { return (x & y) | (~x & z); }
    static uint32_t G(uint32_t x, uint32_t y, uint32_t z) { return (x & z) | (y & ~z); }
    static uint32_t H(uint32_t x, uint32_t y, uint32_t z) { return x ^ y ^ z; }
    static uint32_t I(uint32_t x, uint32_t y, uint32_t z) { return y ^ (x | ~z); }

    void Block(const unsigned char* p) {
        static const uint32_t K[64] = {
            0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
            0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
            0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
            0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
            0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
            0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
            0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
            0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391};
        static const int S[64] = {7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
                                  5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
                                  4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
                                  6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21};
        uint32_t M[16];
        for (int i = 0; i < 16; ++i)
            M[i] = static_cast<uint32_t>(p[i * 4]) | (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 16) | (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
        uint32_t A = a, B = b, C = c, D = d;
        for (int i = 0; i < 64; ++i) {
            uint32_t f; int g;
            if (i < 16) { f = F(B, C, D); g = i; }
            else if (i < 32) { f = G(B, C, D); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = H(B, C, D); g = (3 * i + 5) % 16; }
            else { f = I(B, C, D); g = (7 * i) % 16; }
            f = f + A + K[i] + M[g];
            A = D; D = C; C = B;
            B = B + Rotl(f, S[i]);
        }
        a += A; b += B; c += C; d += D;
    }

    void Update(const unsigned char* data, size_t n) {
        len += n;
        while (n > 0) {
            size_t take = std::min<size_t>(n, 64 - bufLen);
            std::memcpy(buf + bufLen, data, take);
            bufLen += take;
            data += take;
            n -= take;
            if (bufLen == 64) { Block(buf); bufLen = 0; }
        }
    }

    std::string FinalHex() {
        uint64_t bits = len * 8;
        unsigned char pad = 0x80;
        Update(&pad, 1);
        unsigned char zero = 0;
        while (bufLen != 56) Update(&zero, 1);
        unsigned char lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = static_cast<unsigned char>((bits >> (8 * i)) & 0xFF);
        Update(lenb, 8);
        unsigned char digest[16];
        uint32_t vals[4] = {a, b, c, d};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                digest[i * 4 + j] = static_cast<unsigned char>((vals[i] >> (8 * j)) & 0xFF);
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(32);
        for (unsigned char ch : digest) {
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0x0F]);
        }
        return out;
    }
};

// ---- UTF-16 <-> UTF-8 (GBK 也顺手支持, 用于兜底解析非 UTF8 的 lrc) ----
std::string WideToUtf8Local(const std::wstring& w) { return AudioTagParser::WideToUtf8(w); }
std::wstring Utf8ToWideLocal(const std::string& s) { return AudioTagParser::Utf8ToWide(s); }

std::string ToLowerAscii(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// 规范化:去空白/各种括号装饰、去常见后缀、统一大小写,用于歌名歌手比对的「宁缺毋错」
std::wstring NormalizeImpl(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t c : in) {
        if (c == L' ' || c == L'\t' || c == L'\u3000' || c == L'-' || c == L'_' || c == L'.' || c == L'\'' ||
            c == L'"' || c == L'(' || c == L')' || c == L'[' || c == L']' || c == L'\uff08' || c == L'\uff09' ||
            c == L'\u3010' || c == L'\u3011' || c == L'!' || c == L'?' || c == L',' || c == L'\uff0c' ||
            c == L'\u3002' || c == L'\u300a' || c == L'\u300b') {
            continue;
        }
        out.push_back(static_cast<wchar_t>(std::towlower(c)));
    }
    return out;
}

// 歌名主干:去掉括号 / 书名号 / 波浪号里的附注 (「aLIEz (《ALDNOAH.ZERO》片尾曲)」「secret base ~君がくれたもの~」)
// 和「 - 」之后的副标题,再规范化。去完为空时保留原名
std::wstring TitleCoreImpl(const std::wstring& in) {
    std::wstring out;
    int depth = 0;
    bool tilde = false;
    for (size_t i = 0; i < in.size(); ++i) {
        const wchar_t c = in[i];
        if (c == L'(' || c == L'[' || c == L'（' || c == L'【' || c == L'《' || c == L'<') { ++depth; continue; }
        if (c == L')' || c == L']' || c == L'）' || c == L'】' || c == L'》' || c == L'>') { if (depth) --depth; continue; }
        if (c == L'~' || c == L'～' || c == L'〜') { tilde = !tilde; continue; }
        if (depth || tilde) continue;
        if (c == L'-' && i > 0 && i + 1 < in.size() && in[i - 1] == L' ' && in[i + 1] == L' ') break;
        out.push_back(c);
    }
    std::wstring core = NormalizeImpl(out);
    return core.empty() ? NormalizeImpl(in) : core;
}

// 多位歌手 (「/」「、」「,」「&」分隔,各家顺序和写法不同) 里有任意一位对得上;
// 每位再去掉括号附注,「瑞葵 (mizuki)」与「瑞葵(mizuki)」「SawanoHiroyuki(nZk) (泽野弘之)」与「SawanoHiroyuki[nZk]」都算同一人
bool ArtistsOverlapImpl(const std::wstring& a, const std::wstring& b) {
    auto split = [](const std::wstring& s) {
        std::vector<std::wstring> parts;
        std::wstring cur;
        int depth = 0;
        auto flush = [&]() {
            std::wstring n = NormalizeImpl(cur);
            if (n.size() >= 2) parts.push_back(n);
            cur.clear();
        };
        for (wchar_t c : s) {
            if (c == L'(' || c == L'[' || c == L'（' || c == L'【') { ++depth; continue; }
            if (c == L')' || c == L']' || c == L'）' || c == L'】') { if (depth) --depth; continue; }
            if (depth) continue;
            if (c == L'/' || c == L'、' || c == L',' || c == L'，' || c == L'&' || c == L';') flush();
            else cur.push_back(c);
        }
        flush();
        return parts;
    };
    for (const auto& x : split(a))
        for (const auto& y : split(b))
            if (x == y) return true;
    return false;
}

// 现场 / 演唱会版本 (时间轴通常和录音室版对不上)
bool IsLiveImpl(const std::wstring& s) {
    std::wstring low;
    for (wchar_t c : s) low.push_back(static_cast<wchar_t>(std::towlower(c)));
    for (const wchar_t* k : {L"live", L"现场", L"演唱会", L"ライブ", L"concert"})
        if (low.find(k) != std::wstring::npos) return true;
    return false;
}

// 粗判是否含 [mm:ss] / [mm:ss.xx] 时间轴
bool LooksLikeLrcImpl(const std::string& text) {
    for (size_t i = 0; i + 4 < text.size(); ++i) {
        if (text[i] != '[') continue;
        size_t j = i + 1;
        int digits = 0;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) { ++j; ++digits; }
        if (digits < 1 || digits > 3) continue;
        if (j >= text.size() || text[j] != ':') continue;
        ++j;
        int d2 = 0;
        while (j < text.size() && std::isdigit(static_cast<unsigned char>(text[j]))) { ++j; ++d2; }
        if (d2 < 1 || d2 > 2) continue;
        if (j < text.size() && (text[j] == ']' || text[j] == '.' || text[j] == ':')) return true;
    }
    return false;
}

} // namespace

// ============================================================================
//  二、公开工具方法的转发 (保持 ABI 简单)
// ============================================================================

std::string LyricProvider::UrlEncode(const std::string& utf8) { return UrlEncodeImpl(utf8); }
bool LyricProvider::Base64Decode(const std::string& in, std::string& out) { return Base64DecodeImpl(in, out); }
std::wstring LyricProvider::Normalize(const std::wstring& s) { return NormalizeImpl(s); }
bool LyricProvider::LooksLikeLrc(const std::string& text) { return LooksLikeLrcImpl(text); }

std::string LyricProvider::Md5Hex(const std::string& in) {
    Md5 m;
    m.Init();
    m.Update(reinterpret_cast<const unsigned char*>(in.data()), in.size());
    return m.FinalHex();
}

std::wstring LyricQuery::Keyword() const {
    if (artist.empty()) return title;
    if (title.empty()) return artist;
    return artist + L" " + title;
}

// ============================================================================
//  三、HTTP 层 (默认 WinHTTP)
// ============================================================================

bool LyricProvider::DefaultHttpGet(const std::string& url, std::string& body) {
#ifdef _WIN32
    body.clear();
    std::wstring wurl = Utf8ToWideLocal(url);

    // 解析 URL 为 主机 / 端口 / 路径 / scheme
    std::wstring scheme, host, object;
    {
        size_t p = wurl.find(L"://");
        if (p == std::wstring::npos) return false;
        scheme = wurl.substr(0, p);
        std::wstring rest = wurl.substr(p + 3);
        size_t slash = rest.find(L'/');
        std::wstring hostport = slash == std::wstring::npos ? rest : rest.substr(0, slash);
        object = slash == std::wstring::npos ? L"/" : rest.substr(slash);
        size_t colon = hostport.find(L':');
        if (colon != std::wstring::npos) {
            host = hostport.substr(0, colon);
        } else {
            host = hostport;
        }
    }
    if (host.empty()) return false;
    const bool secure = (scheme == L"https");
    const INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) KugouPlayer/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 4000, 4000, 6000, 8000);
    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
    if (conn) {
        DWORD flags = secure ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", object.c_str(), NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (req) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, NULL)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    ok = true;
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
                        if (avail == 0) break;
                        size_t old = body.size();
                        body.resize(old + avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(req, &body[old], avail, &got)) { ok = false; break; }
                        body.resize(old + got);
                        if (body.size() > 8 * 1024 * 1024) { ok = false; break; } // 歌词/响应不会这么大
                    }
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return ok && !body.empty();
#else
    (void)url;
    body.clear();
    return false;
#endif
}

LyricProvider::LyricProvider(HttpGetFn http)
    : http_(http ? std::move(http) : HttpGetFn(&LyricProvider::DefaultHttpGet)) {}

std::vector<std::string> LyricProvider::KnownSources() { return {"lrclib", "netease", "kugou"}; }

void LyricProvider::SetSourceEnabled(const std::string& source, bool on) {
    if (source == "lrclib") lrclib_on_ = on;
    else if (source == "netease") netease_on_ = on;
    else if (source == "kugou") kugou_on_ = on;
}

bool LyricProvider::SourceEnabled(const std::string& source) const {
    if (source == "lrclib") return lrclib_on_;
    if (source == "netease") return netease_on_;
    if (source == "kugou") return kugou_on_;
    return false;
}

// ============================================================================
//  四、候选排序 (跨源统一打分)
// ============================================================================

void LyricProvider::RankCandidates(const LyricQuery& q, std::vector<LyricCandidate>& list) {
    const std::wstring wantTitle = NormalizeImpl(q.title);
    const std::wstring wantArtist = NormalizeImpl(q.artist);
    const std::wstring wantCore = TitleCoreImpl(q.title);
    const bool queryLive = IsLiveImpl(q.title) || IsLiveImpl(q.album);

    for (auto& c : list) {
        int score = 0;
        const std::wstring ct = NormalizeImpl(c.title);
        const std::wstring ca = NormalizeImpl(c.artist);

        // 歌名:相同加权重,包含次之 (短的一方至少占长的 60%,免得「爱」这种短歌名匹配上一切)
        auto contains = [](const std::wstring& a, const std::wstring& b) {
            const std::wstring& lo = a.size() < b.size() ? a : b;
            const std::wstring& hi = a.size() < b.size() ? b : a;
            return hi.find(lo) != std::wstring::npos && lo.size() * 10 >= hi.size() * 6;
        };
        c.title_match = false;
        if (!wantTitle.empty() && !ct.empty()) {
            if (ct == wantTitle) { score += 100; c.title_match = true; }
            else if (TitleCoreImpl(c.title) == wantCore) { score += 70; c.title_match = true; } // 只差括号里的附注
            else if (contains(ct, wantTitle)) { score += 40; c.title_match = true; }
            else if (!q.loose) score -= 40;
        }
        // 查的不是现场版,候选却是:排到录音室版后面
        if (!queryLive && (IsLiveImpl(c.title) || IsLiveImpl(c.album))) score -= 35;
        // 歌手:相同加权重,包含次之
        if (!wantArtist.empty() && !ca.empty()) {
            if (ca == wantArtist) score += 60;
            else if (ca.find(wantArtist) != std::wstring::npos || wantArtist.find(ca) != std::wstring::npos ||
                     ArtistsOverlapImpl(q.artist, c.artist)) score += 25;
            else if (!q.loose) score -= 20;
        }
        // 时长:5 秒内命中,8 秒内算勉强,否则视为别的版本
        if (q.duration_ms && c.duration_ms) {
            int64_t diff = std::llabs(static_cast<int64_t>(c.duration_ms) - static_cast<int64_t>(q.duration_ms));
            if (diff <= 2000) score += 50;
            else if (diff <= 5000) score += 30;
            else if (diff <= 8000) score += 10;
            else score -= 60;
        }
        // 来源:酷狗 KRC 带逐字时间与中文翻译,明显优先 (同一首歌的酷狗版本总排在别的源前面;
        // 但不足以盖过歌名 / 时长对不上的扣分);没有时间轴的纯文本排到后面
        if (c.source == "kugou") score += 45;
        if (!c.has_synced) score -= 30;

        c.match_score = score;
    }

    std::stable_sort(list.begin(), list.end(),
                     [](const LyricCandidate& x, const LyricCandidate& y) { return x.match_score > y.match_score; });
}

int LyricProvider::PickBest(const LyricQuery& q, const std::vector<LyricCandidate>& list) {
    if (list.empty()) return -1;
    std::vector<LyricCandidate> tmp = list;
    RankCandidates(q, tmp);
    // 找回原下标 (按 id+source 匹配)
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].source == tmp[0].source && list[i].id == tmp[0].id && list[i].accesskey == tmp[0].accesskey)
            return static_cast<int>(i);
    }
    return 0;
}

// ============================================================================
//  五、并发聚合入口
// ============================================================================

LyricSearchResult LyricProvider::SearchAll(const LyricQuery& q) const {
    LyricSearchResult result;
    if (q.Empty()) return result;

    // 顺序查询各源。
    // 说明:曾试过 std::thread 并行打三个源,但 WinHTTP 在多线程并发下
    // 会出现请求失败(串行时三源全部正常)。歌词搜索本身是后台异步任务,
    // 1~2 秒的总耗时用户可接受,可靠性优先,故改为串行。
    auto collect = [&](const char* source, std::vector<LyricCandidate>& dst) {
        std::string err;
        std::vector<LyricCandidate> list;
        const bool ok = Search(source, q, list, &err);
        if (ok || err.find("http failed") == std::string::npos) ++result.answered;
        if (ok) {
            for (auto& c : list) dst.push_back(std::move(c));
        } else if (!err.empty()) {
            result.source_errors.push_back(source + std::string(": ") + err);
        } else {
            result.source_errors.push_back(std::string(source) + ": no result");
        }
    };

    std::vector<LyricCandidate> all;
    if (lrclib_on_) collect("lrclib", all);
    if (kugou_on_) collect("kugou", all);
    if (netease_on_) collect("netease", all);

    // 去重 + 过滤明显不相关的候选 (全字段都不匹配的会拿到很低的负分)
    for (auto& c : all) {
        bool dup = false;
        for (const auto& u : result.candidates) {
            if (u.source == c.source && u.id == c.id) { dup = true; break; }
        }
        if (!dup) result.candidates.push_back(std::move(c));
    }
    RankCandidates(q, result.candidates);
    // 弃掉低分候选:正常命中一般 >= 40 分;负分说明歌名/歌手/时长全不符,是噪声。
    // 手动搜索 (loose) 时关键词不一定是歌名,全部留给用户挑
    if (!q.loose) {
        auto cut = std::remove_if(result.candidates.begin(), result.candidates.end(),
                                  [](const LyricCandidate& c) { return c.match_score < 0; });
        result.candidates.erase(cut, result.candidates.end());
    }
    return result;
}

bool LyricProvider::Search(const std::string& source, const LyricQuery& q, std::vector<LyricCandidate>& out,
                           std::string* error) const {
    out.clear();
    if (source == "lrclib") return SearchLrclib(q, out, error);
    if (source == "netease") return SearchNetease(q, out, error);
    if (source == "kugou") return SearchKugou(q, out, error);
    if (error) *error = "unknown source: " + source;
    return false;
}

bool LyricProvider::Fetch(const LyricCandidate& c, RawLyricDoc& out, std::string* error) const {
    if (c.source == "lrclib") return FetchLrclib(c, out, error);
    if (c.source == "netease") return FetchNetease(c, out, error);
    if (c.source == "kugou") return FetchKugou(c, out, error);
    if (error) *error = "unknown source: " + c.source;
    return false;
}

bool LyricProvider::SearchAndFetch(const LyricQuery& q, RawLyricDoc& out, LyricCandidate* picked,
                                   LyricSearchResult* all) const {
    LyricSearchResult r = SearchAll(q);
    if (all) *all = r;
    // 按排名依次拉取,第一名拉不到 (接口出错 / 只有纯文本) 就试下一名;
    // 自动挑选只认歌名对得上的,宁可没有也不要同一歌手的另一首歌
    int tried = 0;
    for (const auto& c : r.candidates) {
        if (!c.title_match && !q.loose) continue;
        if (++tried > 4) break;
        std::string err;
        if (!Fetch(c, out, &err) || out.kind == RawLyricDoc::Payload::Plain) continue;
        if (picked) *picked = c;
        return true;
    }
    return false;
}

// ============================================================================
//  六、源 1:LRCLIB —— 开放 REST API,零签名零加密,响应直接就是 LRC
//     GET https://lrclib.net/api/search?q=<keyword>
//     GET https://lrclib.net/api/get?artist_name=&track_name=&album_name=&duration=
//     候选字段: id / trackName / artistName / albumName / duration / syncedLyrics / plainLyrics
// ============================================================================

bool LyricProvider::SearchLrclib(const LyricQuery& q, std::vector<LyricCandidate>& out, std::string* error) const {
    out.clear();
    // 有歌手时用 (track + artist) 精确检索,否则退回全文关键词
    std::string url;
    if (!q.title.empty() && !q.artist.empty()) {
        url = "https://lrclib.net/api/search?track_name=" + UrlEncodeImpl(WideToUtf8Local(q.title)) +
              "&artist_name=" + UrlEncodeImpl(WideToUtf8Local(q.artist));
    } else {
        url = "https://lrclib.net/api/search?q=" + UrlEncodeImpl(WideToUtf8Local(q.Keyword()));
    }

    std::string body;
    if (!http_(url, body)) {
        if (error) *error = "lrclib: http failed";
        return false;
    }
    JVal root;
    if (!ParseJson(body, root)) {
        if (error) *error = "lrclib: bad json";
        return false;
    }
    // /api/search 返回数组;某些错误返回对象
    if (root.type != JVal::Array) {
        if (error) *error = "lrclib: unexpected payload";
        return false;
    }
    for (const auto& it : root.arr) {
        LyricCandidate c;
        c.source = "lrclib";
        if (const JVal* v = it.Get("id")) c.id = std::to_string(static_cast<long long>(v->AsNumber()));
        if (const JVal* v = it.Get("trackName")) c.title = Utf8ToWideLocal(v->AsString());
        if (const JVal* v = it.Get("artistName")) c.artist = Utf8ToWideLocal(v->AsString());
        if (const JVal* v = it.Get("albumName")) c.album = Utf8ToWideLocal(v->AsString());
        if (const JVal* v = it.Get("duration")) c.duration_ms = static_cast<uint32_t>(v->AsNumber() * 1000.0);
        const JVal* synced = it.Get("syncedLyrics");
        c.has_synced = synced && synced->type == JVal::String && LooksLikeLrcImpl(synced->str);
        if (c.id.empty()) continue;
        out.push_back(std::move(c));
    }
    if (out.empty() && error) *error = "lrclib: none";
    return !out.empty();
}

bool LyricProvider::FetchLrclib(const LyricCandidate& c, RawLyricDoc& out, std::string* error) const {
    // /api/get/{id} 直接给完整 LRC;这里用 search 结果里的 id 去取
    std::string url = "https://lrclib.net/api/get/" + c.id;
    std::string body;
    if (!http_(url, body)) {
        // 退回按元数据查询
        std::string q = "https://lrclib.net/api/get?track_name=" + UrlEncodeImpl(WideToUtf8Local(c.title)) +
                        "&artist_name=" + UrlEncodeImpl(WideToUtf8Local(c.artist));
        if (c.duration_ms) q += "&duration=" + std::to_string(c.duration_ms / 1000);
        if (!http_(q, body)) {
            if (error) *error = "lrclib: fetch failed";
            return false;
        }
    }
    JVal root;
    if (!ParseJson(body, root)) {
        if (error) *error = "lrclib: bad json";
        return false;
    }
    const JVal* synced = root.Get("syncedLyrics");
    const JVal* plain = root.Get("plainLyrics");
    out = RawLyricDoc{};
    out.source = "lrclib";
    if (synced && synced->type == JVal::String && !synced->str.empty() && LooksLikeLrcImpl(synced->str)) {
        out.lrc_text = synced->str;
        out.kind = RawLyricDoc::Payload::Lrc;
    } else if (plain && plain->type == JVal::String && !plain->str.empty()) {
        // 只有纯文本:交给上层按 Plain 处理 (没有时间轴)
        out.raw_payload = plain->str;
        out.kind = RawLyricDoc::Payload::Plain;
    } else {
        if (error) *error = "lrclib: empty lyrics";
        return false;
    }
    return true;
}

// ============================================================================
//  七、源 2:网易云 (NetEase)
//     search (明文 eapi,无需加密): POST/GET https://music.163.com/api/search/get/web
//       s=<keyword>&type=1&limit=10  ->  result.songs[{id,name,artists,album,duration}]
//     lyrics: GET https://music.163.com/api/song/lyric?id=<id>&lv=-1&tv=-1
//       ->  lrc.lyric (原文) / tlyric.lyric (翻译) / romalrc.lyric (罗马音)
//     说明:该域名的老版 /api/ 接口无需 EAPI 加密,是本内核选用它的原因;
//          若日后失效,可换成公开部署的 netease-cloud-music-api。
//     有意不做 yrc 逐字 / eapi 加密载荷 (需复刻客户端加密,性价比低)。
// ============================================================================

bool LyricProvider::SearchNetease(const LyricQuery& q, std::vector<LyricCandidate>& out, std::string* error) const {
    out.clear();
    const std::string kw = UrlEncodeImpl(WideToUtf8Local(q.Keyword()));
    std::string url = "https://music.163.com/api/search/get/web?s=" + kw + "&type=1&limit=10&offset=0";
    std::string body;
    if (!http_(url, body)) {
        if (error) *error = "netease: http failed";
        return false;
    }
    JVal root;
    if (!ParseJson(body, root)) {
        if (error) *error = "netease: bad json";
        return false;
    }
    // 请求太频繁时接口返回 {"code":405,"msg":"操作频繁..."}:当作没连上 (不能当成「查无此歌」记下来)
    if (const JVal* code = root.Get("code")) {
        if (code->type == JVal::Number && code->AsNumber() != 200) {
            if (error) *error = "netease: http failed (code " + std::to_string(static_cast<long long>(code->AsNumber())) + ")";
            return false;
        }
    }
    const JVal* result = root.Get("result");
    if (!result) {
        if (error) *error = "netease: no result";
        return false;
    }
    const JVal* songs = result->Get("songs");
    if (!songs || songs->type != JVal::Array) {
        if (error) *error = "netease: no songs";
        return false;
    }
    for (const auto& s : songs->arr) {
        LyricCandidate c;
        c.source = "netease";
        if (const JVal* v = s.Get("id")) c.id = std::to_string(static_cast<long long>(v->AsNumber()));
        if (const JVal* v = s.Get("name")) c.title = Utf8ToWideLocal(v->AsString());
        if (const JVal* v = s.Get("duration")) c.duration_ms = static_cast<uint32_t>(v->AsNumber());
        if (const JVal* artists = s.Get("artists")) {
            if (artists->type == JVal::Array) {
                std::wstring joined;
                for (const auto& a : artists->arr) {
                    if (const JVal* n = a.Get("name")) {
                        if (!joined.empty()) joined += L"/";
                        joined += Utf8ToWideLocal(n->AsString());
                    }
                }
                c.artist = joined;
            }
        }
        if (const JVal* album = s.Get("album")) {
            if (const JVal* n = album->Get("name")) c.album = Utf8ToWideLocal(n->AsString());
        }
        c.has_synced = true; // 网易云歌词通常带时间轴,取回后再判
        if (c.id.empty()) continue;
        out.push_back(std::move(c));
    }
    if (out.empty() && error) *error = "netease: none";
    return !out.empty();
}

bool LyricProvider::FetchNetease(const LyricCandidate& c, RawLyricDoc& out, std::string* error) const {
    std::string url = "https://music.163.com/api/song/lyric?id=" + UrlEncodeImpl(c.id) + "&lv=-1&tv=-1&rv=-1";
    std::string body;
    if (!http_(url, body)) {
        if (error) *error = "netease: fetch failed";
        return false;
    }
    JVal root;
    if (!ParseJson(body, root)) {
        if (error) *error = "netease: bad json";
        return false;
    }
    auto pickText = [&root](const char* key) -> std::string {
        const JVal* blk = root.Get(key);
        if (!blk) return {};
        const JVal* txt = blk->Get("lyric");
        if (!txt || txt->type != JVal::String) return {};
        return txt->str;
    };
    std::string lrc = pickText("lrc");
    std::string tlrc = pickText("tlyric");
    std::string roma = pickText("romalrc");

    out = RawLyricDoc{};
    out.source = "netease";
    if (!lrc.empty() && LooksLikeLrcImpl(lrc)) {
        out.lrc_text = std::move(lrc);
        out.translation_text = std::move(tlrc);
        out.romaji_text = std::move(roma);
        out.kind = RawLyricDoc::Payload::Lrc;
    } else if (!lrc.empty()) {
        out.raw_payload = std::move(lrc);
        out.kind = RawLyricDoc::Payload::Plain;
    } else {
        if (error) *error = "netease: empty lyrics";
        return false;
    }
    return true;
}

// ============================================================================
//  八、源 3:酷狗 (KuGou) —— 两段式
//     阶段 A 拿音频 hash (老 krcs/search 已废弃,必须先从新版歌曲搜索取 hash):
//       GET https://songsearch.kugou.com/song_search_v2?keyword=&page=1&pagesize=N
//       ->  {"data":{"lists":[{"FileHash","SongName","SingerName","Duration"(秒),
//                              "AlbumName","EMixSongID"/"Audioid"}...]}}
//     阶段 B 用 hash 换歌词候选,再下载:
//       GET http://krcs.kugou.com/search?ver=1&man=yes&client=pc&keyword=&duration=ms&hash=
//       GET http://krcs.kugou.com/download?ver=1&client=pc&id=&accesskey=&fmt=krc|lrc&charset=utf8
//     备用域名 lyrics.kugou.com。content 为 Base64;krc 解码后是 krc1x 加密流(交给 KrcDecoder)
// ============================================================================

namespace {

// 先在内存里存下「krcs 候选 id/accesskey 需要配的 hash / 时长」
// (LyricCandidate 只暴露通用字段,酷狗的第二段查询需要用户后续用 id 直接下载,
//  故我们把 hash 塞进 LyricCandidate::accesskey 之外的字段不可行 —— 改为:
//  SearchKugou 阶段 A 就顺手把阶段 B 也做了,只保留有歌词的候选)

std::string KugouHostPath(const std::string& path, bool* ok, const HttpGetFn& http) {
    for (const char* host : {"http://krcs.kugou.com", "http://lyrics.kugou.com"}) {
        std::string body;
        if (http(std::string(host) + path, body)) { *ok = true; return body; }
    }
    *ok = false;
    return {};
}

} // namespace

bool LyricProvider::SearchKugou(const LyricQuery& q, std::vector<LyricCandidate>& out, std::string* error) const {
    out.clear();

    // ---- 阶段 A: 新版歌曲搜索拿 hash ----
    std::string kurl = "https://songsearch.kugou.com/song_search_v2?keyword=" +
                       UrlEncodeImpl(WideToUtf8Local(q.Keyword())) + "&page=1&pagesize=10&platform=WebFilter";
    std::string sbody;
    if (!http_(kurl, sbody)) {
        if (error) *error = "kugou: search http failed";
        return false;
    }
    JVal sroot;
    if (!ParseJson(sbody, sroot)) {
        if (error) *error = "kugou: search bad json";
        return false;
    }
    const JVal* data = sroot.Get("data");
    const JVal* lists = data ? data->Get("lists") : nullptr;
    if (!lists || lists->type != JVal::Array) {
        if (error) *error = "kugou: no song list";
        return false;
    }

    // ---- 阶段 B: 对每个 hash 查 lyrics 候选,合并成统一候选 ----
    // 每个 hash 都要单独请求一次:最多查 6 个、拿到 4 个就停;时长差太多的版本 (自动匹配时) 直接跳过,
    // 免得接口慢或被墙时一首歌要等几十个请求
    int lookups = 0;
    for (const auto& s : lists->arr) {
        if (lookups >= 6 || out.size() >= 4) break;
        const JVal* vh = s.Get("FileHash");
        const JVal* vn = s.Get("SongName");
        const JVal* vs = s.Get("SingerName");
        const JVal* vd = s.Get("Duration");
        const JVal* va = s.Get("AlbumName");
        if (!vh || vh->type != JVal::String) continue;
        const std::string hash = vh->AsString();
        if (hash.empty()) continue;
        const std::wstring name = vn ? Utf8ToWideLocal(vn->AsString()) : std::wstring();
        const std::wstring singer = vs ? Utf8ToWideLocal(vs->AsString()) : std::wstring();
        const uint32_t dur = vd ? static_cast<uint32_t>(vd->AsNumber() * 1000.0) : 0;
        const std::wstring album = va ? Utf8ToWideLocal(va->AsString()) : std::wstring();
        if (name.empty()) continue;
        if (!q.loose && q.duration_ms && dur &&
            std::llabs(static_cast<int64_t>(dur) - static_cast<int64_t>(q.duration_ms)) > 8000) continue;
        ++lookups;

        std::string kq = "/search?ver=1&man=yes&client=pc&keyword=" +
                         UrlEncodeImpl(WideToUtf8Local(singer.empty() ? name : singer + L" - " + name));
        kq += "&duration=" + (dur ? std::to_string(dur) : std::string());
        kq += "&hash=" + UrlEncodeImpl(hash);

        bool ok = false;
        std::string lbody = KugouHostPath(kq, &ok, http_);
        if (!ok) continue;
        JVal lroot;
        if (!ParseJson(lbody, lroot)) continue;
        const JVal* cands = lroot.Get("candidates");
        if (!cands || cands->type != JVal::Array || cands->arr.empty()) continue;

        // 每个音频 hash 通常有 1~2 个歌词候选;只取第一条最相关的,
        // 避免同一首歌在候选列表里刷屏 (用户要换版本可再换歌名重搜)
        const JVal& c = cands->arr[0];
        const JVal* cid = c.Get("id");
        const JVal* cak = c.Get("accesskey");
        if (!cid || !cak) continue;
        LyricCandidate item;
        item.source = "kugou";
        // 酷狗把 id 以字符串返回 ("136759220"),也可能给数字;两种都兼容
        item.id = cid->type == JVal::String ? cid->str
                                            : std::to_string(static_cast<long long>(cid->AsNumber()));
        item.accesskey = cak->type == JVal::String ? cak->str : cak->AsString();
        if (const JVal* v = c.Get("song")) item.title = Utf8ToWideLocal(v->AsString());
        if (const JVal* v = c.Get("singer")) item.artist = Utf8ToWideLocal(v->AsString());
        if (item.title.empty()) item.title = name;
        if (item.artist.empty()) item.artist = singer;
        if (const JVal* v = c.Get("duration")) item.duration_ms = static_cast<uint32_t>(v->AsNumber());
        if (!item.duration_ms) item.duration_ms = dur;
        item.album = album;
        item.has_synced = true; // 酷狗 KRC 带逐字时间轴
        if (item.id.empty() || item.accesskey.empty()) continue;
        bool seen = false; // 不同音质的 hash 常对应同一份歌词
        for (const auto& o : out) seen = seen || o.id == item.id;
        if (!seen) out.push_back(std::move(item));
    }

    if (out.empty() && error) *error = "kugou: none";
    return !out.empty();
}

bool LyricProvider::FetchKugou(const LyricCandidate& c, RawLyricDoc& out, std::string* error) const {
    std::string path = "/download?ver=1&client=pc&id=" + UrlEncodeImpl(c.id) +
                       "&accesskey=" + UrlEncodeImpl(c.accesskey) + "&fmt=krc&charset=utf8";
    std::string body;
    bool got = false;
    for (const char* host : {"http://krcs.kugou.com", "http://lyrics.kugou.com"}) {
        if (http_(std::string(host) + path, body)) { got = true; break; }
    }
    if (got) {
        JVal root;
        if (ParseJson(body, root)) {
            const JVal* content = root.Get("content");
            const JVal* fmt = root.Get("fmt");
            if (content && content->type == JVal::String) {
                std::string bytes;
                if (Base64DecodeImpl(content->str, bytes) && !bytes.empty()) {
                    out = RawLyricDoc{};
                    out.source = "kugou";
                    if (fmt && fmt->AsString() == "krc" && bytes.size() > 4 && bytes[0] == 'k' && bytes[1] == 'r' &&
                        bytes[2] == 'c' && bytes[3] == '1') {
                        out.raw_payload = std::move(bytes);
                        out.kind = RawLyricDoc::Payload::Krc; // 交给上层 KrcDecoder::DecodeMemory
                        return true;
                    }
                    if (LooksLikeLrcImpl(bytes)) {
                        out.lrc_text = std::move(bytes);
                        out.kind = RawLyricDoc::Payload::Lrc;
                        return true;
                    }
                }
            }
        }
    }
    // 退回 lrc 格式
    std::string pathLrc = "/download?ver=1&client=pc&id=" + UrlEncodeImpl(c.id) +
                          "&accesskey=" + UrlEncodeImpl(c.accesskey) + "&fmt=lrc&charset=utf8";
    body.clear();
    got = false;
    for (const char* host : {"http://krcs.kugou.com", "http://lyrics.kugou.com"}) {
        if (http_(std::string(host) + pathLrc, body)) { got = true; break; }
    }
    if (!got) {
        if (error) *error = "kugou: fetch failed";
        return false;
    }
    JVal root;
    if (!ParseJson(body, root)) {
        if (error) *error = "kugou: bad json";
        return false;
    }
    const JVal* content = root.Get("content");
    if (!content || content->type != JVal::String) {
        if (error) *error = "kugou: empty content";
        return false;
    }
    std::string bytes;
    if (!Base64DecodeImpl(content->str, bytes) || bytes.empty()) {
        if (error) *error = "kugou: base64 failed";
        return false;
    }
    out = RawLyricDoc{};
    out.source = "kugou";
    if (LooksLikeLrcImpl(bytes)) {
        out.lrc_text = std::move(bytes);
        out.kind = RawLyricDoc::Payload::Lrc;
    } else {
        out.raw_payload = std::move(bytes); // 纯文本与其他源一致放 raw_payload
        out.kind = RawLyricDoc::Payload::Plain;
    }
    return true;
}

} // namespace lyric_core
