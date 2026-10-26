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
#include <io.h>
#include <fcntl.h>
#include "../include/audio_playback_engine.hpp"
#include "../include/audio_ingestion_pipeline.hpp"
#include "../include/music_catalog.hpp"
#include "../include/playlist_manager.hpp"
#include "../include/play_queue_controller.hpp"

namespace fs = std::filesystem;

// 一律使用绝对路径，避免依赖调用者的工作目录（相对路径会在错误 cwd 下静默失败）
static const std::wstring BASE = L"F:\\软件\\酷狗最新版转换器\\KugouConverterWeb\\";
static std::wstring P(const std::wstring& rel) { return BASE + rel; }

static int g_pass = 0;
static int g_fail = 0;

static void Check(const std::wstring& name, bool cond, const std::wstring& detail = L"") {
    if (cond) { ++g_pass; std::wcout << L"  [PASS] "; }
    else      { ++g_fail; std::wcout << L"  [FAIL] "; }
    std::wcout << name;
    if (!detail.empty()) std::wcout << L"  (" << detail << L")";
    std::wcout << std::endl;
}

static bool FileStartsWith(const std::wstring& path, const char* magic, size_t magicLen) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs.is_open()) return false;
    std::vector<char> buf(magicLen, 0);
    ifs.read(buf.data(), magicLen);
    return ifs.gcount() == static_cast<std::streamsize>(magicLen) &&
           std::memcmp(buf.data(), magic, magicLen) == 0;
}

static double SumBars(const std::vector<float>& bars) {
    return std::accumulate(bars.begin(), bars.end(), 0.0);
}

static void PrintSpectrumAscii(const std::vector<float>& bars) {
    std::wcout << L"  [32 Band FFT Spectrum]:\n   ";
    for (size_t i = 0; i < bars.size(); ++i) {
        int h = static_cast<int>(bars[i] * 8.0f);
        if (h <= 1) std::wcout << L" ";
        else if (h == 2) std::wcout << L"▂";
        else if (h == 3) std::wcout << L"▃";
        else if (h == 4) std::wcout << L"▄";
        else if (h == 5) std::wcout << L"▅";
        else if (h == 6) std::wcout << L"▆";
        else if (h == 7) std::wcout << L"▇";
        else std::wcout << L"█";
    }
    std::wcout << std::endl;
}

int main() {
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    std::wcout << L"================ [Phase 2 全量集成] 音频转码、播放引擎、歌单与队列闭环测试 ================\n" << std::endl;

    // ============================================================
    // 测试项 1: 密文自动嗅探、解密转码落盘与全自动入库
    // ============================================================
    std::wcout << L"▶ [测试项 1] 自动化转码与入库流水线 (AudioIngestionPipeline):" << std::endl;

    const std::wstring kgmaSrc = P(L"output\\放課後ティータイム - Listen!!.kgma");
    const std::wstring mp3A    = P(L"output\\Creepy Nuts - 堕天.mp3");
    const std::wstring mp3B    = P(L"output\\2hollis - poster boy.mp3");

    MusicCatalog catalog;
    AudioIngestionPipeline pipeline;
    pipeline.SetTargetLibraryDir(P(L"player_core\\test\\ingestion_output"));
    pipeline.SetKgmMaskPath(P(L"tools\\kgm\\kgm.mask"));
    pipeline.SetThreadCount(4); // 启用 4 线程并行

    pipeline.AddInputPath(kgmaSrc);
    pipeline.AddInputPath(mp3A);
    pipeline.AddInputPath(mp3B);

    auto results = pipeline.RunSync(catalog);
    std::wcout << L"  流水线多线程并发执行完毕，共处理 " << results.size() << L" 个项目:" << std::endl;

    size_t okCount = 0;
    std::wstring convertedPath;
    for (const auto& r : results) {
        if (r.success) ++okCount;
        std::wcout << L"    - 源文件: " << r.source_path << std::endl;
        std::wcout << L"      落盘文件: " << r.target_path
                   << L" (密文转码: " << (r.was_encrypted ? L"是" : L"否")
                   << L", 状态: " << r.message << L")" << std::endl;
        if (r.was_encrypted && r.target_path.find(L".flac") != std::wstring::npos) {
            convertedPath = r.target_path;
        }
    }

    Check(L"流水线处理条目数 == 3", results.size() == 3,
          L"实际 " + std::to_wstring(results.size()));
    Check(L"全部条目 success", okCount == results.size(),
          L"成功 " + std::to_wstring(okCount) + L"/" + std::to_wstring(results.size()));

    bool convExists = !convertedPath.empty() && std::ifstream(convertedPath, std::ios::binary).good();
    Check(L"KGMA 转码产物存在", convExists, convertedPath.empty() ? L"未产出" : convertedPath);
    if (convExists) {
        Check(L"转码产物文件头 == 'fLaC'", FileStartsWith(convertedPath, "fLaC", 4),
              L"前4字节非 fLaC 即解密算法有误");
    }

    auto stats = catalog.GetStats();
    Check(L"曲库入库数 == 3 (含转码产物)", stats.total_tracks == 3,
          L"实际 " + std::to_wstring(stats.total_tracks));

    // ============================================================
    // 测试项 2: 播放引擎初始化与解码
    // ============================================================
    std::wcout << L"\n▶ [测试项 2] 音频播放引擎初始化与格式解码 (AudioPlaybackEngine):" << std::endl;
    AudioPlaybackEngine player;
    bool initOk = player.Init(false);
    Check(L"音频设备初始化成功", initOk);
    if (!initOk) {
        std::wcout << L"\n[FATAL] 设备初始化失败，后续项无法进行。" << std::endl;
        return 1;
    }

    bool openOk = player.OpenFile(mp3A);
    Check(L"打开《堕天.mp3》成功", openOk);
    double dur = player.GetDurationSeconds();
    Check(L"检测时长 ≈ 172.98s (±1.0s)", std::fabs(dur - 172.98) <= 1.0,
          L"实际 " + std::to_wstring(dur));

    // ============================================================
    // 测试项 3: 回放推进与 Seek
    // ============================================================
    std::wcout << L"\n▶ [测试项 3] 物理音频回放驱动与精准 Seek 定位:" << std::endl;
    player.SetVolume(0.8f);
    Check(L"音量设置 80%", std::fabs(player.GetVolume() - 0.8f) < 0.001f);

    player.Play();
    Check(L"Play 后状态 == Playing", player.GetState() == PlaybackState::Playing);

    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    double pos1 = player.GetPositionSeconds();
    Check(L"时间戳真实推进 (0.40s ~ 0.90s)", pos1 > 0.40 && pos1 < 0.90,
          L"实际 " + std::to_wstring(pos1) + L"s");

    auto spectrum1 = player.GetVisualizer().GetSpectrumBars();
    PrintSpectrumAscii(spectrum1);
    Check(L"播放中 FFT 频谱有能量", SumBars(spectrum1) > 0.5,
          L"能量和 " + std::to_wstring(SumBars(spectrum1)));

    player.SeekTo(60.0);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    double pos2 = player.GetPositionSeconds();
    Check(L"Seek 真实跳转到 ~60s", pos2 >= 59.5 && pos2 <= 61.5,
          L"实际 " + std::to_wstring(pos2) + L"s");

    auto spectrum2 = player.GetVisualizer().GetSpectrumBars();
    PrintSpectrumAscii(spectrum2);

    // ============================================================
    // 测试项 4: Pause 与频谱阻尼衰减
    // ============================================================
    std::wcout << L"\n▶ [测试项 4] 暂停与 FFT 能量柱阻尼衰减验证:" << std::endl;
    player.Pause();
    Check(L"Pause 后状态 == Paused", player.GetState() == PlaybackState::Paused);

    double firstSum = 0.0, lastSum = 0.0;
    for (int frame = 1; frame <= 3; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        auto s = player.GetVisualizer().GetSpectrumBars();
        double sum = SumBars(s);
        if (frame == 1) firstSum = sum;
        lastSum = sum;
        std::wcout << L"  Frame " << frame << L" (能量和 " << std::setprecision(4) << sum << L"):";
        PrintSpectrumAscii(s);
    }
    Check(L"暂停后频谱能量单调衰减", lastSum < firstSum - 0.05,
          L"首帧 " + std::to_wstring(firstSum) + L" -> 末帧 " + std::to_wstring(lastSum));

    player.Stop();
    Check(L"Stop 后状态 == Stopped", player.GetState() == PlaybackState::Stopped);

    // ============================================================
    // 测试项 5: 歌单管理内核 (PlaylistManager) 与 M3U8/JSON 持久化
    // ============================================================
    std::wcout << L"\n▶ [测试项 5] 歌单内核管理与持久化 (PlaylistManager):" << std::endl;
    PlaylistManager plm;
    std::string plId = plm.CreatePlaylist(L"我的发烧HiFi试音单", L"用于测试高解析音频与动效");
    Check(L"创建歌单成功", !plId.empty(), L"ID: " + AudioTagParser::Utf8ToWide(plId));

    // 添加全部 3 首曲目
    std::vector<uint64_t> trackIds;
    for (const auto& t : catalog.GetAllTracks()) {
        trackIds.push_back(t.id);
    }
    plm.AddTracks(plId, trackIds);
    const Playlist* pPl = plm.GetPlaylist(plId);
    Check(L"歌单添加曲目数 == 3", pPl && pPl->track_ids.size() == 3,
          L"实际 " + std::to_wstring(pPl ? pPl->track_ids.size() : 0));

    // 移动歌曲位置 (将第 0 首移动至末尾第 2 位)
    uint64_t origFirst = pPl->track_ids[0];
    plm.MoveTrack(plId, 0, 2);
    pPl = plm.GetPlaylist(plId);
    Check(L"MoveTrack 移动曲目成功", pPl && pPl->track_ids[2] == origFirst);

    // 导出 M3U8
    std::wstring m3u8Path = P(L"player_core\\test\\test_export.m3u8");
    bool expM3u8Ok = plm.ExportM3U8(plId, m3u8Path, catalog);
    Check(L"导出标准 M3U8 成功", expM3u8Ok && fs::exists(m3u8Path));
    Check(L"M3U8 文件包含 #EXTM3U 标签", FileStartsWith(m3u8Path, "\xEF\xBB\xBF#EXTM3U", 10) ||
                                              FileStartsWith(m3u8Path, "#EXTM3U", 7));

    // 导入 M3U8 到新歌单
    std::string impPlId;
    bool impM3u8Ok = plm.ImportM3U8(m3u8Path, catalog, impPlId);
    const Playlist* pImpPl = plm.GetPlaylist(impPlId);
    Check(L"重新导入 M3U8 成功并匹配曲库", impM3u8Ok && pImpPl && pImpPl->track_ids.size() == 3,
          L"导入曲目数: " + std::to_wstring(pImpPl ? pImpPl->track_ids.size() : 0));

    // JSON 集合持久化测试
    std::wstring jsonPath = P(L"player_core\\test\\playlists.json");
    bool jsonSaveOk = plm.SaveToJson(jsonPath);
    Check(L"歌单集合保存为 JSON 成功", jsonSaveOk && fs::exists(jsonPath));

    PlaylistManager plm2;
    bool jsonLoadOk = plm2.LoadFromJson(jsonPath);
    Check(L"从 JSON 反序列化加载歌单成功", jsonLoadOk && plm2.GetPlaylistCount() == 2,
          L"加载歌单总数: " + std::to_wstring(plm2.GetPlaylistCount()));

    // ============================================================
    // 测试项 6: 播放队列与切歌控制器 (PlayQueueController)
    // ============================================================
    std::wcout << L"\n▶ [测试项 6] 播放队列与切歌状态机 (PlayQueueController):" << std::endl;
    PlayQueueController queueCtrl;
    queueCtrl.SetQueue({ 1001, 1002, 1003 }, 0);
    Check(L"队列初始容量 == 3", queueCtrl.GetQueueSize() == 3);
    Check(L"初始播放游标 == 1001", queueCtrl.GetCurrentTrackId() == 1001);

    // 1. 顺序播放模式测试 (Sequential)
    queueCtrl.SetPlayMode(PlayMode::Sequential);
    Check(L"Next() 顺序切至 1002", queueCtrl.Next() == 1002);
    Check(L"Next() 顺序切至 1003", queueCtrl.Next() == 1003);
    Check(L"到达末尾 Next() 停止 (== 0)", queueCtrl.Next() == 0);

    // 2. 真实历史栈回退测试 (Previous)
    Check(L"Previous() 从末尾回退至 1003", queueCtrl.Previous() == 1003);
    Check(L"Previous() 继续回退至 1002", queueCtrl.Previous() == 1002);
    Check(L"Previous() 回退至起始 1001", queueCtrl.Previous() == 1001);

    // 3. 下一首插播测试 (InsertNext)
    queueCtrl.InsertNext(9999);
    Check(L"下一首插播 9999 成功", queueCtrl.Next() == 9999);
    Check(L"插播后下一首恢复为原后续曲目", queueCtrl.Next() == 1002);

    // 4. 列表循环测试 (RepeatAll)
    queueCtrl.SetPlayMode(PlayMode::RepeatAll);
    queueCtrl.JumpToIndex(queueCtrl.GetQueueSize() - 1);
    uint64_t loopedId = queueCtrl.Next();
    Check(L"列表循环 RepeatAll 末尾折返首位", loopedId == 1001);

    // 5. 单曲循环测试 (RepeatOne)
    queueCtrl.SetPlayMode(PlayMode::RepeatOne);
    uint64_t r1 = queueCtrl.Next();
    uint64_t r2 = queueCtrl.Next();
    Check(L"单曲循环 RepeatOne 连续 Next 保持恒定", r1 == 1001 && r2 == 1001);

    // 6. 真随机洗牌测试 (Shuffle - Fisher-Yates 置乱)
    queueCtrl.SetPlayMode(PlayMode::Shuffle);
    std::vector<uint64_t> shuffledSeen;
    for (size_t i = 0; i < queueCtrl.GetQueueSize(); ++i) {
        shuffledSeen.push_back(queueCtrl.GetCurrentTrackId());
        queueCtrl.Next();
    }
    // 验证置乱后集合元素无遗漏
    std::sort(shuffledSeen.begin(), shuffledSeen.end());
    auto expected = queueCtrl.GetCurrentQueue();
    std::sort(expected.begin(), expected.end());
    Check(L"随机洗牌 Shuffle 遍历无遗漏且无重复", shuffledSeen == expected);

    // ============================================================
    // 测试项 7: 多线程异步并发流水线 (StartAsync)
    // ============================================================
    std::wcout << L"\n▶ [测试项 7] 多线程并发全异步导入流水线 (StartAsync):" << std::endl;
    AudioIngestionPipeline asyncPipeline;
    asyncPipeline.SetTargetLibraryDir(P(L"player_core\\test\\ingestion_output"));
    asyncPipeline.SetKgmMaskPath(P(L"tools\\kgm\\kgm.mask"));
    asyncPipeline.SetThreadCount(4);
    asyncPipeline.AddInputPath(mp3A);
    asyncPipeline.AddInputPath(mp3B);

    MusicCatalog asyncCatalog;
    std::atomic<bool> asyncFinished{false};
    IngestionSummary finalSummary;

    bool startOk = asyncPipeline.StartAsync(asyncCatalog,
        [](const IngestionProgress& prog) {
            // 异步进度回调 (非阻塞)
        },
        [&asyncFinished, &finalSummary](const IngestionSummary& summary) {
            finalSummary = summary;
            asyncFinished.store(true);
        }
    );

    Check(L"StartAsync 立即返回非阻塞", startOk && asyncPipeline.IsRunning());

    // 主线程可自由执行其他逻辑，等待异步工作池完成
    int waitMs = 0;
    while (!asyncFinished.load() && waitMs < 5000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        waitMs += 50;
    }

    Check(L"异步流水线在后台完成", asyncFinished.load(),
          L"耗时 " + std::to_wstring(finalSummary.elapsed_seconds) + L" 秒");
    Check(L"异步入库结果成功", finalSummary.success_count == 2);
    Check(L"异步执行后 IsRunning() == false", !asyncPipeline.IsRunning());

    // ============================================================
    std::wcout << L"\n================ 最终物理断言统计: 通过 " << g_pass << L" 项 / 失败 " << g_fail << L" 项 ================" << std::endl;
    return g_fail == 0 ? 0 : 1;
}
