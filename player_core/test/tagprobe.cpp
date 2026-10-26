// 用项目自己的 AudioTagParser 解析指定文件,确认 has_cover / cover_offset 是否正确
#include "audio_tag_parser.hpp"

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif
#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, &w[0], n);

    ParseOptions opt;
    opt.load_cover = true;       // 先试"读全图"
    AudioMetadata m;
    const bool ok = AudioTagParser::ParseFile(w, m, opt);
    std::printf("ParseFile  ok=%d\n", ok ? 1 : 0);
    std::printf("  format      = %ls\n", m.format.c_str());
    std::printf("  has_cover   = %d\n", m.has_cover ? 1 : 0);
    std::printf("  cover_mime  = %s\n", m.cover_mime_type.c_str());
    std::printf("  cover_offset= %llu\n", static_cast<unsigned long long>(m.cover_offset));
    std::printf("  cover_size  = %u\n", m.cover_size);
    std::printf("  cover_data  = %zu 字节\n", m.cover_data.size());
    std::printf("  cover_w/h   = %u x %u\n", m.cover_width, m.cover_height);
    std::printf("  pic_type    = %u\n", static_cast<unsigned>(m.cover_picture_type));

    // 再试惰性模式(曲库扫描实际用的)
    ParseOptions opt2;
    opt2.load_cover = false;
    AudioMetadata m2;
    const bool ok2 = AudioTagParser::ParseFile(w, m2, opt2);
    std::printf("\n惰性模式 (load_cover=false):\n");
    std::printf("  ok=%d  has_cover=%d  offset=%llu  size=%u  data=%zu\n",
                ok2 ? 1 : 0, m2.has_cover ? 1 : 0,
                static_cast<unsigned long long>(m2.cover_offset), m2.cover_size, m2.cover_data.size());
    return 0;
}
