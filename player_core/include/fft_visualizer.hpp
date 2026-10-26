#pragma once
#include <vector>
#include <cstdint>
#include <mutex>
#include <cmath>
#include <atomic>

// ============================================================================
//  FftVisualizer —— 采样率自适应的 32 频段频谱分析器
//   · 音频线程写入为无分配、无阻塞 (固定环形缓冲 + try_lock,抢不到锁就丢一帧可视化数据,绝不卡声卡)
//   · 频段按「物理频率」对数划分 (30 Hz ~ 20 kHz),而非按 FFT bin 序号划分:
//     44.1k / 96k / 192k 母带下同一频率落在同一根频柱上,192k 不会把一半频柱浪费在超声波区
//   · FFT 点数随采样率自动放大 (1024 @48k → 4096 @192k),保持约 47 Hz 的恒定频率分辨率
//   · 预计算 Hann 窗与旋转因子;幅度按窗函数相干增益校准 (满幅正弦 = 0 dBFS)
// ============================================================================
class FftVisualizer {
public:
    static constexpr size_t FFT_SIZE = 1024;      // 基准 FFT 点数 (48 kHz 及以下)
    static constexpr size_t NUM_BANDS = 32;       // 32 段离散频域能量柱
    static constexpr size_t MAX_FFT_SIZE = 8192;

    FftVisualizer();
    ~FftVisualizer() = default;

    // 设置输入采样率 (切歌时由播放引擎调用),自动选择 FFT 点数并重建窗函数/旋转因子
    void SetSampleRate(uint32_t sampleRate);
    uint32_t GetSampleRate() const { return sample_rate_.load(); }

    // 供给音频回调写入 PCM 采样数据 (单声道或双声道交错输入);实时安全,不分配内存
    void PushSamples(const float* samples, size_t count, size_t channels);

    // 获取当前平滑后的 32 频段能量值 (范围 0.0f ~ 1.0f，按 60FPS 动效刷新调用)
    // dtSeconds:距上次调用的时间,平滑系数按它换算,任意调用频率 (60 / 120 / 144 Hz) 下回落速度一致
    std::vector<float> GetSpectrumBars(float dtSeconds = 1.0f / 60.0f);

    // 每个频柱的中心频率 (Hz),供 UI 绘制刻度
    std::vector<float> GetBandCenterFrequencies() const;

    // 重置能量柱
    void Reset();

private:
    void Rebuild(size_t fftSize);
    void ComputeFft(std::vector<float>& real, std::vector<float>& imag) const;

    std::mutex mtx_;
    std::atomic<uint32_t> sample_rate_{44100};
    size_t fft_size_ = FFT_SIZE;
    std::vector<float> ring_;            // 单声道环形缓冲 (容量 MAX_FFT_SIZE)
    size_t ring_pos_ = 0;
    size_t ring_filled_ = 0;
    std::vector<float> window_;          // 预计算 Hann 窗
    std::vector<float> twiddle_re_;      // 预计算旋转因子
    std::vector<float> twiddle_im_;
    std::vector<float> current_bars_;    // 当前瞬时频段幅值
    std::vector<float> smooth_bars_;     // 动效平滑阻尼能量柱
};
