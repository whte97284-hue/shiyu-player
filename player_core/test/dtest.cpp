// 端到端验证:KGG 解密 → 自动补封面
// 直接调用内核 DecryptorRegistry,走的就是程序里那条路径。
#include "decryptor.hpp"
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

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

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
    if (argc < 3) {
        std::printf("usage: dtest <tools目录> <kgg文件> [kgg文件...]\n");
        return 1;
    }
    // tools 目录 = 含 kgg/kgg-dec.exe 与 kgg/infra.dll 的那个
    const std::wstring tools = W(argv[1]);
    auto& reg = DecryptorRegistry::Instance();
    reg.SetToolsDir(tools);
    // 输出到源目录旁边的 out 子目录
    const std::wstring outDir = tools + L"/../_decrypt_out";

    int ok = 0, fail = 0;
    for (int i = 2; i < argc; ++i) {
        const std::wstring src = W(argv[i]);
        std::printf("\n%s\n", argv[i]);
        DecryptResult r;
        DecryptOptions opt;
        opt.output_dir = outDir;
        auto t0 = Clock::now();
        r = reg.ByExtension(L".kgg")->Decrypt(src, opt);
        auto t1 = Clock::now();
        const double t_dec = Ms(t0, t1);
        if (!r.ok) {
            std::printf("  ✗ 解密失败: %s  (%.0f ms)\n", N(r.message).c_str(), t_dec);
            ++fail;
            continue;
        }
        std::printf("  解密 ✔ %.0f ms → %s\n", t_dec, N(r.output).c_str());

        t0 = Clock::now();
        const bool has = cover_core::FlacHasCover(r.output);
        t1 = Clock::now();
        std::printf("  产物封面: %s  (%.2f ms)  ← 这一步由 decryptor 内部的 EnsureCover 完成\n",
                    has ? "★ 有" : "✗ 无", Ms(t0, t1));
        if (has) ++ok;
        else ++fail;
    }
    std::printf("\n成功 %d / 失败 %d\n", ok, fail);
    return ok > 0 ? 0 : 2;
}
