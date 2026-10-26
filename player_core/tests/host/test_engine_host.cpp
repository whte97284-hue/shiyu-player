// 播放引擎集成测试 (miniaudio null 后端:虚拟声卡,按真实时间节奏回调)
// 编译时需定义 MA_ENABLE_ONLY_SPECIFIC_BACKENDS + MA_ENABLE_NULL
// 用法: test_engine_host <fixture_dir>
#include "../../include/audio_playback_engine.hpp"
#include "../../include/audio_tag_parser.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>

static int g_pass = 0, g_fail = 0;
static void Check(const char* name, bool cond, const std::string& detail = "") {
    (cond ? g_pass : g_fail)++;
    std::printf("  [%s] %s%s%s\n", cond ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
}
static void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
static std::wstring W(const std::string& s) { return AudioTagParser::Utf8ToWide(s); }
static std::string F(double v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::string fx = argv[1];
    std::wstring hires = W(fx + "/flac_24_96_bigcover_first.flac");
    std::wstring cueImg = W(fx + "/library_extras/cue_album/CDImage.flac");
    std::wstring mp3 = W(fx + "/mp3_cbr320_id3v23_utf16_apic.mp3");

    AudioPlaybackEngine eng;
    std::printf("\n[1] 设备与原生采样率\n");
    Check("Init(独占) 成功 (不支持独占时自动回退共享)", eng.Init(true));
    Check("打开 24bit/96k FLAC", eng.OpenFile(hires));
    Check("源采样率识别为 96000", eng.GetSourceSampleRate() == 96000, std::to_string(eng.GetSourceSampleRate()));
    Check("输出链路如实报告 (重采样标志与采样率一致)",
          eng.IsResampling() == (eng.GetSourceSampleRate() != eng.GetOutputSampleRate()) && eng.GetOutputBitDepth() > 0,
          std::string(eng.IsExclusiveMode() ? "独占" : "共享") + " 输出 " + std::to_string(eng.GetOutputSampleRate()) + "Hz/" +
              std::to_string(eng.GetOutputBitDepth()) + "bit");
    Check("时长 = 4.000s (来自解析内核,无需整文件扫描)", std::fabs(eng.GetDurationSeconds() - 4.0) < 0.01, F(eng.GetDurationSeconds()));

    std::printf("\n[2] 播放 / 位置 / 频谱 / Seek\n");
    eng.SetVolume(0.8f);
    eng.Play();
    Sleep(700);
    double pos = eng.GetPositionSeconds();
    Check("播放位置随时间推进 (取自解码器帧游标)", pos > 0.4 && pos < 1.1, F(pos));
    auto bars = eng.GetVisualizer().GetSpectrumBars();
    auto centers = eng.GetVisualizer().GetBandCenterFrequencies();
    size_t peak = 0, expect = 0;
    for (size_t i = 0; i < bars.size(); ++i) {
        if (bars[i] > bars[peak]) peak = i;
        if (std::fabs(std::log(centers[i] / 440.0)) < std::fabs(std::log(centers[expect] / 440.0))) expect = i;
    }
    Check("440Hz 正弦的频谱峰值落在 440Hz 所在频柱 (96k 下频率映射正确)", peak == expect || peak + 1 == expect || peak == expect + 1,
          "峰值柱 " + std::to_string(peak) + " (" + F(centers[peak]) + "Hz), 期望 " + std::to_string(expect));
    Check("Seek 到 2.5s", eng.SeekTo(2.5) && std::fabs(eng.GetPositionSeconds() - 2.5) < 0.01);
    Sleep(300);
    pos = eng.GetPositionSeconds();
    Check("Seek 后从新位置继续推进", pos > 2.6 && pos < 3.1, F(pos));
    eng.Pause();
    double p1 = eng.GetPositionSeconds();
    Sleep(300);
    Check("暂停后位置冻结", std::fabs(eng.GetPositionSeconds() - p1) < 0.02);
    eng.Stop();
    Check("Stop 回到起点", eng.GetState() == PlaybackState::Stopped && eng.GetPositionSeconds() < 0.01);

    std::printf("\n[3] CUE 片段播放 + 结束回调中直接切歌 (不得死锁)\n");
    std::atomic<int> endedCount{0};
    std::atomic<bool> reopened{false};
    std::thread::id cbThread;
    eng.on_playback_ended = [&]() {
        cbThread = std::this_thread::get_id();
        if (endedCount.fetch_add(1) == 0) {
            // 在回调里打开并播放下一首:旧实现会在音频线程持锁时回调,这里必然死锁
            reopened = eng.OpenFile(mp3) && eng.Play();
        }
    };
    Check("打开 CUE 片段 10.0s ~ 10.8s", eng.OpenFile(cueImg, 10.0, 10.8));
    Check("片段时长 = 0.8s", std::fabs(eng.GetDurationSeconds() - 0.8) < 0.01, F(eng.GetDurationSeconds()));
    auto t0 = std::chrono::steady_clock::now();
    eng.Play();
    while (endedCount.load() == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) Sleep(10);
    double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    Check("片段在 0.8s 左右精确结束", endedCount.load() == 1 && took > 0.7 && took < 1.3, F(took) + "s");
    Sleep(200);
    Check("结束回调在通知线程执行,回调内切歌成功且未死锁", reopened.load() && cbThread != std::this_thread::get_id() &&
                                                              eng.GetState() == PlaybackState::Playing);
    Check("切歌后 MP3 时长为 LAME 无缝精确值 5.000s", std::fabs(eng.GetDurationSeconds() - 5.0) < 0.01, F(eng.GetDurationSeconds()));
    Sleep(300);
    Check("切歌后继续推进", eng.GetPositionSeconds() > 0.1, F(eng.GetPositionSeconds()));

    std::printf("\n[4] 无缝播放 (预加载下一首,同一回调内切换)\n");
    {
        std::atomic<int> advanced{0}, ended{0};
        eng.on_track_advanced = [&]() { advanced++; };
        eng.on_playback_ended = [&]() { ended++; };
        Check("打开片段 A 0.0s ~ 0.6s", eng.OpenFile(cueImg, 0.0, 0.6));
        Check("预加载片段 B 10.0s ~ 10.7s", eng.QueueNext(cueImg, 10.0, 10.7, 0.5f));
        eng.Play();
        auto t0 = std::chrono::steady_clock::now();
        while (advanced.load() == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3)) Sleep(20);
        double tookA = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        Check("A 结束时切到 B:触发 on_track_advanced 而非结束回调", advanced.load() == 1 && ended.load() == 0,
              std::to_string(advanced.load()) + "/" + std::to_string(ended.load()));
        Check("切换时刻约 0.6s,播放状态保持", tookA > 0.45 && tookA < 1.0 && eng.GetState() == PlaybackState::Playing, F(tookA) + "s");
        Check("切换后时长为 B 的 0.7s", std::fabs(eng.GetDurationSeconds() - 0.7) < 0.01, F(eng.GetDurationSeconds()));
        while (ended.load() == 0 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4)) Sleep(20);
        Check("B 播完后正常触发结束回调", ended.load() == 1 && advanced.load() == 1);
        eng.on_track_advanced = nullptr;
        eng.on_playback_ended = nullptr;
        eng.OpenFile(hires);
        eng.Play();
    }
    Check("枚举输出设备 (null 后端下至少一个)", !AudioPlaybackEngine::EnumerateOutputDevices().empty());

    std::printf("\n[5] 音量平滑\n");
    eng.SetVolume(0.0f);
    Sleep(200);
    eng.SetVolume(1.0f);
    Sleep(200);
    Check("音量切换过程无崩溃,状态正常", eng.GetState() == PlaybackState::Playing && std::fabs(eng.GetVolume() - 1.0f) < 1e-6);
    eng.Stop();
    eng.Shutdown();

    std::printf("\n==== engine host tests: %d passed / %d failed ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
