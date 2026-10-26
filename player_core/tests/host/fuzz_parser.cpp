// 解析内核变异模糊测试:对真实样本做随机字节翻转、长度字段篡改、截断,
// 在 AddressSanitizer + UBSan 下验证任何畸形输入都不会越界、崩溃或死循环。
// 用法: fuzz_parser <iterations_per_file> <file>...
// 也可用 clang -fsanitize=fuzzer -DLIBFUZZER 编译为 libFuzzer 目标。
#include "../../include/audio_tag_parser.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <string>
#include <vector>

static const wchar_t* const kExts[] = {L".flac", L".mp3", L".ogg", L".wav", L".m4a", L".dsf", L".dff", L".ape", L".ncm", L".kgma"};

static void ParseAll(const uint8_t* d, size_t n) {
    for (int lazy = 0; lazy < 2; ++lazy) {
        ParseOptions opt;
        opt.load_cover = lazy == 0;
        opt.max_cover_bytes = 1u << 20;
        AudioMetadata m;
        AudioTagParser::ParseMemory(d, n, kExts[n % (sizeof(kExts) / sizeof(kExts[0]))], m, opt);
        // 声明的逻辑大小大于实际数据 (模拟只读到头部的场景)
        AudioMetadata m2;
        AudioTagParser::ParseMemory(d, n, L".mp3", m2, opt, static_cast<uint64_t>(n) * 3 + 7);
    }
}

#ifdef LIBFUZZER
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* d, size_t n) {
    ParseAll(d, n);
    return 0;
}
#else
int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <iterations> <file>...\n", argv[0]);
        return 2;
    }
    int iters = std::atoi(argv[1]);
    std::mt19937_64 rng(20261011);
    auto t0 = std::chrono::steady_clock::now();
    size_t total = 0;
    for (int f = 2; f < argc; ++f) {
        std::ifstream in(argv[f], std::ios::binary);
        std::vector<uint8_t> orig((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (orig.size() > (4u << 20)) orig.resize(4u << 20);
        ParseAll(orig.data(), orig.size());
        // 头部集中变异:绝大多数结构字段都在前 64KB
        size_t hot = std::min<size_t>(orig.size(), 64 * 1024);
        for (int i = 0; i < iters; ++i) {
            std::vector<uint8_t> d = orig;
            int muts = 1 + static_cast<int>(rng() % 8);
            for (int k = 0; k < muts && !d.empty(); ++k) {
                size_t pos = (rng() % 4 == 0) ? rng() % d.size() : rng() % hot;
                switch (rng() % 5) {
                    case 0: d[pos] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
                    case 1: d[pos] = static_cast<uint8_t>(rng()); break;
                    case 2: // 篡改一个疑似长度字段为极端值
                        for (int b = 0; b < 4 && pos + b < d.size(); ++b) d[pos + b] = (rng() % 2) ? 0xFF : 0x7F;
                        break;
                    case 3: d.resize(rng() % (d.size() + 1)); break; // 截断
                    case 4: if (pos + 1 < d.size()) d[pos] = d[pos + 1] = 0; break;
                }
            }
            if (d.size() >= 4) ParseAll(d.data(), d.size());
            ++total;
        }
        std::fprintf(stderr, "  fuzzed %-45s x%d\n", argv[f], iters);
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::printf("fuzz OK: %zu mutated inputs, %.1fs, no crash / no sanitizer report\n", total, sec);
    return 0;
}
#endif
