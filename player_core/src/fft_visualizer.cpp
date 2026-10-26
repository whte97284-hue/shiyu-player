#include "../include/fft_visualizer.hpp"
#include <algorithm>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace {
constexpr float kMinFreq = 30.0f;
constexpr float kMaxFreq = 20000.0f;
constexpr float kFloorDb = -70.0f;

float BandEdge(size_t i, float hi) {
    // 第 i 个频带边界 (共 NUM_BANDS + 1 个),在 [kMinFreq, hi] 上对数均分
    float t = static_cast<float>(i) / static_cast<float>(FftVisualizer::NUM_BANDS);
    return kMinFreq * std::pow(hi / kMinFreq, t);
}
} // namespace

FftVisualizer::FftVisualizer() {
    ring_.assign(MAX_FFT_SIZE, 0.0f);
    current_bars_.assign(NUM_BANDS, 0.0f);
    smooth_bars_.assign(NUM_BANDS, 0.0f);
    Rebuild(FFT_SIZE);
}

void FftVisualizer::Rebuild(size_t fftSize) {
    fft_size_ = fftSize;
    window_.resize(fftSize);
    for (size_t i = 0; i < fftSize; ++i) {
        window_[i] = 0.5f * (1.0f - std::cos(2.0f * static_cast<float>(M_PI) * i / (fftSize - 1)));
    }
    twiddle_re_.resize(fftSize / 2);
    twiddle_im_.resize(fftSize / 2);
    for (size_t k = 0; k < fftSize / 2; ++k) {
        double a = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(fftSize);
        twiddle_re_[k] = static_cast<float>(std::cos(a));
        twiddle_im_[k] = static_cast<float>(std::sin(a));
    }
}

void FftVisualizer::SetSampleRate(uint32_t sampleRate) {
    if (sampleRate == 0) return;
    // 保持约 47 Hz 的频率分辨率:48k->1024, 96k->2048, 192k->4096, 384k->8192
    size_t n = FFT_SIZE;
    while (n < MAX_FFT_SIZE && sampleRate / static_cast<uint32_t>(n) > 48) n *= 2;
    std::lock_guard<std::mutex> lock(mtx_);
    sample_rate_ = sampleRate;
    if (n != fft_size_) Rebuild(n);
    ring_pos_ = 0;
    ring_filled_ = 0;
    std::fill(ring_.begin(), ring_.end(), 0.0f);
}

void FftVisualizer::Reset() {
    std::lock_guard<std::mutex> lock(mtx_);
    ring_pos_ = 0;
    ring_filled_ = 0;
    std::fill(ring_.begin(), ring_.end(), 0.0f);
    std::fill(current_bars_.begin(), current_bars_.end(), 0.0f);
    std::fill(smooth_bars_.begin(), smooth_bars_.end(), 0.0f);
}

void FftVisualizer::PushSamples(const float* samples, size_t count, size_t channels) {
    if (!samples || count == 0 || channels == 0) return;
    // 音频线程:拿不到锁就跳过本块 (UI 正在读取),绝不阻塞声卡回调
    std::unique_lock<std::mutex> lock(mtx_, std::try_to_lock);
    if (!lock.owns_lock()) return;

    const size_t cap = ring_.size();
    const float inv = 1.0f / static_cast<float>(channels);
    size_t frames = count / channels;
    for (size_t i = 0; i < frames; ++i) {
        float mono = 0.0f;
        for (size_t c = 0; c < channels; ++c) mono += samples[i * channels + c];
        ring_[ring_pos_] = mono * inv;
        if (++ring_pos_ == cap) ring_pos_ = 0;
    }
    ring_filled_ = std::min(cap, ring_filled_ + frames);
}

void FftVisualizer::ComputeFft(std::vector<float>& real, std::vector<float>& imag) const {
    const size_t n = real.size();
    // 1. 位反转排列 (Bit-reversal permutation)
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(real[i], real[j]);
            std::swap(imag[i], imag[j]);
        }
    }
    // 2. Cooley-Tukey 蝶形变换 (查表旋转因子,无三角函数调用、无误差累积)
    for (size_t len = 2; len <= n; len <<= 1) {
        size_t half = len / 2;
        size_t step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t k = 0; k < half; ++k) {
                float wr = twiddle_re_[k * step], wi = twiddle_im_[k * step];
                size_t a = i + k, b = a + half;
                float vr = real[b] * wr - imag[b] * wi;
                float vi = real[b] * wi + imag[b] * wr;
                real[b] = real[a] - vr;
                imag[b] = imag[a] - vi;
                real[a] += vr;
                imag[a] += vi;
            }
        }
    }
}

std::vector<float> FftVisualizer::GetSpectrumBars(float dtSeconds) {
    std::vector<float> real, imag;
    uint32_t sr;
    size_t n;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        n = fft_size_;
        sr = sample_rate_;
        real.assign(n, 0.0f);
        // 取最近 n 个样本 (不足时右对齐,前部补零)
        size_t avail = std::min(ring_filled_, n);
        size_t cap = ring_.size();
        for (size_t i = 0; i < avail; ++i) {
            size_t src = (ring_pos_ + cap - avail + i) % cap;
            size_t dst = n - avail + i;
            real[dst] = ring_[src] * window_[dst];
        }
    }
    imag.assign(n, 0.0f);
    ComputeFft(real, imag);

    // 幅度校准:Hann 窗相干增益 0.5,单边谱 ×2 → 满幅正弦峰值 = 1.0 (0 dBFS)
    const size_t half = n / 2;
    const float scale = 4.0f / static_cast<float>(n);
    std::vector<float> mags(half + 1);
    for (size_t i = 0; i <= half && i < n; ++i) mags[i] = std::sqrt(real[i] * real[i] + imag[i] * imag[i]) * scale;

    const float binHz = static_cast<float>(sr) / static_cast<float>(n);
    const float hi = std::min(kMaxFreq, 0.5f * static_cast<float>(sr) * 0.98f);
    std::vector<float> newBars(NUM_BANDS, 0.0f);
    for (size_t b = 0; b < NUM_BANDS; ++b) {
        float f0 = BandEdge(b, hi), f1 = BandEdge(b + 1, hi);
        float lo = f0 / binHz, up = f1 / binHz;
        float peak = 0.0f;
        size_t k0 = static_cast<size_t>(std::ceil(lo)), k1 = static_cast<size_t>(std::floor(up));
        if (k1 >= k0) {
            for (size_t k = k0; k <= k1 && k <= half; ++k) peak = std::max(peak, mags[k]);
        } else {
            // 频带窄于一个 bin (低频区):在频带中心做线性插值,避免多根柱子共用同一个 bin
            float c = 0.5f * (lo + up);
            size_t k = static_cast<size_t>(c);
            float frac = c - static_cast<float>(k);
            if (k + 1 <= half) peak = mags[k] * (1.0f - frac) + mags[k + 1] * frac;
        }
        // 频谱倾斜补偿:以 1 kHz 为基准 +3 dB/倍频程,使粉红噪声/音乐呈现平直视觉
        float fc = std::sqrt(f0 * f1);
        float tiltDb = 3.0f * std::log2(fc / 1000.0f);
        float db = 20.0f * std::log10(peak + 1e-9f) + tiltDb;
        float norm = (db - kFloorDb) / -kFloorDb;
        newBars[b] = std::min(1.0f, std::max(0.0f, norm));
    }

    // 时间轴平滑滤波 (Attack 快速上升 + Decay 慢速阻尼下落);系数以 60 Hz 为基准按实际间隔换算
    const float frames = std::min(6.0f, std::max(0.05f, dtSeconds * 60.0f));
    const float keep = std::pow(0.4f, frames);
    const float fall = 0.04f * frames;
    std::lock_guard<std::mutex> lock(mtx_);
    current_bars_ = newBars;
    for (size_t b = 0; b < NUM_BANDS; ++b) {
        if (newBars[b] >= smooth_bars_[b]) {
            smooth_bars_[b] = smooth_bars_[b] * keep + newBars[b] * (1.0f - keep);
        } else {
            smooth_bars_[b] = std::max(newBars[b], smooth_bars_[b] - fall);
        }
    }
    return smooth_bars_;
}

std::vector<float> FftVisualizer::GetBandCenterFrequencies() const {
    float hi = std::min(kMaxFreq, 0.5f * static_cast<float>(sample_rate_) * 0.98f);
    std::vector<float> out(NUM_BANDS);
    for (size_t b = 0; b < NUM_BANDS; ++b) out[b] = std::sqrt(BandEdge(b, hi) * BandEdge(b + 1, hi));
    return out;
}
