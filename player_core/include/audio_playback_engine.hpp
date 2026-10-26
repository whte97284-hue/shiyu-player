#pragma once
#include <string>
#include <functional>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <cstdint>
#include "fft_visualizer.hpp"
#include "equalizer.hpp"

enum class PlaybackState {
    Stopped,
    Playing,
    Paused
};

// ============================================================================
//  AudioPlaybackEngine —— 发烧级播放引擎
//   · 真·WASAPI 独占模式 (shareMode = exclusive),失败自动回退共享模式
//   · 输出格式跟随设备默认格式 (独占/共享均如此,miniaudio 0.11 限制),
//     需要时由解码器以最高阶低通重采样一次完成;GetOutput* 如实报告实际输出格式
//   · 设备为 16 位时自动加 TPDF 抖动,24bit 母带降位深不产生截断失真
//   · 播放位置取自解码器真实帧游标,无浮点累加漂移
//   · 支持 CUE 分轨片段播放 (起止时间精确到帧)
//   · 音量平滑斜坡,调音量无「拉链噪声」
//   · 播放结束回调在独立通知线程中触发,回调里直接切歌也不会与音频线程死锁
// ============================================================================
// 输出设备 (系统枚举结果)
struct OutputDeviceInfo {
    std::string name;   // UTF-8 设备名,也作为持久化标识
    bool is_default = false;
};

class AudioPlaybackEngine {
public:
    // 枚举系统的播放设备 (不需要已初始化的引擎)
    static std::vector<OutputDeviceInfo> EnumerateOutputDevices();
    // 指定输出设备 (UTF-8 设备名,空 = 系统默认);须在 Init 之前设置
    void SetPreferredDevice(const std::string& name);
    // 当前实际输出设备名
    std::string GetDeviceName() const;

    // ReplayGain:当前曲目的线性增益 (1.0 = 不调整),与用户音量相乘,平滑过渡
    void SetReplayGain(float linearGain);

    // 无缝播放:预先打开下一首 (同一设备格式),当前曲目结束时在同一个音频回调内无缝切换,
    // 随后在通知线程调用 on_track_advanced (而不是 on_playback_ended)
    bool QueueNext(const std::wstring& filePath, double startSeconds, double endSeconds, float replayGain);
    void ClearNext();
    std::function<void()> on_track_advanced;

    AudioPlaybackEngine();
    ~AudioPlaybackEngine();

    // 初始化硬件音频设备 (exclusiveMode: true 开启 WASAPI 独占直通发烧模式)
    bool Init(bool exclusiveMode = false);

    // 关闭音频设备与资源释放
    void Shutdown();

    // 打开本地音频文件 (支持 FLAC, MP3, OGG, WAV)
    bool OpenFile(const std::wstring& filePath);
    // 打开文件中的一个片段 (CUE 分轨),endSeconds <= 0 表示播放到文件结尾
    bool OpenFile(const std::wstring& filePath, double startSeconds, double endSeconds);

    // 播放控制
    bool Play();
    void Pause();
    void Resume();
    void Stop();
    void TogglePlay();

    // 精确跳转进度 (秒,相对于当前片段起点)
    bool SeekTo(double seconds);

    // 音量调节 (0.0f ~ 1.0f)
    void SetVolume(float volume);
    float GetVolume() const;

    // 状态查询
    PlaybackState GetState() const;
    double GetPositionSeconds() const;
    double GetDurationSeconds() const;
    std::wstring GetCurrentFilePath() const;

    // 输出链路信息
    uint32_t GetSourceSampleRate() const { return source_rate_.load(); } // 母带原生采样率
    uint32_t GetOutputSampleRate() const { return output_rate_.load(); } // 实际送往设备的采样率
    uint32_t GetOutputBitDepth() const { return output_bits_.load(); }    // 设备实际位深 (16/24/32)
    bool IsExclusiveMode() const { return exclusive_active_.load(); }
    bool IsResampling() const { return GetSourceSampleRate() != GetOutputSampleRate(); }

    // 获取实时 FFT 频谱分析器引用 (供 UI 读取 32 频段动效)
    FftVisualizer& GetVisualizer() { return visualizer_; }
    // 10 段均衡器 (开关与增益可在任意线程设置)
    Equalizer& GetEqualizer() { return equalizer_; }

    // 回调钩子 (on_playback_ended 在独立通知线程中调用)
    std::function<void(PlaybackState)> on_state_changed;
    std::function<void()> on_playback_ended;

    // 内部数据回调 (由 miniaudio 设备驱动调用)
    void OnAudioFrames(void* pOutput, uint32_t frameCount);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    bool InitDevice(uint32_t sampleRate, uint32_t channels, bool exclusive);
    void NotifierLoop();

    mutable std::mutex state_mutex_;
    std::wstring current_file_;
    std::atomic<PlaybackState> state_{PlaybackState::Stopped};
    std::atomic<float> volume_{1.0f};
    std::atomic<double> current_position_{0.0};
    std::atomic<double> duration_{0.0};
    std::atomic<uint32_t> source_rate_{0};
    std::atomic<uint32_t> output_rate_{0};
    std::atomic<uint32_t> output_bits_{0};
    std::atomic<bool> exclusive_active_{false};

    FftVisualizer visualizer_;
    Equalizer equalizer_;
};
