// 酷狗桥接宿主测试: 用随机掩码表合成 .kgma,验证缓存命中、同名不串歌、m4a 不重复解密、源文件变化后重新解密、并发批量解密
// 用法: test_kugou_bridge_host <work_dir>
#include "../../include/kugou_track_bridge.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace lyric_core;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static std::vector<uint8_t> g_mask(1 << 16);
static std::mt19937 g_rng(11);

static std::vector<uint8_t> ReadAll(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
}

// 头部 magic 决定解密器嗅探出的扩展名 ("fLaC" -> .flac, "....ftyp" -> .m4a)
static std::vector<uint8_t> MakePlain(const char* magic, size_t magicLen, size_t magicOff, size_t n) {
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>(g_rng());
    std::memcpy(v.data() + magicOff, magic, magicLen);
    return v;
}

static void WriteKgma(const fs::path& path, const std::vector<uint8_t>& plain) {
    std::vector<uint8_t> enc = plain;
    uint8_t key17[17];
    for (int i = 0; i < 16; ++i) key17[i] = static_cast<uint8_t>(g_rng());
    key17[16] = 0;
    for (size_t i = 0; i < enc.size(); ++i) {
        uint8_t x = enc[i];
        enc[i] = static_cast<uint8_t>((x ^ (x << 4)) ^ key17[i % 17] ^ g_mask[i % g_mask.size()]);
    }
    std::vector<uint8_t> hdr(1024, 0);
    static const uint8_t kKgmMagic[16] = {0x7C, 0xD5, 0x32, 0xEB, 0x86, 0x02, 0x7F, 0x4B,
                                          0xA8, 0xAF, 0xA6, 0x8E, 0x0F, 0xFF, 0x99, 0x14};
    std::memcpy(hdr.data(), kKgmMagic, 16);
    uint32_t hlen = 1024;
    std::memcpy(&hdr[16], &hlen, 4);
    std::memcpy(&hdr[28], key17, 16);
    fs::create_directories(path.parent_path());
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(hdr.data()), hdr.size());
    o.write(reinterpret_cast<const char*>(enc.data()), enc.size());
}

static size_t CountFiles(const fs::path& dir) {
    size_t n = 0;
    for (const auto& e : fs::recursive_directory_iterator(dir))
        if (e.is_regular_file()) ++n;
    return n;
}

int main(int argc, char** argv) {
    fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "kugou_bridge_host";
    fs::remove_all(work);
    fs::create_directories(work);
    for (auto& b : g_mask) b = static_cast<uint8_t>(g_rng());
    fs::path maskPath = work / "test.mask";
    std::ofstream(maskPath, std::ios::binary).write(reinterpret_cast<const char*>(g_mask.data()), g_mask.size());

    const fs::path cache = work / "cache";
    KuGouTrackBridge bridge;
    bridge.SetMaskPath(maskPath.wstring());
    bridge.SetCacheOutputDir(cache.wstring());

    const auto flacA = MakePlain("fLaC", 4, 0, 300000);
    const auto flacB = MakePlain("fLaC", 4, 0, 200000);
    const auto m4a = MakePlain("ftypM4A ", 8, 4, 150000);
    const fs::path a = work / "KugouMusic" / "歌手 - 同名歌.kgma";
    const fs::path b = work / "GEDAN" / "KugouMusic" / "歌手 - 同名歌.kgma";
    const fs::path m = work / "KugouMusic" / "歌手 - 苹果格式.kgma";
    WriteKgma(a, flacA);
    WriteKgma(b, flacB);
    WriteKgma(m, m4a);

    std::printf("[1] 解密与同名文件不串歌\n");
    KuGouTrackEntry ea, eb;
    CHECK(bridge.PreparePlayableTrack(a.wstring(), ea) && ea.is_ready_to_play);
    CHECK(bridge.PreparePlayableTrack(b.wstring(), eb) && eb.is_ready_to_play);
    CHECK(ea.playable_path != eb.playable_path);
    CHECK(ReadAll(ea.playable_path) == flacA);
    CHECK(ReadAll(eb.playable_path) == flacB);

    std::printf("[2] m4a 产物二次请求命中缓存,不生成 (2)\n");
    KuGouTrackEntry em1, em2;
    CHECK(bridge.PreparePlayableTrack(m.wstring(), em1));
    CHECK(fs::path(em1.playable_path).extension() == ".m4a");
    const size_t before = CountFiles(cache);
    CHECK(bridge.PreparePlayableTrack(m.wstring(), em2));
    CHECK(em1.playable_path == em2.playable_path);
    CHECK(CountFiles(cache) == before);

    std::printf("[3] 源文件变化后重新解密\n");
    const auto flacA2 = MakePlain("fLaC", 4, 0, 310000);
    WriteKgma(a, flacA2);
    fs::last_write_time(a, fs::last_write_time(a) + std::chrono::seconds(5));
    KuGouTrackEntry ea2;
    CHECK(bridge.PreparePlayableTrack(a.wstring(), ea2));
    CHECK(ea2.playable_path != ea.playable_path);
    CHECK(ReadAll(ea2.playable_path) == flacA2);

    std::printf("[4] 扫描:非加密直接可播,已解密的加密文件命中缓存\n");
    std::ofstream(work / "KugouMusic" / "普通.mp3", std::ios::binary) << "ID3 not really an mp3";
    std::ofstream(work / "KugouMusic" / "说明.txt") << "skip";
    auto entries = bridge.ScanKuGouLibrary((work / "KugouMusic").wstring());
    CHECK(entries.size() == 3);
    for (const auto& e : entries) CHECK(e.is_ready_to_play);

    std::printf("[5] 并发批量解密:同一首歌只解密一次\n");
    std::vector<std::wstring> batch;
    std::vector<std::vector<uint8_t>> plains;
    for (int i = 0; i < 6; ++i) {
        fs::path p = work / "batch" / ("曲" + std::to_string(i) + ".kgma");
        plains.push_back(MakePlain("fLaC", 4, 0, 120000 + i * 1000));
        WriteKgma(p, plains.back());
        batch.push_back(p.wstring());
        batch.push_back(p.wstring()); // 重复请求
    }
    const size_t cacheBefore = CountFiles(cache);
    size_t lastDone = 0;
    bool monotonic = true;
    size_t ok = bridge.BatchPrepare(batch, [&](size_t done, size_t total, const std::wstring&) {
        if (done != lastDone + 1 || total != 12) monotonic = false;
        lastDone = done;
    }, 4);
    CHECK(ok == batch.size());
    CHECK(monotonic && lastDone == 12);
    CHECK(CountFiles(cache) == cacheBefore + 6);
    for (int i = 0; i < 6; ++i) {
        KuGouTrackEntry e;
        CHECK(bridge.PreparePlayableTrack(batch[i * 2], e));
        CHECK(ReadAll(e.playable_path) == plains[i]);
    }

    std::printf("[6] 不存在的文件与解不了的文件返回 false,且不会卡住后续调用\n");
    KuGouTrackEntry ex;
    CHECK(!bridge.PreparePlayableTrack((work / "不存在.kgma").wstring(), ex));
    fs::path bad = work / "坏文件.kgma";
    std::ofstream(bad, std::ios::binary) << "garbage";
    CHECK(!bridge.PreparePlayableTrack(bad.wstring(), ex) && !ex.is_ready_to_play);
    CHECK(!bridge.PreparePlayableTrack(bad.wstring(), ex));

    std::printf("[7] 没有 kgm.mask 时用内置掩码解密 (含超过 80MB 掩码长度的偏移)\n");
    {
        DecryptorRegistry::Instance().SetKgmMaskPath((work / "不存在.mask").wstring());
        // 内置掩码流 Z[i] = 用全零密钥解密全零数据的结果;加密 = f(明文 ^ Z) ^ key
        const auto plain = MakePlain("fLaC", 4, 0, 200000);
        uint8_t key17[17];
        for (int i = 0; i < 16; ++i) key17[i] = static_cast<uint8_t>(g_rng());
        key17[16] = 0;
        const uint8_t zeroKey[17] = {0};
        std::vector<uint8_t> z(plain.size(), 0);
        KgmDecryptBlockBuiltin(z.data(), z.size(), 0, zeroKey);
        std::vector<uint8_t> enc(plain.size());
        for (size_t i = 0; i < plain.size(); ++i) {
            uint8_t x = static_cast<uint8_t>(plain[i] ^ z[i]);
            enc[i] = static_cast<uint8_t>((x ^ (x << 4)) ^ key17[i % 17]);
        }
        std::vector<uint8_t> hdr(1024, 0);
        static const uint8_t kKgmMagic[16] = {0x7C, 0xD5, 0x32, 0xEB, 0x86, 0x02, 0x7F, 0x4B,
                                              0xA8, 0xAF, 0xA6, 0x8E, 0x0F, 0xFF, 0x99, 0x14};
        std::memcpy(hdr.data(), kKgmMagic, 16);
        uint32_t hlen = 1024;
        std::memcpy(&hdr[16], &hlen, 4);
        std::memcpy(&hdr[28], key17, 16);
        fs::path p = work / "内置掩码.kgma";
        std::ofstream o(p, std::ios::binary);
        o.write(reinterpret_cast<const char*>(hdr.data()), hdr.size());
        o.write(reinterpret_cast<const char*>(enc.data()), enc.size());
        o.close();
        const Decryptor* dec = DecryptorRegistry::Instance().ByExtension(L".kgma");
        CHECK(dec && dec->IsAvailable());
        KuGouTrackEntry e;
        CHECK(bridge.PreparePlayableTrack(p.wstring(), e));
        CHECK(e.is_ready_to_play && ReadAll(e.playable_path) == plain);

        // 分块解密与整块一致 (偏移跨过 272 字节周期、17 字节密钥周期,以及 81,979,072 之后)
        for (uint64_t base : {0ull, 81979072ull - 100, 81979072ull + 12345}) {
            std::vector<uint8_t> a(5000), b;
            for (auto& x : a) x = static_cast<uint8_t>(g_rng());
            b = a;
            KgmDecryptBlockBuiltin(a.data(), a.size(), base, key17);
            KgmDecryptBlockBuiltin(b.data(), 1234, base, key17);
            KgmDecryptBlockBuiltin(b.data() + 1234, b.size() - 1234, base + 1234, key17);
            CHECK(a == b);
        }
    }

    std::printf(g_fail ? "\n酷狗桥接: %d 项失败\n" : "\n酷狗桥接: 全部通过\n", g_fail);
    return g_fail ? 1 : 0;
}
