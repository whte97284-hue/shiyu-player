// 全库解析审计:递归扫描目录,逐文件输出解析结果 CSV (UTF-8 BOM,可直接用 Excel 打开)
// 目标目录全程只读。用法: library_audit.exe <音乐目录> <输出.csv> [--full]
//   默认惰性封面模式 (与入库一致);--full 同时读取封面,用于核对封面解析
#include "../include/audio_tag_parser.hpp"
#include "../include/platform_io.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

static std::string Csv(const std::wstring& w) {
    std::string s = AudioTagParser::WideToUtf8(w), o = "\"";
    for (char ch : s) {
        if (ch == '"') o += "\"\"";
        else if (ch == '\r' || ch == '\n') o += ' ';
        else o += ch;
    }
    return o + "\"";
}

static int Run(const std::wstring& root, const std::wstring& outPath, bool full) {
    std::vector<platform::DirEntry> files;
    auto te = std::chrono::steady_clock::now();
    platform::EnumerateFilesRecursive(root, [&](const std::wstring&, const std::vector<platform::DirEntry>& fs) {
        for (const auto& f : fs) {
            std::wstring n = f.name;
            size_t d = n.rfind(L'.');
            std::wstring e = d == std::wstring::npos ? L"" : n.substr(d);
            for (auto& c : e) c = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(c)));
            if (AudioTagParser::IsSupportedExtension(e)) files.push_back(f);
        }
        return true;
    });
    double enumSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - te).count();

    platform::WritableFile out;
    if (!out.Create(outPath)) {
        std::fprintf(stderr, "cannot create output csv\n");
        return 2;
    }
    std::string head = "\xEF\xBB\xBFpath,ok,format,codec,lossless,sample_rate,bits,channels,duration_s,bitrate_kbps,"
                       "title,artist,album,album_artist,year,track,has_cover,cover_mime,cover_bytes,cover_offset,"
                       "file_mb,io_calls,io_kb,parse_ms,error\r\n";
    out.Write(head.data(), head.size());

    ParseOptions opt;
    opt.load_cover = full;
    size_t ok = 0, fail = 0;
    uint64_t ioBytes = 0, libBytes = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        AudioMetadata m;
        auto p0 = std::chrono::steady_clock::now();
        bool r = AudioTagParser::ParseFile(f.path, m, opt);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count();
        (r ? ok : fail)++;
        ioBytes += m.io_bytes_read;
        libBytes += f.size;
        char num[512];
        std::snprintf(num, sizeof num, ",%d,", r ? 1 : 0);
        std::string line = Csv(f.path) + num + Csv(m.format) + "," + Csv(m.codec) + ",";
        std::snprintf(num, sizeof num, "%d,%u,%u,%u,%.3f,%u,", m.is_lossless ? 1 : 0, m.sample_rate, m.bits_per_sample,
                      m.channels, m.duration_ms / 1000.0, m.bitrate_kbps);
        line += num;
        line += Csv(m.title) + "," + Csv(m.artist) + "," + Csv(m.album) + "," + Csv(m.album_artist) + "," + Csv(m.year) +
                "," + Csv(m.track_number) + ",";
        std::snprintf(num, sizeof num, "%d,%s,%u,%llu,%.2f,%u,%.1f,%.2f,%s\r\n", m.has_cover ? 1 : 0,
                      m.cover_mime_type.c_str(), m.cover_size, static_cast<unsigned long long>(m.cover_offset),
                      f.size / 1048576.0, m.io_read_calls, m.io_bytes_read / 1024.0, ms, m.error_code.c_str());
        line += num;
        out.Write(line.data(), line.size());
        if ((i + 1) % 200 == 0) std::fprintf(stderr, "  %zu / %zu\n", i + 1, files.size());
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("files=%zu ok=%zu fail=%zu enumerate=%.2fs parse=%.2fs (%.0f files/s, single thread) "
                "library=%.1fGB read=%.1fMB (%.4f%%)\n",
                files.size(), ok, fail, enumSec, sec, files.size() / std::max(sec, 1e-9), libBytes / 1073741824.0,
                ioBytes / 1048576.0, 100.0 * ioBytes / std::max<uint64_t>(libBytes, 1));
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: library_audit <dir> <out.csv> [--full]\n");
        return 1;
    }
    return Run(argv[1], argv[2], argc > 3 && std::wstring(argv[3]) == L"--full");
}
#else
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: library_audit <dir> <out.csv> [--full]\n");
        return 1;
    }
    return Run(AudioTagParser::Utf8ToWide(argv[1]), AudioTagParser::Utf8ToWide(argv[2]),
               argc > 3 && std::string(argv[3]) == "--full");
}
#endif
