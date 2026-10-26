// AudioTagParser 对外入口:格式嗅探分发、文件/内存字节源、惰性封面提取、编码转换
#include "../include/audio_tag_parser.hpp"
#include "../include/platform_io.hpp"
#include "tag_internal.hpp"
#include <algorithm>
#include <cmath>
#include <cwctype>

using namespace tagcore;

namespace {

class FileSource : public ByteSource {
public:
    explicit FileSource(const platform::ReadOnlyFile& f) : f_(f) {}
    uint64_t Size() const override { return f_.Size(); }
    size_t ReadAt(uint64_t off, void* dst, size_t n) override { return f_.ReadAt(off, dst, n); }

private:
    const platform::ReadOnlyFile& f_;
};

std::wstring LowerExt(const std::wstring& pathOrExt) {
    size_t dot = pathOrExt.rfind(L'.');
    size_t sep = pathOrExt.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep)) return L"";
    std::wstring e = pathOrExt.substr(dot);
    for (auto& ch : e) ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    return e;
}

void MergeTags(AudioMetadata& dst, AudioMetadata& src) {
    SetIfEmpty(dst.title, src.title);
    SetIfEmpty(dst.artist, src.artist);
    SetIfEmpty(dst.album, src.album);
    SetIfEmpty(dst.album_artist, src.album_artist);
    SetIfEmpty(dst.genre, src.genre);
    SetIfEmpty(dst.year, src.year);
    SetIfEmpty(dst.track_number, src.track_number);
    SetIfEmpty(dst.disc_number, src.disc_number);
    SetIfEmpty(dst.comment, src.comment);
    SetIfEmpty(dst.lyrics, src.lyrics);
    if (src.has_rg_track && !dst.has_rg_track) {
        dst.has_rg_track = true;
        dst.rg_track_gain = src.rg_track_gain;
        dst.rg_track_peak = src.rg_track_peak;
    }
    if (src.has_rg_album && !dst.has_rg_album) {
        dst.has_rg_album = true;
        dst.rg_album_gain = src.rg_album_gain;
        dst.rg_album_peak = src.rg_album_peak;
    }
    if (src.has_cover && (!dst.has_cover || (dst.cover_picture_type != 3 && src.cover_picture_type == 3))) {
        dst.has_cover = true;
        dst.cover_mime_type = src.cover_mime_type;
        dst.cover_data = std::move(src.cover_data);
        dst.cover_width = src.cover_width;
        dst.cover_height = src.cover_height;
        dst.cover_picture_type = src.cover_picture_type;
        dst.cover_offset = src.cover_offset;
        dst.cover_size = src.cover_size;
    }
}

bool IsMpegSync(const uint8_t* h) { return h[0] == 0xFF && (h[1] & 0xE0) == 0xE0 && (h[1] & 0x06) != 0; }

void ReadMp3Tail(Ctx& c) {
    if (!c.opt.read_id3v1) return;
    if (!c.m.title.empty() && !c.m.artist.empty()) return; // 已有完整 ID3v2,跳过尾部寻道
    ParseId3v1AndApeTail(c, nullptr, nullptr);
}

// 无法识别的文件:区分 QQ 音乐加密、未知加密 (高熵) 与普通未知格式,便于上层给出准确提示
void ClassifyUnknown(Ctx& c) {
    Reader& r = c.r;
    uint64_t size = r.Size();
    uint8_t tail[16] = {0};
    if (size >= 16 && r.Read(size - 16, tail, 16)) {
        if (!std::memcmp(tail + 12, "QTag", 4) || !std::memcmp(tail + 12, "STag", 4) ||
            !std::memcmp(tail + 8, "musicex\0", 8)) {
            c.m.error_code = "encrypted_qq";
            return;
        }
    }
    uint8_t buf[4096];
    size_t got = r.ReadSome(0, buf, sizeof(buf));
    if (got >= 1024) {
        uint32_t hist[256] = {0};
        for (size_t i = 0; i < got; ++i) hist[buf[i]]++;
        double entropy = 0;
        for (uint32_t h : hist) {
            if (!h) continue;
            double p = static_cast<double>(h) / got;
            entropy -= p * std::log2(p);
        }
        if (entropy > 7.8) {
            c.m.error_code = "encrypted_unknown";
            return;
        }
    }
    c.m.error_code = "unknown_format";
}

bool Dispatch(Ctx& c, const std::wstring& ext) {
    Reader& r = c.r;
    AudioMetadata& m = c.m;
    uint8_t h[16] = {0};
    size_t n = r.ReadSome(0, h, sizeof(h));
    if (n < 4) return false;

    // 1. 以 ID3v2 开头:可能是 MP3,也可能是被错误加了 ID3 头的 FLAC / APE。
    //    先顺序解析标签 (与读取窗口同向前进,不回头寻道),再判断其后的真实音频格式。
    uint64_t id3Len = Id3v2TotalSize(r, 0);
    if (id3Len > 0) {
        AudioMetadata id3;
        Ctx ic{id3, c.opt, r};
        ParseId3v2(ic, 0);
        uint64_t after = id3Len;
        // 部分软件会在声明长度之后再追加 NUL 填充
        uint8_t z = 0;
        for (int i = 0; i < 4096 && r.Read(after, &z, 1) && z == 0; ++i) ++after;
        if (r.Match(after, "fLaC", 4)) {
            bool ok = ParseFlac(c, after);
            MergeTags(m, id3); // FLAC 原生标签优先,ID3 仅作补充
            return ok;
        }
        if (r.Match(after, "MAC ", 4)) {
            bool ok = ParseApe(c, after);
            MergeTags(m, id3);
            return ok;
        }
        MergeTags(m, id3);
        m.format = L"MP3";
        bool ok = ParseMpegAudio(c, after);
        if (!ok) m.codec = L"MP3";
        ReadMp3Tail(c);
        return ok || !m.title.empty();
    }

    if (!std::memcmp(h, "fLaC", 4)) return ParseFlac(c, 0);
    if (n >= 12 && (!std::memcmp(h, "RIFF", 4) || !std::memcmp(h, "RF64", 4) || !std::memcmp(h, "BW64", 4)) &&
        !std::memcmp(h + 8, "WAVE", 4))
        return ParseWav(c);
    if (n >= 12 && !std::memcmp(h, "FORM", 4) && (!std::memcmp(h + 8, "AIFF", 4) || !std::memcmp(h + 8, "AIFC", 4)))
        return ParseAiff(c);
    if (!std::memcmp(h, "OggS", 4)) return ParseOgg(c);
    if (!std::memcmp(h, "DSD ", 4)) return ParseDsf(c);
    if (!std::memcmp(h, "FRM8", 4)) return ParseDff(c);
    if (!std::memcmp(h, "MAC ", 4)) return ParseApe(c, 0);
    if (n >= 8 && !std::memcmp(h, "CTENFDAM", 8)) return ParseNcm(c);
    if (n >= 8 && !std::memcmp(h + 4, "ftyp", 4)) return ParseMp4(c);
    if (IsMpegSync(h)) {
        m.format = L"MP3";
        bool ok = ParseMpegAudio(c, 0);
        ReadMp3Tail(c);
        return ok;
    }

    // 酷狗加密头:按魔数识别 (扩展名被改成 .flac/.mp3 也能认出,交由入库流水线解密)
    static const uint8_t kKgmMagic[16] = {0x7C, 0xD5, 0x32, 0xEB, 0x86, 0x02, 0x7F, 0x4B,
                                          0xA8, 0xAF, 0xA6, 0x8E, 0x0F, 0xFF, 0x99, 0x14};
    static const uint8_t kVprMagic[16] = {0x05, 0x28, 0xBC, 0x96, 0xE9, 0xE4, 0x5A, 0x43,
                                          0x91, 0xAA, 0xBD, 0xD0, 0x7A, 0xF5, 0x36, 0x31};
    if (n >= 16 && (!std::memcmp(h, kKgmMagic, 16) || !std::memcmp(h, kVprMagic, 16))) {
        m.format = L"KGMA";
        m.codec = L"KGM";
        m.bits_per_sample = 16;
        m.error_code = !std::memcmp(h, kKgmMagic, 16) ? "encrypted_kgm" : "encrypted_vpr";
        return true;
    }

    // 2. 无法通过魔数识别:按扩展名兜底
    if (ext == L".kgm" || ext == L".kgma" || ext == L".vpr") {
        m.format = L"KGMA";
        m.bits_per_sample = 16;
        return true;
    }
    if (ext == L".kgg") {
        m.format = L"KGG";
        m.bits_per_sample = 16;
        return true;
    }
    if (ext == L".mp3" || ext == L".mp2") {
        m.format = L"MP3";
        bool ok = ParseMpegAudio(c, 0); // 文件头存在垃圾数据时向后搜索帧同步
        ReadMp3Tail(c);
        if (ok) return true;
    }
    ClassifyUnknown(c);
    return false;
}

void Finalize(AudioMetadata& m) {
    if (m.duration_ms > 0 && m.bitrate_kbps == 0 && m.file_size > 0) {
        m.bitrate_kbps = static_cast<uint32_t>(m.file_size * 8 / m.duration_ms);
    }
    if (m.codec.empty()) m.codec = m.format;
}

} // namespace

// ===========================================================================
// 文件解析
// ===========================================================================
bool AudioTagParser::ParseFile(const std::wstring& filePath, AudioMetadata& outMeta) {
    ParseOptions opt;
    return ParseFile(filePath, outMeta, opt);
}

bool AudioTagParser::ParseFile(const std::wstring& filePath, AudioMetadata& outMeta, const ParseOptions& options) {
    platform::ReadOnlyFile f;
    if (!f.Open(filePath, true)) {
        outMeta.error_code = "open_failed";
        return false;
    }
    FileSource src(f);
    Reader r(src);
    Ctx c{outMeta, options, r};
    outMeta.file_size = f.Size();

    bool ok = Dispatch(c, LowerExt(filePath));
    outMeta.io_read_calls = r.io_calls;
    outMeta.io_bytes_read = r.io_bytes;
    if (ok) Finalize(outMeta);
    return ok;
}

bool AudioTagParser::ExtractCover(const std::wstring& filePath, AudioMetadata& meta) {
    if (!meta.cover_data.empty()) return true;
    if (meta.cover_offset > 0 && meta.cover_size > 0) {
        platform::ReadOnlyFile f;
        if (f.Open(filePath, true) && meta.cover_offset + meta.cover_size <= f.Size()) {
            std::vector<uint8_t> data(meta.cover_size);
            if (f.ReadAt(meta.cover_offset, data.data(), data.size()) == data.size()) {
                std::string mime = SniffImageMime(data.data(), data.size());
                if (!mime.empty()) {
                    meta.cover_mime_type = mime;
                    ProbeImageSize(data.data(), data.size(), meta.cover_width, meta.cover_height);
                    meta.cover_data = std::move(data);
                    meta.has_cover = true;
                    return true;
                }
            }
        }
    }
    // 回退:完整解析一次 (反同步 ID3、base64 图片等无法直接定位的情况)
    AudioMetadata full;
    ParseOptions opt;
    opt.load_cover = true;
    if (!ParseFile(filePath, full, opt) || full.cover_data.empty()) return false;
    meta.has_cover = true;
    meta.cover_mime_type = full.cover_mime_type;
    meta.cover_width = full.cover_width;
    meta.cover_height = full.cover_height;
    meta.cover_picture_type = full.cover_picture_type;
    meta.cover_offset = full.cover_offset;
    meta.cover_size = full.cover_size;
    meta.cover_data = std::move(full.cover_data);
    return true;
}

// ===========================================================================
// 内存解析
// ===========================================================================
bool AudioTagParser::ParseMemory(const uint8_t* data, size_t size, const std::wstring& hintExt, AudioMetadata& outMeta,
                                 uint64_t fullFileSize, const uint8_t* tailData, size_t tailSize) {
    ParseOptions opt;
    return ParseMemory(data, size, hintExt, outMeta, opt, fullFileSize, tailData, tailSize);
}

bool AudioTagParser::ParseMemory(const uint8_t* data, size_t size, const std::wstring& hintExt, AudioMetadata& outMeta,
                                 const ParseOptions& options, uint64_t fullFileSize, const uint8_t* tailData,
                                 size_t tailSize) {
    if (!data || size < 4) return false;
    uint64_t logical = fullFileSize > size ? fullFileSize : size;
    MemorySource src(data, size, logical, tailData, tailSize);
    Reader r(src);
    Ctx c{outMeta, options, r};
    outMeta.file_size = logical;
    bool ok = Dispatch(c, LowerExt(hintExt));
    if (ok) Finalize(outMeta);
    return ok;
}

bool AudioTagParser::IsSupportedExtension(const std::wstring& e) {
    static const wchar_t* const kExts[] = {L".flac", L".mp3", L".mp2", L".ogg", L".oga", L".opus", L".wav",
                                           L".wave", L".aif", L".aiff", L".aifc", L".m4a", L".m4b", L".mp4",
                                           L".alac", L".dsf", L".dff", L".ape", L".ncm", L".kgm", L".kgma",
                                           L".kgg", L".vpr"};
    for (const wchar_t* k : kExts)
        if (e == k) return true;
    return false;
}

// ===========================================================================
// 编码转换
// ===========================================================================
std::wstring AudioTagParser::Utf8ToWide(const std::string& str) {
    return DecodeUtf8(reinterpret_cast<const uint8_t*>(str.data()), str.size());
}

std::string AudioTagParser::WideToUtf8(const std::wstring& w) {
    std::string out;
    out.reserve(w.size() * 3);
    for (size_t i = 0; i < w.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(w[i]);
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < w.size()) {
            uint32_t lo = static_cast<uint32_t>(w[i + 1]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                ++i;
            }
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::wstring AudioTagParser::Utf16ToWide(const uint8_t* data, size_t len, bool bigEndian) {
    if (!data || len < 2) return L"";
    return DecodeUtf16(data, len, bigEndian);
}

std::wstring AudioTagParser::AnsiToWide(const std::string& str, unsigned int codePage) {
    if (str.empty()) return L"";
    const uint8_t* p = reinterpret_cast<const uint8_t*>(str.data());
    std::wstring w = DecodeCodePage(p, str.size(), codePage);
    return w.empty() ? DecodeLatin1(p, str.size()) : w;
}

std::wstring AudioTagParser::DecodeTextAuto(const std::string& bytes, unsigned int codePage) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(bytes.data());
    size_t n = bytes.size();
    if (n >= 2 && ((p[0] == 0xFF && p[1] == 0xFE) || (p[0] == 0xFE && p[1] == 0xFF))) {
        std::wstring out;
        bool be = p[0] == 0xFE;
        for (size_t i = 2; i + 1 < n; i += 2) {
            uint32_t u = be ? BE16(p + i) : LE16(p + i);
            if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
                uint32_t lo = be ? BE16(p + i + 2) : LE16(p + i + 2);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    AppendCodepoint(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                    i += 2;
                    continue;
                }
            }
            AppendCodepoint(out, u);
        }
        return out;
    }
    if (n >= 3 && p[0] == 0xEF && p[1] == 0xBB && p[2] == 0xBF) return DecodeUtf8(p + 3, n - 3);
    if (IsAscii(p, n) || IsValidUtf8(p, n)) return DecodeUtf8(p, n);
    std::wstring w = DecodeCodePage(p, n, codePage);
    return w.empty() ? DecodeLatin1(p, n) : w;
}
