#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ============================================================================
//  查找重复歌曲 (参考 foobar2000 foo_audiomd5、beets duplicates、MusicBee、Roon 的做法)
//
//  用法:FindDuplicates 只读不改 (界面据此列出分组,默认已勾好保留哪一份)
//        → 用户可改勾选 / 换自动勾选规则 (AutoSelect) → ApplySelection 批量处理 → 需要时 UndoDedupe。
//
//  由严到宽五种匹配,严的先认领,同一个文件只出现在一个分组里 (Versions 提示分组除外):
//   · Identical  文件逐字节相同 (先按大小分桶,再整文件哈希,最后逐字节确认)。可放心自动处理。
//   · SameAudio  音频数据相同、只是标签不同:FLAC 比对 STREAMINFO 里的 PCM MD5;
//                MP3 去掉 ID3v2 / ID3v1 / APE 标签后比对音频帧。
//   · SameSong   歌手 + 歌名相同 (规范化后,歌手顺序无关),时长相差不超过容差 —— 同一首歌的不同音质
//                (320k MP3 与 FLAC)。默认保留音质最好的,需要用户确认。
//   · SameName   没有歌手 / 歌名标签时,退而按文件名 (去掉曲号、[FLAC] 之类) + 时长匹配。可信度低,需要确认。
//   · Versions   歌名歌手相同但时长差得多 (现场版、加长版):只是提示,默认全部保留。
//  歌名里带 Live / Remix / 伴奏 / Inst 等版本词的不会和原版归为同一首;歌手或歌名是空的 / 占位符时
//  不参与 SameSong (避免一堆「未知」被当成重复)。CUE 分轨不参与 (它们共用一个整轨文件)。
//
//  保留哪一份 (可选规则,默认按分组类型):
//   音质:无损 > 位深 > 采样率 > 码率;然后标签完整度 (歌名 / 歌手 / 专辑 / 曲号 / 封面)、在首选目录里、
//   文件名不像副本 (「 (1)」「- 副本」「copy」)、修改时间更早。
//
//  批量处理:移到回收站 (默认,Windows 回收站 / freedesktop 废纸篓) / 移到隔离文件夹 (保留原目录结构)
//   / 永久删除 / 只从曲库移除 (不动磁盘) / 换成指向保留文件的硬链接 (仅 Identical,同一个盘)。
//   · 处理前确认文件自扫描后没有变化 (大小 + 修改时间),变了就跳过;
//   · 默认不允许一个分组里一份都不留;
//   · 被删那份的同名歌词 (.lrc / .krc ...) 在保留的那份没有歌词时改名给保留的那份,否则一起处理;
//   · 每一步写日志,隔离 / 废纸篓 / 硬链接可撤销 (Windows 回收站由系统还原);
//   · 结果给出「被删文件 → 保留文件」映射,调用方据此把歌单、收藏、播放记录指向保留的那份。
// ============================================================================

namespace dedupe {

struct DupTrack {
    std::wstring path;
    std::wstring title, artist, album;
    uint32_t duration_ms = 0;
    bool lossless = false;
    int sample_rate = 0;
    int bits_per_sample = 0;
    int bitrate_kbps = 0;
    int track_no = 0;
    bool has_cover = false;
    bool cue_track = false;  // CUE 分轨:不参与查重
    uint64_t size = 0;       // 扫描时记下的文件大小 (0 = 未知,查重时自己读取);省掉逐个文件查询大小
};

enum class MatchKind { Identical, SameAudio, SameSong, SameName, Versions };

struct DupMember {
    size_t track = 0;        // DupTrack 下标
    bool keep = true;        // 勾选结果:false = 要处理掉
    uint64_t size = 0;
    int64_t mtime = 0;       // 扫描时的修改时间 (处理前据此确认文件没变)
    std::wstring note;       // 「无损 24bit/96k」「文件名像副本」等,供界面显示
};

struct DupGroup {
    MatchKind kind = MatchKind::Identical;
    std::vector<DupMember> members;
    size_t suggested = 0;        // 建议保留的成员下标 (members 内)
    bool needs_review = false;   // SameSong / SameName / Versions:建议用户确认
    uint64_t reclaim_bytes = 0;  // 按当前勾选能腾出的空间
};

struct FindOptions {
    bool identical = true;
    bool same_audio = true;
    bool same_song = true;
    bool same_name = true;
    bool versions = true;
    uint32_t duration_tolerance_ms = 3000;
    std::vector<std::wstring> preferred_roots;  // 保留时优先留在这些目录里的那份 (主曲库)
};

struct FindProgress {
    size_t done = 0, total = 0;
    uint64_t bytes_done = 0, bytes_total = 0;
    std::wstring current;
};

struct FindResult {
    std::vector<DupGroup> groups;
    size_t files_hashed = 0; // 头尾指纹相同、又做了中间抽样的文件数
    bool cancelled = false;
};

FindResult FindDuplicates(const std::vector<DupTrack>& tracks, const FindOptions& opt = {},
                          std::function<void(const FindProgress&)> onProgress = nullptr,
                          const std::atomic<bool>* cancel = nullptr);

enum class KeepRule {
    Auto,           // 按分组类型:Identical / SameAudio 用 PreferredPath,其余用 BestQuality
    BestQuality,
    PreferredPath,  // 首选目录 → 文件名不像副本 → 路径更短
    Oldest,
    Newest,
    Largest,
    Smallest
};

// 按规则重新勾选:每组只留一份,其余标为处理;Versions 分组全部保留
void AutoSelect(std::vector<DupGroup>& groups, const std::vector<DupTrack>& tracks, KeepRule rule = KeepRule::Auto,
                const std::vector<std::wstring>& preferredRoots = {});
void UpdateReclaim(std::vector<DupGroup>& groups);

enum class DupAction { RecycleBin, MoveToFolder, DeletePermanently, RemoveFromLibrary, HardLinkToKeeper };

struct ApplyOptions {
    DupAction action = DupAction::RecycleBin;
    std::wstring quarantine_root;   // MoveToFolder 的目标
    bool carry_lyrics = true;       // 保留的那份没有歌词时,把被删那份的歌词改名给它
    bool allow_remove_all = false;  // 允许一个分组里一份都不留
    bool verify_unchanged = true;
    std::vector<std::wstring> protect_roots;  // 清理变空的目录时不越过这些目录 (为空则不清理目录)
};

struct DupChange {
    std::wstring removed;   // 被处理掉的文件
    std::wstring keeper;    // 同组保留的文件 (调用方把引用指向它)
    std::wstring moved_to;  // 隔离 / 废纸篓里的位置 (永久删除、只移出曲库时为空)
};

struct ApplyResult {
    size_t done = 0, failed = 0, skipped = 0;
    uint64_t freed_bytes = 0;
    bool cancelled = false;
    std::vector<DupChange> changes;
    std::vector<std::pair<std::wstring, std::wstring>> failures;  // 路径, 原因
    std::vector<std::wstring> lyric_moves;                         // 改名给保留文件的歌词 (新路径)
    std::wstring journal;
};

ApplyResult ApplySelection(const std::vector<DupGroup>& groups, const std::vector<DupTrack>& tracks,
                           const ApplyOptions& opt, const std::wstring& journalPath,
                           std::function<void(const FindProgress&)> onProgress = nullptr,
                           const std::atomic<bool>* cancel = nullptr);

// 撤销:隔离 / 废纸篓 (非 Windows) 里的文件搬回原处,硬链接换回独立文件,改名的歌词改回去
ApplyResult UndoDedupe(const std::wstring& journalPath);

// 供测试与界面:歌名 / 歌手规范化后的匹配键
std::wstring NormalizeTitle(const std::wstring& title);
std::wstring NormalizeArtist(const std::wstring& artist);

} // namespace dedupe
