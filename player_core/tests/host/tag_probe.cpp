// 解析内核命令行探针:对每个文件分别以「完整模式」与「惰性封面模式」解析,输出 JSON 行
// 用法: tag_probe <file>...
#include "../../include/audio_tag_parser.hpp"
#include <cstdio>
#include <string>

static std::string Esc(const std::wstring& w) {
    std::string s = AudioTagParser::WideToUtf8(w), o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
        else if (static_cast<unsigned char>(ch) < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", ch); o += b; }
        else o += ch;
    }
    return o;
}

static unsigned long long Fnv(const std::vector<uint8_t>& v) {
    unsigned long long h = 1469598103934665603ull;
    for (uint8_t b : v) { h ^= b; h *= 1099511628211ull; }
    return h;
}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        std::wstring path = AudioTagParser::Utf8ToWide(argv[i]);
        AudioMetadata full, lazy;
        bool ok = AudioTagParser::ParseFile(path, full);
        ParseOptions lo;
        lo.load_cover = false;
        bool lok = AudioTagParser::ParseFile(path, lazy, lo);
        uint32_t lazyCalls = lazy.io_read_calls;
        unsigned long long lazyBytes = lazy.io_bytes_read;
        bool extracted = lazy.has_cover ? AudioTagParser::ExtractCover(path, lazy) : false;

        std::printf("{\"file\":\"%s\",\"ok\":%s,\"lazy_ok\":%s,\"format\":\"%s\",\"codec\":\"%s\",\"lossless\":%s,"
                    "\"sample_rate\":%u,\"channels\":%u,\"bits\":%u,\"duration_ms\":%u,\"bitrate\":%u,"
                    "\"total_samples\":%llu,\"title\":\"%s\",\"artist\":\"%s\",\"album\":\"%s\",\"album_artist\":\"%s\","
                    "\"genre\":\"%s\",\"year\":\"%s\",\"track\":\"%s\",\"disc\":\"%s\",\"comment\":\"%s\","
                    "\"has_cover\":%s,\"cover_type\":%u,\"cover_mime\":\"%s\",\"cover_len\":%zu,\"cover_w\":%u,\"cover_h\":%u,"
                    "\"cover_hash\":\"%016llx\",\"io_calls\":%u,\"io_bytes\":%llu,"
                    "\"lazy_has_cover\":%s,\"lazy_cover_offset\":%llu,\"lazy_io_calls\":%u,\"lazy_io_bytes\":%llu,"
                    "\"lazy_cover_data_before\":0,\"extract_ok\":%s,\"extract_hash\":\"%016llx\",\"extract_len\":%zu}\n",
                    argv[i], ok ? "true" : "false", lok ? "true" : "false", Esc(full.format).c_str(),
                    Esc(full.codec).c_str(), full.is_lossless ? "true" : "false", full.sample_rate, full.channels,
                    full.bits_per_sample, full.duration_ms, full.bitrate_kbps,
                    static_cast<unsigned long long>(full.total_samples), Esc(full.title).c_str(),
                    Esc(full.artist).c_str(), Esc(full.album).c_str(), Esc(full.album_artist).c_str(),
                    Esc(full.genre).c_str(), Esc(full.year).c_str(), Esc(full.track_number).c_str(),
                    Esc(full.disc_number).c_str(), Esc(full.comment).c_str(), full.has_cover ? "true" : "false",
                    full.cover_picture_type, full.cover_mime_type.c_str(), full.cover_data.size(), full.cover_width,
                    full.cover_height, Fnv(full.cover_data), full.io_read_calls,
                    static_cast<unsigned long long>(full.io_bytes_read), lazy.has_cover ? "true" : "false",
                    static_cast<unsigned long long>(lazy.cover_offset), lazyCalls, lazyBytes,
                    extracted ? "true" : "false", Fnv(lazy.cover_data), lazy.cover_data.size());
    }
    return 0;
}
