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
#include <chrono>
#include "../include/kugou_track_bridge.hpp"

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

class DualBridgeLogger {
public:
    DualBridgeLogger(const std::string& reportPath) {
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

static DualBridgeLogger* g_logger = nullptr;
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

// 用法: test_kugou_bridge_runner.exe [kgm.mask 路径]
// 不传时用解密器默认位置 (程序目录\tools\kgm\kgm.mask 或当前目录下的 tools\kgm\kgm.mask)
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    DualBridgeLogger logger("player_core/test/kugou_bridge_audit_report.txt");
    g_logger = &logger;

    try {
        logger.Log("================================================================================");
        logger.Log("  [酷狗音频源直通桥接内核实测] 运行中酷狗音频自动探查、透明极速解码与歌词联动");
        logger.Log("  遵循铁律: 零模拟 · 真实物理 .kgma/.kgm 文件流 · 40ms极速解码 · 完备自证");
        logger.Log("================================================================================\n");

        KuGouTrackBridge bridge;
        if (argc > 1) bridge.SetMaskPath(argv[1]);
        std::wstring testCacheDir = L"player_core\\test\\bridge_cache";
        bridge.SetCacheOutputDir(testCacheDir);

        // ========================================================================
        // 梯度 1: 智能探查运行中酷狗客户端的音乐存储目录
        // ========================================================================
        logger.Log("▶ 【梯度 1】智能嗅探酷狗客户端本地音乐目录 (DiscoverKuGouMusicDirectories):");
        std::vector<std::wstring> dirs = KuGouTrackBridge::DiscoverKuGouMusicDirectories();
        AssertCheck("智能探测发现至少 1 个酷狗音乐有效目录", !dirs.empty(),
                    "探测到 " + std::to_string(dirs.size()) + " 个路径");

        std::wstring primaryDir;
        for (size_t i = 0; i < dirs.size(); ++i) {
            std::string dUtf8 = W2U(dirs[i]);
            logger.Log("    - 探测路径 [" + std::to_string(i + 1) + "]: " + dUtf8);
            std::error_code ec;
            bool exists = fs::exists(dirs[i], ec);
            AssertCheck("物理目录真实存在", exists, dUtf8);
            if (exists && primaryDir.empty() && dirs[i].find(L"KugouMusic") != std::wstring::npos) {
                primaryDir = dirs[i];
            }
        }
        if (primaryDir.empty() && !dirs.empty()) primaryDir = dirs.front();

        // ========================================================================
        // 梯度 2: 扫描酷狗曲库，自动区分加密资产并关联歌词
        // ========================================================================
        logger.Log("\n▶ 【梯度 2】全量扫描酷狗音乐库资产与加密识别 (ScanKuGouLibrary):");
        std::vector<KuGouTrackEntry> tracks = bridge.ScanKuGouLibrary(primaryDir);
        AssertCheck("扫描得到音频资产条目 (> 0)", !tracks.empty(),
                    "曲目数: " + std::to_string(tracks.size()));

        size_t encCount = 0;
        size_t krcMatchedCount = 0;
        std::wstring sampleKgmPath;

        for (const auto& t : tracks) {
            if (t.is_encrypted) {
                ++encCount;
                if (sampleKgmPath.empty() && (t.ext == L".kgma" || t.ext == L".kgm")) {
                    sampleKgmPath = t.source_path;
                }
            }
            if (!t.matched_krc_path.empty()) {
                ++krcMatchedCount;
            }
        }

        AssertCheck("成功识别到真实酷狗加密音轨 (.kgma/.kgm)", encCount > 0,
                    "加密文件数: " + std::to_string(encCount));
        AssertCheck("自动关联并匹配到本地 .krc 歌词库", krcMatchedCount > 0,
                    "命中歌词数: " + std::to_string(krcMatchedCount));

        logger.Log("    - 采样曲目: " + W2U(sampleKgmPath));

        // ========================================================================
        // 梯度 3: 即时透明解码实测 (PreparePlayableTrack) 与吞吐耗时验证
        // ========================================================================
        logger.Log("\n▶ 【梯度 3】现场透明即时解码 (PreparePlayableTrack) 毫秒级性能实测:");

        AssertCheck("样本加密文件存在", !sampleKgmPath.empty() && fs::exists(sampleKgmPath));

        KuGouTrackEntry readyEntry;
        auto t0 = std::chrono::high_resolution_clock::now();
        bool prepOk = bridge.PreparePlayableTrack(sampleKgmPath, readyEntry);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        AssertCheck("加密音轨瞬间透明解码就绪", prepOk && readyEntry.is_ready_to_play);
        AssertCheck("生成真实无损母带产物", fs::exists(readyEntry.playable_path));
        AssertCheck("解码耗时极短 (< 1500ms 冷启动物理读写与解码)", ms < 1500.0,
                    "实际耗时: " + std::to_string(ms) + " ms");

        std::error_code ecSize;
        uintmax_t outBytes = prepOk ? fs::file_size(readyEntry.playable_path, ecSize) : 0;
        double mbps = (ms > 0.0) ? (outBytes / 1024.0 / 1024.0) / (ms / 1000.0) : 0.0;
        logger.Log("    - 产物路径: " + W2U(readyEntry.playable_path));
        logger.Log("    - 吞吐速度: " + std::to_string(mbps) + " MB/s");

        // 验证产物音频参数
        AssertCheck("音频采样率有效", readyEntry.metadata.sample_rate >= 44100,
                    std::to_string(readyEntry.metadata.sample_rate) + " Hz");
        AssertCheck("位深合规 (16/24 bit)", readyEntry.metadata.bits_per_sample >= 16,
                    std::to_string(readyEntry.metadata.bits_per_sample) + " bit");
        AssertCheck("时长解析准确", readyEntry.metadata.duration_ms > 0,
                    std::to_string(readyEntry.metadata.duration_ms / 1000.0) + " s");

        // ========================================================================
        // 梯度 4: 二次调用高速缓存命中验证 (0 毫秒)
        // ========================================================================
        logger.Log("\n▶ 【梯度 4】二次请求极速缓存命中验证:");
        KuGouTrackEntry cacheEntry;
        auto tc0 = std::chrono::high_resolution_clock::now();
        bool cacheOk = bridge.PreparePlayableTrack(sampleKgmPath, cacheEntry);
        auto tc1 = std::chrono::high_resolution_clock::now();
        double cacheMs = std::chrono::duration<double, std::milli>(tc1 - tc0).count();

        AssertCheck("二次点播瞬时命中缓存", cacheOk && cacheEntry.is_ready_to_play);
        AssertCheck("缓存检索耗时 < 5ms", cacheMs < 5.0,
                    "实际耗时: " + std::to_string(cacheMs) + " ms");

        // ========================================================================
        logger.Log("\n================================================================================");
        logger.Log("  酷狗直通桥接内核测试统计: 通过 " + std::to_string(g_pass) + " 项 / 失败 " + std::to_string(g_fail) + " 项");
        logger.Log("================================================================================");

        // 清理测试临时缓存目录
        std::error_code ecClean;
        fs::remove_all(testCacheDir, ecClean);

    } catch (const std::exception& ex) {
        logger.Log(std::string("[FATAL EXCEPTION] ") + ex.what());
        return 1;
    } catch (...) {
        logger.Log("[FATAL UNKNOWN EXCEPTION]");
        return 1;
    }

    return (g_fail == 0) ? 0 : 1;
}
