#pragma once
#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include "music_catalog.hpp"

// ============================================================================
//  AudioIngestionPipeline —— 曲库入库流水线
//   · 高速目录枚举 (Win32 FindFirstFileEx LARGE_FETCH,一次调用拿到文件名+大小)
//   · 存储感知并发:自动探测机械盘/固态盘,机械盘降低并发避免磁头抖动
//   · 惰性封面:只记录内嵌封面的物理偏移,零图片 I/O 构建曲库
//   · 无锁结果收集 + 工作线程内完成清洗/拼音计算,曲库锁只用于极短的批量提交
//   · CUE 整轨自动拆分、同目录伴生封面 (cover.jpg / folder.jpg) 关联
//   · KGM 解密:掩码表整个运行期仅映射一次,解密内核为单条异或流水线
// ============================================================================

struct IngestionItemResult {
    std::wstring source_path;
    std::wstring target_path;
    bool was_encrypted = false;
    bool success = false;
    std::wstring message;
};

struct IngestionProgress {
    size_t current = 0;
    size_t total = 0;
    std::wstring current_filename;
    std::wstring status_text;
    bool is_transcoding = false;
};

// 扫描遇到加密音频时的处理方式
enum class DecryptPolicy {
    Ask,  // 不解密:收集到 IngestionSummary::encrypted_pending,由调用方 (界面) 询问用户后再解密
    Auto  // 扫描时直接解密 (命令行 / 测试默认)
};

// 待解密的加密源
struct EncryptedFileInfo {
    std::wstring path;
    std::string kind; // 解密工具 id:"kgm" / "ncm" / "kgg"
    uint64_t size = 0;
    uint64_t mtime = 0;
};

struct IngestionSummary {
    size_t total_scanned = 0;
    size_t success_count = 0;
    size_t transcoded_count = 0;
    size_t failed_count = 0;
    double elapsed_seconds = 0.0;

    // 扩展审计指标
    size_t skipped_count = 0;       // 正常跳过的非音乐文件 (如 MP4 视频),不计入 failed_count
    size_t tracks_added = 0;        // 实际入库曲目数 (CUE 分轨后可能多于文件数)
    size_t cue_tracks = 0;          // 由 CUE 拆分出的虚拟曲目数
    size_t external_covers = 0;     // 关联到同目录伴生封面的曲目数
    uint64_t bytes_read = 0;        // 解析阶段真实磁盘读取字节数 (不含解密)
    uint64_t library_bytes = 0;     // 扫描到的音频文件总大小
    double enumerate_seconds = 0.0; // 目录枚举耗时
    size_t thread_count = 0;
    int storage_seek_penalty = -1;  // 1 = 机械盘, 0 = 固态盘, -1 = 未知
    bool cancelled = false;
    size_t cache_hits = 0;          // 增量缓存命中 (未读盘) 的文件数
    size_t cache_misses = 0;        // 实际解析的文件数
    bool cache_saved = false;
    size_t decrypted_count = 0;                     // 本次扫描新解密的文件数
    std::vector<EncryptedFileInfo> encrypted_pending; // Ask 模式下发现、尚无解密产物的加密源
};

class AudioIngestionPipeline {
public:
    AudioIngestionPipeline();
    ~AudioIngestionPipeline();

    // 配置参数
    void SetTargetLibraryDir(const std::wstring& dir);
    void SetKgmMaskPath(const std::wstring& maskPath);
    void SetThreadCount(size_t threads);               // 0 = 按存储类型自适应 (机械盘 4 / 固态盘按核心数)
    void SetLazyCoverExtract(bool lazy);               // 开启后入库不读取封面图片,仅记录偏移,光速构建万级曲库
    void SetSplitCueSheets(bool split);
    // 非 Unicode 标签的回退代码页 (936 GBK / 950 Big5 / 932 Shift-JIS);变更后需重建扫描缓存
    void SetLegacyCodePage(unsigned int codePage);
    // MP3 无 ID3v2 标题时是否回读文件尾部的 ID3v1 / APEv2
    void SetReadId3v1(bool on);                // 默认开启:整轨 + CUE 自动拆分为独立曲目
    // 增量扫描缓存文件 (仅惰性封面模式生效)。设置后,大小与修改时间未变的文件直接复用上次解析结果,零磁盘读取
    void SetScanCacheFile(const std::wstring& path);
    // 加密源处理策略 (默认 Auto) 与解密产物索引文件 (源文件 → 产物,扫描时直接复用产物)
    void SetDecryptPolicy(DecryptPolicy policy);
    void SetDecryptIndexFile(const std::wstring& path);

    // 输入源管理
    void AddInputPath(const std::wstring& path);
    void ClearInputPaths();

    // 模式 A: 多线程并发同步执行 (阻塞当前线程，供测试与脚本使用)
    std::vector<IngestionItemResult> RunSync(MusicCatalog& catalog);

    // 模式 B: 多线程并发异步执行 (立即返回，完全不卡主线程/UI)
    bool StartAsync(MusicCatalog& catalog,
                    std::function<void(const IngestionProgress&)> onProgress = nullptr,
                    std::function<void(const IngestionSummary&)> onFinished = nullptr);

    // 控制与状态
    void Cancel();
    bool IsRunning() const { return is_running_.load(); }
    IngestionSummary GetLastSummary() const;

    // 进度回调通知 (串行化调用,current 严格单调递增)
    std::function<void(const IngestionProgress&)> on_progress;

    // KGM 掩码解密内核 (公开以便单元测试):out = x ^ (x << 4), x = c ^ key[i % 17] ^ mask[i % maskLen]
    static void KgmDecryptBlock(uint8_t* data, size_t len, uint64_t fileOffset, const uint8_t key17[17],
                                const uint8_t* mask, uint64_t maskLen);

private:
    struct Impl;

    std::wstring target_lib_dir_;
    std::wstring kgm_mask_path_;
    std::vector<std::wstring> input_paths_;
    size_t thread_count_ = 0;
    bool lazy_cover_extract_ = false;
    bool split_cue_ = true;
    unsigned int code_page_ = 936;
    bool read_id3v1_ = true;
    std::wstring scan_cache_path_;
    DecryptPolicy decrypt_policy_ = DecryptPolicy::Auto;
    std::wstring decrypt_index_path_;

    std::atomic<bool> is_running_{false};
    std::atomic<bool> cancel_requested_{false};
    std::thread async_worker_thread_;
    mutable std::mutex pipeline_mutex_;
    IngestionSummary last_summary_;
};
