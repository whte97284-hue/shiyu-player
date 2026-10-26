// FLAC 原生流 · Ogg 容器 (Vorbis / Opus / Ogg FLAC)
#include "tag_internal.hpp"
#include <algorithm>

namespace tagcore {

static void ApplyStreamInfo(AudioMetadata& m, const uint8_t* si) {
    // STREAMINFO: 字节 10..17 = 采样率(20) 声道-1(3) 位深-1(5) 总样本数(36)
    uint64_t v = BE64(si + 10);
    m.sample_rate = static_cast<uint32_t>(v >> 44);
    m.channels = static_cast<uint16_t>(((v >> 41) & 0x7) + 1);
    m.bits_per_sample = static_cast<uint16_t>(((v >> 36) & 0x1F) + 1);
    m.total_samples = v & 0xFFFFFFFFFull;
    if (m.sample_rate > 0 && m.total_samples > 0) {
        m.duration_ms = static_cast<uint32_t>(m.total_samples * 1000 / m.sample_rate);
    }
}

// ===========================================================================
// FLAC
// ===========================================================================
bool ParseFlac(Ctx& c, uint64_t base) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    if (!r.Match(base, "fLaC", 4)) return false;
    m.format = L"FLAC";
    m.codec = L"FLAC";
    m.is_lossless = true;

    uint64_t pos = base + 4;
    std::vector<uint8_t> buf;
    bool gotStreamInfo = false;
    for (int guard = 0; guard < 1024; ++guard) {
        uint8_t bh[4];
        if (!r.Read(pos, bh, 4)) break;
        bool last = (bh[0] & 0x80) != 0;
        uint8_t type = bh[0] & 0x7F;
        uint32_t len = BE24(bh + 1);
        uint64_t body = pos + 4;
        if (len > r.Size() - std::min(body, r.Size())) break;

        if (type == 0 && len >= 34) {
            uint8_t si[34];
            if (r.Read(body, si, 34)) {
                ApplyStreamInfo(m, si);
                gotStreamInfo = true;
            }
        } else if (type == 4) {
            size_t cap = static_cast<size_t>(std::min<uint32_t>(len, 64u << 20));
            if (r.ReadVec(body, cap, buf)) ApplyVorbisComments(c, buf.data(), buf.size());
        } else if (type == 6) {
            bool full = c.opt.load_cover && len <= c.opt.max_cover_bytes + 4096ull;
            size_t want = full ? len : std::min<uint32_t>(len, 4096);
            if (r.ReadVec(body, want, buf)) {
                // 截断读取时,结构体中的数据长度字段仍完整,图片按物理偏移惰性定位
                ParseFlacPictureStruct(c, buf.data(), buf.size(), body, true);
            }
        } else if (type == 127) {
            break; // 非法块类型
        }
        pos = body + len;
        if (last) break;
    }

    m.audio_offset = pos;
    uint64_t fileSize = r.Size();
    if (fileSize > pos) m.audio_bytes = fileSize - pos;
    if (m.duration_ms > 0 && m.audio_bytes > 0) {
        m.bitrate_kbps = static_cast<uint32_t>(m.audio_bytes * 8 / m.duration_ms);
    }
    return gotStreamInfo || !m.title.empty();
}

// ===========================================================================
// Ogg
// ===========================================================================
struct OggPageHdr {
    uint8_t flags = 0;
    int64_t granule = 0;
    uint32_t serial = 0;
    uint32_t headerLen = 0;
    uint32_t bodyLen = 0;
    uint8_t lacing[255];
    uint8_t segs = 0;
};

static bool ReadOggPage(Reader& r, uint64_t off, OggPageHdr& pg) {
    uint8_t h[27];
    if (!r.Read(off, h, 27)) return false;
    if (std::memcmp(h, "OggS", 4) != 0 || h[4] != 0) return false;
    pg.flags = h[5];
    pg.granule = static_cast<int64_t>(LE64(h + 6));
    pg.serial = LE32(h + 14);
    pg.segs = h[26];
    if (pg.segs && !r.Read(off + 27, pg.lacing, pg.segs)) return false;
    pg.headerLen = 27u + pg.segs;
    pg.bodyLen = 0;
    for (int i = 0; i < pg.segs; ++i) pg.bodyLen += pg.lacing[i];
    return true;
}

// 从文件头开始,按 lacing 规则为首个逻辑流重组前 maxPackets 个数据包
static void CollectOggPackets(Reader& r, size_t maxPackets, std::vector<std::vector<uint8_t>>& packets,
                              uint32_t& serialOut) {
    uint64_t off = 0;
    bool haveSerial = false;
    std::vector<uint8_t> cur, body;
    size_t totalBytes = 0;
    const size_t kCap = 64u << 20;

    for (int pageGuard = 0; pageGuard < 4096 && packets.size() < maxPackets; ++pageGuard) {
        OggPageHdr pg;
        if (!ReadOggPage(r, off, pg)) break;
        uint64_t bodyOff = off + pg.headerLen;
        off = bodyOff + pg.bodyLen;
        if (!haveSerial) {
            serialOut = pg.serial;
            haveSerial = true;
        } else if (pg.serial != serialOut) {
            continue; // 复用流中的其他逻辑流
        }
        if (!r.ReadVec(bodyOff, pg.bodyLen, body)) break;
        size_t bp = 0;
        for (int i = 0; i < pg.segs && packets.size() < maxPackets; ++i) {
            uint8_t lv = pg.lacing[i];
            if (totalBytes + lv <= kCap) {
                cur.insert(cur.end(), body.begin() + bp, body.begin() + bp + lv);
                totalBytes += lv;
            }
            bp += lv;
            if (lv < 255) {
                packets.push_back(std::move(cur));
                cur.clear();
            }
        }
    }
}

// 从文件尾部回溯最后一个属于指定逻辑流的页,取其 granule position
static int64_t FindLastGranule(Reader& r, uint32_t serial) {
    uint64_t size = r.Size();
    uint64_t span = 64 * 1024;
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint64_t start = size > span ? size - span : 0;
        std::vector<uint8_t> tail;
        if (!r.ReadVec(start, static_cast<size_t>(size - start), tail)) return -1;
        for (size_t i = tail.size() >= 27 ? tail.size() - 27 : 0;; --i) {
            if (tail[i] == 'O' && i + 27 <= tail.size() && std::memcmp(&tail[i], "OggS", 4) == 0 && tail[i + 4] == 0) {
                int64_t g = static_cast<int64_t>(LE64(&tail[i + 6]));
                uint32_t s = LE32(&tail[i + 14]);
                if (s == serial && g > 0) return g;
            }
            if (i == 0) break;
        }
        if (start == 0) break;
        span *= 4;
    }
    return -1;
}

bool ParseOgg(Ctx& c) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    if (!r.Match(0, "OggS", 4)) return false;

    std::vector<std::vector<uint8_t>> pk;
    uint32_t serial = 0;
    CollectOggPackets(r, 2, pk, serial);
    if (pk.empty()) return false;
    const std::vector<uint8_t>& id = pk[0];

    uint32_t preSkip = 0;
    bool known = false;
    if (id.size() >= 30 && id[0] == 1 && std::memcmp(&id[1], "vorbis", 6) == 0) {
        known = true;
        m.format = L"OGG";
        m.codec = L"Vorbis";
        m.channels = id[11];
        m.sample_rate = LE32(&id[12]);
        int32_t nominal = static_cast<int32_t>(LE32(&id[20]));
        if (nominal > 0) m.bitrate_kbps = static_cast<uint32_t>(nominal / 1000);
        m.bits_per_sample = 16;
        if (pk.size() > 1 && pk[1].size() > 7 && pk[1][0] == 3 && std::memcmp(&pk[1][1], "vorbis", 6) == 0)
            ApplyVorbisComments(c, pk[1].data() + 7, pk[1].size() - 7);
    } else if (id.size() >= 19 && std::memcmp(id.data(), "OpusHead", 8) == 0) {
        known = true;
        m.format = L"OPUS";
        m.codec = L"Opus";
        m.channels = id[9];
        preSkip = LE16(&id[10]);
        m.sample_rate = 48000; // Opus 解码输出恒为 48 kHz
        m.bits_per_sample = 16;
        if (pk.size() > 1 && pk[1].size() > 8 && std::memcmp(pk[1].data(), "OpusTags", 8) == 0)
            ApplyVorbisComments(c, pk[1].data() + 8, pk[1].size() - 8);
    } else if (id.size() >= 51 && id[0] == 0x7F && std::memcmp(&id[1], "FLAC", 4) == 0 &&
               std::memcmp(&id[9], "fLaC", 4) == 0) {
        known = true;
        m.format = L"OGG";
        m.codec = L"FLAC";
        m.is_lossless = true;
        ApplyStreamInfo(m, &id[17]);
        // 后续数据包均为 FLAC 元数据块
        uint16_t headerPackets = BE16(&id[7]);
        std::vector<std::vector<uint8_t>> more;
        uint32_t s2 = 0;
        CollectOggPackets(r, std::min<size_t>(1u + headerPackets, 16), more, s2);
        for (size_t i = 1; i < more.size(); ++i) {
            const auto& b = more[i];
            if (b.size() < 4) continue;
            uint8_t type = b[0] & 0x7F;
            if (type == 4) ApplyVorbisComments(c, b.data() + 4, b.size() - 4);
            else if (type == 6) ParseFlacPictureStruct(c, b.data() + 4, b.size() - 4, 0, false);
        }
    } else if (id.size() >= 80 && std::memcmp(id.data(), "Speex   ", 8) == 0) {
        known = true;
        m.format = L"OGG";
        m.codec = L"Speex";
        m.sample_rate = LE32(&id[36]);
        m.channels = static_cast<uint16_t>(LE32(&id[48]));
        m.bits_per_sample = 16;
    }
    if (!known) return false;

    int64_t g = FindLastGranule(r, serial);
    if (g > 0 && m.sample_rate > 0) {
        uint64_t samples = static_cast<uint64_t>(g);
        if (preSkip && samples > preSkip) samples -= preSkip;
        m.total_samples = samples;
        m.duration_ms = static_cast<uint32_t>(samples * 1000 / m.sample_rate);
    }
    m.audio_bytes = r.Size();
    if (m.duration_ms > 0) {
        uint32_t avg = static_cast<uint32_t>(r.Size() * 8 / m.duration_ms);
        if (m.bitrate_kbps == 0 || m.codec != L"Vorbis") m.bitrate_kbps = avg;
    }
    return true;
}

} // namespace tagcore
