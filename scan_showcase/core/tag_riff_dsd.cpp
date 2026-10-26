// RIFF WAVE / RF64 · AIFF / AIFC · DSD (DSF / DSDIFF)
#include "tag_internal.hpp"
#include <algorithm>
#include <cmath>

namespace tagcore {

// ===========================================================================
// WAV / RF64 / BW64
// ===========================================================================
static void ApplyRiffInfo(Ctx& c, const uint8_t* p, size_t n) {
    // p 指向 "INFO" 之后的子块序列
    AudioMetadata& m = c.m;
    size_t o = 0;
    while (o + 8 <= n) {
        const uint8_t* id = p + o;
        uint32_t len = LE32(p + o + 4);
        o += 8;
        if (len > n - o) break;
        std::wstring v = TrimTag(DecodeLegacy(p + o, len, c.opt.legacy_code_page));
        if (!std::memcmp(id, "INAM", 4)) SetIfEmpty(m.title, v);
        else if (!std::memcmp(id, "IART", 4)) SetIfEmpty(m.artist, v);
        else if (!std::memcmp(id, "IPRD", 4)) SetIfEmpty(m.album, v);
        else if (!std::memcmp(id, "IGNR", 4)) SetIfEmpty(m.genre, v);
        else if (!std::memcmp(id, "ICRD", 4)) SetIfEmpty(m.year, v);
        else if (!std::memcmp(id, "ICMT", 4)) SetIfEmpty(m.comment, v);
        else if (!std::memcmp(id, "ITRK", 4) || !std::memcmp(id, "IPRT", 4)) SetIfEmpty(m.track_number, v);
        o += len + (len & 1);
    }
}

bool ParseWav(Ctx& c) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    uint64_t infoOff = 0;   // LIST/INFO 的位置:延迟到最后解析,让 ID3 优先
    uint64_t infoLen = 0;
    uint8_t h[12];
    if (!r.Read(0, h, 12)) return false;
    bool rf64 = !std::memcmp(h, "RF64", 4) || !std::memcmp(h, "BW64", 4);
    if ((!rf64 && std::memcmp(h, "RIFF", 4) != 0) || std::memcmp(h + 8, "WAVE", 4) != 0) return false;
    m.format = L"WAV";

    const uint64_t fileSize = r.Size();
    uint64_t pos = 12;
    uint64_t ds64Data = 0;
    uint64_t dataOff = 0, dataSize = 0;
    uint32_t byteRate = 0;
    uint16_t blockAlign = 0, fmtTag = 0;
    bool gotFmt = false;
    std::vector<uint8_t> buf;

    for (int guard = 0; guard < 512 && pos + 8 <= fileSize; ++guard) {
        uint8_t ch[8];
        if (!r.Read(pos, ch, 8)) break;
        uint64_t len = LE32(ch + 4);
        uint64_t body = pos + 8;

        if (!std::memcmp(ch, "ds64", 4) && len >= 16) {
            uint64_t v;
            if (r.U64LE(body + 8, v)) ds64Data = v;
        } else if (!std::memcmp(ch, "fmt ", 4) && len >= 16) {
            uint8_t f[40] = {0};
            size_t want = static_cast<size_t>(std::min<uint64_t>(len, 40));
            if (r.Read(body, f, want)) {
                gotFmt = true;
                fmtTag = LE16(f);
                m.channels = LE16(f + 2);
                m.sample_rate = LE32(f + 4);
                byteRate = LE32(f + 8);
                blockAlign = LE16(f + 12);
                m.bits_per_sample = LE16(f + 14);
                if (fmtTag == 0xFFFE && want >= 26) {
                    uint16_t valid = LE16(f + 18);
                    if (valid > 0 && valid <= m.bits_per_sample) m.bits_per_sample = valid;
                    fmtTag = LE16(f + 24); // SubFormat GUID 前两字节即格式码
                }
            }
        } else if (!std::memcmp(ch, "data", 4)) {
            if (rf64 && LE32(ch + 4) == 0xFFFFFFFFu && ds64Data) len = ds64Data;
            dataOff = body;
            if (len == 0 || len > fileSize - body) len = fileSize - body; // 流式写入未回填 / 截断文件
            dataSize = len;
        } else if (!std::memcmp(ch, "LIST", 4) && len >= 4) {
            // 先记下位置,等整个文件扫完再解析 —— 有些工具写 WAV 时会把 LIST/INFO 放在
            // id3 chunk 前面,而 INFO 里塞的是 ANSI 编码的窄字符串(日文全变成 '?')。
            // 以前这里立即解析,SetIfEmpty 把字段占了,后面正确的 ID3(UTF-16)反而填不进去,
            // 界面上专辑名/歌手名就全是问号。现在让 ID3 先解析,INFO 只补空缺。
            infoOff = body;
            infoLen = len;
        } else if ((!std::memcmp(ch, "id3 ", 4) || !std::memcmp(ch, "ID3 ", 4)) && len > 10) {
            ParseId3v2(c, body);
        }

        uint64_t next = body + len + (len & 1);
        if (next <= pos) break;
        pos = next;
    }
    if (!gotFmt) return false;

    // ID3 已经解析完了,这时再用 INFO 补剩下的空缺(它优先级最低)
    if (infoOff != 0 && infoLen >= 4) {
        size_t cap = static_cast<size_t>(std::min<uint64_t>(infoLen, 1u << 20));
        std::vector<uint8_t> ibuf;
        if (r.ReadVec(infoOff, cap, ibuf) && ibuf.size() >= 4 && !std::memcmp(ibuf.data(), "INFO", 4))
            ApplyRiffInfo(c, ibuf.data() + 4, ibuf.size() - 4);
    }

    switch (fmtTag) {
        case 1: m.codec = L"PCM"; m.is_lossless = true; break;
        case 3: m.codec = L"PCM Float"; m.is_lossless = true; break;
        case 6: m.codec = L"A-law"; break;
        case 7: m.codec = L"mu-law"; break;
        case 0x55: m.codec = L"MP3"; break;
        default: m.codec = L"WAV"; break;
    }
    if (dataSize == 0 && fileSize > 44) dataSize = fileSize - 44;
    m.audio_offset = dataOff;
    m.audio_bytes = dataSize;
    if (byteRate > 0) {
        m.duration_ms = static_cast<uint32_t>(dataSize * 1000 / byteRate);
        m.bitrate_kbps = static_cast<uint32_t>(static_cast<uint64_t>(byteRate) * 8 / 1000);
    }
    if (blockAlign > 0) m.total_samples = dataSize / blockAlign;
    return m.sample_rate > 0 && m.channels > 0;
}

// ===========================================================================
// AIFF / AIFC
// ===========================================================================
static double Ext80ToDouble(const uint8_t* b) {
    int exp = ((b[0] & 0x7F) << 8) | b[1];
    uint64_t mant = BE64(b + 2);
    if (exp == 0 && mant == 0) return 0.0;
    double v = std::ldexp(static_cast<double>(mant), exp - 16383 - 63);
    return (b[0] & 0x80) ? -v : v;
}

bool ParseAiff(Ctx& c) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    uint8_t h[12];
    if (!r.Read(0, h, 12) || std::memcmp(h, "FORM", 4) != 0) return false;
    bool aifc = !std::memcmp(h + 8, "AIFC", 4);
    if (!aifc && std::memcmp(h + 8, "AIFF", 4) != 0) return false;
    m.format = L"AIFF";
    m.codec = L"PCM";
    m.is_lossless = true;

    const uint64_t fileSize = r.Size();
    uint64_t pos = 12;
    uint32_t frames = 0;
    bool gotComm = false;
    std::vector<uint8_t> buf;
    for (int guard = 0; guard < 512 && pos + 8 <= fileSize; ++guard) {
        uint8_t ch[8];
        if (!r.Read(pos, ch, 8)) break;
        uint64_t len = BE32(ch + 4);
        uint64_t body = pos + 8;
        if (!std::memcmp(ch, "COMM", 4) && len >= 18) {
            uint8_t cm[22] = {0};
            if (r.Read(body, cm, static_cast<size_t>(std::min<uint64_t>(len, 22)))) {
                gotComm = true;
                m.channels = BE16(cm);
                frames = BE32(cm + 2);
                m.bits_per_sample = BE16(cm + 6);
                m.sample_rate = static_cast<uint32_t>(Ext80ToDouble(cm + 8) + 0.5);
                if (aifc && len >= 22) {
                    if (!std::memcmp(cm + 18, "fl32", 4) || !std::memcmp(cm + 18, "FL32", 4)) m.codec = L"PCM Float";
                    else if (std::memcmp(cm + 18, "NONE", 4) != 0 && std::memcmp(cm + 18, "sowt", 4) != 0) {
                        m.codec = L"AIFC";
                        m.is_lossless = false;
                    }
                }
            }
        } else if (!std::memcmp(ch, "SSND", 4) && len >= 8) {
            uint32_t off = 0;
            r.U32BE(body, off);
            m.audio_offset = body + 8 + off;
            uint64_t avail = len > 8 + off ? len - 8 - off : 0;
            m.audio_bytes = std::min<uint64_t>(avail, fileSize - std::min(fileSize, m.audio_offset));
        } else if ((!std::memcmp(ch, "ID3 ", 4) || !std::memcmp(ch, "id3 ", 4)) && len > 10) {
            ParseId3v2(c, body);
        } else if ((!std::memcmp(ch, "NAME", 4) || !std::memcmp(ch, "AUTH", 4)) && len > 0) {
            if (r.ReadVec(body, static_cast<size_t>(std::min<uint64_t>(len, 4096)), buf)) {
                std::wstring v = DecodeLegacy(buf.data(), buf.size(), c.opt.legacy_code_page);
                if (!std::memcmp(ch, "NAME", 4)) SetIfEmpty(m.title, v);
                else SetIfEmpty(m.artist, v);
            }
        }
        uint64_t next = body + len + (len & 1);
        if (next <= pos) break;
        pos = next;
    }
    if (!gotComm) return false;
    m.total_samples = frames;
    if (m.sample_rate > 0) m.duration_ms = static_cast<uint32_t>(static_cast<uint64_t>(frames) * 1000 / m.sample_rate);
    m.bitrate_kbps = static_cast<uint32_t>(static_cast<uint64_t>(m.sample_rate) * m.channels * m.bits_per_sample / 1000);
    return m.sample_rate > 0;
}

// ===========================================================================
// DSD 公共
// ===========================================================================
static std::wstring DsdCodecName(uint32_t rate) {
    if (rate % 44100 == 0) return L"DSD" + std::to_wstring(rate / 44100);
    if (rate % 48000 == 0) return L"DSD" + std::to_wstring(rate / 48000) + L" (48k)";
    return L"DSD";
}

// ===========================================================================
// DSF (Sony DSD Stream File)
// ===========================================================================
bool ParseDsf(Ctx& c) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    uint8_t h[92];
    if (!r.Read(0, h, 92)) return false;
    if (std::memcmp(h, "DSD ", 4) != 0 || std::memcmp(h + 28, "fmt ", 4) != 0) return false;
    uint64_t metaPtr = LE64(h + 20);
    uint32_t channels = LE32(h + 52);
    uint32_t rate = LE32(h + 56);
    uint32_t bits = LE32(h + 60);
    uint64_t samples = LE64(h + 64);
    if (rate == 0 || channels == 0 || channels > 8) return false;

    m.format = L"DSF";
    m.codec = DsdCodecName(rate);
    m.is_lossless = true;
    m.sample_rate = rate;
    m.channels = static_cast<uint16_t>(channels);
    m.bits_per_sample = static_cast<uint16_t>(bits == 8 ? 8 : 1);
    m.total_samples = samples;
    m.duration_ms = static_cast<uint32_t>(samples * 1000 / rate);
    m.bitrate_kbps = static_cast<uint32_t>(static_cast<uint64_t>(rate) * channels / 1000);
    if (!std::memcmp(h + 80, "data", 4)) {
        m.audio_offset = 92;
        uint64_t len = LE64(h + 84);
        m.audio_bytes = len > 12 ? len - 12 : 0;
    }
    if (metaPtr > 0 && metaPtr + 10 <= r.Size()) ParseId3v2(c, metaPtr);
    return true;
}

// ===========================================================================
// DFF (Philips DSDIFF)
// ===========================================================================
static void ParseDiin(Ctx& c, uint64_t pos, uint64_t end) {
    std::vector<uint8_t> buf;
    for (int guard = 0; guard < 64 && pos + 12 <= end; ++guard) {
        uint8_t ch[12];
        if (!c.r.Read(pos, ch, 12)) return;
        uint64_t len = BE64(ch + 4);
        uint64_t body = pos + 12;
        if (len > end - body) return;
        bool isArtist = !std::memcmp(ch, "DIAR", 4), isTitle = !std::memcmp(ch, "DITI", 4);
        if ((isArtist || isTitle) && len > 4 && c.r.ReadVec(body, static_cast<size_t>(std::min<uint64_t>(len, 4096)), buf)) {
            uint32_t n = BE32(buf.data());
            n = static_cast<uint32_t>(std::min<size_t>(n, buf.size() - 4));
            std::wstring v = DecodeLegacy(buf.data() + 4, n, c.opt.legacy_code_page);
            if (isArtist) SetIfEmpty(c.m.artist, v);
            else SetIfEmpty(c.m.title, v);
        }
        pos = body + len + (len & 1);
    }
}

bool ParseDff(Ctx& c) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    uint8_t h[16];
    if (!r.Read(0, h, 16) || std::memcmp(h, "FRM8", 4) != 0 || std::memcmp(h + 12, "DSD ", 4) != 0) return false;
    m.format = L"DFF";
    m.codec = L"DSD";
    m.is_lossless = true;
    m.bits_per_sample = 1;

    const uint64_t fileSize = r.Size();
    uint64_t pos = 16;
    uint64_t dsdBytes = 0;
    uint32_t dstFrames = 0;
    uint16_t dstRate = 0;
    bool dst = false;

    for (int guard = 0; guard < 256 && pos + 12 <= fileSize; ++guard) {
        uint8_t ch[12];
        if (!r.Read(pos, ch, 12)) break;
        uint64_t len = BE64(ch + 4);
        uint64_t body = pos + 12;
        if (len > fileSize - body) len = fileSize - body;

        if (!std::memcmp(ch, "PROP", 4) && len >= 4 && r.Match(body, "SND ", 4)) {
            uint64_t p = body + 4, end = body + len;
            for (int g2 = 0; g2 < 64 && p + 12 <= end; ++g2) {
                uint8_t sc[12];
                if (!r.Read(p, sc, 12)) break;
                uint64_t sl = BE64(sc + 4);
                uint64_t sb = p + 12;
                if (sl > end - sb) break;
                if (!std::memcmp(sc, "FS  ", 4) && sl >= 4) {
                    uint32_t fs = 0;
                    if (r.U32BE(sb, fs)) m.sample_rate = fs;
                } else if (!std::memcmp(sc, "CHNL", 4) && sl >= 2) {
                    uint16_t n = 0;
                    if (r.U16BE(sb, n)) m.channels = n;
                } else if (!std::memcmp(sc, "CMPR", 4) && sl >= 4) {
                    dst = r.Match(sb, "DST ", 4);
                }
                p = sb + sl + (sl & 1);
            }
        } else if (!std::memcmp(ch, "DSD ", 4)) {
            m.audio_offset = body;
            dsdBytes = len;
        } else if (!std::memcmp(ch, "DST ", 4)) {
            m.audio_offset = body;
            dsdBytes = len;
            uint8_t fr[18];
            if (r.Read(body, fr, 18) && !std::memcmp(fr, "FRTE", 4)) {
                dstFrames = BE32(fr + 12);
                dstRate = BE16(fr + 16);
            }
        } else if (!std::memcmp(ch, "DIIN", 4)) {
            ParseDiin(c, body, body + len);
        } else if (!std::memcmp(ch, "ID3 ", 4) && len > 10) {
            ParseId3v2(c, body);
        }
        uint64_t next = body + len + (len & 1);
        if (next <= pos) break;
        pos = next;
    }
    if (m.sample_rate == 0 || m.channels == 0) return false;
    m.codec = dst ? L"DST" : DsdCodecName(m.sample_rate);
    m.audio_bytes = dsdBytes;
    if (dst && dstFrames && dstRate) {
        m.duration_ms = static_cast<uint32_t>(static_cast<uint64_t>(dstFrames) * 1000 / dstRate);
        m.total_samples = static_cast<uint64_t>(dstFrames) * m.sample_rate / dstRate;
    } else if (!dst) {
        m.total_samples = dsdBytes * 8 / m.channels;
        m.duration_ms = static_cast<uint32_t>(m.total_samples * 1000 / m.sample_rate);
    }
    m.bitrate_kbps = static_cast<uint32_t>(static_cast<uint64_t>(m.sample_rate) * m.channels / 1000);
    return true;
}

} // namespace tagcore
