#include "../include/scan_cache.hpp"
#include "../include/platform_io.hpp"
#include <cstring>

namespace {

const char kMagic[8] = {'K', 'P', 'S', 'C', 'A', 'C', 'H', 'E'};

class Writer {
public:
    void U8(uint8_t v) { buf.push_back(v); }
    void U16(uint16_t v) { Raw(&v, 2); }
    void U32(uint32_t v) { Raw(&v, 4); }
    void U64(uint64_t v) { Raw(&v, 8); }
    void Str(const std::string& s) {
        U32(static_cast<uint32_t>(s.size()));
        buf.insert(buf.end(), s.begin(), s.end());
    }
    void WStr(const std::wstring& w) { Str(AudioTagParser::WideToUtf8(w)); }
    std::vector<uint8_t> buf;

private:
    // 统一小端序 (x86/x64/ARM64 均为小端,直接拷贝)
    void Raw(const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        buf.insert(buf.end(), b, b + n);
    }
};

class Parser {
public:
    Parser(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    bool U8(uint8_t& v) { return Raw(&v, 1); }
    bool U16(uint16_t& v) { return Raw(&v, 2); }
    bool U32(uint32_t& v) { return Raw(&v, 4); }
    bool U64(uint64_t& v) { return Raw(&v, 8); }
    bool Str(std::string& s) {
        uint32_t len;
        if (!U32(len) || len > n_ - o_) return false;
        s.assign(reinterpret_cast<const char*>(p_ + o_), len);
        o_ += len;
        return true;
    }
    bool WStr(std::wstring& w) {
        std::string s;
        if (!Str(s)) return false;
        w = AudioTagParser::Utf8ToWide(s);
        return true;
    }

private:
    bool Raw(void* d, size_t n) {
        if (n > n_ - o_) return false;
        std::memcpy(d, p_ + o_, n);
        o_ += n;
        return true;
    }
    const uint8_t* p_;
    size_t n_;
    size_t o_ = 0;
};

void WriteEntry(Writer& w, const std::wstring& path, const ScanCacheEntry& e) {
    const AudioMetadata& m = e.meta;
    w.WStr(path);
    w.U64(e.size);
    w.U64(e.mtime);
    w.U8(e.parse_ok ? 1 : 0);
    w.WStr(e.target_path);
    for (const std::wstring* s : {&m.title, &m.artist, &m.album, &m.album_artist, &m.genre, &m.year,
                                  &m.track_number, &m.disc_number, &m.comment, &m.format, &m.codec})
        w.WStr(*s);
    w.U8(m.is_lossless ? 1 : 0);
    w.U32(m.duration_ms);
    w.U32(m.bitrate_kbps);
    w.U32(m.sample_rate);
    w.U16(m.channels);
    w.U16(m.bits_per_sample);
    w.U64(m.total_samples);
    w.U64(m.audio_offset);
    w.U64(m.audio_bytes);
    w.U64(m.file_size);
    w.U8(m.has_cover ? 1 : 0);
    w.Str(m.cover_mime_type);
    w.U32(m.cover_width);
    w.U32(m.cover_height);
    w.U8(m.cover_picture_type);
    w.U64(m.cover_offset);
    w.U32(m.cover_size);
    w.Str(m.error_code);
    // ReplayGain:标志位 + 4 个 float (按位存储)
    w.U8(static_cast<uint8_t>((m.has_rg_track ? 1 : 0) | (m.has_rg_album ? 2 : 0)));
    for (float f : {m.rg_track_gain, m.rg_track_peak, m.rg_album_gain, m.rg_album_peak}) {
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        w.U32(bits);
    }
}

bool ReadEntry(Parser& r, std::wstring& path, ScanCacheEntry& e) {
    AudioMetadata& m = e.meta;
    uint8_t ok = 0, lossless = 0, cover = 0;
    if (!r.WStr(path) || !r.U64(e.size) || !r.U64(e.mtime) || !r.U8(ok) || !r.WStr(e.target_path)) return false;
    for (std::wstring* s : {&m.title, &m.artist, &m.album, &m.album_artist, &m.genre, &m.year, &m.track_number,
                            &m.disc_number, &m.comment, &m.format, &m.codec})
        if (!r.WStr(*s)) return false;
    if (!r.U8(lossless) || !r.U32(m.duration_ms) || !r.U32(m.bitrate_kbps) || !r.U32(m.sample_rate) ||
        !r.U16(m.channels) || !r.U16(m.bits_per_sample) || !r.U64(m.total_samples) || !r.U64(m.audio_offset) ||
        !r.U64(m.audio_bytes) || !r.U64(m.file_size) || !r.U8(cover) || !r.Str(m.cover_mime_type) ||
        !r.U32(m.cover_width) || !r.U32(m.cover_height) || !r.U8(m.cover_picture_type) || !r.U64(m.cover_offset) ||
        !r.U32(m.cover_size) || !r.Str(m.error_code))
        return false;
    uint8_t rg = 0;
    if (!r.U8(rg)) return false;
    for (float* f : {&m.rg_track_gain, &m.rg_track_peak, &m.rg_album_gain, &m.rg_album_peak}) {
        uint32_t bits = 0;
        if (!r.U32(bits)) return false;
        std::memcpy(f, &bits, 4);
    }
    m.has_rg_track = rg & 1;
    m.has_rg_album = rg & 2;
    e.parse_ok = ok != 0;
    m.is_lossless = lossless != 0;
    m.has_cover = cover != 0;
    return true;
}

} // namespace

bool ScanCache::Load(const std::wstring& path) {
    entries_.clear();
    platform::ReadOnlyFile f;
    if (!f.Open(path, false) || f.Size() < 20 || f.Size() > (1ull << 31)) return false;
    std::vector<uint8_t> data(static_cast<size_t>(f.Size()));
    if (f.ReadAt(0, data.data(), data.size()) != data.size()) return false;
    if (std::memcmp(data.data(), kMagic, 8) != 0) return false;

    Parser r(data.data() + 8, data.size() - 8);
    uint32_t version = 0, count = 0;
    if (!r.U32(version) || version != kParserVersion || !r.U32(count)) return false;
    entries_.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::wstring p;
        ScanCacheEntry e;
        if (!ReadEntry(r, p, e)) {
            entries_.clear(); // 截断或损坏:整体放弃,退化为全量扫描
            return false;
        }
        entries_.emplace(std::move(p), std::move(e));
    }
    return true;
}

bool ScanCache::Save(const std::wstring& path) const {
    Writer w;
    w.buf.reserve(entries_.size() * 256 + 16);
    w.buf.insert(w.buf.end(), kMagic, kMagic + 8);
    w.U32(kParserVersion);
    w.U32(static_cast<uint32_t>(entries_.size()));
    for (const auto& kv : entries_) WriteEntry(w, kv.first, kv.second);

    std::wstring tmp = path + L".tmp";
    {
        platform::WritableFile out;
        if (!out.Create(tmp) || !out.Write(w.buf.data(), w.buf.size())) {
            platform::RemoveFile(tmp);
            return false;
        }
    }
    if (!platform::AtomicReplaceFile(tmp, path)) {
        platform::RemoveFile(tmp);
        return false;
    }
    return true;
}

const ScanCacheEntry* ScanCache::Find(const std::wstring& path, uint64_t size, uint64_t mtime) const {
    auto it = entries_.find(path);
    if (it == entries_.end() || it->second.size != size || it->second.mtime != mtime) return nullptr;
    return &it->second;
}

void ScanCache::Put(const std::wstring& path, ScanCacheEntry entry) {
    entry.meta.cover_data.clear();
    entries_[path] = std::move(entry);
}

size_t ScanCache::Carry(const std::vector<std::pair<std::wstring, std::wstring>>& moves, bool keepSource) {
    size_t n = 0;
    for (const auto& mv : moves) {
        auto it = entries_.find(mv.first);
        if (it == entries_.end() || mv.first == mv.second) continue;
        ScanCacheEntry e = it->second;
        if (!keepSource) entries_.erase(it);
        entries_[mv.second] = std::move(e);
        ++n;
    }
    return n;
}
