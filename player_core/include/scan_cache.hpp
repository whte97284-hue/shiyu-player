#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>
#include "audio_tag_parser.hpp"

// ============================================================================
//  ScanCache —— 增量扫描缓存
//   以「路径 + 文件大小 + 修改时间」为键保存解析结果。再次扫描时,未变化的文件直接复用,
//   零磁盘读取;只有新增/修改的文件才会真正解析。
//   · 缓存记录解析器版本号,解析内核升级后旧缓存自动整体失效
//   · 二进制紧凑格式,写入走「临时文件 + 原子改名」,中途断电不会损坏旧缓存
//   · 读取全程越界校验,缓存文件损坏时安全地退化为全量扫描
//   · 只缓存惰性封面模式下的结果 (封面以物理偏移形式保存,不保存图片字节)
// ============================================================================

struct ScanCacheEntry {
    uint64_t size = 0;
    uint64_t mtime = 0;
    bool parse_ok = false;
    std::wstring target_path; // 加密源文件:解密产物路径 (产物仍存在时可跳过解密与解析)
    AudioMetadata meta;       // cover_data 恒为空
};

class ScanCache {
public:
    // 解析结果格式版本:解析逻辑有任何影响输出的修改都必须递增
    static constexpr uint32_t kParserVersion = 6; // 6: WAV 里 ID3 优先于 LIST/INFO(修日文变问号); 5:ReplayGain 字段

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;

    // 命中条件:路径存在且大小、修改时间均一致
    const ScanCacheEntry* Find(const std::wstring& path, uint64_t size, uint64_t mtime) const;
    void Put(const std::wstring& path, ScanCacheEntry entry);
    // 文件被移动 / 复制到新路径后沿用原来的解析结果 (下次扫描时大小与修改时间一致才会命中);
    // keepSource = false 时删掉旧路径的记录。返回沿用的条数
    size_t Carry(const std::vector<std::pair<std::wstring, std::wstring>>& moves, bool keepSource);

    size_t Size() const { return entries_.size(); }
    void Clear() { entries_.clear(); }

private:
    std::unordered_map<std::wstring, ScanCacheEntry> entries_;
};
