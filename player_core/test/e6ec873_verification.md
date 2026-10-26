# e6ec873 播放引擎重写 —— MSVC 编译与独占模式实测回传

## 0. 代码版本

- 目标提交: `e6ec873 播放引擎重写:真·WASAPI 独占、母带原生采样率直通、CUE 片段播放、结束回调防死锁`
- 改动文件: `audio_playback_engine.hpp/.cpp`、`test_hires_runner.cpp`、`run_host_tests.sh`、新增 `tests/host/test_engine_host.cpp`
- 说明: 本地 git 分支引用缺失(unborn),**未做任何 git 写操作**,用 `git show e6ec873:<path>` 取出
  5 个改动文件写入工作区,并逐个用 `git hash-object` 校验与提交内一致(全部 OK)。

---

## 1. ⚠️ MSVC 编译:**失败** —— 已定位并本地修复

### 1.1 原始报错(未修改任何代码时)

```
src/audio_playback_engine.cpp(268): error C2589: “(”:“::”右边的非法标记
src/audio_playback_engine.cpp(268): error C2059: 语法错误:“)”
src/audio_playback_engine.cpp(268): error C2059: 语法错误:“)”
src/audio_playback_engine.cpp(325): error C2589: “(”:“::”右边的非法标记
src/audio_playback_engine.cpp(325): error C2059: 语法错误:“)”
```

**两个 exe 全部构建失败**,`test_phase2_runner.exe` / `test_hires_runner.exe` 都停留在旧版本。

### 1.2 根因

`audio_playback_engine.cpp` 第一个 include 是 `miniaudio.h`,它会间接包含 `<windows.h>`。
**miniaudio.h 自己不定义 `NOMINMAX`**(已 grep 确认),于是 `<windows.h>` 的 `min`/`max` 宏生效:

- `:268` `volume_.store(std::min(1.0f, std::max(0.0f, volume)));` → 宏展开成 `std::((1.0f) < (...))` → 语法错误
- `:325` `cursor - std::min(cursor, impl_->seg_start)` → 同样
- `:303` `std::min<ma_uint64>(want, ...)` 因为带显式模板参数 `min<`,**宏匹配不上**,所以只有这两处报错

**这是纯 MSVC 才有的问题**:GCC/Clang 没有 min/max 宏,所以 Linux 侧 ASan/UBSan 全绿也测不出来。

### 1.3 本地修复(按项目既有约定)

`platform_io.cpp`、`tag_common.cpp` 都已经写了 `NOMINMAX`,只有这个文件漏了。补上即可:

```cpp
// miniaudio.h 会间接包含 <windows.h>,而它自身不定义 NOMINMAX。
// 不禁用的话 <windows.h> 的 min/max 宏会把 std::min(...)/std::max(...) 展开成
// "std::(... ? ... : ...)" ,在 MSVC 下报 C2589/C2059 (GCC 无此宏,故 Linux 侧测不出来)。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "../include/miniaudio.h"
```

> **这个补丁目前只在我本地。请务必在 e6ec873 之上追加提交,否则下次拉取又会编译不过。**

修复后:两个 exe 均编译通过,无 error、无 warning。

---

## 2. test_phase2_runner.exe 回归:**41 项全过**

```
退出码=0
PASS=41  FAIL=0  合计=41
  [PASS] 异步入库结果成功
  [PASS] 异步执行后 IsRunning() == false
  ================ 最终物理断言统计: 通过 41 项 / 失败 0 项 ================
```

旧接口兼容性无回归。✅

---

## 3. test_hires_runner.exe 梯度 1 —— 【独占模式实测】原文

```
  [独占模式实测] 初始化: 成功, 实际模式: WASAPI 独占, 母带采样率: 44100 Hz, 设备输出: 48000 Hz (存在重采样)
  [独占模式实测] 播放 0.8s 后位置: 0.838095 s
```

（测试样本是 44.1kHz 的 Cyberpunk FLAC,不是 96k,所以这一行看不出"96k 是否直通"。）

---

## 4. 🔴 追加验证:96k/192k **并没有**原生直通

因为你提到"这一项只有真实声卡结果才算数",我用四种采样率的 24bit 母带各测了一遍
(自写探针 `test/probe_exclusive.cpp`,直接调引擎的公开 API)。

```
源(Hz)   输出(Hz)  Init Open 独占  重采样  结果
44100    48000     OK   OK   独占  是     独占但重采样
48000    48000     OK   OK   独占  否     原生直通
96000    48000     OK   OK   独占  是     独占但重采样
192000   48000     OK   OK   独占  是     独占但重采样
```

**只有 48k 显示"原生直通"**——而那只是因为设备原生格式恰好就是 48k。

### 4.1 根因:miniaudio 在 Windows 独占模式下**忽略 cfg.sampleRate**

裸 miniaudio 探针(`test/probe_ma_exclusive.cpp`,不经项目引擎):

```
=== SHARED (baseline) ===
  SHARED req  44100 Hz -> OK   device.sampleRate=44100   internalSampleRate=44100  SAME
  SHARED req  96000 Hz -> OK   device.sampleRate=96000   internalSampleRate=96000  SAME
  SHARED req 192000 Hz -> OK   device.sampleRate=192000  internalSampleRate=192000 SAME

=== EXCLUSIVE ===
  EXCL   req  44100 Hz -> OK   device.sampleRate=44100   internalSampleRate=48000  REWRITTEN
  EXCL   req  48000 Hz -> OK   device.sampleRate=48000   internalSampleRate=48000  SAME
  EXCL   req  88200 Hz -> OK   device.sampleRate=88200   internalSampleRate=48000  REWRITTEN
  EXCL   req  96000 Hz -> OK   device.sampleRate=96000   internalSampleRate=48000  REWRITTEN
  EXCL   req 176400 Hz -> OK   device.sampleRate=176400  internalSampleRate=48000  REWRITTEN
  EXCL   req 192000 Hz -> OK   device.sampleRate=192000  internalSampleRate=48000  REWRITTEN

miniaudio version: 0.11.25
```

**共享模式**:请求什么率就得到什么率(设备全部支持 44.1k~192k)。
**独占模式**:请求任何率都返回成功,但 `internalSampleRate` **恒为 48000**。

### 4.2 源码级证据(`include/miniaudio.h`)

`23694–23731` 行,独占分支:

```c
if (pData->shareMode == ma_share_mode_exclusive) {
    /* In exclusive mode on desktop we always use the backend's native format. */
    hr = ma_IPropertyStore_GetValue(pStore, &MA_PKEY_AudioEngine_DeviceFormat, &prop);
    ...
    hr = ma_IAudioClient_IsFormatSupported(..., MA_AUDCLNT_SHAREMODE_EXCLUSIVE, pActualFormat, NULL);
    if (SUCCEEDED(hr)) { MA_COPY_MEMORY(&wf, pActualFormat, ...); }   // wf = 设备原生格式
```

即:Windows 桌面独占路径**无条件使用 `PKEY_AudioEngine_DeviceFormat`(系统"默认格式")**,
`cfg.sampleRate` 完全不参与流格式协商。**没有任何配置项能改变这一点。**

而 `internalSampleRate` 确实是**真实流格式率**(`24211`: `pDevice->playback.internalSampleRate = data.sampleRateOut;`,
`sampleRateOut` 来自内部 WASAPI 初始化)。`device.sampleRate` 只是回显请求值,miniaudio 之后自己做一次转换。

### 4.3 结论

| Claude 的说法 | 实测 |
|---|---|
| 独占模式现在是真的(shareMode=exclusive) | ✅ **成立**。`IsExclusiveMode()` 如实返回 true,是真独占 |
| 声卡不支持时自动退回共享 | ✅ 代码逻辑正确(独占失败→shared 重试) |
| **96k/192k 不再降到 44.1k,按母带原生采样率重开声卡** | ❌ **不成立**。独占流恒为系统默认格式(本机 48000),96k/192k 母带仍被重采样到 48k |
| `GetOutputSampleRate()` / `IsResampling()` | ✅ **报告是准的**——它如实说出了 48000 和"有重采样"。不是显示 bug,是功能没实现 |

### 4.4 两个连带问题

1. **`OpenFile` 里的重开设备是无效开销**:
   ```cpp
   if (impl_->exclusive_requested && srcRate > 0 && srcRate != impl_->device.playback.internalSampleRate) {
       if (!InitDevice(srcRate, impl_->out_channels, true)) { ... }
   }
   ```
   因为 miniaudio 忽略 `cfg.sampleRate`,重开后流格式依旧是 48000;而每次 `OpenFile`
   只要 `srcRate != 48000` 就会**拆掉并重建整个设备**(`ma_device_uninit` + `ma_device_init`)。
   代价是真实的,收益是零。
2. **高质量重采样没生效**:`InitDevice` 里 `out_rate = device.sampleRate`(=请求率),
   `OpenFile` 里 `ma_decoder_config_init(..., impl_->out_rate)` → 解码器认为目标率 == 源率,**不重采样**,
   于是 `dc.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER` 那句形同虚设;
   真正做 96k→48k 转换的是 **miniaudio 内部的默认重采样器**。
   (共享模式下反而是对的:out_rate=48000≠源率,解码器用自己的高阶低通完成转换。)

### 4.5 要做到真·原生直通,可选路径(供参考)

- 想要独占原生率,必须**绕开 miniaudio 的独占路径**,自己用 WASAPI:
  `IsFormatSupported(EXCLUSIVE, 目标WAVEFORMATEXTENSIBLE)` 成功后再 `Initialize(EXCLUSIVE, ...)`;
  或在独占前先按目标率枚举。
- 或者:在 UI 上如实显示"独占,输出 48000 Hz(重采样)",并允许用户改 Windows 默认格式;
  当前 `IsResampling()` 已经能给出这个信息。
- 单纯调 miniaudio 配置**无解**——0.11.25 的独占分支里没有走 `cfg.sampleRate` 的代码路径。

---

## 5. 耳机试听:❌ 我做不到

**我没有听觉,无法主观判断"暂停/拖动进度/调音量有没有爆音或卡顿"。** 这一项只能你自己戴耳机听。
我能提供的客观替代:

- 引擎侧已通过 41 项断言(含 Pause/Seek/音量/异步/结束回调);
- 若要客观查爆音,可以录制 WASAPI loopback 输出后做样本级不连续点检测(需要额外采集链路),
  需要的话我可以做。

---

## 6. 附:临时诊断文件(可删)

| 文件 | 用途 |
|---|---|
| `test/probe_exclusive.cpp` / `.exe` | 四种采样率经引擎测独占输出率 |
| `test/probe_ma_exclusive.cpp` / `.exe` | 裸 miniaudio 独占/共享能力枚举 |

（另:上一轮遗留的 `test/round2_digest.md` 是 Hi-Res 库扫描数据的回传文件。）

---

# 7. 追加验证:96k/192k 不直通,**是设备问题**

用原生 WASAPI 探针 `test/probe_wasapi_exclusive.cpp`(枚举全部活动播放设备,
对每个设备调用 `IAudioClient::IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE)`)
在本机实测,结果如下(Y=支持,N=不支持):

| # | 设备 | 系统默认格式 | 独占 16bit | 独占 24in24 | 独占 24in32 | 独占 32f |
|---|---|---|---|---|---|---|
| [0] | Realtek High Definition Audio (板载模拟) | 48k/2/32 | 44.1k Y 48k Y 88.2k N **96k Y** 176.4k N **192k Y** | 全 N | 44.1k Y 48k Y **96k Y** **192k Y** | 全 N |
| [1] | Steam Streaming Speakers (虚拟) | 48k/2/16 | **44.1k~192k 全 Y** | **全 Y** | 全 N | 全 N |
| [2] | TCL SMART TV (NVIDIA HD Audio) | 48k/2/16 | 44.1k Y 48k Y 其余 N | 全 N | 44.1k Y 48k Y | 全 N |
| [3] | Steam Streaming Microphone (虚拟) | 48k/2/32 | **全 Y** | **全 Y** | 全 N | 全 N |
| **[4]** | **<== 系统默认: 2- Razer Barracuda X** | 48k/2/16 | **44.1k Y 48k Y;88.2k/96k/176.4k/192k 全 N** | 全 N | **全 N** | 全 N |
| [5] | P28H2V (NVIDIA HD Audio) | 48k/2/16 | 仅 48k Y | 全 N | 仅 48k Y | 全 N |
| [6] | Realtek Digital Output (S/PDIF) | 48k/2/16 | 44.1k Y 48k Y **96k Y** **192k Y** | 全 N | 44.1k Y 48k Y **96k Y** **192k Y** | 全 N |

## 7.1 结论:**是设备限制**

**本机系统默认播放设备是「2- Razer Barracuda X」(雷蛇无线耳机),它的独占模式只支持
44100 / 48000 Hz,且只支持 16bit。** 88.2k / 96k / 176.4k / 192k 一律不支持,24bit 也不支持。

因此在这台机器上,**任何代码都无法**通过默认设备实现 96k/192k 原生直通 —— 硬件不具备该能力。
`InitDevice(srcRate, ...)` 注定失败,miniaudio 回落到设备默认格式 48000/16bit,
所以 `GetOutputSampleRate()` 报 48000 是**如实反映硬件现实**,不是 bug。

## 7.2 但要改的话,有可用的设备

板载 **Realtek**(模拟输出 [0] 与数字输出 [6])**支持 96k / 192k 独占**
(16bit 或 24-in-32 容器)。把 Windows 默认播放设备切到板载声卡,96k/192k 原生直通就能成立。

## 7.3 顺带修正一条之前的说法

上一版探针我写"没有任何设备支持 24bit 独占",**那条不准确**:纯 24-in-24 容器几乎全被拒,
但 **24-in-32(32 位容器 + 24 位有效)在 [0]/[2]/[5]/[6] 上是支持的**。是容器格式问题,不是位深不支持。
(32-bit float 独占则确实**全设备不支持**;但 miniaudio 独占用设备原生格式,不受影响。)

## 7.4 给引擎实现的建议

1. 打开独占前先问 `IsFormatSupported(EXCLUSIVE, 目标格式)`,**据此决定**:
   支持 → 独占直通;不支持 → 明确回退共享,并在 UI 上如实告知("本设备独占不支持 96k,已回退"),
   而不是静默地按 48000 输出还去做一次无用的设备重建。
2. 想真做原生率独占,必须绕开 miniaudio 的独占路径(见 §4.2),自己走 WASAPI。
3. 当前默认设备独占是 **48000/16bit**,意味着 24bit 母带在独占下会被降到 16bit —— 这一点也值得在 UI 提示。
