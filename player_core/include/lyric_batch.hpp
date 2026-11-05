#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ============================================================================
//  LyricBatch —— 批量补全歌词
//   · 三级降级:酷狗 krcs 快路径 (2 请求) → 酷狗 hash 兜底 → lrclib / 网易云
//   · 多线程并发 (酷狗实测稳定);降级源串行 + 连续失败熔断
//   · 线程本地复用 WinHTTP session,省掉每个请求的 TLS 握手
//   · 写盘统一转成标准 LRC (带翻译),任何播放器都能读
// ============================================================================

namespace lyric_core {

struct BatchTrack {
    std::wstring file_path;
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    uint32_t duration_ms = 0;
};

struct BatchOptions {
    int threads = 8;
    // 把歌词写成音频旁边的「歌名.lrc」
    bool write_lrc = true;
    // 已经存在 .lrc 时是否覆盖。默认不覆盖 —— 用户手里的版本可能更好
    bool overwrite = false;
    // 酷狗没命中时是否降级到 lrclib / 网易云
    bool allow_fallback = true;
    // 进度回调 (完成数, 总数)。在工作线程里调用,别做重活
    std::function<void(size_t, size_t)> on_progress;
    const std::atomic<bool>* cancel = nullptr;
};

// level: 0 没找到 / 1 只有歌名对上 / 2 歌名+时长对上 / 3 歌名+歌手+时长全对
// source: 0 无 / 1 酷狗 / 2 lrclib / 3 网易云
struct BatchOutcome {
    int level = 0;
    int source = 0;
    std::wstring hit_title;
    std::wstring hit_artist;
    uint32_t hit_duration_ms = 0;
    bool saved = false;
    std::wstring saved_path;
    std::string note;
    double elapsed_ms = 0;
    int requests = 0;
};

class LyricBatch {
public:
    // 返回与 tracks 一一对应的结果
    static std::vector<BatchOutcome> Run(const std::vector<BatchTrack>& tracks, const BatchOptions& opt);
};

} // namespace lyric_core
