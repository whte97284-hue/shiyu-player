#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <unordered_set>
#include "audio_tag_parser.hpp"
#include "decryptor.hpp"
#include "lyric_manager.hpp"

namespace lyric_core {

// 酷狗本地音轨资产条目 (包含加密状态、原路径与解析产物)
struct KuGouTrackEntry {
    std::wstring source_path;       // 原始文件路径 (例如 F:\KuGou\KugouMusic\王力宏 - 你不知道的事.kgma)
    std::wstring file_name;         // 文件名
    std::wstring ext;               // 扩展名 (.kgma, .kgm, .kgg, .flac 等)
    uint64_t file_size = 0;         // 原始文件大小 (字节)
    bool is_encrypted = false;      // 是否为加密文件
    std::wstring matched_krc_path;  // 自动关联命中的 .krc 歌词物理路径
    std::wstring playable_path;     // 可直接播放的无损音频路径 (已解密产物或原生音频)
    bool is_ready_to_play = false;  // 是否可立即直通播放
    AudioMetadata metadata;         // 提取出的音频元数据 (采样率、位深、时长、标签)
};

// 酷狗音频源智能探查与即时透明解码内核
class KuGouTrackBridge {
public:
    KuGouTrackBridge();
    ~KuGouTrackBridge() = default;

    // 智能嗅探酷狗客户端当前配置的所有真实音乐下载/存储目录 (零硬编码)
    static std::vector<std::wstring> DiscoverKuGouMusicDirectories();

    // 设置/覆盖解密掩码表路径 (默认用解密器注册表的 tools 目录;找不到时退到程序所在目录的 tools\kgm\kgm.mask)
    void SetMaskPath(const std::wstring& maskPath);

    // 设置解密产物缓存目录 (默认 %LOCALAPPDATA%\KugouPlayer\decrypted,非 Windows 为系统临时目录下的 kugou_player_decrypted)。
    // 每首源文件对应其中一个子目录,目录名由源文件路径、大小和修改时间算出,源文件变了会重新解密
    void SetCacheOutputDir(const std::wstring& cacheDir);

    // 与播放器共用的解密记录 (数据目录\decrypt.index)。设置后先查这里:播放器已经转换过的歌直接用它的产物,
    // 桥接新解密的结果也记进去,两边不会各转一份
    void SetDecryptIndexFile(const std::wstring& indexPath);

    // 扫描酷狗音乐库目录，枚举所有歌曲 (自动区分加密音轨与非加密音轨，自动关联歌词)
    std::vector<KuGouTrackEntry> ScanKuGouLibrary(const std::wstring& customDir = L"");

    // 准备可播音频:非加密文件直接返回原路径;加密文件先查缓存,没有再解密 (无损文件约需 1 秒)。
    // 线程安全:不同歌曲可以并行解密,同一首歌同时请求时只解密一次。返回值与 outEntry.is_ready_to_play 一致
    bool PreparePlayableTrack(const std::wstring& sourcePath, KuGouTrackEntry& outEntry);

    // 批量解码,多线程并行 (threads 为 0 时按 CPU 核数,最多 4 个)。进度回调在工作线程里串行调用
    size_t BatchPrepare(const std::vector<std::wstring>& sourcePaths,
                        std::function<void(size_t done, size_t total, const std::wstring& current)> onProgress = nullptr,
                        unsigned threads = 0);

private:
    std::wstring CacheDir() const;
    std::wstring CacheSlotFor(const std::wstring& sourcePath) const; // 这首歌的缓存子目录
    void FillPlainEntry(const std::wstring& sourcePath, KuGouTrackEntry& e);
    static std::wstring FindCachedOutput(const std::wstring& slotDir);

    std::wstring mask_path_;
    std::wstring cache_dir_;
    std::wstring index_path_;
    std::wstring SharedProduct(const std::wstring& sourcePath) const; // 共用记录里的产物
    void RecordProduct(const std::wstring& sourcePath, const std::wstring& product);
    LyricManager lyric_manager_;           // 自带锁,可并发调用
    mutable std::mutex mutex_;             // 只保护 mask_path_ / cache_dir_ / inflight_,解密时不持有
    std::condition_variable inflight_cv_;
    std::unordered_set<std::wstring> inflight_; // 正在解密的缓存子目录
};

} // namespace lyric_core
