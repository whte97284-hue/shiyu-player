// FLAC 封面补齐:读标签 → 在线取图 → 以 PICTURE 块写入
// 全部自研,零第三方依赖 (HTTP 复用 lyric_provider 的 WinHTTP 封装)
#include "cover_fetcher.hpp"
#include "lyric_provider.hpp"
#include "platform_io.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace cover_core {

// ===========================================================================
//  一、HTTP 钩子
// ===========================================================================
namespace {
HttpGetFn g_http;
} // namespace

void SetHttpGet(HttpGetFn fn) { g_http = std::move(fn); }
HttpGetFn GetHttp() {
    if (g_http) return g_http;
    return HttpGetFn(&lyric_core::LyricProvider::DefaultHttpGet);
}

namespace {
bool Http(const std::string& url, std::string& body) { return GetHttp()(url, body); }

// UTF-8 <-> 宽字符 (Windows 上路径是 UTF-16)
std::string Narrow(const std::wstring& w) {
#ifdef _WIN32
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(std::max(0, n)), '\0');
    if (n > 0) ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
#else
    return std::string(w.begin(), w.end());
#endif
}
} // namespace

// ===========================================================================
//  二、MD5 (RFC 1321) —— 酷狗的 sqhash 就是「整个文件 MD5」
// ===========================================================================
namespace {

struct Md5Ctx {
    uint32_t h[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};
    uint64_t bytes = 0;
    uint8_t buf[64];
    size_t used = 0;

    static uint32_t Rol(uint32_t x, int c) { return (x << c) | (x >> (32 - c)); }

    void Block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u, 0xfd469501u,
            0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu, 0x49b40821u,
            0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u,
            0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au,
            0xfffa3942u, 0x8771f681u, 0x6d9d6122u, 0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
            0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u,
            0xf4292244u, 0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
            0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
        static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<uint32_t>(p[i * 4]) | (static_cast<uint32_t>(p[i * 4 + 1]) << 8) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 16) | (static_cast<uint32_t>(p[i * 4 + 3]) << 24);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
            else { f = c ^ (b | ~d); g = (7 * i) & 15; }
            const uint32_t tmp = d;
            d = c;
            c = b;
            b = b + Rol(a + f + K[i] + w[g], S[i]);
            a = tmp;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }

    void Update(const uint8_t* data, size_t n) {
        bytes += n;
        if (used) {
            const size_t take = std::min(n, 64 - used);
            std::memcpy(buf + used, data, take);
            used += take;
            data += take;
            n -= take;
            if (used == 64) { Block(buf); used = 0; }
        }
        while (n >= 64) { Block(data); data += 64; n -= 64; }
        if (n) { std::memcpy(buf, data, n); used = n; }
    }

    std::string Hex() {
        const uint64_t bits = bytes * 8;
        uint8_t pad[72];
        size_t padLen = (used < 56) ? (56 - used) : (120 - used);
        std::memset(pad, 0, sizeof(pad));
        pad[0] = 0x80;
        for (int i = 0; i < 8; ++i) pad[padLen + i] = static_cast<uint8_t>(bits >> (8 * i));
        Update(pad, padLen + 8);
        static const char* kHex = "0123456789abcdef";
        std::string s;
        s.reserve(32);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                const uint8_t v = static_cast<uint8_t>(h[i] >> (8 * j));
                s.push_back(kHex[v >> 4]);
                s.push_back(kHex[v & 15]);
            }
        return s;
    }
};

} // namespace

std::string Md5File(const std::wstring& path) {
    platform::ReadOnlyFile f;
    if (!f.Open(path)) return {};
    Md5Ctx ctx;
    // 1MB 缓冲:20MB 文件 20 次读,实测约 30ms (比 4KB 快一个量级)
    std::vector<uint8_t> buf(1 << 20);
    uint64_t off = 0;
    const uint64_t size = f.Size();
    while (off < size) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - off));
        const size_t got = f.ReadAt(off, buf.data(), want);
        if (got == 0) return {};
        ctx.Update(buf.data(), got);
        off += got;
    }
    return ctx.Hex();
}

// ===========================================================================
//  三、FLAC 元数据
// ===========================================================================
namespace {

constexpr uint32_t kBe24(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 16) | (static_cast<uint32_t>(p[1]) << 8) | p[2];
}

struct BlockHead {
    uint8_t type = 0;
    bool last = false;
    uint32_t len = 0;
};

// 读 pos 处的块头;返回 false 表示越界
bool ReadHead(const platform::ReadOnlyFile& f, uint64_t pos, BlockHead& h) {
    uint8_t bh[4];
    if (f.ReadAt(pos, bh, 4) != 4) return false;
    h.last = (bh[0] & 0x80) != 0;
    h.type = static_cast<uint8_t>(bh[0] & 0x7F);
    h.len = kBe24(bh + 1);
    if (h.len > 0xFFFFFFu) return false;
    return pos + 4 + h.len <= f.Size();
}

// 扫一遍元数据块。audio_offset 返回音频数据起点
bool ScanBlocks(const platform::ReadOnlyFile& f, bool* hasPicture, uint64_t* audio_offset) {
    uint8_t magic[4];
    if (f.ReadAt(0, magic, 4) != 4 || std::memcmp(magic, "fLaC", 4) != 0) return false;
    bool pic = false;
    uint64_t pos = 4;
    for (int guard = 0; guard < 1024; ++guard) {
        BlockHead h;
        if (!ReadHead(f, pos, h)) break;
        if (h.type == 6) pic = true;
        pos += 4 + h.len;
        if (h.last) break;
    }
    if (hasPicture) *hasPicture = pic;
    if (audio_offset) *audio_offset = pos;
    return true;
}

} // namespace

bool IsFlac(const std::wstring& path) {
    platform::ReadOnlyFile f;
    if (!f.Open(path)) return false;
    uint8_t m[4];
    return f.ReadAt(0, m, 4) == 4 && std::memcmp(m, "fLaC", 4) == 0;
}

bool FlacHasCover(const std::wstring& path) {
    platform::ReadOnlyFile f;
    if (!f.Open(path)) return false;
    bool pic = false;
    return ScanBlocks(f, &pic, nullptr) && pic;
}

bool ReadFlacTags(const std::wstring& path, FlacTags& out) {
    out = FlacTags{};
    platform::ReadOnlyFile f;
    if (!f.Open(path)) return false;
    uint8_t magic[4];
    if (f.ReadAt(0, magic, 4) != 4 || std::memcmp(magic, "fLaC", 4) != 0) return false;

    uint64_t pos = 4;
    for (int guard = 0; guard < 1024; ++guard) {
        BlockHead h;
        if (!ReadHead(f, pos, h)) break;
        if (h.type == 4 && h.len >= 8 && h.len < (1u << 20)) {
            std::vector<uint8_t> b(h.len);
            if (f.ReadAt(pos + 4, b.data(), h.len) != h.len) break;
            // Vorbis Comment: vendor_len(LE32) + vendor + count(LE32) + count × (len + "KEY=value")
            const auto rd32 = [&](size_t at) {
                return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
                       (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
            };
            size_t p = rd32(0);
            if (p + 4 > b.size()) break;
            p += 4;
            const uint32_t n = rd32(p);
            p += 4;
            std::string albumArtist;
            for (uint32_t i = 0; i < n && p + 4 <= b.size(); ++i) {
                const uint32_t cl = rd32(p);
                p += 4;
                if (cl > b.size() - p) break;
                const std::string kv(reinterpret_cast<const char*>(b.data() + p), cl);
                p += cl;
                const size_t eq = kv.find('=');
                if (eq == std::string::npos) continue;
                std::string k = kv.substr(0, eq);
                for (char& c : k) c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
                const std::string v = kv.substr(eq + 1);
                if (k == "TITLE") out.title = v;
                else if (k == "ARTIST") out.artist = v;
                else if (k == "ALBUMARTIST") albumArtist = v;
                else if (k == "ALBUM") out.album = v;
            }
            if (out.artist.empty()) out.artist = albumArtist;
        }
        pos += 4 + h.len;
        if (h.last) break;
    }
    return !out.title.empty();
}

bool EmbedFlacPicture(const std::wstring& path, const std::string& jpeg, int width, int height,
                      std::string* error, const std::string& mimeIn) {
    const char* sniffed = SniffImage(jpeg);
    if (!sniffed) {
        if (error) *error = "图片格式不认识";
        return false;
    }
    const std::string mimeStr = mimeIn.empty() ? std::string(sniffed) : mimeIn;
    if (jpeg.size() > 0xFFFFF0u) { // 单块最大 16MB-16
        if (error) *error = "封面过大";
        return false;
    }

    platform::ReadOnlyFile in;
    if (!in.Open(path)) {
        if (error) *error = "无法打开文件";
        return false;
    }
    bool hasPic = false;
    uint64_t audio = 0;
    if (!ScanBlocks(in, &hasPic, &audio)) {
        if (error) *error = "不是 FLAC";
        return false;
    }
    if (hasPic) {
        if (error) *error = "已有封面";
        return false;
    }

    // PICTURE 块体
    std::vector<uint8_t> body;
    body.reserve(32 + mimeStr.size() + jpeg.size());
    const auto put32 = [&](uint32_t v) {
        body.push_back(static_cast<uint8_t>(v));
        body.push_back(static_cast<uint8_t>(v >> 8));
        body.push_back(static_cast<uint8_t>(v >> 16));
        body.push_back(static_cast<uint8_t>(v >> 24));
    };
    const auto putBE32 = [&](uint32_t v) {
        body.push_back(static_cast<uint8_t>(v >> 24));
        body.push_back(static_cast<uint8_t>(v >> 16));
        body.push_back(static_cast<uint8_t>(v >> 8));
        body.push_back(static_cast<uint8_t>(v));
    };
    putBE32(3);                                        // picture type: Cover (front)
    putBE32(static_cast<uint32_t>(mimeStr.size()));    // MIME 长度
    body.insert(body.end(), mimeStr.begin(), mimeStr.end());
    putBE32(0);                                        // 描述长度
    putBE32(static_cast<uint32_t>(width));             // 宽 (未知填 0)
    putBE32(static_cast<uint32_t>(height));            // 高
    putBE32(24);                                       // 色深
    putBE32(0);                                        // 颜色数 (非调色板填 0)
    putBE32(static_cast<uint32_t>(jpeg.size()));
    body.insert(body.end(), jpeg.begin(), jpeg.end());

    const std::wstring tmp = path + L".cover.tmp";
    platform::WritableFile out;
    if (!out.Create(tmp)) {
        if (error) *error = "无法创建临时文件";
        return false;
    }

    bool ok = true;
    static const uint8_t kMagic[4] = {'f', 'L', 'a', 'C'};
    ok = ok && out.Write(kMagic, 4);

    // 原有块原样复制,但把 last 位清掉(新的 PICTURE 才是最后一块)
    uint64_t pos = 4;
    for (int guard = 0; guard < 1024 && ok; ++guard) {
        BlockHead h;
        if (!ReadHead(in, pos, h)) break;
        std::vector<uint8_t> blk(4 + h.len);
        if (in.ReadAt(pos, blk.data(), blk.size()) != blk.size()) { ok = false; break; }
        blk[0] = h.type; // 清 last
        ok = ok && out.Write(blk.data(), blk.size());
        pos += 4 + h.len;
        if (h.last) break;
    }
    // 新 PICTURE 块(last=1)
    if (ok && body.size() <= 0xFFFFFFu) {
        uint8_t hd[4] = {static_cast<uint8_t>(0x80 | 6), static_cast<uint8_t>(body.size() >> 16),
                         static_cast<uint8_t>(body.size() >> 8), static_cast<uint8_t>(body.size())};
        ok = out.Write(hd, 4) && out.Write(body.data(), body.size());
    } else {
        ok = false;
    }
    // 音频数据
    if (ok) {
        std::vector<uint8_t> buf(1 << 20);
        uint64_t off = audio;
        const uint64_t size = in.Size();
        while (off < size && ok) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - off));
            const size_t got = in.ReadAt(off, buf.data(), want);
            if (got == 0) { ok = false; break; }
            ok = out.Write(buf.data(), got);
            off += got;
        }
    }
    out.Close();
    in.Close();

    if (!ok) {
        platform::RemoveFile(tmp);
        if (error) *error = "写入失败";
        return false;
    }
    if (!platform::AtomicReplaceFile(tmp, path)) {
        platform::RemoveFile(tmp);
        if (error) *error = "替换失败";
        return false;
    }
    return true;
}

// ===========================================================================
//  四、轻量 JSON 取值 (够用即可:只处理本模块遇到的两种响应)
// ===========================================================================
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

// 歌名规范化:去掉括号内容(半角/全角)、空白与 ASCII 标点,ASCII 转小写。
// 用于「严格匹配失败时按歌名认同一首歌」—— 「プレイ (PLAY)」应等于「プレイ」。
// 多字节字符(中文/日文)原样保留。
std::string NormalizeTitle(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    int depth = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '(' || c == '[' || c == '<') { ++depth; continue; }
        if (c == ')' || c == ']' || c == '>') { if (depth) --depth; continue; }
        if (depth) continue;
        // 全角括号 (UTF-8: EF BC 88 / EF BC 89 / E3 80 90 / E3 80 91)
        if (c == 0xEF && i + 2 < s.size()) {
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
            if (b1 == 0xBC && (b2 == 0x88 || b2 == 0x89)) { // （ ）
                depth += (b2 == 0x88) ? 1 : -1;
                if (depth < 0) depth = 0;
                i += 2;
                continue;
            }
        }
        if (c == 0xE3 && i + 2 < s.size() &&
            static_cast<unsigned char>(s[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(s[i + 2]) == 0x90 || static_cast<unsigned char>(s[i + 2]) == 0x91)) {
            depth += (static_cast<unsigned char>(s[i + 2]) == 0x90) ? 1 : -1;
            if (depth < 0) depth = 0;
            i += 2;
            continue;
        }
        if (c < 0x80) {
            if (c == ' ' || c == '\t' || c == '-' || c == '_' || c == '.' || c == ',' || c == '!' ||
                c == '?' || c == 0x27 /* ' */ || c == 0x22 /* " */ || c == '/' || c == ':' || c == ';' ||
                c == '&')
                continue;
            out.push_back(static_cast<char>(::tolower(c)));
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

// 找 "key": 之后的值起点(跳过空白);返回 npos 表示没找到
size_t ValueStart(const std::string& js, const std::string& key, size_t from) {
    const std::string pat = "\"" + key + "\"";
    size_t p = js.find(pat, from);
    while (p != std::string::npos) {
        size_t q = p + pat.size();
        while (q < js.size() && (js[q] == ' ' || js[q] == '\t')) ++q;
        if (q < js.size() && js[q] == ':') {
            ++q;
            while (q < js.size() && (js[q] == ' ' || js[q] == '\t')) ++q;
            return q;
        }
        p = js.find(pat, p + 1);
    }
    return std::string::npos;
}

} // namespace

bool JsonGetString(const std::string& json, const std::string& key, std::string& out, size_t from) {
    size_t p = ValueStart(json, key, from);
    if (p == std::string::npos || p >= json.size() || json[p] != '"') return false;
    ++p;
    std::string s;
    while (p < json.size() && json[p] != '"') {
        if (json[p] == '\\' && p + 1 < json.size()) {
            ++p;
            switch (json[p]) {
                case 'n': s.push_back('\n'); break;
                case 't': s.push_back('\t'); break;
                case 'u': { // \uXXXX → UTF-8
                    if (p + 4 < json.size()) {
                        unsigned cp = 0;
                        for (int i = 1; i <= 4; ++i) {
                            const char c = json[p + i];
                            cp <<= 4;
                            if (c >= '0' && c <= '9') cp |= static_cast<unsigned>(c - '0');
                            else if (c >= 'a' && c <= 'f') cp |= static_cast<unsigned>(c - 'a' + 10);
                            else if (c >= 'A' && c <= 'F') cp |= static_cast<unsigned>(c - 'A' + 10);
                        }
                        p += 4;
                        if (cp < 0x80) s.push_back(static_cast<char>(cp));
                        else if (cp < 0x800) {
                            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        } else {
                            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                        }
                    }
                    break;
                }
                default: s.push_back(json[p]); break;
            }
        } else {
            s.push_back(json[p]);
        }
        ++p;
    }
    out = std::move(s);
    return true;
}

bool JsonGetInt(const std::string& json, const std::string& key, long long& out, size_t from) {
    size_t p = ValueStart(json, key, from);
    if (p == std::string::npos || p >= json.size()) return false;
    if (json[p] == '"') ++p; // 有些接口把数字写成字符串
    if (p >= json.size() || !(json[p] == '-' || (json[p] >= '0' && json[p] <= '9'))) return false;
    long long v = 0;
    bool neg = false;
    if (json[p] == '-') { neg = true; ++p; }
    while (p < json.size() && json[p] >= '0' && json[p] <= '9') v = v * 10 + (json[p++] - '0');
    out = neg ? -v : v;
    return true;
}

const char* SniffImage(const std::string& d) {
    const size_t n = d.size();
    if (n >= 3 && static_cast<uint8_t>(d[0]) == 0xFF && static_cast<uint8_t>(d[1]) == 0xD8) return "image/jpeg";
    if (n >= 8 && static_cast<uint8_t>(d[0]) == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return "image/png";
    if (n >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F' && d[8] == 'W' && d[9] == 'E' &&
        d[10] == 'B' && d[11] == 'P')
        return "image/webp";
    if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') return "image/gif";
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return "image/bmp";
    return nullptr;
}

std::string HexHead(const std::string& d) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    const size_t n = std::min<size_t>(8, d.size());
    for (size_t i = 0; i < n; ++i) {
        const uint8_t v = static_cast<uint8_t>(d[i]);
        out.push_back(kHex[v >> 4]);
        out.push_back(kHex[v & 15]);
        out.push_back(' ');
    }
    return out;
}

std::string UrlEncode(const std::string& utf8) {
    static const char* kHex = "0123456789ABCDEF";
    std::string s;
    s.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            s.push_back(static_cast<char>(c));
        } else {
            s.push_back('%');
            s.push_back(kHex[c >> 4]);
            s.push_back(kHex[c & 15]);
        }
    }
    return s;
}

// ===========================================================================
//  五、在线取封面
// ===========================================================================
namespace {

const char kUa[] = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) KugouPlayer/1.0";
const size_t kMaxCover = 8u << 20; // 8MB 上限,超过视为异常

// 酷狗:搜歌 → sqhash/sqfilesize 校验 → album_id → imgurl → 下载
// 用一组关键词依次搜索,任何一次命中就返回 album_id。
// 为什么要多关键词:单曲版本差异(「Luv(Sic)」只搜到「Luv(sic) Grand Finale」)、
// 罗马字与原文差异(「Sakurairo Diary」vs「桜色ダイアリー」)—— 换关键词常能命中。
std::string SearchAlbumId(const FlacTags& tags, const std::wstring& probe, bool* fuzzyOut, std::string* lastErr) {
    std::vector<std::string> keywords;
    if (!tags.artist.empty() && !tags.title.empty()) keywords.push_back(tags.artist + " " + tags.title);
    if (!tags.title.empty()) keywords.push_back(tags.title);            // 只搜标题:版本歧义时更准
    // 备用标题(来自文件名):标签是罗马字时,原文只在文件名里
    if (!tags.alt_title.empty() && tags.alt_title != tags.title) keywords.push_back(tags.alt_title);
    if (!tags.album.empty() && tags.album != tags.title) keywords.push_back(tags.album); // 合辑/自建专辑

    long long fsize = 0;
    if (!probe.empty()) {
        uint64_t sz = 0, mt = 0;
        if (platform::GetFileInfo(probe, sz, mt)) fsize = static_cast<long long>(sz);
    }
    std::string md5;
    bool md5Done = false;
    // 可接受的目标标题:标签标题 + 文件名标题(罗马字 / 原文 各一份),任一命中即可。
    // 只认标签标题的话,「Sakurairo Diary」永远等不到官方「桜色ダイアリー」。
    std::vector<std::string> wants;
    auto addWant = [&wants](const std::string& t) {
        const std::string n = NormalizeTitle(t);
        if (n.size() >= 2 && std::find(wants.begin(), wants.end(), n) == wants.end()) wants.push_back(n);
    };
    addWant(tags.title);
    addWant(tags.alt_title);

    for (const std::string& kwRaw : keywords) {
        std::string body;
        if (!Http("http://mobilecdn.kugou.com/api/v3/search/song?format=json&keyword=" + UrlEncode(kwRaw) +
                      "&page=1&pagesize=10&showtype=1",
                  body)) {
            if (lastErr) *lastErr = "搜索请求失败";
            continue;
        }
        // 把候选切成小段 (每段含一个 hash / songname / album_id ...)
        std::vector<std::string> chunks;
        for (size_t p = body.find("\"hash\""); p != std::string::npos && chunks.size() < 10;) {
            const size_t nxt = body.find("\"hash\"", p + 6);
            chunks.push_back(body.substr(p, (nxt == std::string::npos ? body.size() : nxt) - p));
            p = nxt;
        }

        // 一阶段:文件大小精确匹配(同一份文件必然同大小,零额外计算)
        for (const auto& chunk : chunks) {
            long long sqsz = 0;
            if (fsize <= 0 || !JsonGetInt(chunk, "sqfilesize", sqsz) || sqsz != fsize) continue;
            std::string id;
            JsonGetString(chunk, "album_id", id);
            if (!id.empty()) return id;
        }
        // 二阶段:MD5 对 sqhash(酷狗的无损 hash 就是整文件 MD5)
        if (!probe.empty()) {
            if (!md5Done) {
                md5 = Md5File(probe);
                md5Done = true;
            }
            if (!md5.empty()) {
                for (const auto& chunk : chunks) {
                    std::string sq;
                    if (!JsonGetString(chunk, "sqhash", sq) || Lower(sq) != md5) continue;
                    std::string id;
                    JsonGetString(chunk, "album_id", id);
                    if (!id.empty()) return id;
                }
            }
        }
        // 三阶段:歌名规范化全等(版本不同但封面通常一致)
        for (const auto& chunk : chunks) {
            std::string sn;
            if (!JsonGetString(chunk, "songname", sn)) continue;
            const std::string a = NormalizeTitle(sn);
            if (a.empty()) continue;
            if (std::find(wants.begin(), wants.end(), a) == wants.end()) continue;
            std::string id;
            JsonGetString(chunk, "album_id", id);
            if (!id.empty()) {
                if (fuzzyOut) *fuzzyOut = true;
                return id;
            }
        }
        // 四阶段:前缀匹配。「桜色ダイアリー(樱色日记)」多一个译名后缀,
        // 与官方「桜色ダイアリー」只差尾巴;短的一方不短于 3 字符且占比过半才认,避免误伤。
        for (const auto& chunk : chunks) {
            std::string sn;
            if (!JsonGetString(chunk, "songname", sn)) continue;
            const std::string a = NormalizeTitle(sn);
            if (a.size() < 3) continue;
            bool hit = false;
            for (const std::string& w : wants) {
                if (w.size() < 3) continue;
                const bool aPre = (w.size() > a.size() && w.compare(0, a.size(), a) == 0);
                const bool wPre = (a.size() > w.size() && a.compare(0, w.size(), w) == 0);
                if (!aPre && !wPre) continue;
                const size_t shorter = std::min(a.size(), w.size());
                const size_t longer = std::max(a.size(), w.size());
                if (shorter * 2 < longer) continue;
                hit = true;
                break;
            }
            if (!hit) continue;
            std::string id;
            JsonGetString(chunk, "album_id", id);
            if (!id.empty()) {
                if (fuzzyOut) *fuzzyOut = true;
                return id;
            }
        }
        if (lastErr) *lastErr = "无匹配";
    }
    return {};
}

bool FetchKugou(const FlacTags& tags, const std::wstring& flac_path, CoverResult& out, std::string* err) {
    bool fuzzy = false;
    std::string sub;
    const std::string album_id = SearchAlbumId(tags, flac_path, &fuzzy, &sub);
    if (album_id.empty()) {
        if (err) *err = "kugou: " + (sub.empty() ? std::string("无结果") : sub);
        return false;
    }

    std::string body;
    if (!Http("http://mobilecdn.kugou.com/api/v3/album/info?format=json&albumid=" + album_id, body)) {
        if (err) *err = "kugou: 专辑信息失败";
        return false;
    }
    std::string img;
    if (!JsonGetString(body, "imgurl", img) || img.find("{size}") == std::string::npos) {
        if (err) *err = "kugou: 无 imgurl";
        return false;
    }
    const size_t at = img.find("{size}");
    img.replace(at, 6, "1080");
    if (!Http(img, body)) {
        if (err) *err = "kugou: 图片 HTTP 失败";
        return false;
    }
    const char* fmt = SniffImage(body);
    if (!fmt) {
        if (err) *err = "kugou: 图片格式不认识 " + HexHead(body);
        return false;
    }
    if (body.size() > kMaxCover) {
        if (err) *err = "kugou: 图片过大";
        return false;
    }
    out.jpeg = std::move(body);
    out.mime = fmt;
    out.source = "kugou";
    out.detail = "album_id=" + album_id + (fuzzy ? "/fuzzy" : "");
    return true;
}

// 网易云(回退):搜索 → 详情 → picUrl?param=1080y1080
bool FetchNetease(const FlacTags& tags, CoverResult& out, std::string* err) {
    const std::string kw = UrlEncode(tags.artist.empty() ? tags.title : tags.artist + " " + tags.title);
    std::string body;
    if (!Http("http://music.163.com/api/search/get?s=" + kw + "&type=1&limit=5", body)) {
        if (err) *err = "netease: 搜索失败";
        return false;
    }
    // 在候选里挑「歌名规范化全等」的那条 —— 直接取第一条会拿到单曲EP/翻唱的封面
    const std::string want = NormalizeTitle(tags.title);
    long long sid = 0;
    {
        const size_t songs = body.find("\"songs\"");
        if (songs == std::string::npos) {
            if (err) *err = "netease: 无结果";
            return false;
        }
        size_t p = songs;
        for (int i = 0; i < 10; ++i) {
            const size_t obj = body.find("\"name\"", p);
            if (obj == std::string::npos) break;
            size_t nxt = body.find("\"name\"", obj + 6);
            if (nxt == std::string::npos) nxt = body.size();
            const std::string chunk = body.substr(obj, nxt - obj);
            std::string nm;
            long long id2 = 0;
            if (JsonGetString(chunk, "name", nm) && JsonGetInt(chunk, "id", id2) && id2 > 0 &&
                !want.empty() && NormalizeTitle(nm) == want) {
                sid = id2;
                break;
            }
            p = nxt;
        }
    }
    if (sid <= 0) {
        if (err) *err = "netease: 无歌名匹配";
        return false;
    }
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "http://music.163.com/api/song/detail/?id=%lld&ids=%%5B%lld%%5D", sid, sid);
    if (!Http(buf, body)) {
        if (err) *err = "netease: 详情失败";
        return false;
    }
    std::string pic;
    if (!JsonGetString(body, "picUrl", pic) || pic.empty()) {
        if (err) *err = "netease: 无 picUrl";
        return false;
    }
    if (!Http(pic + "?param=1080y1080", body)) {
        if (err) *err = "netease: 图片 HTTP 失败";
        return false;
    }
    const char* nfmt = SniffImage(body);
    if (!nfmt) {
        if (err) *err = "netease: 图片格式不认识, 前8字节=" + HexHead(body);
        return false;
    }
    if (body.size() > kMaxCover) {
        if (err) *err = "netease: 图片过大";
        return false;
    }
    out.jpeg = std::move(body);
    out.mime = nfmt;
    out.source = "netease";
    out.detail = "song_id=" + std::to_string(sid);
    return true;
}

} // namespace

bool FetchOnline(const FlacTags& tags, const std::wstring& flac_path, CoverResult& out, std::string* error) {
    out = CoverResult{};
    if (tags.title.empty()) {
        if (error) *error = "无标题";
        return false;
    }
    std::string e1, e2;
    if (FetchKugou(tags, flac_path, out, &e1)) return true;
    if (FetchNetease(tags, out, &e2)) return true;
    if (error) *error = e1 + " | " + e2;
    return false;
}

bool EnsureCover(const std::wstring& flac_path, std::string* error, std::string* source) {
    if (!IsFlac(flac_path)) {
        if (error) *error = "不是 FLAC";
        return false;
    }
    if (FlacHasCover(flac_path)) {
        if (source) *source = "existing";
        return true;
    }
    FlacTags tags;
    if (!ReadFlacTags(flac_path, tags)) {
        if (error) *error = "无 TITLE 标签";
        return false;
    }
    CoverResult r;
    if (!FetchOnline(tags, flac_path, r, error)) return false;
    if (!EmbedFlacPicture(flac_path, r.jpeg, 0, 0, error)) return false;
    if (source) *source = r.source;
    return true;
}

} // namespace cover_core
