#include "lyric_batch.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <vector>

#include "lyric_manager.hpp"
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

// 去掉歌名里的括号附注
std::wstring Core(const std::wstring& s) {
    std::wstring out;
    int depth = 0;
    for (wchar_t c : s) {
        if (c == L'(' || c == L'（' || c == L'[' || c == L'【') { ++depth; continue; }
        if (c == L')' || c == L'）' || c == L']' || c == L'】') { if (depth) --depth; continue; }
        if (!depth) out += c;
    }
    return Norm(out);
}

// 去括号但**保留原始大小写**。搜索关键词必须用它:
// 酷狗 krcs 区分大小写,实测「Bad Apple!!」有 10 条,「bad apple!!」0 条。
std::wstring CoreRaw(const std::wstring& s) {
    std::wstring out;
    int depth = 0;
    for (wchar_t c : s) {
        if (c == L'(' || c == L'（' || c == L'[' || c == L'【') { ++depth; continue; }
        if (c == L')' || c == L'）' || c == L']' || c == L'】') { if (depth) --depth; continue; }
        if (!depth) out += c;
    }
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

bool TitleHit(const std::wstring& qt, const std::wstring& ct) {
    if (qt.empty() || ct.empty()) return false;
    return Norm(qt) == Norm(ct) || Core(qt) == Core(ct);
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

// 降级源:用 LyricProvider 自己的搜索,只认歌名对得上的
bool TryProviderSource(LyricProvider& p, const char* source, const BatchTrack& t, RawLyricDoc& doc,
                       LyricCandidate& picked) {
    LyricQuery q;
    q.title = t.title;
    q.artist = t.artist;
    q.album = t.album;
    q.duration_ms = t.duration_ms;
    std::vector<LyricCandidate> list;
    std::string err;
    if (!p.Search(source, q, list, &err) || list.empty()) return false;
    int best = -1, bestScore = -9999;
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& c = list[i];
        if (!TitleHit(t.title, c.title)) continue;
        int sc = 0;
        if (Norm(t.title) == Norm(c.title)) sc += 120;
        else if (Core(t.title) == Core(c.title)) sc += 90;
        if (ArtistHit(t.artist, c.artist)) sc += 60;
        if (t.duration_ms && c.duration_ms) {
            const long long dd = std::llabs((long long)c.duration_ms - (long long)t.duration_ms);
            if (dd <= 3000) sc += 50;
            else if (dd <= 8000) sc += 10;
            else sc -= 100;
        }
        if (sc > bestScore) { bestScore = sc; best = (int)i; }
    }
    if (best < 0 || bestScore < 0) return false;
    if (!p.Fetch(list[best], doc, &err)) return false;
    if (doc.kind == RawLyricDoc::Payload::Plain) return false;
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

void Judge(BatchOutcome& r, const BatchTrack& t, const LyricCandidate& picked) {
    r.hit_title = picked.title;
    r.hit_artist = picked.artist;
    r.hit_duration_ms = picked.duration_ms;
    const bool th = TitleHit(t.title, picked.title);
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

                {
                    // 关键词太长会搜不到:曲库里有「Bad Apple!! (feat.SEKAI、初音ミク、宵崎奏、
                    // 朝比奈まふゆ、東雲絵名、暁山瑞希)」这种标题,整串丢给 krcs 直接返回空。
                    // 先用「歌手 - 去括号标题」,搜不到再退化成只用标题。
                    // krcs 必须带歌手才认:实测「Bad Apple!!」单搜 0 条,
                    // 「初音ミク、25時、ナイトコードで。 - Bad Apple!!」能搜到 10 条。
                    // 所以顺序是「歌手 - 完整标题」→「歌手 - 去括号标题」→ 只标题 → 只去括号标题
                    const std::wstring cTitle = Clean(t.title);
                    const std::wstring cArtist = Clean(t.artist);
                    std::wstring coreTitle = CoreRaw(cTitle);
                    if (coreTitle.empty()) coreTitle = cTitle;
                    std::vector<std::wstring> keys;
                    if (!cArtist.empty()) keys.push_back(cArtist + L" - " + cTitle);
                    if (!cArtist.empty() && coreTitle != cTitle)
                        keys.push_back(cArtist + L" - " + coreTitle);
                    keys.push_back(cTitle);
                    if (coreTitle != cTitle) keys.push_back(coreTitle);
                    // 去重,保持顺序
                    std::vector<std::wstring> uniq;
                    for (const auto& k : keys)
                        if (std::find(uniq.begin(), uniq.end(), k) == uniq.end()) uniq.push_back(k);
                    keys.swap(uniq);

                    std::vector<KrcsCand> cands;
                    std::string kerr;
                    bool searched = false;
                    for (const auto& kw : keys) {
                        if (KrcsSearch(kw, t.duration_ms, cands, &kerr)) { searched = true; break; }
                    }
                    if (searched) {
                        const int best = PickCand(t, cands);
                        if (best >= 0) {
                            LyricCandidate lc;
                            lc.source = "kugou";
                            lc.id = cands[best].id;
                            lc.accesskey = cands[best].accesskey;
                            lc.title = cands[best].song;
                            lc.artist = cands[best].singer;
                            lc.duration_ms = cands[best].duration_ms;
                            lc.has_synced = true;
                            std::string ferr;
                            if (provider.Fetch(lc, doc, &ferr) && doc.kind != RawLyricDoc::Payload::Plain) {
                                picked = lc;
                                src = 1;
                            } else {
                                r.note = "kugou fast: fetch " + ferr;
                            }
                        } else {
                            r.note = "kugou fast: 0/" + std::to_string(cands.size()) + " title match";
                        }
                    } else {
                        r.note = "kugou fast: " + kerr;
                    }
                }
                if (src == 0 && TryProviderSource(provider, "kugou", t, doc, picked)) src = 1;

                r.elapsed_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                r.requests = t_requests;
                r.source = src;
                if (src) {
                    Judge(r, t, picked);
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

    // 阶段 2:降级源串行补,连续失败就熔断
    if (opt.allow_fallback && !pending.empty() && !(opt.cancel && opt.cancel->load())) {
        LyricProvider fb(&FastGet);
        int fails = 0;
        for (size_t i : pending) {
            if (fails >= 8) break;
            if (opt.cancel && opt.cancel->load()) break;
            const BatchTrack& t = tracks[i];
            const auto t0 = std::chrono::steady_clock::now();
            t_requests = 0;
            RawLyricDoc doc;
            LyricCandidate picked;
            int src = 0;
            if (TryProviderSource(fb, "lrclib", t, doc, picked)) src = 2;
            else if (TryProviderSource(fb, "netease", t, doc, picked)) src = 3;
            BatchOutcome& r = results[i];
            r.elapsed_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            r.requests += t_requests;
            r.source = src;
            if (src) {
                fails = 0;
                Judge(r, t, picked);
                if (opt.write_lrc) {
                std::string swhy;
                r.saved = SaveAsLrc(t, doc, opt.overwrite, &r.saved_path, &swhy);
                if (!r.saved) r.note = "save: " + swhy;
            }
                if (!r.saved) r.saved_path.clear();
            } else {
                ++fails;
            }
            const size_t d = ++done;
            if (opt.on_progress) opt.on_progress(d, tracks.size());
        }
    }
    return results;
}

} // namespace lyric_core
