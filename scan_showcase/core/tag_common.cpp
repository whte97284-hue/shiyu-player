#include <cstdlib>
// 解析内核公共设施:字节源、带缓存的读取器、文本编解码、封面择优、Vorbis Comment
#include "tag_internal.hpp"
#include <algorithm>
#include <cwctype>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace tagcore {

// ===========================================================================
// MemorySource
// ===========================================================================
MemorySource::MemorySource(const uint8_t* head, size_t headSize, uint64_t logicalSize,
                           const uint8_t* tail, size_t tailSize)
    : head_(head), head_size_(headSize), logical_size_(std::max<uint64_t>(logicalSize, headSize)),
      tail_(tail), tail_size_(tail ? tailSize : 0) {
    if (tail_size_ > logical_size_) tail_size_ = static_cast<size_t>(logical_size_);
}

size_t MemorySource::ReadAt(uint64_t off, void* dst, size_t n) {
    if (off >= logical_size_ || n == 0) return 0;
    uint8_t* out = static_cast<uint8_t*>(dst);
    // 头部缓冲
    if (off < head_size_) {
        size_t avail = static_cast<size_t>(head_size_ - off);
        size_t take = std::min(avail, n);
        std::memcpy(out, head_ + off, take);
        if (take == n) return n;
        // 头尾缓冲可能相接
        size_t more = ReadAt(off + take, out + take, n - take);
        return take + more;
    }
    // 尾部缓冲
    if (tail_size_ > 0) {
        uint64_t tailStart = logical_size_ - tail_size_;
        if (off >= tailStart) {
            size_t avail = static_cast<size_t>(logical_size_ - off);
            size_t take = std::min(avail, n);
            std::memcpy(out, tail_ + (off - tailStart), take);
            return take;
        }
    }
    return 0;
}

// ===========================================================================
// Reader
// ===========================================================================
Reader::Reader(ByteSource& src) : src_(src), size_(src.Size()) {}

bool Reader::Fill(uint64_t off, size_t want) {
    size_t len = static_cast<size_t>(std::min<uint64_t>(std::max(want, kWindow), size_ - off));
    if (win_.size() < len) win_.resize(len);
    size_t got = src_.ReadAt(off, win_.data(), len);
    ++io_calls;
    io_bytes += got;
    win_off_ = off;
    win_len_ = got;
    return got >= want;
}

size_t Reader::ReadSome(uint64_t off, void* dst, size_t n) {
    if (off >= size_ || n == 0) return 0;
    n = static_cast<size_t>(std::min<uint64_t>(n, size_ - off));
    if (src_.IsMemory()) return src_.ReadAt(off, dst, n);

    if (off >= win_off_ && off + n <= win_off_ + win_len_) {
        std::memcpy(dst, win_.data() + (off - win_off_), n);
        return n;
    }
    if (n > kWindow) {
        size_t got = src_.ReadAt(off, dst, n);
        ++io_calls;
        io_bytes += got;
        return got;
    }
    Fill(off, n);
    size_t avail = (off >= win_off_ && off < win_off_ + win_len_) ? static_cast<size_t>(win_off_ + win_len_ - off) : 0;
    size_t take = std::min(avail, n);
    if (take) std::memcpy(dst, win_.data() + (off - win_off_), take);
    return take;
}

bool Reader::Read(uint64_t off, void* dst, size_t n) {
    if (n == 0) return true;
    if (off > size_ || n > size_ - off) return false;
    return ReadSome(off, dst, n) == n;
}

bool Reader::ReadVec(uint64_t off, size_t n, std::vector<uint8_t>& out) {
    if (off > size_ || n > size_ - off) return false;
    out.resize(n);
    return Read(off, out.data(), n);
}

bool Reader::U8(uint64_t off, uint8_t& v) { return Read(off, &v, 1); }
bool Reader::U16BE(uint64_t off, uint16_t& v) {
    uint8_t b[2];
    if (!Read(off, b, 2)) return false;
    v = BE16(b);
    return true;
}
bool Reader::U16LE(uint64_t off, uint16_t& v) {
    uint8_t b[2];
    if (!Read(off, b, 2)) return false;
    v = LE16(b);
    return true;
}
bool Reader::U24BE(uint64_t off, uint32_t& v) {
    uint8_t b[3];
    if (!Read(off, b, 3)) return false;
    v = BE24(b);
    return true;
}
bool Reader::U32BE(uint64_t off, uint32_t& v) {
    uint8_t b[4];
    if (!Read(off, b, 4)) return false;
    v = BE32(b);
    return true;
}
bool Reader::U32LE(uint64_t off, uint32_t& v) {
    uint8_t b[4];
    if (!Read(off, b, 4)) return false;
    v = LE32(b);
    return true;
}
bool Reader::U64BE(uint64_t off, uint64_t& v) {
    uint8_t b[8];
    if (!Read(off, b, 8)) return false;
    v = BE64(b);
    return true;
}
bool Reader::U64LE(uint64_t off, uint64_t& v) {
    uint8_t b[8];
    if (!Read(off, b, 8)) return false;
    v = LE64(b);
    return true;
}
bool Reader::Match(uint64_t off, const char* magic, size_t n) {
    uint8_t b[16];
    if (n > sizeof(b) || !Read(off, b, n)) return false;
    return std::memcmp(b, magic, n) == 0;
}

// ===========================================================================
// 文本编解码
// ===========================================================================
void AppendCodepoint(std::wstring& out, uint32_t cp) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
    if (sizeof(wchar_t) == 2 && cp >= 0x10000) {
        cp -= 0x10000;
        out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
        out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
    } else {
        out.push_back(static_cast<wchar_t>(cp));
    }
}

// 解码一个 UTF-8 序列,返回长度 (非法返回 0)
static size_t Utf8Seq(const uint8_t* p, size_t n, uint32_t& cp) {
    uint8_t c = p[0];
    if (c < 0x80) { cp = c; return 1; }
    size_t len;
    uint32_t minCp;
    if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; minCp = 0x80; }
    else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; minCp = 0x800; }
    else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; minCp = 0x10000; }
    else return 0;
    if (len > n) return 0;
    for (size_t i = 1; i < len; ++i) {
        if ((p[i] & 0xC0) != 0x80) return 0;
        cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (cp < minCp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
    return len;
}

bool IsValidUtf8(const uint8_t* p, size_t n) {
    size_t i = 0;
    while (i < n) {
        if (p[i] < 0x80) { ++i; continue; }
        uint32_t cp;
        size_t l = Utf8Seq(p + i, n - i, cp);
        if (!l) return false;
        i += l;
    }
    return true;
}

bool IsAscii(const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (p[i] >= 0x80) return false;
    return true;
}

std::wstring DecodeUtf8(const uint8_t* p, size_t n) {
    std::wstring out;
    out.reserve(n);
    size_t i = 0;
    // 跳过 BOM
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) i = 3;
    while (i < n) {
        uint32_t cp;
        size_t l = Utf8Seq(p + i, n - i, cp);
        if (!l) { AppendCodepoint(out, 0xFFFD); ++i; continue; }
        AppendCodepoint(out, cp);
        i += l;
    }
    return out;
}

std::wstring DecodeUtf16(const uint8_t* p, size_t n, bool bigEndian) {
    std::wstring out;
    size_t i = 0;
    if (n >= 2) {
        if (p[0] == 0xFF && p[1] == 0xFE) { bigEndian = false; i = 2; }
        else if (p[0] == 0xFE && p[1] == 0xFF) { bigEndian = true; i = 2; }
    }
    out.reserve(n / 2);
    while (i + 1 < n) {
        uint32_t u = bigEndian ? BE16(p + i) : LE16(p + i);
        i += 2;
        if (u == 0) break;
        if (u >= 0xD800 && u <= 0xDBFF && i + 1 < n) {
            uint32_t lo = bigEndian ? BE16(p + i) : LE16(p + i);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                i += 2;
                AppendCodepoint(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                continue;
            }
        }
        AppendCodepoint(out, u);
    }
    return out;
}

std::wstring DecodeLatin1(const uint8_t* p, size_t n) {
    std::wstring out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out.push_back(static_cast<wchar_t>(p[i]));
    return out;
}

std::wstring DecodeCodePage(const uint8_t* p, size_t n, unsigned int codePage) {
    if (n == 0) return L"";
    if (codePage == 65001) return DecodeUtf8(p, n);
    if (codePage == 28591 || codePage == 0) return DecodeLatin1(p, n);
#ifdef _WIN32
    int req = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(p), static_cast<int>(n), NULL, 0);
    if (req <= 0) return L"";
    std::wstring w(static_cast<size_t>(req), L'\0');
    MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, reinterpret_cast<const char*>(p), static_cast<int>(n), &w[0], req);
    return w;
#else
    const char* name = nullptr;
    switch (codePage) {
        case 936: case 54936: name = "GB18030"; break;
        case 932: name = "CP932"; break;
        case 950: name = "BIG5"; break;
        case 949: name = "CP949"; break;
        case 1252: name = "CP1252"; break;
        default: return L"";
    }
    iconv_t cd = iconv_open("UTF-32LE", name);
    if (cd == reinterpret_cast<iconv_t>(-1)) return L"";
    std::vector<char> outBuf(n * 4 + 16);
    char* inPtr = const_cast<char*>(reinterpret_cast<const char*>(p));
    size_t inLeft = n;
    char* outPtr = outBuf.data();
    size_t outLeft = outBuf.size();
    size_t rc = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
    iconv_close(cd);
    if (rc == static_cast<size_t>(-1)) return L"";
    std::wstring w;
    size_t produced = outBuf.size() - outLeft;
    for (size_t i = 0; i + 3 < produced; i += 4) {
        AppendCodepoint(w, LE32(reinterpret_cast<const uint8_t*>(outBuf.data()) + i));
    }
    return w;
#endif
}

// 解出来的字符串里有多少"不可打印"字符。
// 编码猜错时最常见的表现就是解出一堆 U+0080~U+009F 控制字符(字体画不出来,界面里显示成 ?)
static int BadCharCount(const std::wstring& s) {
    int n = 0;
    for (wchar_t c : s) {
        if ((c < 0x20 && c != L'\t') || (c >= 0x7F && c < 0xA0)) ++n;
    }
    return n;
}

std::wstring DecodeLegacy(const uint8_t* p, size_t n, unsigned int codePage) {
    while (n > 0 && p[n - 1] == 0) --n; // 去尾部 NUL
    if (n == 0) return L"";
    if (IsAscii(p, n)) return DecodeLatin1(p, n);
    if (IsValidUtf8(p, n)) return DecodeUtf8(p, n);

    // 先用调用方指定的代码页(默认 936 GBK)。
    // 但日文 ID3v2.3 经常把 Shift-JIS 的内容标成 encoding=0(Latin-1),
    // 拿 GBK 去解会撞上非法字节 —— MultiByteToWideChar 带 MB_ERR_INVALID_CHARS 时
    // 整串直接失败,以前就回退到 Latin-1,解出一堆控制字符,界面上全变成 "?"。
    // 所以这里失败(或解出脏字符)就换几种常见东亚编码重试,取最干净的那个。
    std::wstring w = DecodeCodePage(p, n, codePage);
    if (!w.empty() && BadCharCount(w) == 0) return w;

    std::wstring fallback = w;
    for (unsigned int cp : {932u, 936u, 950u, 949u}) {
        if (cp == codePage) continue;
        std::wstring t = DecodeCodePage(p, n, cp);
        if (t.empty()) continue;
        if (BadCharCount(t) == 0) return t;   // 干净就直接用
        if (fallback.empty()) fallback = t;
    }
    if (!fallback.empty()) return fallback;
    return DecodeLatin1(p, n);
}

std::wstring TrimTag(const std::wstring& s) {
    size_t b = 0, e = s.size();
    auto junk = [](wchar_t ch) { return ch == 0 || ch == 0xFEFF || std::iswspace(static_cast<wint_t>(ch)); };
    while (b < e && junk(s[b])) ++b;
    while (e > b && junk(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// ===========================================================================
// 封面
// ===========================================================================
std::string SniffImageMime(const uint8_t* p, size_t n) {
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "image/jpeg";
    if (n >= 8 && std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0) return "image/png";
    if (n >= 6 && (std::memcmp(p, "GIF87a", 6) == 0 || std::memcmp(p, "GIF89a", 6) == 0)) return "image/gif";
    if (n >= 2 && p[0] == 'B' && p[1] == 'M') return "image/bmp";
    if (n >= 12 && std::memcmp(p, "RIFF", 4) == 0 && std::memcmp(p + 8, "WEBP", 4) == 0) return "image/webp";
    return "";
}

void ProbeImageSize(const uint8_t* p, size_t n, uint32_t& w, uint32_t& h) {
    if (n >= 24 && std::memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0 && std::memcmp(p + 12, "IHDR", 4) == 0) {
        w = BE32(p + 16);
        h = BE32(p + 20);
        return;
    }
    if (n >= 4 && p[0] == 0xFF && p[1] == 0xD8) {
        size_t i = 2;
        while (i + 4 <= n) {
            if (p[i] != 0xFF) { ++i; continue; }
            uint8_t marker = p[i + 1];
            if (marker == 0xFF) { ++i; continue; }
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { i += 2; continue; }
            if (marker == 0xD9 || marker == 0xDA) return;
            uint16_t segLen = BE16(p + i + 2);
            bool isSof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
            if (isSof && i + 9 <= n) {
                h = BE16(p + i + 5);
                w = BE16(p + i + 7);
                return;
            }
            if (segLen < 2) return;
            i += 2 + segLen;
        }
    }
}

bool OfferCover(Ctx& c, uint8_t pictureType, const std::string& mime, uint64_t offset, uint64_t size,
                bool offsetReliable, const uint8_t* inlineData) {
    if (size == 0) return false;
    AudioMetadata& m = c.m;
    bool better = !m.has_cover || (m.cover_picture_type != 3 && pictureType == 3);
    if (!better) return false;

    m.has_cover = true;
    m.cover_picture_type = pictureType;
    m.cover_mime_type = mime;
    m.cover_offset = offsetReliable ? offset : 0;
    m.cover_size = static_cast<uint32_t>(std::min<uint64_t>(size, 0xFFFFFFFFull));
    m.cover_data.clear();
    m.cover_width = 0;
    m.cover_height = 0;

    if (c.opt.load_cover && size <= c.opt.max_cover_bytes) {
        if (inlineData) {
            m.cover_data.assign(inlineData, inlineData + size);
        } else if (offsetReliable) {
            if (!c.r.ReadVec(offset, static_cast<size_t>(size), m.cover_data)) m.cover_data.clear();
        }
    }
    const uint8_t* probe = !m.cover_data.empty() ? m.cover_data.data() : inlineData;
    size_t probeLen = !m.cover_data.empty() ? m.cover_data.size() : (inlineData ? static_cast<size_t>(size) : 0);
    if (probe && probeLen) {
        std::string sniffed = SniffImageMime(probe, probeLen);
        if (!sniffed.empty()) m.cover_mime_type = sniffed;
        ProbeImageSize(probe, probeLen, m.cover_width, m.cover_height);
    }
    if (m.cover_mime_type.empty() || m.cover_mime_type == "image/jpg" || m.cover_mime_type == "JPG")
        m.cover_mime_type = "image/jpeg";
    else if (m.cover_mime_type == "PNG")
        m.cover_mime_type = "image/png";
    return true;
}

std::vector<uint8_t> Base64Decode(const uint8_t* p, size_t n) {
    static int8_t T[256];
    static bool init = false;
    if (!init) {
        std::memset(T, -1, sizeof(T));
        const char* b = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) T[static_cast<uint8_t>(b[i])] = static_cast<int8_t>(i);
        T[static_cast<uint8_t>('-')] = 62; // base64url 容错
        T[static_cast<uint8_t>('_')] = 63;
        init = true;
    }
    std::vector<uint8_t> out;
    out.reserve(n * 3 / 4);
    uint32_t val = 0;
    int bits = -8;
    for (size_t i = 0; i < n; ++i) {
        uint8_t ch = p[i];
        if (ch == '=') break;
        int8_t d = T[ch];
        if (d < 0) continue; // 跳过换行等空白
        val = (val << 6) | static_cast<uint32_t>(d);
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    return out;
}

bool ParseFlacPictureStruct(Ctx& c, const uint8_t* p, size_t n, uint64_t fileOffset, bool offsetReliable) {
    if (n < 32) return false;
    size_t o = 0;
    uint32_t type = BE32(p + o); o += 4;
    uint32_t mimeLen = BE32(p + o); o += 4;
    if (mimeLen > n - o) return false;
    std::string mime(reinterpret_cast<const char*>(p + o), mimeLen); o += mimeLen;
    if (n - o < 4) return false;
    uint32_t descLen = BE32(p + o); o += 4;
    if (descLen > n - o) return false;
    o += descLen;
    if (n - o < 20) return false;
    uint32_t w = BE32(p + o), h = BE32(p + o + 4);
    uint32_t dataLen = BE32(p + o + 16);
    o += 20;
    bool inBuf = dataLen <= n - o;
    if (dataLen == 0) return false;
    uint8_t pt = static_cast<uint8_t>(std::min<uint32_t>(type, 255));
    bool accepted = OfferCover(c, pt, mime, fileOffset + o, dataLen, offsetReliable, inBuf ? p + o : nullptr);
    if (accepted && c.m.cover_width == 0 && w && h) {
        c.m.cover_width = w;
        c.m.cover_height = h;
    }
    return true;
}

// ===========================================================================
// Vorbis Comment
// ===========================================================================
static bool KeyEq(const uint8_t* k, size_t kl, const char* name) {
    size_t nl = std::strlen(name);
    if (kl != nl) return false;
    for (size_t i = 0; i < kl; ++i) {
        uint8_t a = k[i];
        if (a >= 'a' && a <= 'z') a = static_cast<uint8_t>(a - 32);
        if (a != static_cast<uint8_t>(name[i])) return false;
    }
    return true;
}

bool ApplyExtraTag(Ctx& c, const std::string& key, const std::wstring& value) {
    std::string k;
    for (char ch : key) k += static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
    AudioMetadata& m = c.m;
    if (k == "LYRICS" || k == "UNSYNCEDLYRICS" || k == "UNSYNCED LYRICS") {
        if (c.opt.load_lyrics && m.lyrics.empty()) m.lyrics = value;
        return true;
    }
    if (k.compare(0, 11, "REPLAYGAIN_") != 0) return false;
    // "-6.54 dB" / "+1.2 dB" / "0.988"
    std::string num;
    for (wchar_t ch : value) {
        if ((ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L'-' || ch == L'+') num += static_cast<char>(ch);
        else if (!num.empty()) break;
    }
    if (num.empty()) return true;
    char* endp = nullptr;
    float v = std::strtof(num.c_str(), &endp);
    if (endp == num.c_str() || !(v == v)) return true;
    if (k == "REPLAYGAIN_TRACK_GAIN") { m.rg_track_gain = v; m.has_rg_track = true; }
    else if (k == "REPLAYGAIN_TRACK_PEAK") m.rg_track_peak = v;
    else if (k == "REPLAYGAIN_ALBUM_GAIN") { m.rg_album_gain = v; m.has_rg_album = true; }
    else if (k == "REPLAYGAIN_ALBUM_PEAK") m.rg_album_peak = v;
    return true;
}

void ApplyVorbisComments(Ctx& c, const uint8_t* p, size_t n) {
    if (n < 8) return;
    uint32_t vendorLen = LE32(p);
    if (vendorLen > n - 4) return;
    size_t o = 4 + vendorLen;
    if (n - o < 4) return;
    uint32_t count = LE32(p + o);
    o += 4;

    std::vector<std::wstring> artists, albumArtists;
    std::wstring title, album, genre, date, year, track, disc, comment, description;
    const uint8_t* legacyCover = nullptr;
    size_t legacyCoverLen = 0;

    for (uint32_t i = 0; i < count && n - o >= 4; ++i) {
        uint32_t len = LE32(p + o);
        o += 4;
        if (len > n - o) break;
        const uint8_t* e = p + o;
        o += len;
        const uint8_t* eq = static_cast<const uint8_t*>(std::memchr(e, '=', len));
        if (!eq) continue;
        size_t kl = static_cast<size_t>(eq - e);
        const uint8_t* v = eq + 1;
        size_t vl = len - kl - 1;

        if (KeyEq(e, kl, "METADATA_BLOCK_PICTURE")) {
            if (c.opt.load_cover) {
                std::vector<uint8_t> pic = Base64Decode(v, vl);
                ParseFlacPictureStruct(c, pic.data(), pic.size(), 0, false);
            } else {
                // 惰性模式:只解码前缀以获得图片类型与 MIME,不解码整张图片
                std::vector<uint8_t> head = Base64Decode(v, std::min<size_t>(vl, 4096));
                if (head.size() >= 8) {
                    uint32_t type = BE32(head.data());
                    uint32_t mimeLen = BE32(head.data() + 4);
                    std::string mime;
                    if (mimeLen <= head.size() - 8) mime.assign(reinterpret_cast<const char*>(head.data() + 8), mimeLen);
                    OfferCover(c, static_cast<uint8_t>(std::min<uint32_t>(type, 255)), mime, 0, vl * 3 / 4, false, nullptr);
                }
            }
            continue;
        }
        if (KeyEq(e, kl, "COVERART")) {
            legacyCover = v;
            legacyCoverLen = vl;
            continue;
        }

        std::wstring val = TrimTag(DecodeUtf8(v, vl));
        if (val.empty()) continue;
        if (ApplyExtraTag(c, std::string(reinterpret_cast<const char*>(e), kl), val)) continue;
        if (KeyEq(e, kl, "TITLE")) { if (title.empty()) title = val; }
        else if (KeyEq(e, kl, "ARTIST")) artists.push_back(val);
        else if (KeyEq(e, kl, "ALBUM")) { if (album.empty()) album = val; }
        else if (KeyEq(e, kl, "ALBUMARTIST") || KeyEq(e, kl, "ALBUM ARTIST") || KeyEq(e, kl, "ALBUM_ARTIST"))
            albumArtists.push_back(val);
        else if (KeyEq(e, kl, "GENRE")) { if (genre.empty()) genre = val; }
        else if (KeyEq(e, kl, "DATE")) { if (date.empty()) date = val; }
        else if (KeyEq(e, kl, "YEAR")) { if (year.empty()) year = val; }
        else if (KeyEq(e, kl, "TRACKNUMBER")) { if (track.empty()) track = val; }
        else if (KeyEq(e, kl, "DISCNUMBER")) { if (disc.empty()) disc = val; }
        else if (KeyEq(e, kl, "COMMENT")) { if (comment.empty()) comment = val; }
        else if (KeyEq(e, kl, "DESCRIPTION")) { if (description.empty()) description = val; }
    }

    auto join = [](const std::vector<std::wstring>& v) {
        std::wstring s;
        for (size_t i = 0; i < v.size(); ++i) {
            if (i) s += L"/";
            s += v[i];
        }
        return s;
    };
    AudioMetadata& m = c.m;
    SetIfEmpty(m.title, title);
    SetIfEmpty(m.artist, join(artists));
    SetIfEmpty(m.album, album);
    SetIfEmpty(m.album_artist, join(albumArtists));
    SetIfEmpty(m.genre, genre);
    SetIfEmpty(m.year, date.empty() ? year : date);
    auto beforeSlash = [](const std::wstring& v) {
        size_t sl = v.find(L'/');
        return TrimTag(sl == std::wstring::npos ? v : v.substr(0, sl));
    };
    SetIfEmpty(m.track_number, beforeSlash(track));
    SetIfEmpty(m.disc_number, beforeSlash(disc));
    SetIfEmpty(m.comment, comment.empty() ? description : comment);

    if (legacyCover && !m.has_cover) {
        if (c.opt.load_cover) {
            std::vector<uint8_t> img = Base64Decode(legacyCover, legacyCoverLen);
            OfferCover(c, 3, "", 0, img.size(), false, img.data());
        } else {
            OfferCover(c, 3, "", 0, legacyCoverLen * 3 / 4, false, nullptr);
        }
    }
}

// ===========================================================================
// ID3v1 流派表 (含 Winamp 扩展)
// ===========================================================================
static const wchar_t* const kGenres[] = {
    L"Blues", L"Classic Rock", L"Country", L"Dance", L"Disco", L"Funk", L"Grunge", L"Hip-Hop", L"Jazz",
    L"Metal", L"New Age", L"Oldies", L"Other", L"Pop", L"R&B", L"Rap", L"Reggae", L"Rock", L"Techno",
    L"Industrial", L"Alternative", L"Ska", L"Death Metal", L"Pranks", L"Soundtrack", L"Euro-Techno",
    L"Ambient", L"Trip-Hop", L"Vocal", L"Jazz+Funk", L"Fusion", L"Trance", L"Classical", L"Instrumental",
    L"Acid", L"House", L"Game", L"Sound Clip", L"Gospel", L"Noise", L"Alt. Rock", L"Bass", L"Soul", L"Punk",
    L"Space", L"Meditative", L"Instrumental Pop", L"Instrumental Rock", L"Ethnic", L"Gothic", L"Darkwave",
    L"Techno-Industrial", L"Electronic", L"Pop-Folk", L"Eurodance", L"Dream", L"Southern Rock", L"Comedy",
    L"Cult", L"Gangsta Rap", L"Top 40", L"Christian Rap", L"Pop/Funk", L"Jungle", L"Native American",
    L"Cabaret", L"New Wave", L"Psychedelic", L"Rave", L"Showtunes", L"Trailer", L"Lo-Fi", L"Tribal",
    L"Acid Punk", L"Acid Jazz", L"Polka", L"Retro", L"Musical", L"Rock & Roll", L"Hard Rock", L"Folk",
    L"Folk-Rock", L"National Folk", L"Swing", L"Fast-Fusion", L"Bebop", L"Latin", L"Revival", L"Celtic",
    L"Bluegrass", L"Avantgarde", L"Gothic Rock", L"Progressive Rock", L"Psychedelic Rock", L"Symphonic Rock",
    L"Slow Rock", L"Big Band", L"Chorus", L"Easy Listening", L"Acoustic", L"Humour", L"Speech", L"Chanson",
    L"Opera", L"Chamber Music", L"Sonata", L"Symphony", L"Booty Bass", L"Primus", L"Porn Groove", L"Satire",
    L"Slow Jam", L"Club", L"Tango", L"Samba", L"Folklore", L"Ballad", L"Power Ballad", L"Rhythmic Soul",
    L"Freestyle", L"Duet", L"Punk Rock", L"Drum Solo", L"A Cappella", L"Euro-House", L"Dance Hall", L"Goa",
    L"Drum & Bass", L"Club-House", L"Hardcore", L"Terror", L"Indie", L"BritPop", L"Afro-Punk", L"Polsk Punk",
    L"Beat", L"Christian Gangsta Rap", L"Heavy Metal", L"Black Metal", L"Crossover",
    L"Contemporary Christian", L"Christian Rock", L"Merengue", L"Salsa", L"Thrash Metal", L"Anime", L"JPop",
    L"Synthpop", L"Abstract", L"Art Rock", L"Baroque", L"Bhangra", L"Big Beat", L"Breakbeat", L"Chillout",
    L"Downtempo", L"Dub", L"EBM", L"Eclectic", L"Electro", L"Electroclash", L"Emo", L"Experimental",
    L"Garage", L"Global", L"IDM", L"Illbient", L"Industro-Goth", L"Jam Band", L"Krautrock", L"Leftfield",
    L"Lounge", L"Math Rock", L"New Romantic", L"Nu-Breakz", L"Post-Punk", L"Post-Rock", L"Psytrance",
    L"Shoegaze", L"Space Rock", L"Trop Rock", L"World Music", L"Neoclassical", L"Audiobook",
    L"Audio Theatre", L"Neue Deutsche Welle", L"Podcast", L"Indie Rock", L"G-Funk", L"Dubstep",
    L"Garage Rock", L"Psybient"
};

const wchar_t* Id3v1GenreName(unsigned idx) {
    if (idx < sizeof(kGenres) / sizeof(kGenres[0])) return kGenres[idx];
    return nullptr;
}

} // namespace tagcore
