// 多格式封面写入的往返验证:写 → 用项目自己的 AudioTagParser 读回 → 比对
// 用法: wtest.exe <文件...>
#include "audio_tag_parser.hpp"
#include "cover_fetcher.hpp"
#include "cover_writer.hpp"
#include "platform_io.hpp"

#include <cstdio>
#include <string>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

static std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n - 1 : 0), L'\0');
    if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
static std::string N(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: wtest <file> [...]\n");
        return 1;
    }
    // 造一张假 JPEG(只要魔数对,写入流程就能跑;读回时也按魔数判定)
    std::string jpeg = "\xFF\xD8\xFF\xE0";
    jpeg.append(2048, '\x20');
    jpeg += "\xFF\xD9";
    std::printf("测试图片: %zu 字节\n", jpeg.size());

    int ok = 0, bad = 0;
    for (int i = 1; i < argc; ++i) {
        const std::wstring path = W(argv[i]);
        std::printf("\n%s\n", argv[i]);

        uint64_t sz0 = 0, mt0 = 0;
        platform::GetFileInfo(path, sz0, mt0);
        std::printf("  写入前: %llu 字节  canEmbed=%d\n", static_cast<unsigned long long>(sz0),
                    cover_core::CanEmbedCover(path) ? 1 : 0);
        if (!cover_core::CanEmbedCover(path)) {
            ++bad;
            continue;
        }

        std::string err;
        if (!cover_core::EmbedCover(path, jpeg, "image/jpeg", &err)) {
            std::printf("  ✗ 写入失败: %s\n", err.c_str());
            ++bad;
            continue;
        }
        uint64_t sz1 = 0, mt1 = 0;
        platform::GetFileInfo(path, sz1, mt1);
        std::printf("  写入后: %llu 字节 (+%lld)\n", static_cast<unsigned long long>(sz1),
                    static_cast<long long>(sz1) - static_cast<long long>(sz0));

        // 用项目自己的解析器读回
        ParseOptions po;
        po.load_cover = true;
        AudioMetadata m;
        const bool parsed = AudioTagParser::ParseFile(path, m, po);
        std::printf("  解析: ok=%d format=%s\n", parsed ? 1 : 0, N(m.format).c_str());
        std::printf("  has_cover=%d mime=%s offset=%llu size=%u data=%zu\n", m.has_cover ? 1 : 0,
                    m.cover_mime_type.c_str(), static_cast<unsigned long long>(m.cover_offset),
                    m.cover_size, m.cover_data.size());
        std::printf("  标题=%s 歌手=%s\n", N(m.title).c_str(), N(m.artist).c_str());

        const bool good = parsed && m.has_cover && m.cover_data.size() == jpeg.size();
        std::printf("  %s\n", good ? "★ 读回成功,封面字节数一致" : "✗ 读回失败或不一致");
        if (good) ++ok; else ++bad;
    }
    std::printf("\n成功 %d / 失败 %d\n", ok, bad);
    return ok > 0 ? 0 : 2;
}
