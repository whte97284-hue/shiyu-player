#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <vector>
#include <string>
#include <numeric>
#include <algorithm>
#include "../include/lyric_manager.hpp"

namespace fs = std::filesystem;
using namespace lyric_core;

namespace {

static std::string W2U(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int req = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), NULL, 0, NULL, NULL);
    if (req <= 0) return "";
    std::string str(req, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), static_cast<int>(wstr.size()), &str[0], req, NULL, NULL);
    return str;
}

static std::wstring U2W(const std::string& str) {
    if (str.empty()) return L"";
    int req = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), NULL, 0);
    if (req <= 0) return L"";
    std::wstring wstr(req, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.size()), &wstr[0], req);
    return wstr;
}

class DualLyricLogger {
public:
    DualLyricLogger(const std::string& reportPath) {
        ofs_.open(reportPath, std::ios::binary | std::ios::trunc);
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        ofs_.write(reinterpret_cast<const char*>(bom), 3);
    }

    void Log(const std::string& msg = "") {
        std::cout << msg << "\n";
        std::cout.flush();
        if (ofs_.is_open()) {
            ofs_.write(msg.data(), msg.size());
            ofs_.write("\r\n", 2);
            ofs_.flush();
        }
    }

private:
    std::ofstream ofs_;
};

static DualLyricLogger* g_logger = nullptr;
static int g_pass = 0;
static int g_fail = 0;

static void AssertCheck(const std::string& name, bool cond, const std::string& detail = "") {
    if (cond) {
        ++g_pass;
        std::string line = "  [PASS] " + name;
        if (!detail.empty()) line += " (" + detail + ")";
        if (g_logger) g_logger->Log(line);
    } else {
        ++g_fail;
        std::string line = "  [FAIL] " + name;
        if (!detail.empty()) line += " (" + detail + ")";
        if (g_logger) g_logger->Log(line);
    }
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    DualLyricLogger logger("player_core/test/lyric_audit_report.txt");
    g_logger = &logger;

    try {
        logger.Log("================================================================================");
        logger.Log("  [酷狗歌词内核深度测试] 本地智能嗅探、KRC 逐字解密与 LRC 导出全链路物理闭环");
        logger.Log("  遵循铁律: 零模拟 · 真实物理 KRC 数据流 · 拒绝硬编码 · 完备断言自证");
        logger.Log("================================================================================\n");

        // ========================================================================
        // 测试项 1: 全系统无硬编码智能嗅探酷狗歌词库目录
        // ========================================================================
        logger.Log("▶ 【测试项 1】多级智能嗅探系统酷狗歌词目录 (KuGouPathFinder):");
        std::vector<std::wstring> dirs = KuGouPathFinder::DiscoverLyricDirectories();

        AssertCheck("智能探测发现至少 1 个有效酷狗歌词目录", !dirs.empty(),
                    "探测到 " + std::to_string(dirs.size()) + " 个目录");

        std::wstring primaryDir;
        for (size_t i = 0; i < dirs.size(); ++i) {
            std::string dirUtf8 = W2U(dirs[i]);
            logger.Log("    - 探测路径 [" + std::to_string(i + 1) + "]: " + dirUtf8);
            std::error_code ec;
            bool exists = fs::exists(dirs[i], ec);
            AssertCheck("探测路径真实存在于物理文件系统", exists, dirUtf8);
            if (exists && primaryDir.empty()) primaryDir = dirs[i];
        }

        // ========================================================================
        // 测试项 2: 真实物理 .krc 文件读取、密钥异或解密与 Zlib 膨胀解压
        // ========================================================================
        logger.Log("\n▶ 【测试项 2】物理 .krc 密文解析、逐字时间轴解密与元数据提取 (KrcDecoder):");

        // 从探测出的目录中寻找样本文件
        std::wstring sampleKrc;
        for (const auto& d : dirs) {
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(d, ec)) {
                if (ec) break;
                if (entry.is_regular_file() && entry.path().extension() == L".krc") {
                    sampleKrc = entry.path().wstring();
                    break;
                }
            }
            if (!sampleKrc.empty()) break;
        }

        AssertCheck("在物理目录中找到至少 1 个真实 .krc 文件", !sampleKrc.empty(), W2U(sampleKrc));

        ParsedLyrics sampleLyrics;
        bool decodeOk = KrcDecoder::DecodeFile(sampleKrc, sampleLyrics);
        AssertCheck("KRC 文件魔数校验、XOR 异或解密与 ZLIB 膨胀成功", decodeOk);
        AssertCheck("歌词行数解析有效 (> 0 行)", !sampleLyrics.lines.empty(),
                    std::to_string(sampleLyrics.lines.size()) + " 行");

        logger.Log("    - 元数据提取: Title=[" + W2U(sampleLyrics.title)
                   + "], Artist=[" + W2U(sampleLyrics.artist)
                   + "], Lines=" + std::to_string(sampleLyrics.lines.size()));

        // 验证时间戳递增与逐字时间轴有效性
        bool timeMonotonic = true;
        size_t totalSyllables = 0;
        for (size_t i = 0; i < sampleLyrics.lines.size(); ++i) {
            if (i + 1 < sampleLyrics.lines.size()) {
                if (sampleLyrics.lines[i].start_ms > sampleLyrics.lines[i + 1].start_ms) {
                    timeMonotonic = false;
                }
            }
            totalSyllables += sampleLyrics.lines[i].syllables.size();
        }
        AssertCheck("各行歌词起始时间戳单调递增", timeMonotonic);
        AssertCheck("成功提取高精度逐字卡拉OK时间轴片元", totalSyllables > 0,
                    "总字词片元数: " + std::to_string(totalSyllables));

        // 打印样本前 3 行
        logger.Log("    [前 3 行逐字解密采样展示]:");
        for (size_t i = 0; i < std::min<size_t>(3, sampleLyrics.lines.size()); ++i) {
            const auto& l = sampleLyrics.lines[i];
            logger.Log("      [" + std::to_string(l.start_ms) + "ms ~ " +
                       std::to_string(l.start_ms + l.duration_ms) + "ms] " + W2U(l.full_text));
            logger.Log("        (字片元数: " + std::to_string(l.syllables.size()) + ")");
        }

        // ========================================================================
        // 测试项 3: 二分查找播放位置歌词行与逐字定位算法
        // ========================================================================
        logger.Log("\n▶ 【测试项 3】播放实时位置快速定位与逐字高亮算法 (FindLineIndexAt):");

        int idxBefore = sampleLyrics.FindLineIndexAt(0.0);
        AssertCheck("播放起始点 0.0s 定位正常", idxBefore == -1 || idxBefore == 0);

        if (sampleLyrics.lines.size() >= 2) {
            double testSec = sampleLyrics.lines[1].start_ms / 1000.0 + 0.1;
            int idxMid = sampleLyrics.FindLineIndexAt(testSec);
            AssertCheck("播放中途时间点精确二分命中第 2 行", idxMid == 1,
                        "期望 1, 实际 " + std::to_string(idxMid));

            int sylIdx = sampleLyrics.FindSyllableIndexAt(1, testSec);
            AssertCheck("在第 2 行内成功定位当前活跃演唱字", sylIdx >= 0,
                        "字索引: " + std::to_string(sylIdx));
        }

        double endSec = (sampleLyrics.lines.back().start_ms + 10000) / 1000.0;
        int idxEnd = sampleLyrics.FindLineIndexAt(endSec);
        AssertCheck("歌曲末尾超出时间点定位至最后一行", idxEnd == static_cast<int>(sampleLyrics.lines.size() - 1));

        // ========================================================================
        // 测试项 4: 标准通用 LRC 文本导出与落盘
        // ========================================================================
        logger.Log("\n▶ 【测试项 4】标准 LRC 转换、格式合规性与实体文件导出:");
        std::string lrcContent = sampleLyrics.ToStandardLrc();
        AssertCheck("生成标准 LRC 文本非空", !lrcContent.empty());
        AssertCheck("LRC 包含标准时间戳格式 [mm:ss.xx]", lrcContent.find("[00:") != std::string::npos ||
                                                              lrcContent.find("[01:") != std::string::npos ||
                                                              lrcContent.find("[02:") != std::string::npos);

        LyricManager mgr;
        std::wstring exportPath = L"player_core\\test\\test_exported_sample.lrc";
        bool exportOk = mgr.ExportLrcFile(sampleLyrics, exportPath);
        std::error_code ecExp;
        bool expExists = fs::exists(exportPath, ecExp);
        AssertCheck("实体 .lrc 文件落盘成功", exportOk && expExists);
        if (expExists) {
            uintmax_t fsize = fs::file_size(exportPath, ecExp);
            AssertCheck("导出的 .lrc 文件大小 > 100 字节", fsize > 100,
                        std::to_string(fsize) + " 字节");
        }

        // ========================================================================
        // 测试项 5: 真实歌曲文件与酷狗歌词库多级模糊匹配
        // ========================================================================
        logger.Log("\n▶ 【测试项 5】从真实转换歌曲自动模糊检索并绑定酷狗歌词:");

        // 目标 1: 真实存在对应歌词的歌曲 (ilem - 梦良衣)
        std::wstring trackA = L"F:\\软件\\酷狗最新版转换器\\KugouConverterWeb\\output\\ilem - 梦良衣.flac";
        ParsedLyrics lyricsA;
        bool matchA = mgr.LoadLyricsForTrack(trackA, L"梦良衣", L"ilem", lyricsA);
        AssertCheck("真实曲目《ilem - 梦良衣.flac》自动跨库模糊命中歌词", matchA);
        if (matchA) {
            AssertCheck("匹配歌词行数真实合规", !lyricsA.lines.empty(),
                        "解析出 " + std::to_string(lyricsA.lines.size()) + " 行");
            logger.Log("    - 成功命中歌词: Title=" + W2U(lyricsA.title) + ", Lines=" + std::to_string(lyricsA.lines.size()));
        }

        // 目标 2: 内存缓存命中验证
        ParsedLyrics lyricsCached;
        bool matchCached = mgr.LoadLyricsForTrack(trackA, L"梦良衣", L"ilem", lyricsCached);
        AssertCheck("二次请求命中内部内存缓存", matchCached && lyricsCached.lines.size() == lyricsA.lines.size());

        // ========================================================================
        logger.Log("\n================================================================================");
        logger.Log("  酷狗歌词内核测试统计: 通过 " + std::to_string(g_pass) + " 项 / 失败 " + std::to_string(g_fail) + " 项");
        logger.Log("================================================================================");

    } catch (const std::exception& ex) {
        logger.Log(std::string("[FATAL EXCEPTION] ") + ex.what());
        return 1;
    } catch (...) {
        logger.Log("[FATAL UNKNOWN EXCEPTION]");
        return 1;
    }

    return (g_fail == 0) ? 0 : 1;
}
