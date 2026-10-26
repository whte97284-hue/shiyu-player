// cover_fetcher 的功能 + 性能测试 (直接链接生产代码,不复制逻辑)
// 用法: covertest.exe <flac文件或目录> [...]
#include "cover_fetcher.hpp"
#include "platform_io.hpp"

#include <chrono>
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

namespace {

using Clock = std::chrono::steady_clock;
double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::vector<std::wstring> CollectFlac(const std::wstring& root) {
    std::vector<std::wstring> out;
    if (!platform::IsDirectory(root)) {
        out.push_back(root);
        return out;
    }
    platform::EnumerateFilesRecursive(root, [&](const std::wstring&, const std::vector<platform::DirEntry>& files) {
        for (const auto& f : files) {
            if (f.name.size() > 5) {
                std::wstring e = f.name.substr(f.name.size() - 5);
                for (wchar_t& c : e) c = static_cast<wchar_t>(::towlower(c));
                if (e == L".flac") out.push_back(f.path);
            }
        }
        return true;
    });
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: covertest <flac|dir> [...]\n");
        return 1;
    }

    std::vector<std::wstring> files;
    for (int i = 1; i < argc; ++i) {
        std::wstring w;
        const int n = ::MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, nullptr, 0);
        w.resize(static_cast<size_t>(n > 0 ? n - 1 : 0));
        if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, argv[i], -1, &w[0], n);
        for (const auto& f : CollectFlac(w)) files.push_back(f);
    }
    if (files.empty()) {
        std::printf("no flac found\n");
        return 2;
    }

    double total_md5 = 0, total_net = 0, total_embed = 0;
    int ok_count = 0, skip_count = 0, fail_count = 0;

    for (const auto& path : files) {
        std::printf("\n%s\n", Narrow(path).c_str());

        if (!cover_core::IsFlac(path)) {
            std::printf("  非 FLAC,跳过\n");
            ++fail_count;
            continue;
        }

        auto t0 = Clock::now();
        const bool has = cover_core::FlacHasCover(path);
        auto t1 = Clock::now();
        std::printf("  [扫块]   %-6s  %.2f ms\n", has ? "有封面" : "无封面", Ms(t0, t1));
        if (has) {
            ++skip_count;
            continue;
        }

        t0 = Clock::now();
        const std::string md5 = cover_core::Md5File(path);
        t1 = Clock::now();
        const double t_md5 = Ms(t0, t1);
        total_md5 += t_md5;
        std::printf("  [MD5]    %s  %.1f ms  (基准参考:生产路径 size 命中时不算)\n", md5.c_str(), t_md5);

        cover_core::FlacTags tags;
        t0 = Clock::now();
        const bool gotTags = cover_core::ReadFlacTags(path, tags);
        t1 = Clock::now();
        std::printf("  [标签]   TITLE=%s ARTIST=%s  %.2f ms\n",
                    tags.title.c_str(), tags.artist.c_str(), Ms(t0, t1));
        if (!gotTags) {
            std::printf("  → 无 TITLE,跳过\n");
            ++fail_count;
            continue;
        }

        t0 = Clock::now();
        cover_core::CoverResult r;
        std::string err;
        const bool fetched = cover_core::FetchOnline(tags, path, r, &err);
        t1 = Clock::now();
        const double t_net = Ms(t0, t1);
        total_net += t_net;
        if (!fetched) {
            std::printf("  [网络]   失败 (%.0f ms): %s\n", t_net, err.c_str());
            ++fail_count;
            continue;
        }
        std::printf("  [网络]   %s  %zu B (%.0f KB)  %s  %.0f ms\n", r.source.c_str(), r.jpeg.size(),
                    r.jpeg.size() / 1024.0, r.detail.c_str(), t_net);

        t0 = Clock::now();
        const bool embedded = cover_core::EmbedFlacPicture(path, r.jpeg, 0, 0, &err);
        t1 = Clock::now();
        const double t_emb = Ms(t0, t1);
        total_embed += t_emb;
        if (!embedded) {
            std::printf("  [嵌入]   失败 (%.0f ms): %s\n", t_emb, err.c_str());
            ++fail_count;
            continue;
        }
        std::printf("  [嵌入]   ✔ %.0f ms\n", t_emb);

        t0 = Clock::now();
        const bool verify = cover_core::FlacHasCover(path);
        t1 = Clock::now();
        std::printf("  [复验]   %s  %.2f ms\n", verify ? "★ 已有封面" : "✗ 写入失败", Ms(t0, t1));
        if (verify) ++ok_count;
        else ++fail_count;
    }

    std::printf("\n================ 汇总 ================\n");
    std::printf("  文件数: %zu\n", files.size());
    std::printf("  成功嵌入: %d   跳过(已有): %d   失败: %d\n", ok_count, skip_count, fail_count);
    if (ok_count + fail_count > 0) {
        const int n = ok_count + fail_count;
        std::printf("  平均 MD5:   %.1f ms\n", total_md5 / n);
        std::printf("  平均 网络:  %.0f ms\n", total_net / n);
        if (ok_count > 0) std::printf("  平均 嵌入:  %.1f ms\n", total_embed / ok_count);
    }
    return 0;
}
