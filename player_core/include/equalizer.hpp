#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>

// ============================================================================
//  Equalizer —— 10 段图示均衡器 (滤波器用 miniaudio 自带的 RBJ 双二阶:
//  两端为搁架 ma_loshelf2 / ma_hishelf2,中间 8 段为峰值 ma_peak2,Q = √2 即一个倍频程)
//   · 线程模型:Configure 在非音频线程、音频线程不在 Process 时调用 (引擎在持有解码器锁、
//     重建设备时调用);SetEnabled / SetGain 任意线程随时调用 (原子量);Process 只在音频线程
//   · 拖动增益不会有拉链噪声:每个回调里各段实际增益按 60 dB/s 逼近目标并重算系数
//     (ma_*_reinit 只换系数、保留滤波器状态,不会爆音)
//   · 关闭或全部归零后实际增益回到 0 dB 即整段旁路:不碰样本,独占直通依然逐位无损
//   · 自动前级衰减:按最大正增益压低整体电平,提升低频不会削波
// ============================================================================
class Equalizer {
public:
    static constexpr int kBands = 10;
    static constexpr float kMaxGainDb = 12.0f;
    static const std::array<float, kBands> kFrequencies; // 31 Hz … 16 kHz

    Equalizer();
    ~Equalizer();
    Equalizer(const Equalizer&) = delete;
    Equalizer& operator=(const Equalizer&) = delete;

    // 输出格式 (f32 交错) 变化时重建滤波器;当前增益保留
    void Configure(uint32_t sampleRate, uint32_t channels);

    void SetEnabled(bool on) { enabled_.store(on); }
    bool IsEnabled() const { return enabled_.load(); }
    void SetGain(int band, float db);
    float GetGain(int band) const;

    // 正在改动样本 (开着且有增益,或者还在往 0 dB 回落)
    bool IsActive() const { return active_.load(); }

    // 音频线程:原地处理交错 f32 样本
    void Process(float* samples, uint64_t frames);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> enabled_{false};
    std::atomic<bool> active_{false};
    std::array<std::atomic<float>, kBands> target_{};
};
