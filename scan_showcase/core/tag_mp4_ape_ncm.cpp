// MP4 / M4A (ALAC · AAC · FLAC-in-MP4) · Monkey's Audio (APE) · 网易云 NCM 元数据
#include "tag_internal.hpp"
#include <algorithm>
#include <map>
#include <cstdlib>
#include <cwctype>

namespace tagcore {

// ===========================================================================
// MP4 / ISO BMFF
// ===========================================================================
struct Box {
    uint64_t off = 0;
    uint64_t size = 0;
    uint32_t hdr = 8;
    char type[5] = {0, 0, 0, 0, 0};
    uint64_t Body() const { return off + hdr; }
    uint64_t End() const { return off + size; }
    bool Is(const char* t) const { return std::memcmp(type, t, 4) == 0; }
};

static bool ReadBox(Reader& r, uint64_t off, uint64_t parentEnd, Box& b) {
    if (off + 8 > parentEnd) return false;
    uint8_t h[8];
    if (!r.Read(off, h, 8)) return false;
    b.off = off;
    b.hdr = 8;
    std::memcpy(b.type, h + 4, 4);
    uint64_t sz = BE32(h);
    if (sz == 1) {
        if (!r.U64BE(off + 8, sz)) return false;
        b.hdr = 16;
    } else if (sz == 0) {
        sz = parentEnd - off;
    }
    if (sz < b.hdr || sz > parentEnd - off) return false;
    b.size = sz;
    return true;
}

// 遍历 [begin, end) 内的子 box,回调返回 false 停止
template <typename F>
static void ForEachBox(Reader& r, uint64_t begin, uint64_t end, F&& fn) {
    uint64_t pos = begin;
    for (int guard = 0; guard < 4096 && pos + 8 <= end; ++guard) {
        Box b;
        if (!ReadBox(r, pos, end, b)) return;
        if (!fn(b)) return;
        pos = b.End();
    }
}

struct Mp4Track {
    bool sound = false;
    bool video = false;
    char entry[5] = {0, 0, 0, 0, 0};
    uint32_t timescale = 0;
    uint64_t duration = 0;
    std::wstring codec;
    bool lossless = false;
    uint32_t rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint32_t avgBitrate = 0;
};

static bool ReadFullBoxTimes(Reader& r, const Box& b, uint32_t& timescale, uint64_t& duration) {
    uint8_t v[32];
    size_t want = static_cast<size_t>(std::min<uint64_t>(b.size - b.hdr, 32));
    if (want < 20 || !r.Read(b.Body(), v, want)) return false;
    if (v[0] == 1) {
        if (want < 32) return false;
        timescale = BE32(v + 20);
        duration = BE64(v + 24);
    } else {
        timescale = BE32(v + 12);
        duration = BE32(v + 16);
    }
    return true;
}

static uint32_t ReadDescLen(const uint8_t* p, size_t n, size_t& i) {
    uint32_t len = 0;
    for (int k = 0; k < 4 && i < n; ++k) {
        uint8_t b = p[i++];
        len = (len << 7) | (b & 0x7F);
        if (!(b & 0x80)) break;
    }
    return len;
}

static void ParseEsds(const uint8_t* p, size_t n, Mp4Track& t) {
    static const uint32_t kAacRates[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                         22050, 16000, 12000, 11025, 8000,  7350};
    size_t i = 4; // version/flags
    while (i + 2 <= n) {
        uint8_t tag = p[i++];
        uint32_t len = ReadDescLen(p, n, i);
        if (tag == 3) {
            if (i + 3 > n) return;
            uint8_t fl = p[i + 2];
            i += 3;
            if (fl & 0x80) i += 2;
            if (fl & 0x40) { if (i < n) i += 1 + p[i]; }
            if (fl & 0x20) i += 2;
            continue; // 进入嵌套描述符
        }
        if (tag == 4) {
            if (i + 13 > n) return;
            uint8_t oti = p[i];
            t.avgBitrate = BE32(p + i + 9);
            if (oti == 0x6B || oti == 0x69) t.codec = L"MP3";
            else if (oti == 0xA5) t.codec = L"AC-3";
            else if (oti == 0xA6) t.codec = L"E-AC-3";
            i += 13;
            continue;
        }
        if (tag == 5) {
            if (len < 2 || i + len > n || t.codec == L"MP3") return;
            // AudioSpecificConfig 位流
            uint64_t bits = 0;
            int avail = 0;
            size_t bp = i;
            auto get = [&](int nb) -> uint32_t {
                while (avail < nb && bp < i + len) {
                    bits = (bits << 8) | p[bp++];
                    avail += 8;
                }
                if (avail < nb) return 0;
                avail -= nb;
                return static_cast<uint32_t>((bits >> avail) & ((1ull << nb) - 1));
            };
            auto rate = [&]() -> uint32_t {
                uint32_t idx = get(4);
                if (idx == 15) return get(24);
                return idx < 13 ? kAacRates[idx] : 0;
            };
            uint32_t aot = get(5);
            if (aot == 31) aot = 32 + get(6);
            uint32_t sr = rate();
            uint32_t chCfg = get(4);
            t.codec = L"AAC";
            if (aot == 5 || aot == 29) {
                uint32_t ext = rate();
                if (ext) sr = ext;
                t.codec = (aot == 29) ? L"HE-AACv2" : L"HE-AAC";
                if (aot == 29) chCfg = 2;
            }
            if (sr) t.rate = sr;
            if (chCfg >= 1 && chCfg <= 7) t.channels = static_cast<uint16_t>(chCfg == 7 ? 8 : chCfg);
            return;
        }
        i += len;
    }
}

static void ParseSampleEntry(Reader& r, const Box& e, Mp4Track& t) {
    uint8_t v[72] = {0};
    size_t want = static_cast<size_t>(std::min<uint64_t>(e.size, 72));
    if (want < 36 || !r.Read(e.off, v, want)) return;
    uint16_t qtVer = BE16(v + 16);
    t.channels = BE16(v + 24);
    t.bits = BE16(v + 26);
    t.rate = BE32(v + 32) >> 16;
    uint64_t childStart = e.off + 36;
    if (qtVer == 1) childStart = e.off + 52;
    else if (qtVer == 2 && want >= 72) {
        uint64_t bitsD = BE64(v + 40);
        double d;
        std::memcpy(&d, &bitsD, sizeof(d));
        if (d > 0 && d < 1e7) t.rate = static_cast<uint32_t>(d + 0.5);
        t.channels = static_cast<uint16_t>(BE32(v + 48));
        t.bits = static_cast<uint16_t>(BE32(v + 56));
        childStart = e.off + 72;
    }

    if (e.Is("alac")) { t.codec = L"ALAC"; t.lossless = true; }
    else if (e.Is("fLaC")) { t.codec = L"FLAC"; t.lossless = true; }
    else if (e.Is("Opus")) { t.codec = L"Opus"; }
    else if (e.Is("ac-3")) { t.codec = L"AC-3"; }
    else if (e.Is("ec-3")) { t.codec = L"E-AC-3"; }
    else if (e.Is("mp4a")) { t.codec = L"AAC"; }
    else if (e.Is(".mp3")) { t.codec = L"MP3"; }
    else if (e.Is("lpcm") || e.Is("sowt") || e.Is("twos") || e.Is("ipcm")) { t.codec = L"PCM"; t.lossless = true; }

    std::vector<uint8_t> buf;
    auto handleChild = [&](const Box& cb) -> bool {
        uint64_t len = cb.size - cb.hdr;
        if (cb.Is("alac") && len >= 28 && r.ReadVec(cb.Body(), 28, buf)) {
            const uint8_t* a = buf.data() + 4;
            t.bits = a[5];
            t.channels = a[9];
            uint32_t avg = BE32(a + 16), sr = BE32(a + 20);
            if (avg) t.avgBitrate = avg;
            if (sr) t.rate = sr;
        } else if (cb.Is("esds") && len <= 4096 && r.ReadVec(cb.Body(), static_cast<size_t>(len), buf)) {
            ParseEsds(buf.data(), buf.size(), t);
        } else if (cb.Is("dfLa") && len >= 4 + 4 + 34 && r.ReadVec(cb.Body(), 42, buf)) {
            uint64_t s = BE64(buf.data() + 8 + 10);
            t.rate = static_cast<uint32_t>(s >> 44);
            t.channels = static_cast<uint16_t>(((s >> 41) & 7) + 1);
            t.bits = static_cast<uint16_t>(((s >> 36) & 0x1F) + 1);
        }
        return true;
    };
    ForEachBox(r, childStart, e.End(), [&](const Box& cb) {
        if (cb.Is("wave")) ForEachBox(r, cb.Body(), cb.End(), handleChild); // QuickTime 包装
        else handleChild(cb);
        return true;
    });
}

static void ParseTrak(Reader& r, const Box& trak, Mp4Track& t) {
    ForEachBox(r, trak.Body(), trak.End(), [&](const Box& mdia) {
        if (!mdia.Is("mdia")) return true;
        ForEachBox(r, mdia.Body(), mdia.End(), [&](const Box& b) {
            if (b.Is("hdlr")) {
                t.sound = r.Match(b.Body() + 8, "soun", 4);
                t.video = r.Match(b.Body() + 8, "vide", 4);
            } else if (b.Is("mdhd")) {
                ReadFullBoxTimes(r, b, t.timescale, t.duration);
            } else if (b.Is("minf")) {
                ForEachBox(r, b.Body(), b.End(), [&](const Box& stbl) {
                    if (!stbl.Is("stbl")) return true;
                    ForEachBox(r, stbl.Body(), stbl.End(), [&](const Box& stsd) {
                        if (!stsd.Is("stsd")) return true;
                        Box entry;
                        if (ReadBox(r, stsd.Body() + 8, stsd.End(), entry)) {
                            std::memcpy(t.entry, entry.type, 4);
                            if (t.sound) ParseSampleEntry(r, entry, t);
                        }
                        return false;
                    });
                    return false;
                });
            }
            return true;
        });
        return false;
    });
}

static void ParseIlst(Ctx& c, const Box& ilst) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    std::vector<uint8_t> buf;
    ForEachBox(r, ilst.Body(), ilst.End(), [&](const Box& item) {
        // 自由格式 ----:mean / name / data (iTunes 的 replaygain_track_gain 等)
        if (item.Is("----")) {
            std::string name;
            std::wstring value;
            ForEachBox(r, item.Body(), item.End(), [&](const Box& sub) {
                if (sub.Is("name") && sub.size > sub.hdr + 4) {
                    std::vector<uint8_t> nb;
                    if (r.ReadVec(sub.Body() + 4, static_cast<size_t>(std::min<uint64_t>(sub.End() - sub.Body() - 4, 256)), nb))
                        name.assign(reinterpret_cast<const char*>(nb.data()), nb.size());
                } else if (sub.Is("data") && sub.size > sub.hdr + 8) {
                    std::vector<uint8_t> vb;
                    if (r.ReadVec(sub.Body() + 8, static_cast<size_t>(std::min<uint64_t>(sub.End() - sub.Body() - 8, 4096)), vb))
                        value = DecodeUtf8(vb.data(), vb.size());
                }
                return true;
            });
            if (!name.empty() && !value.empty()) ApplyExtraTag(c, name, value);
            return true;
        }
        Box data;
        if (!ReadBox(r, item.Body(), item.End(), data) || !data.Is("data") || data.size < data.hdr + 8) return true;
        uint32_t typeCode = 0;
        r.U32BE(data.Body(), typeCode);
        typeCode &= 0xFFFFFF;
        uint64_t valOff = data.Body() + 8;
        uint64_t valLen = data.End() - valOff;
        const char* t = item.type;

        if (item.Is("covr")) {
            std::string mime = (typeCode == 14) ? "image/png" : (typeCode == 27 ? "image/bmp" : "image/jpeg");
            OfferCover(c, 3, mime, valOff, valLen, true, nullptr);
            return true;
        }
        if (item.Is("trkn") || item.Is("disk")) {
            uint8_t v[4];
            if (valLen >= 4 && r.Read(valOff, v, 4)) {
                uint16_t num = BE16(v + 2);
                if (num) SetIfEmpty(item.Is("trkn") ? m.track_number : m.disc_number, std::to_wstring(num));
            }
            return true;
        }
        if (item.Is("gnre")) {
            uint16_t g = 0;
            if (valLen >= 2 && r.U16BE(valOff, g) && g > 0) {
                const wchar_t* name = Id3v1GenreName(g - 1u);
                if (name) SetIfEmpty(m.genre, name);
            }
            return true;
        }
        if (typeCode != 1 && typeCode != 2) return true;
        const bool isLyrics = static_cast<uint8_t>(t[0]) == 0xA9 && !std::memcmp(t + 1, "lyr", 3);
        if (isLyrics && !c.opt.load_lyrics) return true;
        if (!r.ReadVec(valOff, static_cast<size_t>(std::min<uint64_t>(valLen, 64 * 1024)), buf)) return true;
        std::wstring v = (typeCode == 1) ? DecodeUtf8(buf.data(), buf.size()) : DecodeUtf16(buf.data(), buf.size(), true);
        if (static_cast<uint8_t>(t[0]) == 0xA9) {
            if (!std::memcmp(t + 1, "nam", 3)) SetIfEmpty(m.title, v);
            else if (!std::memcmp(t + 1, "ART", 3)) SetIfEmpty(m.artist, v);
            else if (!std::memcmp(t + 1, "alb", 3)) SetIfEmpty(m.album, v);
            else if (!std::memcmp(t + 1, "gen", 3)) SetIfEmpty(m.genre, v);
            else if (!std::memcmp(t + 1, "day", 3)) SetIfEmpty(m.year, v);
            else if (!std::memcmp(t + 1, "cmt", 3)) SetIfEmpty(m.comment, v);
            else if (isLyrics) SetIfEmpty(m.lyrics, v);
        } else if (item.Is("aART")) {
            SetIfEmpty(m.album_artist, v);
        }
        return true;
    });
}

static void ParseMetaBox(Ctx& c, const Box& meta) {
    // ISO 规范 meta 为 FullBox (4 字节 version/flags);QuickTime 风格则没有
    uint64_t start = meta.Body();
    if (!c.r.Match(start + 4, "hdlr", 4)) start += 4;
    ForEachBox(c.r, start, meta.End(), [&](const Box& b) {
        if (b.Is("ilst")) ParseIlst(c, b);
        return true;
    });
}

bool ParseMp4(Ctx& c) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    const uint64_t fileSize = r.Size();
    Box first;
    if (!ReadBox(r, 0, fileSize, first) || !first.Is("ftyp")) return false;

    Mp4Track audio;
    bool haveAudio = false;
    uint32_t movieScale = 0;
    uint64_t movieDur = 0;
    uint64_t mdatBytes = 0, mdatOff = 0;
    bool sawMoov = false;
    bool sawVideo = false;

    ForEachBox(r, 0, fileSize, [&](const Box& top) {
        if (top.Is("mdat")) {
            mdatBytes += top.size - top.hdr;
            if (!mdatOff) mdatOff = top.Body();
        } else if (top.Is("moov")) {
            sawMoov = true;
            ForEachBox(r, top.Body(), top.End(), [&](const Box& b) {
                if (b.Is("mvhd")) {
                    ReadFullBoxTimes(r, b, movieScale, movieDur);
                } else if (b.Is("trak")) {
                    Mp4Track t;
                    ParseTrak(r, b, t);
                    // 真实视频编码轨 (排除部分 m4a 用 jpeg/png 帧做章节封面的情况)
                    if (t.video && std::memcmp(t.entry, "jpeg", 4) != 0 && std::memcmp(t.entry, "png ", 4) != 0 &&
                        std::memcmp(t.entry, "mjp2", 4) != 0 && t.entry[0] != 0)
                        sawVideo = true;
                    if (t.sound && !haveAudio) {
                        audio = t;
                        haveAudio = true;
                    }
                } else if (b.Is("udta")) {
                    ForEachBox(r, b.Body(), b.End(), [&](const Box& u) {
                        if (u.Is("meta")) ParseMetaBox(c, u);
                        return true;
                    });
                } else if (b.Is("meta")) {
                    ParseMetaBox(c, b);
                }
                return true;
            });
        }
        return true;
    });
    if (!sawMoov) return false;
    if (sawVideo) {
        m.error_code = "video_file";
        m.format = L"MP4";
        return false;
    }

    m.format = L"M4A";
    m.codec = audio.codec.empty() ? L"AAC" : audio.codec;
    m.is_lossless = audio.lossless;
    m.channels = audio.channels;
    m.sample_rate = audio.rate;
    if (m.sample_rate == 0 && audio.timescale >= 8000) m.sample_rate = audio.timescale;
    m.bits_per_sample = audio.lossless ? audio.bits : 16;
    if (m.bits_per_sample == 0) m.bits_per_sample = 16;

    if (audio.timescale && audio.duration) {
        m.duration_ms = static_cast<uint32_t>(audio.duration * 1000 / audio.timescale);
        if (m.sample_rate) m.total_samples = audio.duration * m.sample_rate / audio.timescale;
    } else if (movieScale && movieDur) {
        m.duration_ms = static_cast<uint32_t>(movieDur * 1000 / movieScale);
    }
    m.audio_offset = mdatOff;
    m.audio_bytes = mdatBytes;
    if (m.duration_ms > 0 && mdatBytes > 0) m.bitrate_kbps = static_cast<uint32_t>(mdatBytes * 8 / m.duration_ms);
    else if (audio.avgBitrate) m.bitrate_kbps = audio.avgBitrate / 1000;
    return haveAudio || !m.title.empty();
}

// ===========================================================================
// Monkey's Audio (APE)
// ===========================================================================
bool ParseApe(Ctx& c, uint64_t base) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    uint8_t h[76];
    size_t got = r.ReadSome(base, h, sizeof(h));
    if (got < 32 || std::memcmp(h, "MAC ", 4) != 0) return false;
    uint16_t version = LE16(h + 4);

    uint32_t blocksPerFrame = 0, finalFrameBlocks = 0, totalFrames = 0, sampleRate = 0;
    uint16_t bits = 16, channels = 0;
    if (version >= 3980) {
        uint32_t descBytes = LE32(h + 8);
        uint8_t hd[24];
        if (!r.Read(base + descBytes, hd, 24)) return false;
        blocksPerFrame = LE32(hd + 4);
        finalFrameBlocks = LE32(hd + 8);
        totalFrames = LE32(hd + 12);
        bits = LE16(hd + 16);
        channels = LE16(hd + 18);
        sampleRate = LE32(hd + 20);
    } else {
        uint16_t level = LE16(h + 6), flags = LE16(h + 8);
        channels = LE16(h + 10);
        sampleRate = LE32(h + 12);
        totalFrames = LE32(h + 24);
        finalFrameBlocks = LE32(h + 28);
        bits = (flags & 1) ? 8 : ((flags & 8) ? 24 : 16);
        if (version >= 3950) blocksPerFrame = 73728 * 4;
        else if (version >= 3900 || (version >= 3800 && level == 4000)) blocksPerFrame = 73728;
        else blocksPerFrame = 9216;
    }
    if (sampleRate == 0 || channels == 0) return false;

    m.format = L"APE";
    m.codec = L"APE";
    m.is_lossless = true;
    m.sample_rate = sampleRate;
    m.channels = channels;
    m.bits_per_sample = bits;
    if (totalFrames > 0) {
        m.total_samples = static_cast<uint64_t>(totalFrames - 1) * blocksPerFrame + finalFrameBlocks;
        m.duration_ms = static_cast<uint32_t>(m.total_samples * 1000 / sampleRate);
    }
    bool hasV1 = false;
    uint64_t apeBytes = 0;
    ParseId3v1AndApeTail(c, &hasV1, &apeBytes);
    uint64_t audio = r.Size() - base - (hasV1 ? 128 : 0) - apeBytes;
    m.audio_offset = base;
    m.audio_bytes = audio;
    if (m.duration_ms) m.bitrate_kbps = static_cast<uint32_t>(audio * 8 / m.duration_ms);
    return true;
}

// ===========================================================================
// 网易云 NCM:解密头部元数据 (AES-128-ECB) 获取歌名/歌手/专辑/码率/时长/封面
// ===========================================================================
namespace {

struct AesTables {
    uint8_t sbox[256];
    uint8_t inv[256];
    AesTables() {
        auto rotl = [](uint8_t x, int s) { return static_cast<uint8_t>((x << s) | (x >> (8 - s))); };
        uint8_t p = 1, q = 1;
        do {
            p = static_cast<uint8_t>(p ^ (p << 1) ^ ((p & 0x80) ? 0x1B : 0));
            q = static_cast<uint8_t>(q ^ (q << 1));
            q = static_cast<uint8_t>(q ^ (q << 2));
            q = static_cast<uint8_t>(q ^ (q << 4));
            if (q & 0x80) q = static_cast<uint8_t>(q ^ 0x09);
            uint8_t x = static_cast<uint8_t>(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4));
            sbox[p] = static_cast<uint8_t>(x ^ 0x63);
        } while (p != 1);
        sbox[0] = 0x63;
        for (int i = 0; i < 256; ++i) inv[sbox[i]] = static_cast<uint8_t>(i);
    }
};

const AesTables& Aes() {
    static const AesTables t;
    return t;
}

uint8_t GMul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = static_cast<uint8_t>((a << 1) ^ ((a & 0x80) ? 0x1B : 0));
        b >>= 1;
    }
    return r;
}

class Aes128Decryptor {
public:
    explicit Aes128Decryptor(const uint8_t key[16]) {
        const uint8_t* sb = Aes().sbox;
        std::memcpy(rk_, key, 16);
        uint8_t rcon = 1;
        for (int i = 16; i < 176; i += 4) {
            uint8_t t[4] = {rk_[i - 4], rk_[i - 3], rk_[i - 2], rk_[i - 1]};
            if (i % 16 == 0) {
                uint8_t tmp = t[0];
                t[0] = static_cast<uint8_t>(sb[t[1]] ^ rcon);
                t[1] = sb[t[2]];
                t[2] = sb[t[3]];
                t[3] = sb[tmp];
                rcon = static_cast<uint8_t>((rcon << 1) ^ ((rcon & 0x80) ? 0x1B : 0));
            }
            for (int k = 0; k < 4; ++k) rk_[i + k] = static_cast<uint8_t>(rk_[i - 16 + k] ^ t[k]);
        }
    }

    void DecryptBlock(uint8_t s[16]) const {
        const uint8_t* inv = Aes().inv;
        for (int i = 0; i < 16; ++i) s[i] ^= rk_[160 + i];
        for (int round = 9; round >= 0; --round) {
            uint8_t t[16];
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 4; ++row) t[row + 4 * ((col + row) & 3)] = s[row + 4 * col];
            for (int i = 0; i < 16; ++i) s[i] = static_cast<uint8_t>(inv[t[i]] ^ rk_[round * 16 + i]);
            if (round == 0) break;
            for (int col = 0; col < 4; ++col) {
                uint8_t* a = s + 4 * col;
                uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
                a[0] = static_cast<uint8_t>(GMul(a0, 14) ^ GMul(a1, 11) ^ GMul(a2, 13) ^ GMul(a3, 9));
                a[1] = static_cast<uint8_t>(GMul(a0, 9) ^ GMul(a1, 14) ^ GMul(a2, 11) ^ GMul(a3, 13));
                a[2] = static_cast<uint8_t>(GMul(a0, 13) ^ GMul(a1, 9) ^ GMul(a2, 14) ^ GMul(a3, 11));
                a[3] = static_cast<uint8_t>(GMul(a0, 11) ^ GMul(a1, 13) ^ GMul(a2, 9) ^ GMul(a3, 14));
            }
        }
    }

private:
    uint8_t rk_[176];
};

// ---------------- 迷你 JSON 解析器 ----------------
struct JVal {
    enum Kind { Null, Bool, Num, Str, Arr, Obj } kind = Null;
    double num = 0;
    std::string str;
    std::vector<JVal> arr;
    std::vector<std::pair<std::string, JVal>> obj;
    const JVal* Get(const char* k) const {
        for (const auto& kv : obj)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

class JParser {
public:
    JParser(const char* p, size_t n) : p_(p), e_(p + n) {}
    bool Parse(JVal& v, int depth = 0) {
        Ws();
        if (p_ >= e_ || depth > 32) return false;
        char ch = *p_;
        if (ch == '{') {
            v.kind = JVal::Obj;
            ++p_;
            Ws();
            if (p_ < e_ && *p_ == '}') { ++p_; return true; }
            while (p_ < e_) {
                Ws();
                std::string key;
                if (!Str(key)) return false;
                Ws();
                if (p_ >= e_ || *p_ != ':') return false;
                ++p_;
                JVal child;
                if (!Parse(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                Ws();
                if (p_ < e_ && *p_ == ',') { ++p_; continue; }
                if (p_ < e_ && *p_ == '}') { ++p_; return true; }
                return false;
            }
            return false;
        }
        if (ch == '[') {
            v.kind = JVal::Arr;
            ++p_;
            Ws();
            if (p_ < e_ && *p_ == ']') { ++p_; return true; }
            while (p_ < e_) {
                JVal child;
                if (!Parse(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                Ws();
                if (p_ < e_ && *p_ == ',') { ++p_; continue; }
                if (p_ < e_ && *p_ == ']') { ++p_; return true; }
                return false;
            }
            return false;
        }
        if (ch == '"') { v.kind = JVal::Str; return Str(v.str); }
        if (Lit("true")) { v.kind = JVal::Bool; v.num = 1; return true; }
        if (Lit("false")) { v.kind = JVal::Bool; return true; }
        if (Lit("null")) { v.kind = JVal::Null; return true; }
        // 数字
        const char* s = p_;
        while (p_ < e_ && (std::strchr("+-0123456789.eE", *p_) != nullptr)) ++p_;
        if (p_ == s) return false;
        v.kind = JVal::Num;
        v.num = std::strtod(std::string(s, p_).c_str(), nullptr);
        return true;
    }

private:
    void Ws() { while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_; }
    bool Lit(const char* l) {
        size_t n = std::strlen(l);
        if (static_cast<size_t>(e_ - p_) >= n && std::memcmp(p_, l, n) == 0) { p_ += n; return true; }
        return false;
    }
    static void PutUtf8(std::string& o, uint32_t cp) {
        if (cp < 0x80) o += static_cast<char>(cp);
        else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            o += static_cast<char>(0xE0 | (cp >> 12));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            o += static_cast<char>(0xF0 | (cp >> 18));
            o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            o += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool Hex4(uint32_t& v) {
        if (e_ - p_ < 4) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p_++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool Str(std::string& out) {
        if (p_ >= e_ || *p_ != '"') return false;
        ++p_;
        while (p_ < e_) {
            char ch = *p_++;
            if (ch == '"') return true;
            if (ch != '\\') { out += ch; continue; }
            if (p_ >= e_) return false;
            char esc = *p_++;
            switch (esc) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t cp;
                    if (!Hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && e_ - p_ >= 6 && p_[0] == '\\' && p_[1] == 'u') {
                        p_ += 2;
                        uint32_t lo;
                        if (!Hex4(lo)) return false;
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    PutUtf8(out, cp);
                    break;
                }
                default: out += esc; break;
            }
        }
        return false;
    }
    const char* p_;
    const char* e_;
};

std::wstring JStr(const JVal* v) {
    if (!v || v->kind != JVal::Str) return L"";
    return DecodeUtf8(reinterpret_cast<const uint8_t*>(v->str.data()), v->str.size());
}

} // namespace

void Aes128EcbDecrypt(const uint8_t key[16], uint8_t* data, size_t n) {
    Aes128Decryptor aes(key);
    for (size_t i = 0; i + 16 <= n; i += 16) aes.DecryptBlock(data + i);
}

bool ParseNcm(Ctx& c) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    if (!r.Match(0, "CTENFDAM", 8)) return false;
    m.format = L"NCM";

    uint32_t keyLen = 0, metaLen = 0;
    if (!r.U32LE(10, keyLen)) return false;
    uint64_t metaLenOff = 14ull + keyLen;
    if (!r.U32LE(metaLenOff, metaLen)) return false;
    uint64_t metaOff = metaLenOff + 4;
    uint64_t afterMeta = metaOff + metaLen;

    std::vector<uint8_t> meta;
    if (metaLen > 22 && metaLen < (1u << 20) && r.ReadVec(metaOff, metaLen, meta)) {
        for (auto& b : meta) b ^= 0x63;
        static const char kPrefix[] = "163 key(Don't modify):";
        if (std::memcmp(meta.data(), kPrefix, 22) == 0) {
            std::vector<uint8_t> enc = Base64Decode(meta.data() + 22, meta.size() - 22);
            if (!enc.empty() && enc.size() % 16 == 0) {
                static const uint8_t kMetaKey[16] = {0x23, 0x31, 0x34, 0x6C, 0x6A, 0x6B, 0x5F, 0x21,
                                                     0x5C, 0x5D, 0x26, 0x30, 0x55, 0x3C, 0x27, 0x28};
                Aes128Decryptor aes(kMetaKey);
                for (size_t i = 0; i < enc.size(); i += 16) aes.DecryptBlock(&enc[i]);
                uint8_t pad = enc.back();
                if (pad >= 1 && pad <= 16 && pad <= enc.size()) enc.resize(enc.size() - pad);
                const char* js = reinterpret_cast<const char*>(enc.data());
                size_t jn = enc.size();
                const void* colon = std::memchr(js, ':', std::min<size_t>(jn, 16));
                if (colon) {
                    size_t skip = static_cast<size_t>(static_cast<const char*>(colon) - js) + 1;
                    JVal root;
                    JParser jp(js + skip, jn - skip);
                    if (jp.Parse(root) && root.kind == JVal::Obj) {
                        const JVal* music = root.Get("mainMusic"); // 电台节目 (dj:) 嵌套
                        const JVal& o = (music && music->kind == JVal::Obj) ? *music : root;
                        SetIfEmpty(m.title, JStr(o.Get("musicName")));
                        SetIfEmpty(m.album, JStr(o.Get("album")));
                        if (const JVal* ar = o.Get("artist")) {
                            std::wstring joined;
                            if (ar->kind == JVal::Arr) {
                                for (const auto& a : ar->arr) {
                                    std::wstring name = (a.kind == JVal::Arr && !a.arr.empty()) ? JStr(&a.arr[0]) : JStr(&a);
                                    if (name.empty()) continue;
                                    if (!joined.empty()) joined += L"/";
                                    joined += name;
                                }
                            }
                            SetIfEmpty(m.artist, joined);
                        }
                        std::wstring fmt = JStr(o.Get("format"));
                        for (auto& ch : fmt) ch = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(ch)));
                        if (!fmt.empty()) m.codec = fmt;
                        m.is_lossless = (fmt == L"FLAC");
                        if (const JVal* br = o.Get("bitrate")) if (br->kind == JVal::Num && br->num > 0) m.bitrate_kbps = static_cast<uint32_t>(br->num / 1000);
                        if (const JVal* du = o.Get("duration")) if (du->kind == JVal::Num && du->num > 0) m.duration_ms = static_cast<uint32_t>(du->num);
                    }
                }
            }
        }
    }

    // 封面:兼容新旧两种布局 (CRC32 + 5 字节间隙之后)
    uint64_t p = afterMeta + 9;
    uint8_t hb[16];
    if (r.Read(p, hb, 16)) {
        uint32_t a = LE32(hb), b = LE32(hb + 4);
        auto isImg = [](const uint8_t* x) { return (x[0] == 0xFF && x[1] == 0xD8) || (x[0] == 0x89 && x[1] == 'P'); };
        if (b > 0 && b <= a && isImg(hb + 8)) {
            OfferCover(c, 3, "", p + 8, b, true, nullptr);
            m.audio_offset = p + 8 + a;
        } else if (a > 0 && isImg(hb + 4)) {
            OfferCover(c, 3, "", p + 4, a, true, nullptr);
            m.audio_offset = p + 4 + a;
        }
    }
    if (m.bits_per_sample == 0) m.bits_per_sample = 16;
    return true;
}

} // namespace tagcore
