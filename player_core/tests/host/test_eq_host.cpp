// 均衡器:旁路逐位不变、各段响应、前级防削波、增益平滑、高采样率下低频段精度
#include "../../include/equalizer.hpp"
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

static int g_pass = 0, g_fail = 0;
static void Check(const char* name, bool cond, const std::string& detail = "") {
    std::printf("%s %s %s\n", cond ? "[PASS]" : "[FAIL]", name, detail.c_str());
    (cond ? g_pass : g_fail)++;
}

static const double kPi = 3.14159265358979323846;

// 立体声正弦,按 block 帧一批送进均衡器;返回最后 measure 秒的 RMS 增益 (dB)
struct Run {
    double rate;
    double phase = 0;
    Run(double r) : rate(r) {}
    std::vector<float> Sine(double freq, size_t frames) {
        std::vector<float> v(frames * 2);
        for (size_t i = 0; i < frames; ++i) {
            const float s = static_cast<float>(0.25 * std::sin(phase));
            v[2 * i] = v[2 * i + 1] = s;
            phase += 2 * kPi * freq / rate;
        }
        return v;
    }
};

static double GainDb(Equalizer& eq, double rate, double freq, double settle = 0.6, double measure = 0.2) {
    Run r(rate);
    const size_t block = 480;
    const size_t settleFrames = static_cast<size_t>(settle * rate), measureFrames = static_cast<size_t>(measure * rate);
    double in2 = 0, out2 = 0;
    for (size_t done = 0; done < settleFrames + measureFrames; done += block) {
        auto v = r.Sine(freq, block);
        auto orig = v;
        eq.Process(v.data(), block);
        if (done >= settleFrames)
            for (size_t i = 0; i < v.size(); ++i) { in2 += orig[i] * orig[i]; out2 += v[i] * v[i]; }
    }
    return 10 * std::log10(out2 / in2);
}

static std::string F(double v) { char b[32]; std::snprintf(b, sizeof b, "%.2f dB", v); return b; }

int main() {
    // ---- 旁路 ----
    {
        Equalizer eq;
        eq.Configure(48000, 2);
        Run r(48000);
        auto v = r.Sine(997, 4800), orig = v;
        eq.Process(v.data(), 4800);
        Check("关闭时样本逐位不变", v == orig);
        Check("关闭时不算启用", !eq.IsActive());
        eq.SetEnabled(true);
        v = orig;
        eq.Process(v.data(), 4800);
        Check("打开但全部 0 dB 时样本逐位不变", v == orig);
        eq.SetGain(3, 30.0f);
        Check("增益限制在 ±12 dB", eq.GetGain(3) == 12.0f);
        eq.SetGain(3, NAN);
        Check("非法增益被忽略", eq.GetGain(3) == 12.0f);
    }

    // ---- 峰值段:1 kHz +12 dB,前级压低 12 dB → 1 kHz 净 0 dB,远处 -12 dB ----
    {
        Equalizer eq;
        eq.Configure(48000, 2);
        eq.SetEnabled(true);
        eq.SetGain(5, 12.0f);
        const double at = GainDb(eq, 48000, 1000), far = GainDb(eq, 48000, 8000), low = GainDb(eq, 48000, 100);
        Check("1 kHz 提升 12 dB 后净增益约 0 dB (前级抵消)", std::abs(at) < 0.3, F(at));
        Check("8 kHz 几乎不受影响 (只剩前级 -12 dB)", std::abs(far + 12) < 0.6, F(far));
        Check("100 Hz 几乎不受影响", std::abs(low + 12) < 0.6, F(low));
        Check("提升时处于启用状态", eq.IsActive());
    }

    // ---- 搁架:31 Hz 低架 +6 dB ----
    {
        Equalizer eq;
        eq.Configure(44100, 2);
        eq.SetEnabled(true);
        eq.SetGain(0, 6.0f);
        const double sub = GainDb(eq, 44100, 8, 1.5), mid = GainDb(eq, 44100, 2000);
        Check("低架 8 Hz 净增益约 0 dB (搁架拐点 31 Hz 处为一半)", std::abs(sub) < 0.6, F(sub));
        Check("低架 2 kHz 约 -6 dB (前级)", std::abs(mid + 6) < 0.3, F(mid));
    }

    // ---- 衰减不加前级;192 kHz 下 31 Hz 段仍准确 ----
    {
        Equalizer eq;
        eq.Configure(192000, 2);
        eq.SetEnabled(true);
        eq.SetGain(1, -12.0f);
        const double at = GainDb(eq, 192000, 62), far = GainDb(eq, 192000, 4000);
        Check("192 kHz 下 62 Hz 衰减 12 dB", std::abs(at + 12) < 0.5, F(at));
        Check("只衰减时不压前级", std::abs(far) < 0.2, F(far));
    }

    // ---- 采样率太低时最高段停用 ----
    {
        Equalizer eq;
        eq.Configure(32000, 2);
        eq.SetEnabled(true);
        eq.SetGain(9, 12.0f);
        Run r(32000);
        auto v = r.Sine(440, 3200), orig = v;
        eq.Process(v.data(), 3200);
        Check("32 kHz 下 16 kHz 段停用 (不处理样本)", v == orig);
    }

    // ---- 平滑:猛推 12 dB 不出现跳变;关闭后回到逐位旁路 ----
    {
        Equalizer eq;
        eq.Configure(48000, 2);
        eq.SetEnabled(true);
        Run r(48000);
        const size_t block = 480;
        double maxJump = 0;
        float last = 0;
        for (int i = 0; i < 100; ++i) {
            if (i == 10) eq.SetGain(5, 12.0f);
            auto v = r.Sine(50, block);
            eq.Process(v.data(), block);
            for (size_t k = 0; k < block; ++k) { maxJump = std::max(maxJump, std::abs(double(v[2 * k] - last))); last = v[2 * k]; }
        }
        // 50 Hz、0.25 幅度的正弦相邻样本差最多约 0.0016
        Check("增益突变时没有样本跳变", maxJump < 0.004, std::to_string(maxJump));
        eq.SetEnabled(false);
        for (int i = 0; i < 40; ++i) { auto v = r.Sine(50, block); eq.Process(v.data(), block); }
        Check("关闭后回落完成,不再启用", !eq.IsActive());
        auto v = r.Sine(997, block), orig = v;
        eq.Process(v.data(), block);
        Check("回落后样本逐位不变", v == orig);
        // 再打开:从清零的状态起步,不应有瞬态
        eq.SetEnabled(true);
        auto w = r.Sine(997, block), worig = w;
        eq.Process(w.data(), block);
        double dev = 0;
        for (size_t i = 0; i < w.size(); ++i) dev = std::max(dev, std::abs(double(w[i] - worig[i])));
        Check("重新打开的第一批样本只有平滑的小变化", dev < 0.05, std::to_string(dev));
    }

    std::printf("\n均衡器: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
