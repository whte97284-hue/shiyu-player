// miniaudio.h 会间接包含 <windows.h>;不定义 NOMINMAX 时 MSVC 的 min/max 宏会破坏 std::min/std::max
#ifndef NOMINMAX
#define NOMINMAX
#endif
// stb_vorbis 必须在 miniaudio.h 之前引入:miniaudio 的 Vorbis 支持是靠
// STB_VORBIS_INCLUDE_STB_VORBIS_H 这个宏触发的,没有它 MA_HAS_VORBIS 就不会定义,
// .ogg/.oga 一律返回 MA_INVALID_FILE(看起来像"播放器不支持 OGG")。
// 实现放在 stb_vorbis_impl.cpp,这里只要声明。
#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"
#define MINIAUDIO_IMPLEMENTATION
#include "../include/miniaudio.h"
#include "../include/audio_playback_engine.hpp"
#include "../include/audio_tag_parser.hpp"
#include "../include/platform_io.hpp"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <string>
#include <iostream>
#include <thread>

// ============================================================================
//  流式解码:miniaudio 自定义回调 -> stream_core::StreamSource
//
//  为什么不能继续用 ma_decoder_init_file:文件模式下 miniaudio 在首次读到 0 字节
//  就认定整首歌结束,而在线播放时"0 字节"往往只是网还没把这段数据送到,
//  结果就是每首歌播到缓冲区末尾就断掉。
// ============================================================================

// 传给 miniaudio 解码器的 pUserData。
// 这里持有一份 StreamSource 的强引用:解码器活着期间数据源一定还活着,
// 引擎那边换曲时即使先把 Impl::stream_ 换掉,也不会在读取中途把对象析构。
struct StreamReader {
    std::shared_ptr<stream_core::StreamSource> src;
    uint64_t offset = 0;   // 下一次读取的位置 (只由音频线程推进)
};

// 单次 ReadAt 的等待上限,以及"连续没数据"的重试次数。
// 60 × 50ms ≈ 3 秒:超过 3 秒一个字节都没等到,就认为这条流已经死了(断网/CDN 卡住),
// 按 EOF 收尾,而不是把音频线程永久挂住 —— 回调里挂死会让设备一直欠载,
// 而且 decoder_mutex 一直被占,切歌/停止/退出都会跟着卡死。
// 反过来,只要数据在 3 秒内能到,就继续等:宁可音频线程短暂停顿(设备欠载),
// 也不能把"暂时没数据"当成"播完了",那正是要修的问题。
static const int kStreamReadTimeoutMs = 50;
static const int kStreamReadMaxRetries = 60;

// onRead:尽量填满请求的字节数再返回。
// 注意不能"读到多少返回多少":dr_mp3/dr_flac 等后端把"返回字节数 < 请求字节数"
// 直接当作流结束,所以只有真 EOF(Finished/Failed)才允许短读。
static ma_result StreamReadProc(ma_decoder* pDecoder, void* pBufferOut, size_t bytesToRead, size_t* pBytesRead) {
    if (pBytesRead) *pBytesRead = 0;
    StreamReader* r = pDecoder ? reinterpret_cast<StreamReader*>(pDecoder->pUserData) : NULL;
    if (r == NULL || !r->src) return MA_INVALID_ARGS;

    uint8_t* dst = reinterpret_cast<uint8_t*>(pBufferOut);
    size_t total = 0;
    int idle = 0;
    while (total < bytesToRead) {
        const size_t n = r->src->ReadAt(r->offset + total, dst + total, bytesToRead - total, kStreamReadTimeoutMs);
        if (n > 0) {
            total += n;
            idle = 0;   // 只要还在进数据就继续填,少一次回调少一次抖动
            continue;
        }
        // 0 字节有两种含义:真的播完了,还是"网还没把这段数据送到"。
        // 只有 Finished/Failed 才算 EOF;否则重试等待。
        if (r->src->Finished() || r->src->Failed()) break;
        if (++idle > kStreamReadMaxRetries) break;   // 等太久了,当流已死
    }

    r->offset += total;
    if (pBytesRead) *pBytesRead = total;
    // 一个字节都没读到(且上面已判定 EOF/超时)才报结束;短读由调用方按 EOF 处理。
    return total == 0 ? MA_AT_END : MA_SUCCESS;
}

// onSeek:只允许在"已经下载到"的范围内跳。
// 流是 append-only 的、offset 从 0 连续,所以没有空洞问题;
// 但数据还没到的位置跳过去也只会阻塞等待,所以未下载完时越界直接失败,
// 让上层(SeekTo / ma_decoder_get_length_in_pcm_frames)拿到明确的 false。
static ma_result StreamSeekProc(ma_decoder* pDecoder, ma_int64 byteOffset, ma_seek_origin origin) {
    StreamReader* r = pDecoder ? reinterpret_cast<StreamReader*>(pDecoder->pUserData) : NULL;
    if (r == NULL || !r->src) return MA_INVALID_ARGS;

    ma_int64 base = 0;
    if (origin == ma_seek_origin_start) {
        base = 0;
    } else if (origin == ma_seek_origin_current) {
        base = static_cast<ma_int64>(r->offset);
    } else {   // ma_seek_origin_end:只有知道总长时才支持(常见于探测 MP3 总帧数)
        const uint64_t total = r->src->TotalBytes();
        if (total == 0) return MA_INVALID_ARGS;
        base = static_cast<ma_int64>(total);
    }

    const ma_int64 target = base + byteOffset;
    if (target < 0) return MA_INVALID_ARGS;
    if (static_cast<uint64_t>(target) > r->src->AvailableBytes() && !r->src->Finished())
        return MA_INVALID_ARGS;   // 还没下到那里
    r->offset = static_cast<uint64_t>(target);
    return MA_SUCCESS;
}

// onTell:返回当前 offset。
// 本版本的 ma_decoder_init 只有 onRead/onSeek 两个槽(tell 槽只出现在内部 preinit),
// 所以初始化成功后手动挂上去;解码后端各自维护游标,这里补上只是让
// ma_decoder_tell_bytes 这类调用也能拿到与 StreamReader::offset 一致的值。
static ma_result StreamTellProc(ma_decoder* pDecoder, ma_int64* pCursor) {
    StreamReader* r = pDecoder ? reinterpret_cast<StreamReader*>(pDecoder->pUserData) : NULL;
    if (r == NULL || pCursor == NULL) return MA_INVALID_ARGS;
    *pCursor = static_cast<ma_int64>(r->offset);
    return MA_SUCCESS;
}

// 一个解码槽:当前曲目与预加载的下一首各占一个 (无缝播放)
struct DecoderSlot {
    ma_decoder dec;
    bool init = false;
    ma_uint64 seg_start = 0; // 片段起止 (单位:输出帧)
    ma_uint64 seg_end = 0;
    double duration = 0.0;
    uint32_t src_rate = 0;
    float rg = 1.0f;         // ReplayGain 线性增益
    std::wstring path;
    // 流式播放时解码器的 pUserData 指向它 (文件播放为空);
    // 槽负责它的生命周期:先 ma_decoder_uninit,再 reset,否则解码器会握着悬空指针。
    std::unique_ptr<StreamReader> reader;
};


struct AudioPlaybackEngine::Impl {
    ma_context context;
    bool context_initialized = false;
    ma_device device;
    bool device_initialized = false;
    bool exclusive_requested = false;
    std::string preferred_device;
    ma_device_id device_id;
    ma_uint32 out_rate = 0;     // 送入设备回调的采样率 (解码器输出采样率)
    ma_uint32 out_channels = 2;
    std::mutex decoder_mutex;

    DecoderSlot slot[2];
    std::atomic<int> cur{0};               // 当前播放的槽
    std::atomic<bool> next_ready{false};   // 另一个槽已就绪,可无缝切换
    int stale = -1;                        // 切换后待释放的旧槽 (只在非音频线程释放)
    std::atomic<ma_uint64> cursor{0};

    // ---- 流式播放 ----
    // stream_ 是引擎对数据源的持有引用,streaming_ 标记当前是不是流式曲目。
    // 单独用一把小锁保护,刻意不复用 decoder_mutex:取消流的时候音频线程可能
    // 正卡在 ReadAt 里并握着 decoder_mutex,那样就会互相等死。
    std::mutex stream_mutex;
    std::shared_ptr<stream_core::StreamSource> stream_;
    bool streaming_ = false;

    // 音频线程私有:当前实际增益 (用于平滑斜坡)
    float gain = 1.0f;
    // 重建音频设备前先把这个降到 0,让回调在约 30ms 内把样本淡到静音,
    // 否则 ma_device_uninit 直接丢弃缓冲会在扬声器上"啪"一声(独占模式尤其明显)
    std::atomic<float> fade{1.0f};
    // 设备实际格式为 16 位时,在 f32 -> s16 截断前加 TPDF 抖动
    bool dither16 = false;
    uint32_t rng = 0x12345678u;

    // 通知线程:播放结束 / 无缝切换到下一首
    std::thread notifier;
    std::mutex notify_mutex;
    std::condition_variable notify_cv;
    std::atomic<bool> ended_pending{false};
    std::atomic<bool> advanced_pending{false};
    std::atomic<bool> quit{false};
    // 拆设备期间不要把 ma_device_uninit 触发的 stopped 通知当成"设备丢了"
    std::atomic<bool> shutting_down{false};

    DecoderSlot& Cur() { return slot[cur.load()]; }
    void Release(DecoderSlot& s) {
        if (s.init) ma_decoder_uninit(&s.dec);
        s.init = false;
        s.path.clear();
        // 解码器已经关掉,这里才放下对 StreamSource 的最后一个引用
        s.reader.reset();
    }

    bool Streaming() {
        std::lock_guard<std::mutex> lock(stream_mutex);
        return streaming_;
    }

    // 中止并放弃当前流。
    // **必须在拿 decoder_mutex 之前调用**:音频线程可能正卡在 StreamReadProc 的
    // ReadAt 里等数据,而它此时握着 decoder_mutex —— 只有 Cancel 能让 ReadAt 立刻返回,
    // 反过来先抢锁就会两边一起等死。
    void CancelStream() {
        std::shared_ptr<stream_core::StreamSource> s;
        {
            std::lock_guard<std::mutex> lock(stream_mutex);
            s.swap(stream_);
            streaming_ = false;
        }
        // 在锁外 Cancel:Cancel 可能要等后台下载线程退出,不该堵着锁
        if (s) s->Cancel();
    }
};


// 打开解码器:Windows 走宽字符 (_wfopen);其他平台直接传 UTF-8,不依赖进程 locale
static ma_result OpenDecoder(const std::wstring& path, const ma_decoder_config* cfg, ma_decoder* dec) {
#ifdef _WIN32
    return ma_decoder_init_file_w(path.c_str(), cfg, dec);
#else
    return ma_decoder_init_file(AudioTagParser::WideToUtf8(path).c_str(), cfg, dec);
#endif
}

static void MaDataCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    AudioPlaybackEngine* engine = reinterpret_cast<AudioPlaybackEngine*>(pDevice->pUserData);
    if (engine) engine->OnAudioFrames(pOutput, frameCount);
}

// 设备状态通知（miniaudio 在音频线程上回调）。
//
// 笔记本休眠唤醒后 WASAPI 端点会失效，设备线程随之停掉；以前完全没监听这件事，
// 表现就是"睡醒之后没声音，要去设置里切一下输出设备才恢复"。
// 这里只做一件事：把"设备没了"告诉上层，由它在 UI 线程重建。
static void MaNotificationCallback(const ma_device_notification* pNotification) {
    if (!pNotification || !pNotification->pDevice) return;
    auto* engine = reinterpret_cast<AudioPlaybackEngine*>(pNotification->pDevice->pUserData);
    if (!engine) return;
    engine->HandleDeviceNotification(static_cast<int>(pNotification->type));
}

AudioPlaybackEngine::AudioPlaybackEngine() : impl_(std::make_unique<Impl>()) {
    impl_->notifier = std::thread([this]() { NotifierLoop(); });
}

AudioPlaybackEngine::~AudioPlaybackEngine() {
    Shutdown();
    impl_->quit.store(true);
    impl_->notify_cv.notify_all();
    if (impl_->notifier.joinable()) impl_->notifier.join();
}

void AudioPlaybackEngine::NotifierLoop() {
    while (!impl_->quit.load()) {
        {
            std::unique_lock<std::mutex> lock(impl_->notify_mutex);
            impl_->notify_cv.wait_for(lock, std::chrono::milliseconds(50), [this] {
                return impl_->quit.load() || impl_->ended_pending.load() || impl_->advanced_pending.load();
            });
        }
        if (impl_->advanced_pending.exchange(false)) {
            {
                // 释放切换前的旧解码器 (音频线程不做任何释放)
                std::lock_guard<std::mutex> lock(impl_->decoder_mutex);
                if (impl_->stale >= 0) {
                    impl_->Release(impl_->slot[impl_->stale]);
                    impl_->stale = -1;
                }
                std::lock_guard<std::mutex> sl(state_mutex_);
                current_file_ = impl_->Cur().path;
            }
            if (on_track_advanced) on_track_advanced();
        }
        if (impl_->ended_pending.exchange(false)) {
            // 在普通线程中回调:上层可以在这里直接 OpenFile/Play 下一首,不会与音频线程互锁
            if (on_state_changed) on_state_changed(PlaybackState::Stopped);
            if (on_playback_ended) on_playback_ended();
        }
    }
}

bool AudioPlaybackEngine::InitDevice(uint32_t sampleRate, uint32_t channels, bool exclusive) {
    if (impl_->device_initialized) {
        ma_device_uninit(&impl_->device); // 阻塞直到回调退出;回调只 try_lock 解码器锁,不会死锁
        impl_->device_initialized = false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = channels;
    cfg.sampleRate = sampleRate; // 0 = 设备原生 (共享模式下即系统混音采样率,避免二次重采样)
    cfg.dataCallback = MaDataCallback;
    cfg.notificationCallback = MaNotificationCallback;   // 设备被收走时能收到通知
    cfg.pUserData = this;
    cfg.noClip = MA_FALSE;
    if (exclusive) {
        // 真·WASAPI 独占:shareMode 才是独占开关,usage 只是 MMCSS 线程优先级提示
        cfg.playback.shareMode = ma_share_mode_exclusive;
        cfg.wasapi.usage = ma_wasapi_usage_pro_audio;
        cfg.performanceProfile = ma_performance_profile_low_latency;
    }

    // 开发/无声卡环境:KP_AUDIO_BACKEND=null 强制使用虚拟输出设备 (按真实时间节奏回调,不发声)
    ma_context* ctx = NULL;
    const char* forced = std::getenv("KP_AUDIO_BACKEND");
    const bool useNull = forced && std::string(forced) == "null";
    if (useNull || !impl_->preferred_device.empty()) {
        if (!impl_->context_initialized) {
            ma_backend nullBackend[] = {ma_backend_null};
            impl_->context_initialized =
                ma_context_init(useNull ? nullBackend : NULL, useNull ? 1 : 0, NULL, &impl_->context) == MA_SUCCESS;
        }
        if (impl_->context_initialized) ctx = &impl_->context;
    }
    // 指定设备:按名字在当前上下文中查找,找不到 (已拔出) 时回退系统默认
    if (ctx && !impl_->preferred_device.empty()) {
        ma_device_info* infos = NULL;
        ma_uint32 count = 0;
        if (ma_context_get_devices(ctx, &infos, &count, NULL, NULL) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < count; ++i) {
                if (impl_->preferred_device == infos[i].name) {
                    impl_->device_id = infos[i].id;
                    cfg.playback.pDeviceID = &impl_->device_id;
                    break;
                }
            }
        }
    }
    ma_result res = ma_device_init(ctx, &cfg, &impl_->device);
    bool exclusiveOk = exclusive && res == MA_SUCCESS;
    if (res != MA_SUCCESS && exclusive) {
        // 声卡被占用或不支持该格式:平滑降级为共享模式
        cfg.playback.shareMode = ma_share_mode_shared;
        cfg.wasapi.usage = ma_wasapi_usage_default;
        cfg.performanceProfile = ma_performance_profile_low_latency;
        res = ma_device_init(ctx, &cfg, &impl_->device);
    }
    if (res != MA_SUCCESS) {
        std::wcerr << L"[ERROR] 无法初始化音频设备: " << res << std::endl;
        return false;
    }
    impl_->device_initialized = true;
    impl_->gain = 0.0f;   // 新设备从静音开始,由回调的斜坡平滑升上来(避免重建后第一帧跳变)
    impl_->out_rate = impl_->device.sampleRate;
    impl_->out_channels = impl_->device.playback.channels;
    equalizer_.Configure(impl_->out_rate, impl_->out_channels);
    exclusive_active_.store(exclusiveOk);
    output_rate_.store(impl_->device.playback.internalSampleRate);
    ma_format internal = impl_->device.playback.internalFormat;
    uint32_t bits = internal == ma_format_s16 ? 16 : internal == ma_format_s24 ? 24 :
                    internal == ma_format_u8 ? 8 : 32; // s32 / f32
    output_bits_.store(bits);
    impl_->dither16 = (internal == ma_format_s16 || internal == ma_format_u8);

    if (ma_device_start(&impl_->device) != MA_SUCCESS) {
        ma_device_uninit(&impl_->device);
        impl_->device_initialized = false;
        return false;
    }
    return true;
}

bool AudioPlaybackEngine::Init(bool exclusiveMode) {
    if (impl_->device_initialized) return true;
    impl_->exclusive_requested = exclusiveMode;
    std::lock_guard<std::mutex> lock(impl_->decoder_mutex);
    return InitDevice(0, 2, exclusiveMode);
}

void AudioPlaybackEngine::Shutdown() {
    state_.store(PlaybackState::Stopped);
    impl_->shutting_down.store(true);   // ma_device_uninit 会触发 stopped 通知，别当成设备丢失
    // 先断流再拆设备:否则后台下载线程会跟着引擎一起被析构,下载缓冲还挂在那儿
    impl_->CancelStream();
    if (impl_->device_initialized) {
        ma_device_uninit(&impl_->device);
        impl_->device_initialized = false;
    }
    std::lock_guard<std::mutex> lock(impl_->decoder_mutex);
    impl_->next_ready.store(false);
    impl_->Release(impl_->slot[0]);
    impl_->Release(impl_->slot[1]);
    impl_->stale = -1;
    if (impl_->context_initialized) {
        ma_context_uninit(&impl_->context);
        impl_->context_initialized = false;
    }
    impl_->shutting_down.store(false);
}

// 设备通知：只有"被系统收走"才需要上层重建，其余忽略
void AudioPlaybackEngine::HandleDeviceNotification(int type) {
    if (impl_->shutting_down.load()) return;
    if (type == ma_device_notification_type_stopped ||
        type == ma_device_notification_type_interruption_began) {
        if (on_device_lost) on_device_lost();
    }
}

// 只换输出设备，解码器 / 流式缓冲 / 播放位置全部保留。
// 休眠唤醒后调用它，在线曲目不用重新解析直链，本地曲目也不用重开文件。
bool AudioPlaybackEngine::RestartDevice() {
    if (!impl_->device_initialized) return false;
    const bool ok = InitDevice(0, 2, impl_->exclusive_requested);
    if (ok) impl_->fade.store(1.0f);   // 新设备从 0 爬升，这里只是把淡出标志复位
    return ok;
}

bool AudioPlaybackEngine::OpenFile(const std::wstring& filePath) { return OpenFile(filePath, 0.0, 0.0); }

// 打开一个解码槽 (不加锁;调用方保证音频线程此时不会读取该槽)
static bool OpenSlot(DecoderSlot& slot, const std::wstring& filePath, double startSeconds, double endSeconds,
                     ma_uint32 outRate, ma_uint32 outChannels) {
    const std::wstring path = platform::LongPath(filePath);
    // 用自己的解析内核取得原生格式与精确时长:只读文件头,避免 MP3 为求时长而整文件扫描
    AudioMetadata meta;
    ParseOptions po;
    po.load_cover = false;
    po.read_id3v1 = false;
    bool haveMeta = AudioTagParser::ParseFile(filePath, meta, po) && meta.sample_rate > 0;

    uint32_t srcRate = haveMeta ? meta.sample_rate : 0;
    if (srcRate == 0) {
        ma_decoder probe;
        ma_decoder_config pc = ma_decoder_config_init(ma_format_f32, 0, 0);
        if (OpenDecoder(path, &pc, &probe) == MA_SUCCESS) {
            ma_format f;
            ma_uint32 ch;
            ma_decoder_get_data_format(&probe, &f, &ch, &srcRate, NULL, 0);
            ma_decoder_uninit(&probe);
        }
    }

    // 注意:miniaudio 0.11.25 在 Windows 桌面独占模式下总是使用设备默认格式 (PKEY_AudioEngine_DeviceFormat),
    // 忽略请求的采样率,因此不按母带采样率重建设备;解码器直接按设备采样率输出,由最高阶低通重采样器一次完成转换。
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, outChannels, outRate);
    dc.resampling.algorithm = ma_resample_algorithm_linear;
    dc.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;
    ma_result res = OpenDecoder(path, &dc, &slot.dec);
    if (res != MA_SUCCESS) {
        std::wcerr << L"[ERROR] 无法解码音频文件: " << filePath << L" (错误码: " << res << L")" << std::endl;
        return false;
    }
    slot.init = true;

    const double rate = static_cast<double>(outRate);
    ma_uint64 totalFrames = 0;
    if (haveMeta && meta.total_samples > 0 && meta.sample_rate > 0) {
        totalFrames = meta.total_samples * outRate / meta.sample_rate;
    } else if (haveMeta && meta.duration_ms > 0) {
        totalFrames = static_cast<ma_uint64>(meta.duration_ms) * outRate / 1000;
    } else if (ma_decoder_get_length_in_pcm_frames(&slot.dec, &totalFrames) != MA_SUCCESS) {
        totalFrames = 0;
    }
    slot.seg_start = startSeconds > 0 ? static_cast<ma_uint64>(startSeconds * rate) : 0;
    slot.seg_end = endSeconds > 0 ? static_cast<ma_uint64>(endSeconds * rate) : totalFrames;
    if (totalFrames && slot.seg_end > totalFrames) slot.seg_end = totalFrames;
    if (slot.seg_start > 0) ma_decoder_seek_to_pcm_frame(&slot.dec, slot.seg_start);
    slot.duration = slot.seg_end > slot.seg_start ? (slot.seg_end - slot.seg_start) / rate : 0.0;
    slot.src_rate = srcRate ? srcRate : outRate;
    slot.path = filePath;
    slot.rg = 1.0f;
    return true;
}

bool AudioPlaybackEngine::OpenFile(const std::wstring& filePath, double startSeconds, double endSeconds) {
    if (!impl_->device_initialized && !Init(impl_->exclusive_requested)) return false;

    // 从流式切回本地文件:先把音频线程停手(OnAudioFrames 见 Stopped 就不再读解码器),
    // 再 Cancel 旧流。这两步必须在设备重建 / 拿 decoder_mutex 之前做 ——
    // 音频线程可能正卡在 ReadAt 里等数据,而它握着 decoder_mutex,
    // 先 Cancel 才能让它立刻返回,否则重建设备会白等好几秒。
    // 非流式时 CancelStream 是空操作,不影响原有 OpenFile 行为。
    state_.store(PlaybackState::Stopped);
    impl_->CancelStream();

    // ---- 采样率跟随(仅独占模式) ----
    // 独占模式下按曲目的**原始采样率**重建输出设备:44.1k 的曲子就以 44.1k 送出去,
    // 不经过 44.1k→48k 的重采样,做到 bit-perfect。
    //
    // 为什么只在独占模式做:共享模式下采样率由系统混音器锁死(通常是 48k),
    // 我们重建也改不了它,只会白白打断别的程序的声音。
    //
    // 代价:重建会让声音中断约 100~200ms(设备要重新握手),正好落在切歌的空档里。
    // 注意必须在拿 decoder_mutex 之前做 —— InitDevice 内部的 ma_device_uninit
    // 要等音频回调退出,而回调里会 try_lock 这把锁。
    if (impl_->exclusive_requested) {
        AudioMetadata meta;
        ParseOptions po;
        po.load_cover = false;
        po.read_id3v1 = false;
        const uint32_t srcRate =
            (AudioTagParser::ParseFile(filePath, meta, po) && meta.sample_rate > 0) ? meta.sample_rate : 0;
        if (srcRate != 0 && srcRate != impl_->out_rate) {
            const uint32_t ch = impl_->out_channels ? impl_->out_channels : 2;
            // 先把当前输出淡到静音再重建:设备拆除时缓冲里残留的样本会被硬切,
            // 在独占模式下就是一声爆音。40ms 足够让 30ms 的斜坡走完。
            impl_->fade.store(0.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            const bool ok = InitDevice(srcRate, ch, true);
            impl_->fade.store(1.0f);   // 新设备的 gain 从 0 开始爬升,顺带就是淡入
            if (!ok) {
                // 设备不接受这个采样率(极少见):InitDevice 失败时旧设备已经被 uninit 了,
                // 必须重新建一个,否则后面根本没法播;退回设备原生格式由解码器重采样
                std::wcerr << L"[WARN] 设备不支持 " << srcRate << L" Hz,回退到设备原生格式" << std::endl;
                if (!Init(true)) return false;
            }
        }
    }

    std::lock_guard<std::mutex> lock(impl_->decoder_mutex);
    state_.store(PlaybackState::Stopped);
    // 重新打开:丢弃当前与预加载的解码器
    impl_->next_ready.store(false);
    impl_->Release(impl_->slot[0]);
    impl_->Release(impl_->slot[1]);
    impl_->stale = -1;
    impl_->cur.store(0);
    DecoderSlot& s = impl_->slot[0];
    if (!OpenSlot(s, filePath, startSeconds, endSeconds, impl_->out_rate, impl_->out_channels)) return false;

    impl_->cursor.store(s.seg_start);
    duration_.store(s.duration);
    current_position_.store(0.0);
    source_rate_.store(s.src_rate);
    output_rate_.store(impl_->device.playback.internalSampleRate);
    visualizer_.SetSampleRate(impl_->out_rate);
    visualizer_.Reset();
    impl_->ended_pending.store(false);
    impl_->advanced_pending.store(false);
    {
        std::lock_guard<std::mutex> sl(state_mutex_);
        current_file_ = filePath;
    }
    return true;
}

bool AudioPlaybackEngine::OpenStream(std::shared_ptr<stream_core::StreamSource> src) {
    if (!src) return false;
    if (!impl_->device_initialized && !Init(impl_->exclusive_requested)) return false;

    // 和 OpenFile 一样的换曲次序:先让音频线程停手,再 Cancel 旧流(可能也是流式),
    // 最后才去拿 decoder_mutex。旧流如果正卡在 ReadAt 里,不 Cancel 就会白等。
    state_.store(PlaybackState::Stopped);
    impl_->CancelStream();

    std::lock_guard<std::mutex> lock(impl_->decoder_mutex);
    // 流式不做无缝预加载(下一首的 URL 由上层决定,不在这里猜),清掉旧的预加载槽
    impl_->next_ready.store(false);
    impl_->Release(impl_->slot[0]);
    impl_->Release(impl_->slot[1]);
    impl_->stale = -1;
    impl_->cur.store(0);

    DecoderSlot& s = impl_->slot[0];
    s.reader = std::make_unique<StreamReader>();
    s.reader->src = src;

    // 采样率固定跟随设备,不做 OpenFile 那套"独占模式按母带采样率重建设备":
    // 流媒体要等头部到达才知道真实采样率,而重建设备会中断输出;不同曲目之间也可能
    // 采样率不一致。统一由解码器内置的高阶低通重采样器一次完成转换。
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, impl_->out_channels, impl_->out_rate);
    dc.resampling.algorithm = ma_resample_algorithm_linear;
    dc.resampling.linear.lpfOrder = MA_MAX_FILTER_ORDER;

    // 注意:这里会同步读到文件头,数据还没下载到时 StreamReadProc 会等(最多约 3 秒)。
    // 也就是说 OpenStream 可能阻塞一小会儿,这是拉模式解码的固有代价。
    ma_result res = ma_decoder_init(StreamReadProc, StreamSeekProc, s.reader.get(), &dc, &s.dec);
    if (res != MA_SUCCESS) {
        std::wcerr << L"[ERROR] 无法打开流式音频 (错误码: " << res << L")" << std::endl;
        s.reader.reset();
        return false;
    }
    s.init = true;
    s.dec.onTell = StreamTellProc;   // 见 StreamTellProc 的注释

    // 时长:只有"总字节已知"且解码器算得出总帧数时才敢用。
    // 拿不到就填 0 —— GetDurationSeconds() 返回 0,UI 按直播式进度显示,
    // 绝不能让它变成 0 除或者负数(OnAudioFrames 里所有除法都走 out_rate,是安全的)。
    ma_uint64 totalFrames = 0;
    if (src->TotalBytes() > 0 && ma_decoder_get_length_in_pcm_frames(&s.dec, &totalFrames) != MA_SUCCESS)
        totalFrames = 0;
    if (totalFrames > 0) ma_decoder_seek_to_pcm_frame(&s.dec, 0);   // 探测总长可能移动过游标

    s.seg_start = 0;
    s.seg_end = totalFrames;   // 0 = 长度未知:一路播到真 EOF 为止
    s.duration = totalFrames ? static_cast<double>(totalFrames) / static_cast<double>(impl_->out_rate) : 0.0;
    // 原始采样率从解码器的转换器里取(输入侧),取不到就按输出采样率算,保证 IsResampling() 不自相矛盾
    s.src_rate = s.dec.converter.sampleRateIn ? s.dec.converter.sampleRateIn : impl_->out_rate;
    s.path.clear();            // 在线播放没有本地路径
    s.rg = 1.0f;

    {
        std::lock_guard<std::mutex> sl(impl_->stream_mutex);
        impl_->stream_ = src;
        impl_->streaming_ = true;
    }

    impl_->cursor.store(0);
    duration_.store(s.duration);
    current_position_.store(0.0);
    source_rate_.store(s.src_rate);
    output_rate_.store(impl_->device.playback.internalSampleRate);
    visualizer_.SetSampleRate(impl_->out_rate);
    visualizer_.Reset();
    impl_->ended_pending.store(false);
    impl_->advanced_pending.store(false);
    {
        std::lock_guard<std::mutex> sl(state_mutex_);
        current_file_.clear();
    }
    return true;
}

bool AudioPlaybackEngine::QueueNext(const std::wstring& filePath, double startSeconds, double endSeconds, float replayGain) {
    if (!impl_->device_initialized) return false;
    int nxt;
    {
        std::lock_guard<std::mutex> lock(impl_->decoder_mutex);
        if (!impl_->Cur().init) return false;
        impl_->next_ready.store(false);
        nxt = 1 - impl_->cur.load();
        if (impl_->stale == nxt) impl_->stale = -1;
        impl_->Release(impl_->slot[nxt]);
    }
    // 打开期间不持有锁:音频线程在 next_ready 为 true 之前不会触碰这个槽
    DecoderSlot& s = impl_->slot[nxt];
    if (!OpenSlot(s, filePath, startSeconds, endSeconds, impl_->out_rate, impl_->out_channels)) return false;
    s.rg = replayGain > 0 ? replayGain : 1.0f;
    std::lock_guard<std::mutex> lock(impl_->decoder_mutex);
    if (impl_->cur.load() == nxt) return false; // 期间被 OpenFile 重置
    impl_->next_ready.store(true);
    return true;
}

void AudioPlaybackEngine::ClearNext() {
    std::lock_guard<std::mutex> lock(impl_->decoder_mutex);
    impl_->next_ready.store(false);
    impl_->Release(impl_->slot[1 - impl_->cur.load()]);
}

void AudioPlaybackEngine::SetReplayGain(float linearGain) {
    std::lock_guard<std::mutex> lock(impl_->decoder_mutex);
    impl_->Cur().rg = linearGain > 0 ? linearGain : 1.0f;
}

void AudioPlaybackEngine::SetPreferredDevice(const std::string& name) { impl_->preferred_device = name; }

std::string AudioPlaybackEngine::GetDeviceName() const {
    return impl_->device_initialized ? std::string(impl_->device.playback.name) : std::string();
}

std::vector<OutputDeviceInfo> AudioPlaybackEngine::EnumerateOutputDevices() {
    std::vector<OutputDeviceInfo> out;
    ma_context ctx;
    const char* forced = std::getenv("KP_AUDIO_BACKEND");
    ma_backend nullBackend[] = {ma_backend_null};
    const bool useNull = forced && std::string(forced) == "null";
    if (ma_context_init(useNull ? nullBackend : NULL, useNull ? 1 : 0, NULL, &ctx) != MA_SUCCESS) return out;
    ma_device_info* infos = NULL;
    ma_uint32 count = 0;
    if (ma_context_get_devices(&ctx, &infos, &count, NULL, NULL) == MA_SUCCESS)
        for (ma_uint32 i = 0; i < count; ++i) out.push_back({infos[i].name, infos[i].isDefault != 0});
    ma_context_uninit(&ctx);
    return out;
}

bool AudioPlaybackEngine::Play() {
    if (!impl_->Cur().init) return false;
    state_.store(PlaybackState::Playing);
    if (on_state_changed) on_state_changed(PlaybackState::Playing);
    return true;
}

void AudioPlaybackEngine::Pause() {
    if (state_.load() == PlaybackState::Playing) {
        state_.store(PlaybackState::Paused);
        if (on_state_changed) on_state_changed(PlaybackState::Paused);
    }
}

void AudioPlaybackEngine::Resume() {
    if (state_.load() == PlaybackState::Paused) {
        state_.store(PlaybackState::Playing);
        if (on_state_changed) on_state_changed(PlaybackState::Playing);
    }
}

void AudioPlaybackEngine::Stop() {
    if (state_.load() != PlaybackState::Stopped) {
        state_.store(PlaybackState::Stopped);
        if (impl_->Streaming()) {
            // 流式:Stop 视为"这条流到此结束",顺手把下载也停掉,
            // 否则后台下载线程会继续把整首歌拉完(用户已经不想听了,白白占带宽)。
            // 代价是 Stop 之后不能再 Play() 续播:StreamSource 的 Cancel 不可逆
            // (append-only 缓冲不再增长),要重听只能重新 OpenStream。
            // 所以这里把解码器一并释放,免得留下一个"init 但读不出数据"的槽骗过 Play()。
            impl_->CancelStream();
            {
                std::lock_guard<std::mutex> lock(impl_->decoder_mutex);
                impl_->next_ready.store(false);
                impl_->Release(impl_->slot[0]);
                impl_->Release(impl_->slot[1]);
                impl_->stale = -1;
            }
        } else {
            SeekTo(0.0);
        }
        visualizer_.Reset();
        if (on_state_changed) on_state_changed(PlaybackState::Stopped);
    }
}

void AudioPlaybackEngine::TogglePlay() {
    PlaybackState s = state_.load();
    if (s == PlaybackState::Playing) Pause();
    else if (s == PlaybackState::Paused) Resume();
    else Play();
}

bool AudioPlaybackEngine::SeekTo(double seconds) {
    std::lock_guard<std::mutex> lock(impl_->decoder_mutex);
    DecoderSlot& cs = impl_->Cur();
    if (!cs.init) return false;
    if (seconds < 0.0) seconds = 0.0;
    double maxDur = duration_.load();
    if (maxDur > 0.0 && seconds > maxDur) seconds = maxDur;

    ma_uint64 target = cs.seg_start + static_cast<ma_uint64>(seconds * impl_->out_rate);
    // 流式播放时这一步会落到 StreamSeekProc:只能跳到已经下载到的范围内,
    // 数据还没到就失败。失败时什么都不改(cursor / current_position_ 保持原样),
    // 让 UI 的进度条停在原处,而不是跳到一个根本没有数据的假位置。
    if (ma_decoder_seek_to_pcm_frame(&cs.dec, target) != MA_SUCCESS) return false;
    impl_->cursor.store(target);
    current_position_.store(seconds);
    visualizer_.Reset();
    return true;
}

void AudioPlaybackEngine::SetVolume(float volume) { volume_.store(std::min(1.0f, std::max(0.0f, volume))); }

float AudioPlaybackEngine::GetVolume() const { return volume_.load(); }

PlaybackState AudioPlaybackEngine::GetState() const { return state_.load(); }

double AudioPlaybackEngine::GetPositionSeconds() const { return current_position_.load(); }

double AudioPlaybackEngine::GetDurationSeconds() const { return duration_.load(); }

std::wstring AudioPlaybackEngine::GetCurrentFilePath() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return current_file_;
}

void AudioPlaybackEngine::OnAudioFrames(void* pOutput, uint32_t frameCount) {
    float* out = reinterpret_cast<float*>(pOutput);
    const size_t ch = impl_->out_channels;
    const size_t totalSamples = static_cast<size_t>(frameCount) * ch;

    if (state_.load() != PlaybackState::Playing) {
        std::fill(out, out + totalSamples, 0.0f);
        // 暂停/停止时持续馈入静音,令频谱柱自然阻尼衰减而非冻结
        visualizer_.PushSamples(out, totalSamples, ch);
        return;
    }

    std::unique_lock<std::mutex> lock(impl_->decoder_mutex, std::try_to_lock);
    if (!lock.owns_lock() || !impl_->Cur().init) {
        std::fill(out, out + totalSamples, 0.0f);
        return;
    }

    // 从当前槽读取;读到片段末尾时若下一首已就绪,在同一个回调里接着读下一首 (无缝)
    auto readSlot = [&](DecoderSlot& s, float* dst, ma_uint64 frames, ma_uint64& cursor) -> ma_uint64 {
        ma_uint64 want = frames;
        if (s.seg_end > 0) want = (cursor < s.seg_end) ? std::min<ma_uint64>(want, s.seg_end - cursor) : 0;
        ma_uint64 got = 0;
        if (want > 0) ma_decoder_read_pcm_frames(&s.dec, dst, want, &got);
        cursor += got;
        return got;
    };

    ma_uint64 cursor = impl_->cursor.load();
    ma_uint64 got = readSlot(impl_->Cur(), out, frameCount, cursor);
    bool reachedEnd = got < frameCount || (impl_->Cur().seg_end > 0 && cursor >= impl_->Cur().seg_end);
    bool advanced = false;
    if (reachedEnd && impl_->next_ready.load()) {
        const int old = impl_->cur.load();
        impl_->cur.store(1 - old);
        impl_->next_ready.store(false);
        impl_->stale = old;
        DecoderSlot& ns = impl_->Cur();
        cursor = ns.seg_start;
        got += readSlot(ns, out + got * ch, frameCount - got, cursor);
        duration_.store(ns.duration);
        source_rate_.store(ns.src_rate);
        advanced = true;
        reachedEnd = got == 0;
    }
    if (got < frameCount) std::fill(out + got * ch, out + totalSamples, 0.0f);
    impl_->cursor.store(cursor);

    // 均衡器 (关闭且已回落到 0 dB 时直接返回,不碰样本)
    if (got > 0) equalizer_.Process(out, got);

    // 音量 × ReplayGain:按样本线性斜坡逼近目标值,避免拉链噪声
    const float target = volume_.load() * impl_->Cur().rg * impl_->fade.load();
    float g = impl_->gain;
    if (!(std::abs(g - 1.0f) < 1e-4f && std::abs(target - 1.0f) < 1e-4f)) {
        const size_t n = static_cast<size_t>(got) * ch;
        const float step = n ? (target - g) / static_cast<float>(n) : 0.0f;
        for (size_t i = 0; i < n; ++i) {
            g += step;
            out[i] *= g;
        }
        impl_->gain = target;
    }

    // 16 位输出:TPDF 抖动 (±1 LSB 三角分布),避免 24bit 母带截断到 16bit 时的谐波失真
    if (impl_->dither16) {
        const size_t n = static_cast<size_t>(got) * ch;
        const float lsb = 1.0f / 32768.0f;
        uint32_t r = impl_->rng;
        for (size_t i = 0; i < n; ++i) {
            r = r * 1664525u + 1013904223u;
            float a = static_cast<float>(r >> 8) * (1.0f / 16777216.0f);
            r = r * 1664525u + 1013904223u;
            float b = static_cast<float>(r >> 8) * (1.0f / 16777216.0f);
            out[i] += (a - b) * lsb;
        }
        impl_->rng = r;
    }

    const double rate = static_cast<double>(impl_->out_rate);
    const ma_uint64 segStart = impl_->Cur().seg_start;
    current_position_.store(static_cast<double>(cursor - std::min(cursor, segStart)) / rate);
    visualizer_.PushSamples(out, static_cast<size_t>(got) * ch, ch);

    if (advanced) {
        impl_->advanced_pending.store(true);
        impl_->notify_cv.notify_one();
    }
    if (reachedEnd && !advanced) {
        state_.store(PlaybackState::Stopped);
        current_position_.store(duration_.load());
        impl_->ended_pending.store(true);
        impl_->notify_cv.notify_one(); // 通知线程负责调用上层回调
    }
}
