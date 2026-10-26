// 跨平台歌词内核测试:KRC 解密 / 解析 / 定位 / LRC 导出 / 歌词库严格匹配 / 变异模糊测试
// 用法: test_lyric_host <work_dir>
#include "../../include/lyric_manager.hpp"
#include "../../include/tinf.hpp"
#include "../../include/audio_tag_parser.hpp"
#include "../../include/platform_io.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
using namespace lyric_core;
static int g_pass = 0, g_fail = 0;

static void Check(const char* name, bool cond, const std::string& detail = "") {
    (cond ? g_pass : g_fail)++;
    std::printf("  [%s] %s%s%s\n", cond ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
}

static std::string U8(const std::wstring& w) { return AudioTagParser::WideToUtf8(w); }
static std::wstring W(const std::string& s) { return AudioTagParser::Utf8ToWide(s); }

// python3 -c "zlib.compress(<KRC 文本>, 9)":行序故意打乱以检验排序
static const uint8_t kZlibKrc[] = {0x78,0xda,0x4d,0x4f,0xb9,0x4e,0xc3,0x40,0x10,0xed,0xfd,0x15,0x2e,0x28,0x40,0x32,0xd2,0xec,0x26,0xeb,0x63,0x65,0x22,0x51,0x42,0x49,0x43,0x84,0x95,0x62,0x4d,0xd6,0x47,0x94,0x38,0xc4,0x36,0x85,0x3b,0x08,0x22,0x15,0xfc,0x00,0xa9,0x38,0x24,0xd2,0x20,0x10,0x2d,0x48,0x7c,0x8c,0x65,0x44,0xaa,0xfc,0x02,0x3b,0x76,0x40,0xd9,0x62,0x76,0xde,0x9b,0xf7,0xe6,0x58,0x7d,0x7c,0x79,0x71,0x9f,0x6f,0xc1,0xfa,0xf5,0x34,0x4f,0xa4,0x7c,0x3f,0x1e,0xc9,0x54,0xdf,0x2e,0xa7,0x8b,0xf2,0xea,0x7e,0x47,0x71,0x79,0xcc,0x8f,0x64,0xc0,0x53,0x11,0x27,0x0a,0xf9,0x05,0x57,0x31,0x12,0x59,0xc4,0x4d,0xdb,0xf2,0xfb,0x0c,0x98,0xef,0x48,0xe2,0x30,0xea,0x07,0x60,0x51,0xbb,0xdd,0x66,0xa7,0x0e,0x98,0x82,0x5a,0x12,0xfb,0x0d,0x51,0x9d,0xc5,0x61,0x82,0xff,0x64,0x82,0x31,0x1f,0xe7,0x8a,0xa6,0x0e,0x69,0x46,0x8e,0x83,0x20,0x93,0x39,0xc7,0x74,0x28,0x92,0xf0,0x5c,0x84,0x92,0xcb,0xe2,0x70,0xe0,0x53,0x06,0x27,0xc7,0x0c,0x0e,0x06,0x67,0x7e,0xb7,0x0b,0x7b,0xaa,0x0e,0x06,0xa5,0x36,0xf4,0x5c,0x30,0x88,0x03,0x06,0x74,0xfe,0xd6,0xd2,0x5d,0xc4,0x0d,0xb7,0xab,0xbb,0x2d,0xbb,0x06,0x88,0xea,0x63,0x34,0x0f,0x6d,0xff,0x5e,0xcb,0xc4,0xca,0xcf,0xeb,0xed,0xea,0xf3,0xce,0x45,0x40,0x18,0x45,0x46,0xa0,0x36,0x8d,0x8a,0x3c,0x1a,0x69,0x1e,0xa1,0x6a,0x3b,0xa3,0x85,0x2b,0x2a,0x0b,0xab,0x9b,0x2d,0xe7,0x0b,0x17,0xb3,0x06,0x95,0x97,0x37,0x2e,0x9e,0x60,0xd4,0x4a,0xe8,0x7c,0xbf,0x3c,0x94,0xd3,0x0b,0xcd,0x33,0x37,0x8d,0x64,0x5d,0x9b,0xbf,0xe3,0xb0,0x4d,0xf9,0xf2,0x71,0x5e,0xcd,0xae,0xab,0xb7,0x59,0xf5,0xf4,0xac,0xfd,0x02,0xcb,0x1d,0x7b,0x16};
static const uint8_t kKey[16] = {0x40, 0x47, 0x61, 0x77, 0x5E, 0x32, 0x74, 0x47, 0x51, 0x36, 0x31, 0x2D, 0xCE, 0xD2, 0x6E, 0x69};

static std::vector<uint8_t> Encrypt(const uint8_t* z, size_t n) {
    std::vector<uint8_t> out = {'k', 'r', 'c', '1'};
    for (size_t i = 0; i < n; ++i) out.push_back(z[i] ^ kKey[i % 16]);
    return out;
}

// 用「不压缩」deflate 块包出合法 zlib 流,任意文本都能做成 KRC
static std::vector<uint8_t> StoredZlib(const std::string& s) {
    std::vector<uint8_t> z = {0x78, 0x01, 0x01};
    uint16_t len = static_cast<uint16_t>(s.size());
    z.push_back(len & 0xFF); z.push_back(len >> 8);
    z.push_back(~len & 0xFF); z.push_back((~len >> 8) & 0xFF);
    z.insert(z.end(), s.begin(), s.end());
    uint32_t a = 1, b = 0;
    for (unsigned char c : s) { a = (a + c) % 65521; b = (b + a) % 65521; }
    uint32_t ad = (b << 16) | a;
    for (int i = 3; i >= 0; --i) z.push_back((ad >> (i * 8)) & 0xFF);
    return z;
}

static void WriteFile(const fs::path& p, const std::vector<uint8_t>& d) {
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(d.data()), d.size());
}

int main(int argc, char** argv) {
    fs::path work = argc > 1 ? fs::path(argv[1]) : fs::path("build/lyric_work");
    fs::remove_all(work);
    fs::create_directories(work / "Lyric");

    std::printf("[1] KRC 解密与解析\n");
    const auto krc = Encrypt(kZlibKrc, sizeof(kZlibKrc));
    ParsedLyrics L;
    Check("DecodeMemory 成功", KrcDecoder::DecodeMemory(krc.data(), krc.size(), L));
    Check("元数据", L.title == W("Ref:rain") && L.artist == W("Aimer (エメ)"), U8(L.title) + " / " + U8(L.artist));
    Check("[language:] / [hash:] 等标签不算歌词行", L.lines.size() == 4, std::to_string(L.lines.size()));
    bool sorted = true;
    for (size_t i = 1; i < L.lines.size(); ++i) sorted &= L.lines[i - 1].start_ms <= L.lines[i].start_ms;
    Check("乱序行已按时间排序", sorted);
    Check("逐字文本拼接", L.lines.size() == 4 && L.lines[3].full_text == W("雨が止む"), L.lines.empty() ? "" : U8(L.lines.back().full_text));
    Check("逐字绝对时间", L.lines.size() == 4 && L.lines[3].syllables.size() == 3 && L.lines[3].syllables[2].start_ms == 13000);
    Check("缺魔数拒绝", !KrcDecoder::DecodeMemory(kZlibKrc, sizeof(kZlibKrc), L) && L.lines.size() == 4);

    std::printf("[2] 播放定位\n");
    KrcDecoder::DecodeMemory(krc.data(), krc.size(), L);
    Check("开头", L.FindLineIndexAt(0.0) == 0);
    Check("2.5s -> 第 2 行", L.FindLineIndexAt(2.5) == 1);
    Check("12.6s -> 末行", L.FindLineIndexAt(12.6) == 3);
    Check("负数 / NaN / 超大", L.FindLineIndexAt(-1) == -1 && L.FindLineIndexAt(0.0 / 0.0) == -1 && L.FindLineIndexAt(1e300) == 3);
    Check("逐字定位", L.FindSyllableIndexAt(3, 12.7) == 1 && L.FindSyllableIndexAt(3, 11.0) == -1);

    std::printf("[3] LRC 导出\n");
    const std::string lrc = L.ToStandardLrc();
    Check("时间戳格式", lrc.find("[00:12.00]") != std::string::npos && lrc.find("[00:02.28]") != std::string::npos);
    LyricManager tmp;
    const fs::path lrcPath = work / "out.lrc";
    Check("写出 .lrc", tmp.ExportLrcFile(L, W(lrcPath.u8string())) && fs::file_size(lrcPath) == lrc.size() + 3);

    std::printf("[4] 歌词库匹配 (宁缺毋错)\n");
    const std::string hash = "-b0ba5f0d646d6b851822e5e64449254c-174827828-00000000.krc";
    auto mk = [&](const std::string& name, const std::string& text) {
        WriteFile(work / "Lyric" / fs::u8path(name + hash), Encrypt(StoredZlib(text).data(), StoredZlib(text).size()));
    };
    mk("ilem - 梦良衣", "[0,1000]<0,1000,0>梦良衣\n");
    mk("ilem - 另一首", "[0,1000]<0,1000,0>另一首\n");
    mk("Aimer - Refrain", "[0,1000]<0,1000,0>refrain\n");
    mk("a-ha - Take On Me", "[0,1000]<0,1000,0>take on me\n");
    mk("周杰伦、费玉清 - 千里之外", "[0,1000]<0,1000,0>千里之外\n");
    WriteFile(work / "Lyric" / "broken - song-0123456789abcdef0123456789abcdef-1-0.krc", {'k', 'r', 'c', '1', 1, 2, 3});

    KuGouPathFinder::AddCustomDirectory(W((work / "Lyric").u8string()));
    LyricManager mgr;
    auto load = [&](const std::string& path, const std::string& title, const std::string& artist) -> std::string {
        ParsedLyrics p;
        return mgr.LoadLyricsForTrack(W(path), W(title), W(artist), p) && !p.lines.empty() ? U8(p.lines[0].full_text) : "";
    };
    Check("探测到自定义目录", mgr.GetLyricDirectories().size() >= 1);
    Check("文件名「歌手 - 歌名_kgg-dec」", load("/music/ilem - 梦良衣_kgg-dec.flac", "", "") == "梦良衣");
    Check("标签歌名 + 歌手", load("/music/x.flac", "梦良衣", "ilem") == "梦良衣");
    Check("缓存命中", load("/music/x.flac", "梦良衣", "ilem") == "梦良衣");
    Check("歌名只是子串不算 (梦)", load("/music/x.flac", "梦", "ilem").empty());
    Check("rain 不误中 Refrain", load("/music/x.flac", "rain", "").empty());
    Check("同歌手未收录的歌不借用别的歌词", load("/music/ilem - 未收录.flac", "未收录", "ilem").empty());
    Check("歌手别名括号 Aimer (エメ)", load("/music/x.flac", "Refrain", "Aimer (エメ)") == "refrain");
    Check("同名不同歌手拒绝", load("/music/x.flac", "Refrain", "Someone Else").empty());
    Check("全角 / 大小写", load("/music/x.flac", "ＲＥＦＲＡＩＮ", "") == "refrain");
    Check("名字里带连字符 (a-ha)", load("/music/a-ha - Take On Me.mp3", "", "") == "take on me");
    Check("多歌手任一相符", load("/music/x.flac", "千里之外", "费玉清") == "千里之外");
    Check("标签不符时回退到文件名", load("/music/ilem - 梦良衣.flac", "梦良衣 (Live)", "ilem") == "梦良衣");
    Check("损坏的 krc 安全失败", load("/music/broken - song.flac", "", "").empty());
    mk("ilem - 未收录", "[0,1000]<0,1000,0>新下载\n");
    Check("酷狗刚下载的歌词立即可见", load("/music/ilem - 未收录.flac", "未收录", "ilem") == "新下载");

    std::printf("[5] 变异模糊测试 (ASan)\n");
    std::mt19937 rng(7);
    int decoded = 0;
    for (int it = 0; it < 20000; ++it) {
        std::vector<uint8_t> m = krc;
        const int flips = 1 + rng() % 6;
        for (int k = 0; k < flips; ++k) m[4 + rng() % (m.size() - 4)] ^= static_cast<uint8_t>(1u << (rng() % 8));
        if (rng() % 4 == 0) m.resize(4 + rng() % (m.size() - 4));
        ParsedLyrics p;
        if (KrcDecoder::DecodeMemory(m.data(), m.size(), p)) {
            ++decoded;
            p.FindLineIndexAt(rng() % 20000 / 1000.0);
            p.ToStandardLrc();
        }
        std::string junk(rng() % 300, '\0');
        for (auto& c : junk) c = "[]<>,0123456789:abc\n"[rng() % 20];
        KrcDecoder::ParseRawKrcText(junk, p);
    }
    Check("20000 次变异无崩溃", true, std::to_string(decoded) + " 次仍可解出");

    std::printf("\n歌词内核: 通过 %d 项 / 失败 %d 项\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
