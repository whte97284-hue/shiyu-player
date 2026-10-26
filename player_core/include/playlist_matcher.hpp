#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "playlist_provider.hpp"

// ============================================================================
//  歌单曲目 → 本地曲库匹配 (导入在线歌单时标出哪些本地已有)
//
//  复用查重 (library_dedupe) 的规范化:歌名去掉曲号 / [FLAC] / feat. 后只留字母汉字并统一大小写全半角,
//  歌手拆成集合 (顺序、分隔符无关)。在此之上:
//   · 先按完整歌名找,再按「去掉括号注释」的歌名找 (《xxx》主题曲、with 某某 之类);
//     带 Live / Remix / 伴奏 / Inst 等版本词的括号不去掉,现场版不会被当成原版
//   · 歌手:集合相同或有交集算同一位;本地没有歌手标签的只能算「可能」
//   · 时长:两边都有时相差在容差内才算,差得多 (加长版、剪辑版) 降为「可能」或不算
//   · 同一首有多份时取音质最好的 (无损 > 位深 > 采样率 > 码率)
// ============================================================================

namespace playlist_core {

struct LocalTrack {
    uint64_t id = 0;
    std::wstring title, artist;
    std::wstring file_name;      // 没有歌名标签时从文件名猜「歌手 - 歌名」
    uint32_t duration_ms = 0;
    bool lossless = false;
    int bits_per_sample = 0;
    int sample_rate = 0;
    int bitrate_kbps = 0;
};

enum class MatchLevel { Missing = 0, Likely = 1, Exact = 2 };

struct TrackMatch {
    MatchLevel level = MatchLevel::Missing;
    size_t local = static_cast<size_t>(-1);   // LocalTrack 下标 (Missing 时无效)
    int duration_diff_ms = -1;                // 两边都有时长时的差值,否则 -1
};

struct MatchOptions {
    uint32_t tolerance_ms = 3000;        // 视为同一份录音
    uint32_t loose_tolerance_ms = 8000;  // 超出容差但在此之内:降为「可能」
};

struct MatchSummary {
    int exact = 0, likely = 0, missing = 0;
};

// 结果与 tracks 一一对应
std::vector<TrackMatch> MatchToLibrary(const std::vector<PlaylistTrack>& tracks, const std::vector<LocalTrack>& library,
                                       const MatchOptions& opt = {}, MatchSummary* summary = nullptr);

// 去掉不含版本词的括号注释后的歌名 (供测试与界面复用)
std::wstring StripNoteBrackets(const std::wstring& title);

} // namespace playlist_core
