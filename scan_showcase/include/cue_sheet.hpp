#pragma once
#include <string>
#include <vector>
#include <cstdint>

// ============================================================================
//  CUE 分轨表解析:整轨母带 (FLAC/WAV/APE + .cue) 拆分为独立虚拟曲目
//  自动识别 UTF-8 / UTF-16 / GBK 编码,时间精度到 CD 帧 (1/75 秒)
// ============================================================================

struct CueTrack {
    int number = 0;
    std::wstring title;
    std::wstring performer;
    std::wstring isrc;
    uint32_t start_ms = 0; // INDEX 01
    uint32_t end_ms = 0;   // 下一轨 INDEX 01;0 表示播放到文件结尾
};

struct CueFileEntry {
    std::wstring file_name; // CUE 中声明的文件名 (相对路径)
    std::vector<CueTrack> tracks;
};

struct CueSheet {
    std::wstring title;     // 专辑名
    std::wstring performer; // 专辑艺术家
    std::wstring genre;
    std::wstring date;
    std::vector<CueFileEntry> files;
};

class CueSheetParser {
public:
    static bool ParseFile(const std::wstring& path, CueSheet& out);
    static bool ParseText(const std::string& bytes, CueSheet& out);
};
