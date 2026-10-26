// ID3v2.2/2.3/2.4 · ID3v1 · APEv2 标签与 MPEG Audio 帧流解析
#include "tag_internal.hpp"
#include <algorithm>

namespace tagcore {

// ===========================================================================
// ID3v2
// ===========================================================================
static bool IsFrameIdChar(uint8_t ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9'); }

static bool IsValidFrameId(const uint8_t* id, size_t len) {
    for (size_t i = 0; i < len; ++i)
        if (!IsFrameIdChar(id[i])) return false;
    return true;
}

uint64_t Id3v2TotalSize(Reader& r, uint64_t base) {
    uint8_t h[10];
    if (!r.Read(base, h, 10)) return 0;
    if (std::memcmp(h, "ID3", 3) != 0 || h[3] == 0xFF || h[4] == 0xFF) return 0;
    if ((h[6] | h[7] | h[8] | h[9]) & 0x80) return 0;
    uint64_t total = 10ull + SyncSafe32(h + 6);
    if (h[3] == 4 && (h[5] & 0x10)) total += 10; // footer
    return total;
}

// 去除反同步 (FF 00 -> FF)
static void Deunsync(std::vector<uint8_t>& v) {
    size_t w = 0;
    for (size_t i = 0; i < v.size(); ++i) {
        v[w++] = v[i];
        if (v[i] == 0xFF && i + 1 < v.size() && v[i + 1] == 0x00) ++i;
    }
    v.resize(w);
}

// 将 ID3 文本帧负载按编码拆分为多个字符串
static std::vector<std::wstring> DecodeId3Strings(uint8_t enc, const uint8_t* p, size_t n, unsigned codePage) {
    std::vector<std::wstring> out;
    bool wide = (enc == 1 || enc == 2);
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        if (wide) {
            while (j + 1 < n && !(p[j] == 0 && p[j + 1] == 0)) j += 2;
            if (j + 1 >= n) j = n;
        } else {
            while (j < n && p[j] != 0) ++j;
        }
        std::wstring s;
        size_t len = j - i;
        if (enc == 0) s = DecodeLegacy(p + i, len, codePage);
        else if (enc == 1) s = DecodeUtf16(p + i, len, false);
        else if (enc == 2) s = DecodeUtf16(p + i, len, true);
        else s = DecodeUtf8(p + i, len);
        s = TrimTag(s);
        if (!s.empty()) out.push_back(s);
        i = j + (wide ? 2 : 1);
    }
    return out;
}

// 跳过一个以编码对应终止符结尾的字符串,返回终止符之后的位置 (失败返回 n+1)
static size_t SkipTerminated(uint8_t enc, const uint8_t* p, size_t n, size_t i) {
    if (enc == 1 || enc == 2) {
        while (i + 1 < n) {
            if (p[i] == 0 && p[i + 1] == 0) return i + 2;
            i += 2;
        }
        return n + 1;
    }
    while (i < n) {
        if (p[i] == 0) return i + 1;
        ++i;
    }
    return n + 1;
}

static std::wstring JoinSlash(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += L"/";
        s += v[i];
    }
    return s;
}

// TCON:"(17)"、"17"、"(17)Rock"、"Rock"、"RX"、"CR"
static std::wstring ResolveGenre(const std::wstring& g) {
    std::wstring s = g;
    if (s == L"RX") return L"Remix";
    if (s == L"CR") return L"Cover";
    if (!s.empty() && s[0] == L'(') {
        size_t close = s.find(L')');
        if (close != std::wstring::npos) {
            std::wstring num = s.substr(1, close - 1);
            std::wstring rest = s.substr(close + 1);
            if (!rest.empty()) return TrimTag(rest);
            s = num;
        }
    }
    bool digits = !s.empty() && s.size() <= 3;
    for (wchar_t ch : s)
        if (ch < L'0' || ch > L'9') digits = false;
    if (digits) {
        const wchar_t* name = Id3v1GenreName(static_cast<unsigned>(std::stoi(s)));
        if (name) return name;
    }
    return s;
}

static std::wstring BeforeSlash(const std::wstring& s) {
    size_t p = s.find(L'/');
    return TrimTag(p == std::wstring::npos ? s : s.substr(0, p));
}

enum class Id3Field { None, Title, Artist, Album, AlbumArtist, Genre, Year, Track, Disc, Comment, Picture, PicV22, UserText, Lyrics };

static Id3Field MapFrame(const uint8_t* id, int ver) {
    if (ver == 2) {
        if (!std::memcmp(id, "TT2", 3)) return Id3Field::Title;
        if (!std::memcmp(id, "TP1", 3)) return Id3Field::Artist;
        if (!std::memcmp(id, "TAL", 3)) return Id3Field::Album;
        if (!std::memcmp(id, "TP2", 3)) return Id3Field::AlbumArtist;
        if (!std::memcmp(id, "TCO", 3)) return Id3Field::Genre;
        if (!std::memcmp(id, "TYE", 3)) return Id3Field::Year;
        if (!std::memcmp(id, "TRK", 3)) return Id3Field::Track;
        if (!std::memcmp(id, "TPA", 3)) return Id3Field::Disc;
        if (!std::memcmp(id, "COM", 3)) return Id3Field::Comment;
        if (!std::memcmp(id, "TXX", 3)) return Id3Field::UserText;
        if (!std::memcmp(id, "ULT", 3)) return Id3Field::Lyrics;
        if (!std::memcmp(id, "PIC", 3)) return Id3Field::PicV22;
        return Id3Field::None;
    }
    if (!std::memcmp(id, "TIT2", 4)) return Id3Field::Title;
    if (!std::memcmp(id, "TPE1", 4)) return Id3Field::Artist;
    if (!std::memcmp(id, "TALB", 4)) return Id3Field::Album;
    if (!std::memcmp(id, "TPE2", 4)) return Id3Field::AlbumArtist;
    if (!std::memcmp(id, "TCON", 4)) return Id3Field::Genre;
    if (!std::memcmp(id, "TYER", 4) || !std::memcmp(id, "TDRC", 4)) return Id3Field::Year;
    if (!std::memcmp(id, "TRCK", 4)) return Id3Field::Track;
    if (!std::memcmp(id, "TPOS", 4)) return Id3Field::Disc;
    if (!std::memcmp(id, "COMM", 4)) return Id3Field::Comment;
    if (!std::memcmp(id, "TXXX", 4)) return Id3Field::UserText;
    if (!std::memcmp(id, "USLT", 4)) return Id3Field::Lyrics;
    if (!std::memcmp(id, "APIC", 4)) return Id3Field::Picture;
    return Id3Field::None;
}

// 判断 pos 处是否像一个合法的帧头 / 填充 / 标签结尾 (用于 v2.4 帧长度容错)
static bool LooksLikeFrameBoundary(Reader& r, uint64_t pos, uint64_t end, int ver) {
    if (pos == end) return true;
    if (pos > end) return false;
    uint8_t id[4] = {0, 0, 0, 0};
    size_t idLen = (ver == 2) ? 3 : 4;
    if (end - pos < idLen) return true;
    if (!r.Read(pos, id, idLen)) return false;
    if (id[0] == 0) return true;
    return IsValidFrameId(id, idLen);
}

static void HandlePictureBody(Ctx& c, Id3Field kind, const uint8_t* p, size_t n, uint64_t fileOff, bool reliable,
                              uint64_t fullFrameSize) {
    if (n < 2) return;
    uint8_t enc = p[0];
    size_t i = 1;
    std::string mime;
    if (kind == Id3Field::PicV22) {
        if (n < 5) return;
        mime.assign(reinterpret_cast<const char*>(p + 1), 3);
        i = 4;
    } else {
        size_t z = i;
        while (z < n && p[z] != 0) ++z;
        if (z >= n) return;
        mime.assign(reinterpret_cast<const char*>(p + i), z - i);
        i = z + 1;
    }
    if (i >= n) return;
    uint8_t type = p[i++];
    i = SkipTerminated(enc, p, n, i);
    if (i > n || i > fullFrameSize) return;
    uint64_t dataLen = fullFrameSize - i;
    if (dataLen == 0) return;
    bool dataInBuf = (i + dataLen <= n);
    OfferCover(c, type, mime, fileOff + i, dataLen, reliable, dataInBuf ? p + i : nullptr);
}

static void HandleTextBody(Ctx& c, Id3Field kind, const uint8_t* p, size_t n) {
    if (n < 1) return;
    AudioMetadata& m = c.m;
    uint8_t enc = p[0];
    if (kind == Id3Field::Comment) {
        if (n < 5) return;
        size_t i = SkipTerminated(enc, p, n, 4);
        if (i > n) return;
        // 跳过 iTunes 内部注释 (iTunNORM / iTunSMPB 等)
        std::vector<std::wstring> desc = DecodeId3Strings(enc, p + 4, i - 4, c.opt.legacy_code_page);
        if (!desc.empty() && desc[0].compare(0, 4, L"iTun") == 0) return;
        std::vector<std::wstring> vals = DecodeId3Strings(enc, p + i, n - i, c.opt.legacy_code_page);
        if (!vals.empty()) SetIfEmpty(m.comment, vals[0]);
        return;
    }
    if (kind == Id3Field::Lyrics) {
        // USLT:编码(1) + 语言(3) + 描述 (以编码对应的终止符结束) + 歌词正文
        if (!c.opt.load_lyrics || n < 5 || !m.lyrics.empty()) return;
        size_t i = SkipTerminated(enc, p, n, 4);
        if (i > n) return;
        std::vector<std::wstring> vals = DecodeId3Strings(enc, p + i, n - i, c.opt.legacy_code_page);
        if (!vals.empty()) m.lyrics = vals[0];
        return;
    }
    std::vector<std::wstring> vals = DecodeId3Strings(enc, p + 1, n - 1, c.opt.legacy_code_page);
    if (vals.empty()) return;
    if (kind == Id3Field::UserText) {
        // TXXX:描述 + 值 (如 REPLAYGAIN_TRACK_GAIN / -6.54 dB)
        if (vals.size() >= 2) ApplyExtraTag(c, AudioTagParser::WideToUtf8(vals[0]), vals[1]);
        return;
    }
    switch (kind) {
        case Id3Field::Title: SetIfEmpty(m.title, vals[0]); break;
        case Id3Field::Artist: SetIfEmpty(m.artist, JoinSlash(vals)); break;
        case Id3Field::Album: SetIfEmpty(m.album, vals[0]); break;
        case Id3Field::AlbumArtist: SetIfEmpty(m.album_artist, JoinSlash(vals)); break;
        case Id3Field::Genre: SetIfEmpty(m.genre, ResolveGenre(vals[0])); break;
        case Id3Field::Year: SetIfEmpty(m.year, vals[0]); break;
        case Id3Field::Track: SetIfEmpty(m.track_number, BeforeSlash(vals[0])); break;
        case Id3Field::Disc: SetIfEmpty(m.disc_number, BeforeSlash(vals[0])); break;
        default: break;
    }
}

static void ParseId3Frames(Ctx& c, uint64_t pos, uint64_t end, int ver, bool reliable) {
    Reader& r = c.r;
    const size_t hdrLen = (ver == 2) ? 6 : 10;
    std::vector<uint8_t> body;
    int guard = 0;

    while (pos + hdrLen <= end && ++guard < 4096) {
        uint8_t fh[10];
        if (!r.Read(pos, fh, hdrLen)) break;
        if (fh[0] == 0) break; // 填充区
        size_t idLen = (ver == 2) ? 3 : 4;
        if (!IsValidFrameId(fh, idLen)) break;

        uint64_t size;
        uint16_t flags = 0;
        if (ver == 2) {
            size = BE24(fh + 3);
        } else if (ver == 3) {
            size = BE32(fh + 4);
            flags = BE16(fh + 8);
        } else {
            uint32_t ss = SyncSafe32(fh + 4), raw = BE32(fh + 4);
            flags = BE16(fh + 8);
            if ((fh[4] | fh[5] | fh[6] | fh[7]) & 0x80) {
                size = raw; // 不可能是 syncsafe,按旧版 iTunes 的错误写法容错
            } else if (ss != raw && !LooksLikeFrameBoundary(r, pos + hdrLen + ss, end, ver) &&
                       LooksLikeFrameBoundary(r, pos + hdrLen + raw, end, ver)) {
                size = raw;
            } else {
                size = ss;
            }
        }

        uint64_t bodyOff = pos + hdrLen;
        if (size > end - bodyOff) break;
        uint64_t next = bodyOff + size;
        Id3Field kind = MapFrame(fh, ver);

        bool skip = false, frameUnsync = false;
        if (ver == 3) {
            if (flags & 0x00C0) skip = true;                     // 压缩 / 加密
            if (flags & 0x0020) { ++bodyOff; if (size) --size; } // 分组标识
        } else if (ver == 4) {
            if (flags & 0x000C) skip = true;
            if (flags & 0x0040) { ++bodyOff; if (size) --size; }
            if (flags & 0x0002) frameUnsync = true;
            if (flags & 0x0001) { bodyOff += 4; size = (size >= 4) ? size - 4 : 0; }
        }

        if (kind == Id3Field::Lyrics && !c.opt.load_lyrics) skip = true; // 入库扫描不读歌词正文
        if (!skip && kind != Id3Field::None && size > 0) {
            bool isPic = (kind == Id3Field::Picture || kind == Id3Field::PicV22);
            if (!isPic) {
                size_t cap = static_cast<size_t>(std::min<uint64_t>(size, 1u << 20));
                if (r.ReadVec(bodyOff, cap, body)) {
                    if (frameUnsync) Deunsync(body);
                    HandleTextBody(c, kind, body.data(), body.size());
                }
            } else if (frameUnsync || !reliable) {
                // 反同步帧:需完整读入内存还原,无法提供物理偏移
                size_t cap = static_cast<size_t>(std::min<uint64_t>(size, c.opt.max_cover_bytes + 4096ull));
                bool want = c.opt.load_cover || cap <= 4096;
                size_t readLen = want ? cap : std::min<size_t>(cap, 4096);
                if (r.ReadVec(bodyOff, readLen, body)) {
                    if (frameUnsync) Deunsync(body);
                    uint64_t full = (readLen == cap && frameUnsync) ? body.size() : size;
                    HandlePictureBody(c, kind, body.data(), body.size(), 0, false, full);
                }
            } else {
                // 惰性:仅读图片头 (MIME/类型/描述),图片数据按偏移按需读取
                size_t headLen = static_cast<size_t>(std::min<uint64_t>(size, c.opt.load_cover ? size : 4096));
                if (headLen > c.opt.max_cover_bytes + 4096ull) headLen = 4096;
                if (r.ReadVec(bodyOff, headLen, body)) {
                    HandlePictureBody(c, kind, body.data(), body.size(), bodyOff, true, size);
                }
            }
        }
        pos = next;
    }
}

uint64_t ParseId3v2(Ctx& c, uint64_t base) {
    Reader& r = c.r;
    uint64_t total = Id3v2TotalSize(r, base);
    if (total == 0) return 0;
    uint8_t h[10];
    r.Read(base, h, 10);
    int ver = h[3];
    uint8_t flags = h[5];
    uint64_t end = std::min<uint64_t>(base + 10 + SyncSafe32(h + 6), r.Size());
    if (ver < 2 || ver > 4) return total;
    if (ver == 2 && (flags & 0x40)) return total; // v2.2 压缩标签,无标准解法

    bool tagUnsync = (flags & 0x80) && ver < 4;
    if (tagUnsync) {
        // 整个标签反同步:读入内存还原后在内存读取器上解析
        size_t cap = static_cast<size_t>(std::min<uint64_t>(end - (base + 10), 64ull << 20));
        std::vector<uint8_t> buf;
        if (!r.ReadVec(base + 10, cap, buf)) return total;
        Deunsync(buf);
        MemorySource ms(buf.data(), buf.size(), buf.size());
        Reader mr(ms);
        Ctx mc{c.m, c.opt, mr};
        uint64_t pos = 0;
        if (flags & 0x40) {
            uint32_t ext = 0;
            if (!mr.U32BE(0, ext)) return total;
            pos = 4ull + ext;
        }
        ParseId3Frames(mc, pos, buf.size(), ver, false);
        return total;
    }

    uint64_t pos = base + 10;
    if (flags & 0x40) {
        uint8_t e[4];
        if (!r.Read(pos, e, 4)) return total;
        pos += (ver == 3) ? 4ull + BE32(e) : static_cast<uint64_t>(SyncSafe32(e));
    }
    ParseId3Frames(c, pos, end, ver, true);
    return total;
}

// ===========================================================================
// APEv2
// ===========================================================================
static bool ApeKeyEq(const std::string& k, const char* name) {
    if (k.size() != std::strlen(name)) return false;
    for (size_t i = 0; i < k.size(); ++i) {
        char a = k[i], b = name[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a + 32);
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b + 32);
        if (a != b) return false;
    }
    return true;
}

bool ParseApeTagAt(Ctx& c, uint64_t footerOff, uint64_t* outTagBytes) {
    Reader& r = c.r;
    uint8_t f[32];
    if (!r.Read(footerOff, f, 32) || std::memcmp(f, "APETAGEX", 8) != 0) return false;
    uint32_t size = LE32(f + 12), count = LE32(f + 16), flags = LE32(f + 20);
    if (size < 32 || size > footerOff + 32) return false;
    uint64_t itemsStart = footerOff + 32 - size;
    uint64_t itemsEnd = footerOff;
    if (outTagBytes) *outTagBytes = size + ((flags & 0x80000000u) ? 32u : 0u);

    AudioMetadata& m = c.m;
    uint64_t pos = itemsStart;
    std::vector<uint8_t> val;
    for (uint32_t i = 0; i < count && i < 512 && pos + 9 <= itemsEnd; ++i) {
        uint8_t ih[8];
        if (!r.Read(pos, ih, 8)) break;
        uint32_t vlen = LE32(ih), iflags = LE32(ih + 4);
        uint8_t keyBuf[256];
        size_t got = r.ReadSome(pos + 8, keyBuf, static_cast<size_t>(std::min<uint64_t>(256, itemsEnd - pos - 8)));
        const uint8_t* z = static_cast<const uint8_t*>(std::memchr(keyBuf, 0, got));
        if (!z) break;
        std::string key(reinterpret_cast<const char*>(keyBuf), static_cast<size_t>(z - keyBuf));
        uint64_t vOff = pos + 8 + key.size() + 1;
        if (vlen > itemsEnd - std::min(vOff, itemsEnd)) break;
        pos = vOff + vlen;

        uint32_t type = (iflags >> 1) & 3;
        if (type == 1) { // 二进制项
            if (ApeKeyEq(key, "Cover Art (Front)") || ApeKeyEq(key, "Cover Art (Other)")) {
                size_t head = static_cast<size_t>(std::min<uint64_t>(vlen, 512));
                if (!r.ReadVec(vOff, head, val)) continue;
                const uint8_t* nz = static_cast<const uint8_t*>(std::memchr(val.data(), 0, val.size()));
                if (!nz) continue;
                size_t skipName = static_cast<size_t>(nz - val.data()) + 1;
                uint8_t ptype = ApeKeyEq(key, "Cover Art (Front)") ? 3 : 0;
                OfferCover(c, ptype, "", vOff + skipName, vlen - skipName, true, nullptr);
            }
            continue;
        }
        if (type != 0 || vlen == 0) continue;
        if (!r.ReadVec(vOff, std::min<size_t>(vlen, 64 * 1024), val)) continue;
        std::vector<std::wstring> parts;
        size_t s = 0;
        for (size_t k = 0; k <= val.size(); ++k) {
            if (k == val.size() || val[k] == 0) {
                std::wstring w = TrimTag(DecodeUtf8(val.data() + s, k - s));
                if (!w.empty()) parts.push_back(w);
                s = k + 1;
            }
        }
        if (parts.empty()) continue;
        if (ApplyExtraTag(c, key, parts[0])) continue;
        if (ApeKeyEq(key, "Title")) SetIfEmpty(m.title, parts[0]);
        else if (ApeKeyEq(key, "Artist")) SetIfEmpty(m.artist, JoinSlash(parts));
        else if (ApeKeyEq(key, "Album")) SetIfEmpty(m.album, parts[0]);
        else if (ApeKeyEq(key, "Album Artist") || ApeKeyEq(key, "AlbumArtist")) SetIfEmpty(m.album_artist, JoinSlash(parts));
        else if (ApeKeyEq(key, "Genre")) SetIfEmpty(m.genre, parts[0]);
        else if (ApeKeyEq(key, "Year")) SetIfEmpty(m.year, parts[0]);
        else if (ApeKeyEq(key, "Track")) SetIfEmpty(m.track_number, BeforeSlash(parts[0]));
        else if (ApeKeyEq(key, "Disc")) SetIfEmpty(m.disc_number, BeforeSlash(parts[0]));
        else if (ApeKeyEq(key, "Comment")) SetIfEmpty(m.comment, parts[0]);
    }
    return true;
}

// ===========================================================================
// ID3v1 + APEv2 (文件尾部)
// ===========================================================================
bool ParseId3v1AndApeTail(Ctx& c, bool* outHasId3v1, uint64_t* outApeBytes) {
    Reader& r = c.r;
    uint64_t size = r.Size();
    if (outHasId3v1) *outHasId3v1 = false;
    if (outApeBytes) *outApeBytes = 0;
    if (size < 32) return false;

    size_t tailLen = static_cast<size_t>(std::min<uint64_t>(size, 128 + 32));
    uint8_t tail[160];
    if (!r.Read(size - tailLen, tail, tailLen)) return false;

    bool hasV1 = tailLen >= 128 && std::memcmp(tail + tailLen - 128, "TAG", 3) == 0;
    bool any = false;
    if (hasV1) {
        any = true;
        if (outHasId3v1) *outHasId3v1 = true;
        const uint8_t* t = tail + tailLen - 128;
        auto field = [&](size_t off, size_t len) {
            size_t n = len;
            while (n > 0 && (t[off + n - 1] == 0 || t[off + n - 1] == ' ')) --n;
            return TrimTag(DecodeLegacy(t + off, n, c.opt.legacy_code_page));
        };
        AudioMetadata& m = c.m;
        SetIfEmpty(m.title, field(3, 30));
        SetIfEmpty(m.artist, field(33, 30));
        SetIfEmpty(m.album, field(63, 30));
        SetIfEmpty(m.year, field(93, 4));
        if (t[125] == 0 && t[126] != 0) {
            SetIfEmpty(m.comment, field(97, 28));
            SetIfEmpty(m.track_number, std::to_wstring(t[126]));
        } else {
            SetIfEmpty(m.comment, field(97, 30));
        }
        const wchar_t* g = Id3v1GenreName(t[127]);
        if (g) SetIfEmpty(m.genre, g);
    }

    uint64_t apeFooter = size - (hasV1 ? 128 : 0);
    if (apeFooter >= 32) {
        uint64_t apeBytes = 0;
        if (ParseApeTagAt(c, apeFooter - 32, &apeBytes)) {
            any = true;
            if (outApeBytes) *outApeBytes = apeBytes;
        }
    }
    return any;
}

// ===========================================================================
// MPEG Audio (Layer I/II/III) 帧头 + Xing/Info/LAME/VBRI
// ===========================================================================
struct MpegHdr {
    int version;   // 3=MPEG1 2=MPEG2 0=MPEG2.5
    int layer;     // 1/2/3
    int bitrate;   // kbps
    int sampleRate;
    int channels;
    int samplesPerFrame;
    uint32_t frameLen;
    int chMode;
};

static bool DecodeMpegHeader(const uint8_t* p, MpegHdr& h) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    int ver = (p[1] >> 3) & 3;
    int layerBits = (p[1] >> 1) & 3;
    int brIdx = (p[2] >> 4) & 0xF;
    int srIdx = (p[2] >> 2) & 3;
    int pad = (p[2] >> 1) & 1;
    if (ver == 1 || layerBits == 0 || brIdx == 0 || brIdx == 15 || srIdx == 3 || (p[3] & 3) == 2) return false;

    static const int kBr[5][15] = {
        {0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448}, // V1 L1
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},    // V1 L2
        {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320},     // V1 L3
        {0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},    // V2 L1
        {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},         // V2 L2/L3
    };
    static const int kSr[3][3] = {{44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};

    h.version = ver;
    h.layer = 4 - layerBits;
    int row = (ver == 3) ? (h.layer - 1) : (h.layer == 1 ? 3 : 4);
    h.bitrate = kBr[row][brIdx];
    h.sampleRate = kSr[ver == 3 ? 0 : (ver == 2 ? 1 : 2)][srIdx];
    h.chMode = (p[3] >> 6) & 3;
    h.channels = (h.chMode == 3) ? 1 : 2;
    if (h.layer == 1) h.samplesPerFrame = 384;
    else if (h.layer == 2) h.samplesPerFrame = 1152;
    else h.samplesPerFrame = (ver == 3) ? 1152 : 576;

    if (h.layer == 1) h.frameLen = static_cast<uint32_t>((12 * h.bitrate * 1000 / h.sampleRate + pad) * 4);
    else h.frameLen = static_cast<uint32_t>(h.samplesPerFrame / 8 * h.bitrate * 1000 / h.sampleRate + pad);
    return h.frameLen >= 21;
}

bool ParseMpegAudio(Ctx& c, uint64_t start) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    const uint64_t fileSize = r.Size();
    const uint64_t scanLimit = std::min<uint64_t>(fileSize, start + 256 * 1024);

    std::vector<uint8_t> buf;
    uint64_t bufOff = start;
    MpegHdr h{};
    uint64_t frameOff = 0;
    bool found = false;

    // 小块扫描:首块通常正好落在读取器已缓存的窗口内,避免为了找帧头额外触发一次磁盘读取
    size_t chunk = 4 * 1024;
    while (!found && bufOff + 4 <= scanLimit) {
        size_t len = static_cast<size_t>(std::min<uint64_t>(scanLimit - bufOff, chunk));
        chunk = 32 * 1024;
        buf.resize(len);
        size_t got = r.ReadSome(bufOff, buf.data(), len);
        if (got < 4) break;
        for (size_t i = 0; i + 4 <= got; ++i) {
            if (buf[i] != 0xFF) continue;
            MpegHdr cand;
            if (!DecodeMpegHeader(buf.data() + i, cand)) continue;
            // 帧同步双重确认:下一帧也必须是兼容的合法帧头,避免把封面/垃圾数据里的 0xFFEx 误判为帧头
            uint64_t nextOff = bufOff + i + cand.frameLen;
            bool confirmed = false;
            if (nextOff + 4 > fileSize) {
                confirmed = (nextOff <= fileSize);
            } else {
                uint8_t nb[4];
                MpegHdr nh;
                if (r.Read(nextOff, nb, 4) && DecodeMpegHeader(nb, nh) && nh.version == cand.version &&
                    nh.layer == cand.layer && nh.sampleRate == cand.sampleRate) {
                    confirmed = true;
                }
            }
            if (confirmed) {
                h = cand;
                frameOff = bufOff + i;
                found = true;
                break;
            }
        }
        if (!found) bufOff += (got > 3 ? got - 3 : got);
    }
    if (!found) return false;

    m.sample_rate = static_cast<uint32_t>(h.sampleRate);
    m.channels = static_cast<uint16_t>(h.channels);
    m.bitrate_kbps = static_cast<uint32_t>(h.bitrate);
    m.bits_per_sample = 16; // 有损编码,按解码输出基准 PCM 位深标注
    m.codec = (h.layer == 3) ? L"MP3" : (h.layer == 2 ? L"MP2" : L"MP1");
    m.is_lossless = false;
    m.audio_offset = frameOff;

    // 读取首帧以查找 Xing/Info/VBRI 头
    uint8_t fb[512];
    size_t fl = r.ReadSome(frameOff, fb, std::min<size_t>(sizeof(fb), h.frameLen > 0 ? std::max<size_t>(h.frameLen, 200) : 200));
    bool vbrHeader = false;
    uint64_t frames = 0, bytes = 0;
    uint32_t encDelay = 0, encPadding = 0;
    bool hasGapless = false;

    if (h.layer == 3) {
        size_t side = (h.version == 3) ? (h.channels == 1 ? 17 : 32) : (h.channels == 1 ? 9 : 17);
        size_t x = 4 + side;
        if (x + 8 <= fl && (std::memcmp(fb + x, "Xing", 4) == 0 || std::memcmp(fb + x, "Info", 4) == 0)) {
            vbrHeader = true;
            bool isInfo = std::memcmp(fb + x, "Info", 4) == 0;
            uint32_t flags = BE32(fb + x + 4);
            size_t p = x + 8;
            if ((flags & 1) && p + 4 <= fl) { frames = BE32(fb + p); p += 4; }
            if ((flags & 2) && p + 4 <= fl) { bytes = BE32(fb + p); p += 4; }
            if (flags & 4) p += 100;
            if (flags & 8) p += 4;
            // LAME / Lavc 扩展标签:编码器延迟与补齐,用于无缝精确时长
            if (p + 24 <= fl && (std::memcmp(fb + p, "LAME", 4) == 0 || std::memcmp(fb + p, "Lavc", 4) == 0 ||
                                 std::memcmp(fb + p, "Lavf", 4) == 0 || std::memcmp(fb + p, "GOGO", 4) == 0)) {
                uint32_t dp = BE24(fb + p + 21);
                encDelay = dp >> 12;
                encPadding = dp & 0xFFF;
                hasGapless = true;
            }
            if (isInfo) vbrHeader = (frames > 0); // Info = CBR,但帧计数同样精确
        } else if (36 + 18 <= fl && std::memcmp(fb + 36, "VBRI", 4) == 0) {
            vbrHeader = true;
            bytes = BE32(fb + 36 + 10);
            frames = BE32(fb + 36 + 14);
        }
    }

    if (vbrHeader && frames > 0) {
        uint64_t samples = frames * static_cast<uint64_t>(h.samplesPerFrame);
        if (hasGapless && samples > encDelay + encPadding) samples -= (encDelay + encPadding);
        m.total_samples = samples;
        m.duration_ms = static_cast<uint32_t>(samples * 1000 / h.sampleRate);
        uint64_t audioBytes = bytes ? bytes : (fileSize - frameOff);
        m.audio_bytes = audioBytes;
        if (m.duration_ms > 0) m.bitrate_kbps = static_cast<uint32_t>(audioBytes * 8 / m.duration_ms);
    } else {
        // 纯 CBR:由音频负载字节数与码率计算
        uint64_t audioBytes = fileSize > frameOff ? fileSize - frameOff : 0;
        m.audio_bytes = audioBytes;
        if (h.bitrate > 0) {
            m.duration_ms = static_cast<uint32_t>(audioBytes * 8 / static_cast<uint64_t>(h.bitrate));
            m.total_samples = static_cast<uint64_t>(m.duration_ms) * h.sampleRate / 1000;
        }
    }
    return true;
}

} // namespace tagcore
