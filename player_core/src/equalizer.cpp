#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../include/miniaudio.h" // 实现在 audio_playback_engine.cpp
#include "../include/equalizer.hpp"
#include <algorithm>
#include <cmath>

const std::array<float, Equalizer::kBands> Equalizer::kFrequencies = {
    31.0f, 62.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f};

namespace {
constexpr double kQ = 1.41421356;       // 一个倍频程带宽
constexpr double kShelfSlope = 1.0;     // 搁架最陡且不过冲
constexpr float kSlewDbPerSec = 60.0f;  // 增益变化速度上限
}

struct Equalizer::Impl {
    ma_loshelf2 lo;
    ma_peak2 peak[kBands - 2];
    ma_hishelf2 hi;
    bool configured = false;
    bool bypassed = true;
    uint32_t rate = 0;
    uint32_t channels = 0;
    std::array<bool, kBands> usable{};   // 中心频率低于 0.45 × 采样率才启用
    std::array<float, kBands> cur{};     // 音频线程:各段实际增益 (dB)
    float pre = 1.0f;                    // 音频线程:实际前级增益 (线性)

    void Uninit() {
        if (!configured) return;
        ma_loshelf2_uninit(&lo, nullptr);
        for (auto& p : peak) ma_peak2_uninit(&p, nullptr);
        ma_hishelf2_uninit(&hi, nullptr);
        configured = false;
    }
    ma_result Apply(int b, bool init) {
        const double f = kFrequencies[b], g = cur[b];
        if (b == 0) {
            auto c = ma_loshelf2_config_init(ma_format_f32, channels, rate, g, kShelfSlope, f);
            return init ? ma_loshelf2_init(&c, nullptr, &lo) : ma_loshelf2_reinit(&c, &lo);
        }
        if (b == kBands - 1) {
            auto c = ma_hishelf2_config_init(ma_format_f32, channels, rate, g, kShelfSlope, f);
            return init ? ma_hishelf2_init(&c, nullptr, &hi) : ma_hishelf2_reinit(&c, &hi);
        }
        auto c = ma_peak2_config_init(ma_format_f32, channels, rate, g, kQ, f);
        return init ? ma_peak2_init(&c, nullptr, &peak[b - 1]) : ma_peak2_reinit(&c, &peak[b - 1]);
    }
    ma_biquad& Bq(int b) { return b == 0 ? lo.bq : b == kBands - 1 ? hi.bq : peak[b - 1].bq; }
    void Run(int b, float* s, ma_uint64 n) {
        if (b == 0) ma_loshelf2_process_pcm_frames(&lo, s, s, n);
        else if (b == kBands - 1) ma_hishelf2_process_pcm_frames(&hi, s, s, n);
        else ma_peak2_process_pcm_frames(&peak[b - 1], s, s, n);
    }
};

Equalizer::Equalizer() : impl_(new Impl) {
    for (auto& t : target_) t.store(0.0f);
}

Equalizer::~Equalizer() { impl_->Uninit(); }

void Equalizer::Configure(uint32_t sampleRate, uint32_t channels) {
    Impl& m = *impl_;
    if (m.configured && m.rate == sampleRate && m.channels == channels) return;
    m.Uninit();
    if (sampleRate == 0 || channels == 0) return;
    m.rate = sampleRate;
    m.channels = channels;
    bool ok = true;
    for (int b = 0; b < kBands; ++b) {
        m.usable[b] = kFrequencies[b] < 0.45f * static_cast<float>(sampleRate);
        if (!m.usable[b]) m.cur[b] = 0.0f;
        ok = ok && m.Apply(b, true) == MA_SUCCESS;
    }
    m.configured = ok;
    m.bypassed = true;
}

void Equalizer::SetGain(int band, float db) {
    if (band < 0 || band >= kBands || !std::isfinite(db)) return;
    target_[band].store(std::max(-kMaxGainDb, std::min(kMaxGainDb, db)));
}

float Equalizer::GetGain(int band) const {
    return band >= 0 && band < kBands ? target_[band].load() : 0.0f;
}

void Equalizer::Process(float* samples, uint64_t frames) {
    Impl& m = *impl_;
    if (!m.configured || frames == 0) return;

    // 各段实际增益逼近目标 (关闭时目标为 0 dB)
    const bool on = enabled_.load();
    const float maxStep = kSlewDbPerSec * static_cast<float>(frames) / static_cast<float>(m.rate);
    bool any = false;
    float peakGain = 0.0f;
    for (int b = 0; b < kBands; ++b) {
        if (!m.usable[b]) continue;
        const float tgt = on ? target_[b].load() : 0.0f;
        float& c = m.cur[b];
        if (c != tgt) {
            c = std::abs(tgt - c) <= maxStep ? tgt : c + (tgt > c ? maxStep : -maxStep);
            m.Apply(b, false);
        }
        if (c != 0.0f) any = true;
        peakGain = std::max(peakGain, c);
    }

    if (!any && m.pre == 1.0f) {
        // 整段旁路:滤波器状态清零,下次重新启用时没有陈旧状态带来的瞬态
        if (!m.bypassed) {
            for (int b = 0; b < kBands; ++b) ma_biquad_clear_cache(&m.Bq(b));
            m.bypassed = true;
        }
        active_.store(false);
        return;
    }
    m.bypassed = false;
    active_.store(true);

    // 0 dB 的段也照样跑 (系数即恒等):保持状态连续,它再被拉起时不会有瞬态
    for (int b = 0; b < kBands; ++b)
        if (m.usable[b]) m.Run(b, samples, frames);

    // 前级:最大正增益多少就整体压低多少,按样本线性过渡
    const float preTarget = peakGain > 0.0f ? std::pow(10.0f, -peakGain / 20.0f) : 1.0f;
    float p = m.pre;
    if (p != preTarget || p != 1.0f) {
        const float step = (preTarget - p) / static_cast<float>(frames);
        for (uint64_t f = 0, i = 0; f < frames; ++f) {
            p += step;
            for (uint32_t c = 0; c < m.channels; ++c, ++i) samples[i] *= p;
        }
        m.pre = preTarget;
    }
}
