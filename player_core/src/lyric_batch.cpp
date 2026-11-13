#include "lyric_batch.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <vector>

#include "lyric_manager.hpp"
#include "audio_tag_parser.hpp"
#include "lyric_provider.hpp"
#include "platform_io.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace lyric_core {
namespace {

// ============================================================================
//  HTTP:线程本地复用 session
//  播放器原来的 DefaultHttpGet 每个请求都 WinHttpOpen 一个 session,批量补 500 首时
//  光 TLS 握手就是主要开销。这里每个线程只建一次,按 host 缓存 connect。
// ============================================================================

#ifdef _WIN32

struct HttpUrl {
    std::wstring host, object;
    INTERNET_PORT port = 0;
    bool secure = false;
};

bool ParseUrl(const std::string& url, HttpUrl& out) {
    const std::wstring w(url.begin(), url.end());
    const size_t p = w.find(L"://");
    if (p == std::wstring::npos) return false;
    const std::wstring scheme = w.substr(0, p);
    const std::wstring rest = w.substr(p + 3);
    const size_t slash = rest.find(L'/');
    const std::wstring hostport = slash == std::wstring::npos ? rest : rest.substr(0, slash);
    out.object = slash == std::wstring::npos ? L"/" : rest.substr(slash);
    const size_t colon = hostport.find(L':');
    if (colon != std::wstring::npos) {
        out.host = hostport.substr(0, colon);
        out.port = static_cast<INTERNET_PORT>(_wtoi(hostport.substr(colon + 1).c_str()));
    } else {
        out.host = hostport;
    }
    out.secure = (scheme == L"https");
    if (out.port == 0) out.port = out.secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    return !out.host.empty();
}

struct HttpTls {
    HINTERNET session = nullptr;
    std::unordered_map<std::wstring, HINTERNET> conns;
    ~HttpTls() {
        for (auto& kv : conns)
            if (kv.second) WinHttpCloseHandle(kv.second);
        if (session) WinHttpCloseHandle(session);
    }
};

HttpTls& HttpLocal() {
    static thread_local HttpTls t;
    return t;
}

thread_local int t_requests = 0;

bool FastGet(const std::string& url, std::string& body) {
    ++t_requests;
    body.clear();
    HttpUrl u;
    if (!ParseUrl(url, u)) return false;
    HttpTls& t = HttpLocal();
    if (!t.session) {
        t.session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) KugouPlayer/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS, 0);
        if (!t.session) return false;
        WinHttpSetTimeouts(t.session, 3000, 3000, 5000, 6000);
    }
    HINTERNET& conn = t.conns[u.host];
    if (!conn) {
        conn = WinHttpConnect(t.session, u.host.c_str(), u.port, 0);
        if (!conn) return false;
    }
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", u.object.c_str(), NULL, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, u.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return false;
    bool ok = false;
    if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(req, NULL)) {
        DWORD status = 0, len = sizeof(status);
        if (WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX) &&
            status == 200) {
            DWORD avail = 0;
            do {
                if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
                std::string chunk(avail, '\0');
                DWORD read = 0;
                if (!WinHttpReadData(req, chunk.data(), avail, &read) || read == 0) break;
                chunk.resize(read);
                body += chunk;
            } while (avail > 0);
            ok = !body.empty();
        }
    }
    WinHttpCloseHandle(req);
    if (!ok) {
        // 连接被服务端回收后句柄还留着,不重置的话之后所有发往同一 host 的请求
        // 都会失败(而且永远不会重建)——批量补全时表现为前几首成功、后面全挂。
        HINTERNET dead = t.conns[u.host];
        if (dead) WinHttpCloseHandle(dead);
        t.conns[u.host] = nullptr;
    }
    return ok;
}

#else

thread_local int t_requests = 0;

bool FastGet(const std::string& url, std::string& body) {
    ++t_requests;
    return LyricProvider::DefaultHttpGet(url, body);
}

#endif

// ============================================================================
//  文本归一化与匹配
// ============================================================================

// 只去掉控制字符(\0 等),不动大小写和标点。
// 标签解析有时会在字符串尾部留下填充的 \0,导致 empty() 为 false 但内容不可见,
// 拼进搜索关键词就成了 " - 歌名",酷狗直接返回空。
std::wstring Clean(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s)
        if (c >= 0x20 || c == L'\t') out += c;
    return out;
}

std::wstring Norm(std::wstring s) {
    // 标签解析可能留下 \0 之类的控制字符。它们会让 URL 编码变成 %00,
    // 请求直接搜不到(酷狗对含 %00 的 keyword 返回空 candidates)。
    {
        std::wstring clean;
        clean.reserve(s.size());
        for (wchar_t c : s)
            if (c >= 0x20 || c == L'\t') clean += c;
        s.swap(clean);
    }
    for (auto& c : s) {
        if (c >= 0xFF01 && c <= 0xFF5E) c = static_cast<wchar_t>(c - 0xFEE0);
        else if (c == 0x3000) c = L' ';
        c = static_cast<wchar_t>(std::towlower(c));
    }
    std::wstring out;
    bool sp = false;
    for (wchar_t c : s) {
        if (std::iswspace(c)) { sp = !out.empty(); continue; }
        if (sp) { out += L' '; sp = false; }
        out += c;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    return out;
}

std::wstring StripParens(const std::wstring& s) {
    std::wstring out;
    int depth = 0;
    for (wchar_t c : s) {
        if (c == L'(' || c == L'（' || c == L'[' || c == L'【' || c == L'《') { ++depth; continue; }
        if (c == L')' || c == L'）' || c == L']' || c == L'】' || c == L'》') { if (depth) --depth; continue; }
        if (!depth) out += c;
    }
    return out;
}

// 去掉歌名里的括号附注
std::wstring Core(const std::wstring& s) {
    return Norm(StripParens(s));
}

// 去括号但**保留原始大小写**。搜索关键词必须用它:
// 酷狗 krcs 区分大小写,实测「Bad Apple!!」有 10 条,「bad apple!!」0 条。
std::wstring CoreRaw(const std::wstring& s) {
    std::wstring out = StripParens(s);
    // 只收掉首尾空白和连续空格,不动大小写
    std::wstring trim;
    bool sp = false;
    for (wchar_t c : out) {
        if (c == L' ' || c == L'\t') { sp = !trim.empty(); continue; }
        if (sp) { trim += L' '; sp = false; }
        trim += c;
    }
    while (!trim.empty() && trim.back() == L' ') trim.pop_back();
    return trim;
}

// 统一复用 LyricProvider::Normalize(去空白/全角折半角/标点/中点/波浪号)做比对键
std::wstring MatchKey(const std::wstring& s) {
    return LyricProvider::Normalize(Clean(s));
}

bool IsCjkOrKana(wchar_t c) {
    return (c >= 0x3040 && c <= 0x30FF) || // Hiragana & Katakana
           (c >= 0x31F0 && c <= 0x31FF) || // Katakana Phonetic Extensions
           (c >= 0x4E00 && c <= 0x9FFF) || // CJK Unified
           (c >= 0x3400 && c <= 0x4DBF) || // CJK Ext A
           (c >= 0xAC00 && c <= 0xD7AF);   // Hangul
}

bool HasNonAsciiWord(const std::wstring& s) {
    int cnt = 0;
    for (wchar_t c : s)
        if (IsCjkOrKana(c) && ++cnt >= 2) return true;
    return false;
}

bool IsStopSeg(const std::wstring& k) {
    return k == L"single" || k == L"ep" || k == L"ver" || k == L"version" || k == L"remix" ||
           k == L"live" || k == L"instrumental" || k == L"inst" || k == L"edit" || k == L"feat" ||
           k == L"ft" || k == L"a" || k == L"the";
}

// 抽取分隔片段及脚本切换片段(如 "Be a flower 花になって" -> ["beaflower", "花になって", ...])
std::vector<std::wstring> TitleSegments(const std::wstring& s) {
    const std::wstring a = Norm(s);
    std::vector<std::wstring> chunks;
    auto pushUnique = [&](const std::wstring& raw) {
        std::wstring k = MatchKey(raw);
        if (!k.empty() && !IsStopSeg(k) &&
            std::find(chunks.begin(), chunks.end(), k) == chunks.end()) {
            chunks.push_back(std::move(k));
        }
    };
    {
        std::wstring cur;
        int prevScript = -1; // 0=ascii/other, 1=cjk/kana/hangul
        for (wchar_t c : a) {
            if (c == L'/' || c == L'-' || c == L'_' || c == L'~' || c == L'\u00b7') {
                if (!cur.empty()) { pushUnique(cur); cur.clear(); }
                prevScript = -1;
                continue;
            }
            if (c == L' ' || c == L'\t') {
                if (!cur.empty()) cur += L' ';
                continue;
            }
            const int sc = IsCjkOrKana(c) ? 1 : 0;
            if (prevScript != -1 && sc != prevScript && !cur.empty()) {
                pushUnique(cur);
                cur.clear();
            }
            cur += c;
            prevScript = sc;
        }
        if (!cur.empty()) pushUnique(cur);
    }
    {
        std::wstring cur;
        for (wchar_t c : a) {
            if (c == L' ' || c == L'\t' || c == L'/' || c == L'-' || c == L'_' || c == L'~' || c == L'\u00b7') {
                if (!cur.empty()) { pushUnique(cur); cur.clear(); }
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) pushUnique(cur);
    }
    return chunks;
}

bool TitleHit(const std::wstring& qt, const std::wstring& ct, int precision = 1) {
    if (qt.empty() || ct.empty()) return false;
    const std::wstring ka = MatchKey(qt), kb = MatchKey(ct);
    const std::wstring ca = MatchKey(StripParens(qt)), cb = MatchKey(StripParens(ct));
    if (ka.empty() || kb.empty()) return false;
    if (ka == kb || (!ca.empty() && ca == cb)) return true;
    if (precision <= 0) return false;   // 快速档:只认精确相等,宁可漏也不误配

    // 拆段匹配:覆盖双语顺序反转、带前缀标签、括号内外重排等情况
    {
        const auto qSegs = TitleSegments(qt);
        const auto qCoreSegs = TitleSegments(StripParens(qt));
        const auto cCoreSegs = TitleSegments(StripParens(ct));
        auto checkSegs = [&](const std::vector<std::wstring>& segs) -> bool {
            if (segs.size() <= 1) return false;
            for (const auto& seg : segs) {
                if (seg == kb || (!cb.empty() && seg == cb)) return true;
                if (seg.size() >= 2 && kb.find(seg) != std::wstring::npos &&
                    seg.size() * 10 >= kb.size() * 7)
                    return true;
                if (seg.size() >= 2 && !cb.empty() && cb.find(seg) != std::wstring::npos &&
                    seg.size() * 10 >= cb.size() * 7)
                    return true;
                for (const auto& cs : cCoreSegs) {
                    if (seg == cs && (HasNonAsciiWord(seg) || seg.size() >= 6)) return true;
                }
            }
            return false;
        };
        if (checkSegs(qSegs) || checkSegs(qCoreSegs)) return true;
    }

    // 兜底:一方完整包含另一方。标准档要求短串占长串 40% 以上,精细档放宽到 30%
    auto containsMatch = [precision](const std::wstring& x, const std::wstring& y) -> bool {
        if (x.empty() || y.empty()) return false;
        const std::wstring& lo = x.size() <= y.size() ? x : y;
        const std::wstring& hi = x.size() <= y.size() ? y : x;
        const int needPct = precision >= 2 ? 3 : 4;
        if (lo.size() >= 2 && hi.find(lo) != std::wstring::npos &&
            lo.size() * 10 >= hi.size() * needPct)
            return true;
        if (precision >= 2 && lo.size() >= 3 && hi.find(lo) != std::wstring::npos) return true;
        return false;
    };
    return containsMatch(ka, kb) || containsMatch(ca, cb);
}

bool ArtistHit(const std::wstring& qa, const std::wstring& ca) {
    if (qa.empty() || ca.empty()) return false;
    const std::wstring a = Norm(qa), b = Norm(ca);
    if (a == b) return true;
    if (a.find(b) != std::wstring::npos || b.find(a) != std::wstring::npos) return true;
    auto split = [](const std::wstring& s) {
        std::vector<std::wstring> v;
        std::wstring cur;
        for (wchar_t c : s) {
            if (c == L'/' || c == L'、' || c == L',' || c == L'，' || c == L'&' || c == L'·' || c == L'；' ||
                c == L';') {
                if (!cur.empty()) v.push_back(Norm(cur));
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty()) v.push_back(Norm(cur));
        return v;
    };
    for (const auto& x : split(qa))
        for (const auto& y : split(ca))
            if (!x.empty() && x == y) return true;
    return false;
}

std::string Utf8Of(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring WideOf(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string UrlEncode(const std::wstring& w) {
    static const char* hex = "0123456789ABCDEF";
    const std::string u8 = Utf8Of(w);
    std::string out;
    out.reserve(u8.size() * 3);
    for (unsigned char c : u8) {
        // 标签里可能残留 \0 等控制字符。URL 里带 %00 会被服务端当成截断,
        // 酷狗对这类 keyword 直接返回空 candidates —— 所以这里整段丢掉。
        if (c < 0x20 || c == 0x7F) continue;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

// ============================================================================
//  酷狗 krcs 快路径:一次搜索拿 20 个候选(不需要 hash),再下载
// ============================================================================

struct KrcsCand {
    std::string id, accesskey;
    std::wstring song, singer;
    uint32_t duration_ms = 0;
    int score = 0;
};

bool JsonStr(const std::string& obj, const char* key, std::string& out) {
    const std::string pat = std::string("\"") + key + "\":\"";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    std::string v;
    for (size_t i = p; i < obj.size(); ++i) {
        const char c = obj[i];
        if (c == '\\' && i + 1 < obj.size()) {
            const char e = obj[++i];
            if (e == 'n') v += '\n';
            else if (e == 't') v += '\t';
            else v += e;
            continue;
        }
        if (c == '"') break;
        v += c;
    }
    out = std::move(v);
    return true;
}

bool JsonNum(const std::string& obj, const char* key, long long& out) {
    const std::string pat = std::string("\"") + key + "\":";
    size_t p = obj.find(pat);
    if (p == std::string::npos) return false;
    p += pat.size();
    if (p < obj.size() && obj[p] == '"') ++p;
    size_t e = p;
    while (e < obj.size() && (isdigit(static_cast<unsigned char>(obj[e])) || obj[e] == '-' || obj[e] == '.')) ++e;
    if (e == p) return false;
    out = std::atoll(obj.substr(p, e - p).c_str());
    return true;
}

size_t MatchBracket(const std::string& s, size_t start) {
    int depth = 0;
    bool inStr = false;
    for (size_t i = start; i < s.size(); ++i) {
        const char c = s[i];
        if (inStr) {
            if (c == '\\') { ++i; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '[' || c == '{') ++depth;
        else if (c == ']' || c == '}') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

bool KrcsSearch(const std::wstring& keyword, uint32_t duration_ms, std::vector<KrcsCand>& out, std::string* err) {
    out.clear();
    std::string url = "http://krcs.kugou.com/search?ver=1&man=yes&client=pc&keyword=" + UrlEncode(keyword);
    if (duration_ms) url += "&duration=" + std::to_string(duration_ms);
    std::string body;
    if (!FastGet(url, body)) {
        if (err) *err = "krcs http failed";
        return false;
    }
    const size_t key = body.find("\"candidates\":");
    if (key == std::string::npos) {
        if (err) *err = "no candidates";
        return false;
    }
    const size_t lb = body.find('[', key);
    if (lb == std::string::npos) {
        if (err) *err = "no array";
        return false;
    }
    const size_t rb = MatchBracket(body, lb);
    if (rb == std::string::npos) {
        if (err) *err = "bad array";
        return false;
    }
    size_t p = lb + 1;
    while (p < rb) {
        const size_t ob = body.find('{', p);
        if (ob == std::string::npos || ob >= rb) break;
        const size_t oe = MatchBracket(body, ob);
        if (oe == std::string::npos || oe > rb) break;
        const std::string obj = body.substr(ob, oe - ob + 1);
        KrcsCand c;
        long long v = 0;
        if (JsonStr(obj, "id", c.id) && JsonStr(obj, "accesskey", c.accesskey)) {
            std::string s;
            if (JsonStr(obj, "song", s)) c.song = WideOf(s);
            if (JsonStr(obj, "singer", s)) c.singer = WideOf(s);
            if (JsonNum(obj, "duration", v)) c.duration_ms = static_cast<uint32_t>(v);
            if (JsonNum(obj, "score", v)) c.score = static_cast<int>(v);
            if (!c.id.empty() && !c.accesskey.empty()) out.push_back(std::move(c));
        }
        p = oe + 1;
    }
    if (out.empty()) {
        if (err) *err = "empty candidates";
        return false;
    }
    return true;
}

// 从候选里挑一条:歌名必须对得上,时长差 >8s 直接排除
int PickCand(const BatchTrack& t, const std::vector<KrcsCand>& cands) {
    int best = -1, bestScore = -9999;
    for (size_t k = 0; k < cands.size(); ++k) {
        const auto& c = cands[k];
        if (!TitleHit(t.title, c.song)) continue;
        int sc = 0;
        if (Norm(t.title) == Norm(c.song)) sc += 120;
        else if (Core(t.title) == Core(c.song)) sc += 90;
        if (ArtistHit(t.artist, c.singer)) sc += 60;
        if (t.duration_ms && c.duration_ms) {
            const long long dd = std::llabs((long long)c.duration_ms - (long long)t.duration_ms);
            if (dd <= 2000) sc += 50;
            else if (dd <= 5000) sc += 15;
            else if (dd <= 8000) sc -= 20;
            else continue;
        }
        if (sc > bestScore) { bestScore = sc; best = (int)k; }
    }
    return best;
}

std::wstring TrimWs(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == L' ' || s[a] == L'\t')) ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'\t')) --b;
    return s.substr(a, b - a);
}

bool IsInstrumentalTitle(const std::wstring& s) {
    const std::wstring k = Norm(s);
    return k.find(L"instrumental") != std::wstring::npos ||
           k.find(L"off vocal") != std::wstring::npos ||
           k.find(L"offvocal") != std::wstring::npos ||
           k.find(L"karaoke") != std::wstring::npos ||
           k.find(L"伴奏") != std::wstring::npos ||
           k.find(L"カラオケ") != std::wstring::npos;
}

bool HasAsciiWord(const std::wstring& s) {
    int cnt = 0;
    for (wchar_t c : s)
        if (((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z')) && ++cnt >= 2) return true;
    return false;
}

// 从「罗马音/英文 + 中日韩文」双语混排里抽取出非 ASCII 主干用于二次检索
std::wstring ExtractCjkPart(const std::wstring& s) {
    if (!HasNonAsciiWord(s) || !HasAsciiWord(s)) return s;
    std::wstring out;
    bool prevCjk = false;
    for (wchar_t c : s) {
        const bool isCjk = IsCjkOrKana(c);
        const bool isJoiner = (c == L'・' || c == L'ー' || c == L'々' || c == L'ゝ' || c == L'ゞ' ||
                               c == L'!' || c == L'?' || c == L'！' || c == L'？');
        if (isCjk || (prevCjk && isJoiner)) {
            out += c;
            prevCjk = true;
        } else {
            if (prevCjk && !out.empty() && out.back() != L' ') out += L' ';
            prevCjk = false;
        }
    }
    out = TrimWs(out);
    return HasNonAsciiWord(out) ? out : s;
}

// 构造双语/杂质清洗后的重试检索词(只在主搜索未命中时启用)
bool BuildRetryQuery(const BatchTrack& t, LyricQuery& out) {
    std::wstring title = TrimWs(CoreRaw(Clean(t.title)));
    std::wstring artist = TrimWs(Clean(t.artist));

    // 去掉标签常见的 "Single " 前缀和尾部混入的 " -歌手" 后缀
    const std::wstring low = Norm(title);
    if (low.rfind(L"single ", 0) == 0 && title.size() > 7) title = TrimWs(title.substr(7));
    if (!artist.empty()) {
        const std::wstring ak = MatchKey(artist);
        const std::wstring akCjk = MatchKey(ExtractCjkPart(artist));
        const size_t dash = title.rfind(L" -");
        if (dash != std::wstring::npos && dash > 0) {
            const std::wstring tail = MatchKey(title.substr(dash + 2));
            if (!tail.empty() && (tail == ak || (!akCjk.empty() && tail == akCjk)))
                title = TrimWs(title.substr(0, dash));
        }
    }

    std::wstring jt = ExtractCjkPart(title);
    // 像 "優しい...-Anime Version-(M3)" 只是标注版型,去完版本修饰后搜没意义且会多跑一倍请求
    if (jt != title) {
        const std::wstring lowT = Norm(title);
        if (lowT.find(L"anime version") != std::wstring::npos || lowT.find(L"instrumental") != std::wstring::npos)
            jt = title;
    }
    const std::wstring ja = ExtractCjkPart(artist);
    if (jt == TrimWs(Clean(t.title)) && ja == TrimWs(Clean(t.artist))) return false;

    out.title = jt.empty() ? title : jt;
    out.artist = ja.empty() ? artist : ja;
    out.album = t.album;
    out.duration_ms = t.duration_ms;
    return MatchKey(out.title) != MatchKey(t.title) || MatchKey(out.artist) != MatchKey(t.artist);
}

bool IsTransportError(const std::string& err) {
    return err.find("http failed") != std::string::npos ||
           err.find("bad json") != std::string::npos ||
           err.find("code 405") != std::string::npos;
}

// 降级源:用 LyricProvider 自己的搜索,只认歌名对得上的
bool TryProviderSource(LyricProvider& p, const char* source, const BatchTrack& t, RawLyricDoc& doc,
                       LyricCandidate& picked, std::string* why = nullptr, int precision = 1,
                       bool* isNetErr = nullptr) {
    if (isNetErr) *isNetErr = false;
    auto pickBest = [&](const std::vector<LyricCandidate>& list, int& outScore) -> int {
        int best = -1;
        outScore = -9999;
        for (size_t i = 0; i < list.size(); ++i) {
            const auto& c = list[i];
            if (!TitleHit(t.title, c.title, precision)) continue;
            // 纯伴奏/卡拉OK版本不贴人声歌词,避免误把主唱歌词写给 -instrumental-
            if (IsInstrumentalTitle(t.title) && !IsInstrumentalTitle(c.title)) continue;
            int sc = 0;
            const std::wstring kt = MatchKey(t.title), kc = MatchKey(c.title);
            const std::wstring ct = MatchKey(StripParens(t.title)), cc = MatchKey(StripParens(c.title));
            if (kt == kc) sc += 120;
            else if (!ct.empty() && ct == cc) sc += 90;
            const bool ah = ArtistHit(t.artist, c.artist);
            if (ah) sc += 60;

            bool durBad = false;
            bool durClose = false;
            if (t.duration_ms && c.duration_ms) {
                const long long dd = std::llabs((long long)c.duration_ms - (long long)t.duration_ms);
                if (dd <= 3000) { sc += 50; durClose = true; }
                else if (dd <= 8000) sc += 10;
                else { sc -= (precision >= 2 ? 20 : 50); durBad = true; }
            }
            // 防护:歌手明确冲突时先扣分(避免「Que Sera Sera ケセラセラ」误抢翻唱,压过后再走双语主干重试)
            if (!t.artist.empty() && !c.artist.empty() && !ah) {
                sc -= 80;
                if (durBad || (kt.size() <= 4 && !durClose)) sc -= 100;
            }
            if (sc > outScore) { outScore = sc; best = (int)i; }
        }
        return best;
    };

    LyricQuery q;
    q.title = t.title;
    q.artist = t.artist;
    q.album = t.album;
    q.duration_ms = t.duration_ms;
    std::vector<LyricCandidate> list;
    std::string err;
    p.Search(source, q, list, &err);
    if (IsTransportError(err)) {
        if (isNetErr) *isNetErr = true;
        if (why) *why = std::string(source) + ": " + err;
        return false;
    }

    int bestScore = -9999;
    int best = list.empty() ? -1 : pickBest(list, bestScore);

    // 主查询没命中或置信度不高(如没对上歌手)时,若歌名/歌手为双语混排,提取中日韩主干重试一次
    LyricQuery q2;
    if ((best < 0 || bestScore < 100) && precision >= 1 && BuildRetryQuery(t, q2)) {
        std::vector<LyricCandidate> list2;
        std::string err2;
        p.Search(source, q2, list2, &err2);
        if (IsTransportError(err2)) {
            if (isNetErr) *isNetErr = true;
            if (why) *why = std::string(source) + ": " + err2;
            return false;
        }
        if (!list2.empty()) {
            int sc2 = -9999;
            int b2 = pickBest(list2, sc2);
            if (b2 >= 0 && (best < 0 || sc2 > bestScore)) {
                list.swap(list2);
                best = b2;
                bestScore = sc2;
            } else if (list.empty()) {
                list.swap(list2);
            }
        }
    }

    if (list.empty()) {
        if (why) *why = std::string(source) + ": " + (err.empty() ? "0 results" : err);
        return false;
    }
    if (best < 0 || bestScore < 0) {
        if (why) {
            *why = std::string(source) + ": " + std::to_string(list.size()) + " results, no title match";
            for (size_t k = 0; k < list.size() && k < 3; ++k) {
                *why += " | " + AudioTagParser::WideToUtf8(list[k].title);
                if (!list[k].artist.empty()) *why += " / " + AudioTagParser::WideToUtf8(list[k].artist);
            }
        }
        return false;
    }
    if (!p.Fetch(list[best], doc, &err)) {
        if (IsTransportError(err) && isNetErr) *isNetErr = true;
        if (why) *why = std::string(source) + ": fetch " + (err.empty() ? "failed" : err);
        return false;
    }
    if (doc.kind == RawLyricDoc::Payload::Plain) {
        if (why) *why = std::string(source) + ": plain lyric only";
        return false;
    }
    picked = list[best];
    return true;
}

// 解码 + 写盘(标准 LRC,带翻译)
bool SaveAsLrc(const BatchTrack& t, const RawLyricDoc& doc, bool overwrite, std::wstring* outPath,
               std::string* why) try {
    ParsedLyrics p;
    if (doc.kind == RawLyricDoc::Payload::Krc) {
        if (!KrcDecoder::DecodeMemory(reinterpret_cast<const uint8_t*>(doc.raw_payload.data()),
                                      doc.raw_payload.size(), p)) {
            if (why) *why = "decode krc failed";
            return false;
        }
    } else if (doc.kind == RawLyricDoc::Payload::Lrc) {
        if (!KrcDecoder::ParseLrcText(doc.lrc_text, p)) {
            if (why) *why = "parse lrc failed";
            return false;
        }
    } else {
        if (why) *why = "unsupported payload";
        return false;
    }
    if (p.IsEmpty()) {
        if (why) *why = "no lyric lines";
        return false;
    }
    const std::string lrc = p.ToStandardLrc(true);
    if (lrc.empty()) {
        if (why) *why = "empty standard lrc";
        return false;
    }

    std::filesystem::path path(t.file_path);
    path.replace_extension(L".lrc");
    if (!overwrite && platform::PathExists(path.wstring())) {
        if (why) *why = "lrc already exists";
        return false;
    }
    platform::WritableFile f;
    if (!f.Create(path.wstring())) {
        if (why) *why = "create failed";
        return false;
    }
    static const char k_bom[3] = {'\xEF', '\xBB', '\xBF'};
    f.Write(k_bom, 3);
    f.Write(lrc.data(), lrc.size());
    f.Close();
    if (outPath) *outPath = path.wstring();
    return true;
} catch (const std::exception& e) {
    // std::filesystem 遇到非法文件名会抛 filesystem_error;不能让异常穿出工作线程
    if (why) *why = std::string("保存异常: ") + e.what();
    return false;
} catch (...) {
    if (why) *why = "保存异常";
    return false;
}

void Judge(BatchOutcome& r, const BatchTrack& t, const LyricCandidate& picked, int precision = 1) {
    r.hit_title = picked.title;
    r.hit_artist = picked.artist;
    r.hit_duration_ms = picked.duration_ms;
    const bool th = TitleHit(t.title, picked.title, precision);
    const bool ah = ArtistHit(t.artist, picked.artist);
    long long dd = -1;
    if (t.duration_ms && picked.duration_ms) dd = std::llabs((long long)picked.duration_ms - (long long)t.duration_ms);
    const bool durOk = (dd < 0) || dd <= 3000;
    if (th && ah && durOk) r.level = 3;
    else if (th && durOk) r.level = 2;
    else r.level = 1;
}

} // namespace

std::vector<BatchOutcome> LyricBatch::Run(const std::vector<BatchTrack>& tracks, const BatchOptions& opt) {
    std::vector<BatchOutcome> results(tracks.size());
    if (tracks.empty()) return results;
    const int threads = std::clamp(opt.threads, 1, 32);

    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};

    // 阶段 1:并发跑完整酷狗(快路径 + hash 兜底) + 降级源
    // 降级源并发会挂(实测 lrclib / 网易云在多线程下整片失败),
    // 所以并发阶段只做酷狗,剩下的交给阶段 2 串行。
    std::vector<size_t> pending;
    std::mutex pendMtx;
    auto worker = [&]() {
        LyricProvider provider(&FastGet);
        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= tracks.size()) break;
            if (opt.cancel && opt.cancel->load()) break;
            try {
                const BatchTrack& t = tracks[i];
                BatchOutcome r;
                const auto t0 = std::chrono::steady_clock::now();
                t_requests = 0;
                RawLyricDoc doc;
                LyricCandidate picked;
                int src = 0;

                // 这里原本还有一条「kugou fast」快路径,用 krcs/search?keyword= 直接搜。
                // 那个接口早已废弃 —— 返回 200 但 candidates 恒为空,每首歌白跑 4 次请求,
                // 还把 note 写成 "kugou fast: no candidates",看起来像网络问题。
                // 统一走 provider 的两段式:songsearch 拿 FileHash,再用 hash 查 krcs。
                if (src == 0 && TryProviderSource(provider, "kugou", t, doc, picked, &r.note, opt.precision))
                    src = 1;

                r.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                r.requests = t_requests;
                r.source = src;
                if (src) {
                    Judge(r, t, picked, opt.precision);
                    if (opt.write_lrc) {
                    std::string swhy;
                    r.saved = SaveAsLrc(t, doc, opt.overwrite, &r.saved_path, &swhy);
                    if (!r.saved) r.note = "save: " + swhy;
                }
                    if (!r.saved) r.saved_path.clear();
                } else {
                    std::lock_guard<std::mutex> lk(pendMtx);
                    pending.push_back(i);
                }
                results[i] = std::move(r);
            } catch (const std::exception& e) {
                // 任何一个环节抛异常都不能让工作线程 terminate 掉整个进程
                BatchOutcome fb;
                fb.note = std::string("异常: ") + e.what();
                results[i] = std::move(fb);
            } catch (...) {
                BatchOutcome fb;
                fb.note = "未知异常";
                results[i] = std::move(fb);
            }
            const size_t d = ++done;
            if (opt.on_progress) opt.on_progress(d, tracks.size());
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(threads);
    for (int i = 0; i < threads; ++i) pool.emplace_back(worker);
    for (auto& th : pool) th.join();

    // 阶段 2:降级源串行补 —— 仅按独立源的「连续网络/限流异常」熔断,
    // 正常的「纯音乐查无此歌」绝不触发熔断,避免前排几首 OST 直接杀死整批兜底。
    if (opt.allow_fallback && !pending.empty() && !(opt.cancel && opt.cancel->load())) {
        LyricProvider fb(&FastGet);
        int lrclibNetFails = 0;
        int neteaseNetFails = 0;
        auto lastNeteaseAt = std::chrono::steady_clock::time_point{};
        for (size_t i : pending) {
            if (lrclibNetFails >= 5 && neteaseNetFails >= 3) break;
            if (opt.cancel && opt.cancel->load()) break;
            const BatchTrack& t = tracks[i];
            const auto t0 = std::chrono::steady_clock::now();
            t_requests = 0;
            RawLyricDoc doc;
            LyricCandidate picked;
            int src = 0;
            std::string fbWhy;

            if (lrclibNetFails < 5) {
                bool netErr = false;
                if (TryProviderSource(fb, "lrclib", t, doc, picked, &fbWhy, opt.precision, &netErr)) {
                    src = 2;
                    lrclibNetFails = 0;
                } else if (netErr) {
                    ++lrclibNetFails;
                } else {
                    lrclibNetFails = 0;
                }
            }
            if (src == 0 && neteaseNetFails < 3 && !(opt.cancel && opt.cancel->load())) {
                // 仅当距上次网易云请求不足 220ms 时补足间隔,避免阻塞 LRCLIB 已耗时的轮次
                if (lastNeteaseAt != std::chrono::steady_clock::time_point{}) {
                    const auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - lastNeteaseAt);
                    if (gap.count() < 220)
                        std::this_thread::sleep_for(std::chrono::milliseconds(220 - gap.count()));
                }
                lastNeteaseAt = std::chrono::steady_clock::now();
                bool netErr = false;
                if (TryProviderSource(fb, "netease", t, doc, picked, &fbWhy, opt.precision, &netErr)) {
                    src = 3;
                    neteaseNetFails = 0;
                } else if (netErr) {
                    ++neteaseNetFails;
                    if (neteaseNetFails == 1 && !(opt.cancel && opt.cancel->load()))
                        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                } else {
                    neteaseNetFails = 0;
                }
            }

            BatchOutcome& r = results[i];
            r.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            r.requests += t_requests;
            r.source = src;
            if (src) {
                Judge(r, t, picked, opt.precision);
                if (opt.write_lrc) {
                    std::string swhy;
                    r.saved = SaveAsLrc(t, doc, opt.overwrite, &r.saved_path, &swhy);
                    if (!r.saved) r.note = "save: " + swhy;
                }
                if (!r.saved) r.saved_path.clear();
            } else if (!fbWhy.empty()) {
                if (r.note.empty()) r.note = fbWhy;
            }
            const size_t d = ++done;
            if (opt.on_progress) opt.on_progress(d, tracks.size());
        }
    }
    return results;
}

} // namespace lyric_core
