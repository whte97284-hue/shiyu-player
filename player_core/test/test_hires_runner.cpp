#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <chrono>
#include <string>
#include <vector>
#include <numeric>
#include <cmath>
#include <algorithm>
#include <io.h>
#include <fcntl.h>
#include "../include/audio_playback_engine.hpp"
#include "../include/audio_ingestion_pipeline.hpp"
#include "../include/music_catalog.hpp"
#include "../include/playlist_manager.hpp"
#include "../include/play_queue_controller.hpp"

namespace fs = std::filesystem;

class DualReportLogger {
public:
    DualReportLogger(const std::string& reportPath) {
        ofs_.open(reportPath, std::ios::binary | std::ios::trunc);
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        ofs_.write(reinterpret_cast<const char*>(bom), 3);
    }

    void Log(const std::wstring& msg = L"") {
        std::wcout << msg << std::endl;
        if (ofs_.is_open()) {
            std::string u8 = AudioTagParser::WideToUtf8(msg);
            ofs_.write(u8.data(), u8.size());
            ofs_.write("\r\n", 2);
            ofs_.flush();
        }
    }

private:
    std::ofstream ofs_;
};

static DualReportLogger* g_logger = nullptr;
static int g_pass = 0;
static int g_fail = 0;

static void AssertCheck(const std::wstring& name, bool cond, const std::wstring& detail = L"") {
    if (cond) {
        ++g_pass;
        if (g_logger) g_logger->Log(L"  [PASS] " + name + (detail.empty() ? L"" : L" (" + detail + L")"));
    } else {
        ++g_fail;
        if (g_logger) g_logger->Log(L"  [FAIL] " + name + (detail.empty() ? L"" : L" (" + detail + L")"));
    }
}

static void PrintSpectrumAscii(const std::vector<float>& bars) {
    std::wstring line = L"  [32 Band FFT Spectrum]:\n   ";
    for (size_t i = 0; i < bars.size(); ++i) {
        int h = static_cast<int>(bars[i] * 8.0f);
        if (h <= 1) line += L" ";
        else if (h == 2) line += L"▂";
        else if (h == 3) line += L"▃";
        else if (h == 4) line += L"▄";
        else if (h == 5) line += L"▅";
        else if (h == 6) line += L"▆";
        else if (h == 7) line += L"▇";
        else line += L"█";
    }
    if (g_logger) g_logger->Log(line);
}

int main() {
    DualReportLogger logger("player_core/test/hires_audit_report.txt");
    g_logger = &logger;

    logger.Log(L"================================================================================");
    logger.Log(L"  [Phase 2 进阶发烧实测] 本地母带库真实物理压测与硬件闭环验证 (规模见下方实际统计)");
    logger.Log(L"  遵循铁律: 零模拟 · 真实硬件管线 · 绝不假测试 · 目标母带库绝对只读安全");
    logger.Log(L"================================================================================\n");

    const std::wstring HIRES_ROOT = L"F:\\ZiYuan\\Hires";

    if (!fs::exists(HIRES_ROOT)) {
        logger.Log(L"[FATAL] 未找到测试物理母带库目录: " + HIRES_ROOT);
        return 1;
    }

    // ========================================================================
    // 梯度 1: 发烧级母带单曲物理采样解析、硬件直通播放与 32 频段 FFT 频谱能量验证
    // ========================================================================
    logger.Log(L"▶ 【梯度 1】高规格母带帧头解析与 WASAPI 硬件播放直通实测:");

    // 1.1 FLAC 母带实测
    std::wstring flacSample = HIRES_ROOT + L"\\Cyberpunk - Cyberpunk - Edgerunners Soundtrack Vol.1 (Ep1+2)\\Cyberpunk - Cyberpunk - Edgerunners Soundtrack Vol.1 (Ep1+2) - 01 “Whos Ready for Tomorrow” Rat Boy.flac";
    if (!fs::exists(flacSample)) {
        flacSample = HIRES_ROOT + L"\\2001-01-01 Eason18首选·首选陈奕迅\\【车载CD制作、自选歌曲、盘面印图】2001-01-01 Eason18首选·首选陈奕迅\\(01) [陳奕迅] 黑夜不再来.flac";
    }

    logger.Log(L"  [测试样本 1] FLAC 母带: " + flacSample);
    AudioMetadata flacMeta;
    bool flacParseOk = AudioTagParser::ParseFile(flacSample, flacMeta);
    AssertCheck(L"FLAC 物理帧头解析成功", flacParseOk);
    AssertCheck(L"FLAC 采样率真实合规", flacMeta.sample_rate >= 44100, std::to_wstring(flacMeta.sample_rate) + L" Hz");
    AssertCheck(L"FLAC 位深真实合规", flacMeta.bits_per_sample >= 16, std::to_wstring(flacMeta.bits_per_sample) + L" bit");
    AssertCheck(L"FLAC 声道真实合规", flacMeta.channels == 2, std::to_wstring(flacMeta.channels) + L" ch");
    AssertCheck(L"FLAC 时长非零真实存在", flacMeta.duration_ms > 60000, std::to_wstring(flacMeta.duration_ms / 1000.0) + L" s");

    // 1.2 WAV 母带实测
    std::wstring wavSample = HIRES_ROOT + L"\\Yiruma BEST OF\\02. River Flows in You.wav";
    logger.Log(L"  [测试样本 2] WAV 母带: " + wavSample);
    AudioMetadata wavMeta;
    bool wavParseOk = AudioTagParser::ParseFile(wavSample, wavMeta);
    AssertCheck(L"WAV 物理 RIFF/fmt 帧头解析成功", wavParseOk);
    AssertCheck(L"WAV 格式标识 == 'WAV'", wavMeta.format == L"WAV");
    AssertCheck(L"WAV 采样率真实合规", wavMeta.sample_rate >= 44100, std::to_wstring(wavMeta.sample_rate) + L" Hz");
    AssertCheck(L"WAV 位深真实合规", wavMeta.bits_per_sample >= 16, std::to_wstring(wavMeta.bits_per_sample) + L" bit");
    AssertCheck(L"WAV 时长非零真实存在", wavMeta.duration_ms > 60000, std::to_wstring(wavMeta.duration_ms / 1000.0) + L" s");

    // 1.3 真实 WASAPI 硬件播放、Seek与 FFT 频谱推流
    logger.Log(L"\n  [硬件直通闭环] 接入 AudioPlaybackEngine 启动物理硬件播放:");
    AudioPlaybackEngine engine;
    bool engineInitOk = engine.Init();
    AssertCheck(L"WASAPI 音频硬件引擎初始化成功", engineInitOk);

    bool loadOk = engine.OpenFile(flacSample);
    AssertCheck(L"母带文件加载入播放引擎", loadOk);

    engine.SetVolume(0.8f);
    engine.Play();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    AssertCheck(L"播放状态 == Playing", engine.GetState() == PlaybackState::Playing);
    double curPos = engine.GetPositionSeconds();
    AssertCheck(L"播放时间正常递进", curPos > 0.2 && curPos < 1.5, std::to_wstring(curPos) + L" s");

    std::vector<float> bars = engine.GetVisualizer().GetSpectrumBars();
    double sumEnergy = std::accumulate(bars.begin(), bars.end(), 0.0);
    PrintSpectrumAscii(bars);
    AssertCheck(L"母带物理 PCM 采样推流 FFT 能量 > 0", sumEnergy > 0.01, L"能量和 " + std::to_wstring(sumEnergy));

    // Seek 实测
    engine.SeekTo(30.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    double seekPos = engine.GetPositionSeconds();
    AssertCheck(L"母带精准 Seek 到 ~30s", seekPos >= 29.5 && seekPos <= 31.5, std::to_wstring(seekPos) + L" s");

    engine.Stop();
    AssertCheck(L"播放引擎停止", engine.GetState() == PlaybackState::Stopped);

    // 独占模式实测:结果取决于声卡驱动是否支持该采样率/位深,只记录不断言
    {
        AudioPlaybackEngine ex;
        bool exInit = ex.Init(true);
        bool exOpen = exInit && ex.OpenFile(flacSample);
        logger.Log(L"  [独占模式实测] 初始化: " + std::wstring(exInit ? L"成功" : L"失败") +
                   L", 实际模式: " + std::wstring(ex.IsExclusiveMode() ? L"WASAPI 独占" : L"共享 (已回退)") +
                   L", 母带采样率: " + std::to_wstring(ex.GetSourceSampleRate()) +
                   L" Hz, 设备输出: " + std::to_wstring(ex.GetOutputSampleRate()) + L" Hz / " +
                   std::to_wstring(ex.GetOutputBitDepth()) + L" bit" +
                   (exOpen && !ex.IsResampling() ? L" (无重采样直通)" : L" (存在重采样)"));
        if (exOpen) {
            ex.Play();
            std::this_thread::sleep_for(std::chrono::milliseconds(800));
            logger.Log(L"  [独占模式实测] 播放 0.8s 后位置: " + std::to_wstring(ex.GetPositionSeconds()) + L" s");
            ex.Stop();
        }
        ex.Shutdown();
    }

    // ========================================================================
    // 梯度 2: 全库多线程异步并发入库压测
    // ========================================================================
    logger.Log(L"\n▶ 【梯度 2】全母带库多线程异步并发扫描与吞吐量压测 (AudioIngestionPipeline):");

    AudioIngestionPipeline hiresPipeline;
    hiresPipeline.SetTargetLibraryDir(L"player_core\\test\\ingestion_output"); // 只读扫描，不修改原库
    hiresPipeline.SetLazyCoverExtract(true); // 两阶段惰性读取：不落盘数千张封面
    size_t hwThreads = 8;
    hiresPipeline.SetThreadCount(hwThreads);
    const std::wstring kScanCache = L"player_core\\test\\hires_scan.cache";
    hiresPipeline.SetScanCacheFile(kScanCache);
    hiresPipeline.AddInputPath(HIRES_ROOT);

    logger.Log(L"  - 扫描源路径: " + HIRES_ROOT);
    logger.Log(L"  - 并发线程数: " + std::to_wstring(hwThreads) + L" (自适应 CPU 物理/逻辑核心)");
    logger.Log(L"  - 惰性策略: 已启用 (封面按需提取，仅解析音频流物理帧头与标签)");

    MusicCatalog hiresCatalog;
    std::atomic<bool> isDone{false};
    IngestionSummary hiresSummary;

    auto tStart = std::chrono::high_resolution_clock::now();

    bool asyncOk = hiresPipeline.StartAsync(hiresCatalog,
        [&](const IngestionProgress& prog) {
            if (prog.current % 500 == 0 || prog.current == prog.total) {
                double pct = (prog.total > 0) ? (prog.current * 100.0 / prog.total) : 0.0;
                std::wstringstream wss;
                wss << L"    [异步压测进度] " << prog.current << L" / " << prog.total
                    << L" (" << std::fixed << std::setprecision(1) << pct << L"%)"
                    << L" - 正在解析: " << fs::path(prog.current_filename).filename().wstring();
                logger.Log(wss.str());
            }
        },
        [&](const IngestionSummary& summary) {
            hiresSummary = summary;
            isDone.store(true);
        }
    );

    AssertCheck(L"全库 StartAsync 立即非阻塞触发", asyncOk && hiresPipeline.IsRunning());

    // 主线程等待后台并发池完成
    while (!isDone.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    auto tEnd = std::chrono::high_resolution_clock::now();
    double totalWallSec = std::chrono::duration<double>(tEnd - tStart).count();

    logger.Log(L"\n  [压测结果指标审计]:");
    logger.Log(L"  - 全局扫描物理文件总数: " + std::to_wstring(hiresSummary.total_scanned));
    logger.Log(L"  - 成功解析入库音频总数: " + std::to_wstring(hiresSummary.success_count));
    // 以下数字全部来自流水线与曲库的实际统计,不写任何手工叙述
    size_t dsdParsed = hiresCatalog.FilterByFormat(L"DSF").size() + hiresCatalog.FilterByFormat(L"DFF").size();
    logger.Log(L"  - 解析失败数: " + std::to_wstring(hiresSummary.failed_count) +
               L" (逐文件原因见 library_audit 输出 CSV 的 error 列)");
    logger.Log(L"  - 跳过的非音乐文件 (视频等): " + std::to_wstring(hiresSummary.skipped_count));
    logger.Log(L"  - 其中 DSD (DSF/DFF) 成功入库: " + std::to_wstring(dsdParsed) + L" 首");
    logger.Log(L"  - CUE 拆分曲目: " + std::to_wstring(hiresSummary.cue_tracks) +
               L", 入库曲目总数: " + std::to_wstring(hiresSummary.tracks_added));
    logger.Log(L"  - 解析阶段真实读盘: " + std::to_wstring(hiresSummary.bytes_read / 1048576) + L" MB / 库体积 " +
               std::to_wstring(hiresSummary.library_bytes / 1073741824) + L" GB");
    logger.Log(L"  - 存储类型: " + std::wstring(hiresSummary.storage_seek_penalty == 1 ? L"机械盘" :
                                               hiresSummary.storage_seek_penalty == 0 ? L"固态盘" : L"未知") +
               L" (注意:若刚运行过 library_audit,文件头已在系统缓存中,本次耗时不代表冷启动)");
    logger.Log(L"  - 总耗时 (Wall Time): " + std::to_wstring(totalWallSec) + L" 秒");
    double speed = hiresSummary.total_scanned > 0 ? (hiresSummary.total_scanned / totalWallSec) : 0.0;
    logger.Log(L"  - 库规模 (实测): " + std::to_wstring(hiresSummary.library_bytes / 1073741824.0) + L" GB, " +
               std::to_wstring(hiresSummary.total_scanned) + L" 个文件");
    logger.Log(L"  - 平均解析吞吐量: " + std::to_wstring(speed) + L" 首/秒");
    logger.Log(L"  - 增量缓存: 命中 " + std::to_wstring(hiresSummary.cache_hits) + L" / 实际解析 " +
               std::to_wstring(hiresSummary.cache_misses) +
               (hiresSummary.cache_hits > 0 ? L" (缓存来自上一次运行,本次不是冷扫描)" : L" (首次全量扫描)"));

    AssertCheck(L"扫描到有效音频数量 >= 3000 首", hiresSummary.total_scanned >= 3000,
                std::to_wstring(hiresSummary.total_scanned) + L" 首");
    AssertCheck(L"入库成功数 >= 3000 首", hiresSummary.success_count >= 3000,
                std::to_wstring(hiresSummary.success_count) + L" 首");
    AssertCheck(L"单机并发吞吐量 >= 50 首/秒", speed >= 50.0,
                std::to_wstring(speed) + L" 首/秒");

    // ------------------------------------------------------------------------
    // 梯度 2b: 增量扫描 —— 再扫一遍同一个库,未变化的文件直接复用缓存
    // ------------------------------------------------------------------------
    {
        logger.Log(L"\n▶ 【梯度 2b】增量扫描 (库未变化,复用上一轮写入的缓存):");
        AudioIngestionPipeline incPipeline;
        incPipeline.SetTargetLibraryDir(L"player_core\\test\\ingestion_output");
        incPipeline.SetLazyCoverExtract(true);
        incPipeline.SetScanCacheFile(kScanCache);
        incPipeline.AddInputPath(HIRES_ROOT);
        MusicCatalog incCatalog;
        auto ti0 = std::chrono::high_resolution_clock::now();
        incPipeline.RunSync(incCatalog);
        double incSec = std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - ti0).count();
        IngestionSummary inc = incPipeline.GetLastSummary();
        logger.Log(L"  - 目录枚举: " + std::to_wstring(inc.enumerate_seconds) + L" 秒, 总耗时: " + std::to_wstring(incSec) + L" 秒");
        logger.Log(L"  - 缓存命中: " + std::to_wstring(inc.cache_hits) + L", 实际解析: " + std::to_wstring(inc.cache_misses) +
                   L", 解析阶段读盘: " + std::to_wstring(inc.bytes_read / 1024) + L" KB");
        AssertCheck(L"增量扫描曲目数与全量扫描一致", incCatalog.GetAllTracks().size() == hiresCatalog.GetAllTracks().size(),
                    std::to_wstring(incCatalog.GetAllTracks().size()) + L" 首");
        AssertCheck(L"库未变化时全部命中缓存", inc.cache_misses == 0, std::to_wstring(inc.cache_misses) + L" 个未命中");
    }

    // ========================================================================
    // 梯度 3: 全库曲库分类、拼音索引、智能检索与去重折叠压测
    // ========================================================================
    logger.Log(L"\n▶ 【梯度 3】大容量母带曲库智能清洗、拼音索引与检索基准测试 (MusicCatalog):");

    auto flacList = hiresCatalog.FilterByFormat(L"FLAC");
    auto wavList = hiresCatalog.FilterByFormat(L"WAV");
    auto mp3List = hiresCatalog.FilterByFormat(L"MP3");

    logger.Log(L"  - 格式分布: FLAC = " + std::to_wstring(flacList.size()) +
               L" 首, WAV = " + std::to_wstring(wavList.size()) +
               L" 首, MP3 = " + std::to_wstring(mp3List.size()) + L" 首");

    AssertCheck(L"FLAC 格式曲目数量显著 (> 2500)", flacList.size() > 2500, std::to_wstring(flacList.size()) + L" 首");
    AssertCheck(L"WAV 格式曲目数量显著 (> 100)", wavList.size() > 100, std::to_wstring(wavList.size()) + L" 首");

    // 拼音检索基准测试
    auto tSearch0 = std::chrono::high_resolution_clock::now();
    auto easonTracks = hiresCatalog.Search(L"陈奕迅");
    auto tSearch1 = std::chrono::high_resolution_clock::now();
    double searchMsEason = std::chrono::duration<double, std::milli>(tSearch1 - tSearch0).count();

    logger.Log(L"  - 检索 '陈奕迅' 命中: " + std::to_wstring(easonTracks.size()) +
               L" 首 (耗时 " + std::to_wstring(searchMsEason) + L" ms)");
    AssertCheck(L"检索 '陈奕迅' 命中大于 0 首", !easonTracks.empty(), std::to_wstring(easonTracks.size()) + L" 首");
    AssertCheck(L"全库检索延迟 < 15ms", searchMsEason < 15.0, std::to_wstring(searchMsEason) + L" ms");

    // 拼音前缀搜索基准测试 (如 "CYX" 拼音首字母检索)
    auto tSearch2 = std::chrono::high_resolution_clock::now();
    auto pinyinTracks = hiresCatalog.Search(L"CYX");
    auto tSearch3 = std::chrono::high_resolution_clock::now();
    double searchMsPinyin = std::chrono::duration<double, std::milli>(tSearch3 - tSearch2).count();

    logger.Log(L"  - 拼音首字母 'CYX' 命中: " + std::to_wstring(pinyinTracks.size()) +
               L" 首 (耗时 " + std::to_wstring(searchMsPinyin) + L" ms)");

    // 英文/罗马音检索测试
    auto yirumaTracks = hiresCatalog.Search(L"Yiruma");
    logger.Log(L"  - 检索 'Yiruma' 命中: " + std::to_wstring(yirumaTracks.size()) + L" 首");
    AssertCheck(L"检索 'Yiruma' 命中大于 0 首", !yirumaTracks.empty(), std::to_wstring(yirumaTracks.size()) + L" 首");

    // 智能去重折叠聚类
    auto deduplicatedGroups = hiresCatalog.SearchGrouped(L"");
    size_t totalTracks = hiresCatalog.GetAllTracks().size();
    logger.Log(L"  - 原始总音轨数: " + std::to_wstring(totalTracks) +
               L" 首 -> 智能去重聚合后独立作品数: " + std::to_wstring(deduplicatedGroups.size()) + L" 首");
    AssertCheck(L"智能去重聚合逻辑生效", deduplicatedGroups.size() > 0 && deduplicatedGroups.size() <= totalTracks,
                std::to_wstring(deduplicatedGroups.size()) + L" 组");

    logger.Log(L"\n================================================================================");
    logger.Log(L"  最终发烧压测物理自证统计: 通过 " + std::to_wstring(g_pass) + L" 项 / 失败 " + std::to_wstring(g_fail) + L" 项");
    logger.Log(L"================================================================================");

    return (g_fail == 0) ? 0 : 1;
}
