// 直接测在线取封面:给定 title/artist/alt_title,看能不能拿到图
// 用法: ftest.exe "<artist>" "<title>" ["<alt_title>"] ["<探测文件路径>"]
#include "audio_tag_parser.hpp"
#include "cover_fetcher.hpp"

#include <cstdio>
#include <string>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: ftest <artist> <title> [alt_title] [probe_file]\n");
        return 1;
    }
    // ★ Windows 命令行给 main 的 argv 是 ANSI(GBK),不是 UTF-8 —— 必须转一次再用
    auto arg = [](const char* a) -> std::string {
        const int n = ::MultiByteToWideChar(CP_ACP, 0, a, -1, nullptr, 0);
        std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
        if (n > 1) ::MultiByteToWideChar(CP_ACP, 0, a, -1, &w[0], n);
        return AudioTagParser::WideToUtf8(w);
    };
    cover_core::FlacTags tags;
    tags.artist = arg(argv[1]);
    tags.title = arg(argv[2]);
    if (argc > 3) tags.alt_title = arg(argv[3]);

    std::wstring probe;
    if (argc > 4 && argv[4][0]) {
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, argv[4], -1, nullptr, 0);
        probe.resize(static_cast<size_t>(n > 0 ? n - 1 : 0));
        if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, argv[4], -1, &probe[0], n);
    }

    std::printf("artist=[%s] title=[%s] alt=[%s]\n", tags.artist.c_str(), tags.title.c_str(),
                tags.alt_title.c_str());
    cover_core::CoverResult r;
    std::string err;
    if (cover_core::FetchOnline(tags, probe, r, &err)) {
        std::printf("  ★ 成功: %zu 字节 mime=%s source=%s %s\n", r.jpeg.size(), r.mime.c_str(),
                    r.source.c_str(), r.detail.c_str());
        return 0;
    }
    std::printf("  ✗ 失败: %s\n", err.c_str());
    return 2;
}
