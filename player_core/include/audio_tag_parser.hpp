#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <memory>

// ============================================================================
//  AudioTagParser —— 零拷贝 · 随机访问 · 惰性封面的音频元数据解析内核
//
//  设计要点:
//   1. 所有格式解析器都基于 tagcore::Reader 的「按需随机读取」实现,只读真正需要的字节:
//      FLAC 的 PICTURE 块、ID3 的 APIC 帧、MP4 的 covr、WAV 的 data 块全部按长度直接跳过,
//      绝不为了读标签而顺带吞下几 MB 的图片或音频。
//   2. 封面支持惰性模式 (ParseOptions::load_cover = false):只记录封面在文件中的物理偏移与长度,
//      需要显示时再用 ExtractCover() 一次定位读出。
//   3. 所有长度字段都做越界校验,畸形/截断文件只会解析失败,绝不会越界读写或崩溃。
//   4. 纯 C++17 + 平台抽象,Windows 下走 Win32 宽字符 I/O,其他平台走 POSIX pread,可跨平台单测。
// ============================================================================

struct AudioMetadata {
    std::wstring title;
    std::wstring artist;
    std::wstring album;
    std::wstring album_artist;
    std::wstring genre;
    std::wstring year;
    std::wstring track_number;
    std::wstring disc_number;
    std::wstring comment;

    uint32_t duration_ms = 0;
    uint32_t bitrate_kbps = 0;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    std::wstring format; // 容器: "FLAC", "MP3", "OGG", "OPUS", "WAV", "AIFF", "M4A", "DSF", "DFF", "APE", "NCM", "KGMA"
    std::wstring codec;  // 编码: "FLAC", "MP3", "Vorbis", "Opus", "PCM", "ALAC", "AAC", "DSD", "DST", "APE"
    bool is_lossless = false;

    uint64_t total_samples = 0; // 单声道样本帧总数 (0 = 未知)
    uint64_t audio_offset = 0;  // 音频负载在文件中的起始偏移
    uint64_t audio_bytes = 0;   // 音频负载字节数 (0 = 未知)
    uint64_t file_size = 0;

    // 专辑封面实体
    bool has_cover = false;          // 文件中存在内嵌封面 (惰性模式下 cover_data 可能为空)
    std::string cover_mime_type;     // "image/jpeg", "image/png"
    std::vector<uint8_t> cover_data; // 原始二进制图像数据 (仅 load_cover = true 时填充)
    uint32_t cover_width = 0;
    uint32_t cover_height = 0;
    uint8_t cover_picture_type = 0;  // ID3/FLAC 图片类型 (3 = 正面封面)
    uint64_t cover_offset = 0;       // 封面原始字节在文件中的物理偏移 (0 = 无法直接定位,需重新解析)
    uint32_t cover_size = 0;         // 封面原始字节长度

    // ReplayGain (dB / 线性峰值);has_* 为 false 表示文件中没有对应标签
    bool has_rg_track = false;
    bool has_rg_album = false;
    float rg_track_gain = 0.0f;
    float rg_track_peak = 0.0f;
    float rg_album_gain = 0.0f;
    float rg_album_peak = 0.0f;

    // 内嵌歌词 (ID3 USLT / Vorbis LYRICS / MP4 ©lyr / APE Lyrics),仅 ParseOptions::load_lyrics 时读取
    std::wstring lyrics;

    // I/O 审计 (ParseFile 填写):本次解析真实发生的磁盘读取次数与字节数
    uint32_t io_read_calls = 0;
    uint64_t io_bytes_read = 0;

    // 解析失败 / 特殊文件的诊断信息
    //  open_failed      无法打开 (权限 / 路径)
    //  unknown_format   非已知音频格式
    //  encrypted_kgm    酷狗 KGM/KGMA 加密 (按魔数识别,扩展名写错也能认出,format = "KGMA")
    //  encrypted_vpr    酷狗 VPR 加密 (format = "KGMA")
    //  encrypted_qq     QQ 音乐加密 (mflac/mgg/qmc,文件尾含 QTag/STag/musicex 标记)
    //  encrypted_unknown 文件内容为高熵随机数据,疑似未知加密
    //  video_file       含视频轨的 MP4 等视频文件,不作为音乐入库
    std::string error_code;
};

struct ParseOptions {
    bool load_cover = true;                        // false = 惰性模式,仅定位不读取封面
    uint32_t max_cover_bytes = 32u * 1024 * 1024;  // 单张封面最大读取上限
    unsigned int legacy_code_page = 936;           // 非 Unicode 标签 (ID3 Latin-1 / RIFF INFO) 的回退代码页
    bool read_id3v1 = true;                        // MP3 无 ID3v2 标题时是否回读文件尾部的 ID3v1/APEv2
    bool load_lyrics = false;                      // 是否读取内嵌歌词 (入库时不读,播放时按需读取)
};

class AudioTagParser {
public:
    // 解析本地物理文件 (默认完整读取封面,保持旧行为)
    static bool ParseFile(const std::wstring& filePath, AudioMetadata& outMeta);
    static bool ParseFile(const std::wstring& filePath, AudioMetadata& outMeta, const ParseOptions& options);

    // 惰性封面:按 meta 中记录的物理偏移一次性读出封面;无法直接定位时自动回退为完整解析
    static bool ExtractCover(const std::wstring& filePath, AudioMetadata& inOutMeta);

    // 解析内存流 (data 为文件头部; tailData 可选为文件尾部; fullFileSize 为完整文件大小)
    static bool ParseMemory(const uint8_t* data, size_t size, const std::wstring& hintExt, AudioMetadata& outMeta,
                            uint64_t fullFileSize = 0, const uint8_t* tailData = nullptr, size_t tailSize = 0);
    static bool ParseMemory(const uint8_t* data, size_t size, const std::wstring& hintExt, AudioMetadata& outMeta,
                            const ParseOptions& options,
                            uint64_t fullFileSize = 0, const uint8_t* tailData = nullptr, size_t tailSize = 0);

    // 该扩展名 (含点、小写) 是否为本内核可识别的音频文件
    static bool IsSupportedExtension(const std::wstring& lowerExt);

    // 辅助编码转换
    static std::wstring Utf8ToWide(const std::string& str);
    static std::string WideToUtf8(const std::wstring& wstr);
    static std::wstring Utf16ToWide(const uint8_t* data, size_t len, bool bigEndian);
    static std::wstring AnsiToWide(const std::string& str, unsigned int codePage = 936);
    // 自动识别文本编码 (UTF-8/UTF-16 BOM、合法 UTF-8、否则按代码页,默认 GBK),用于 CUE / 歌词 / M3U 等外部文本
    static std::wstring DecodeTextAuto(const std::string& bytes, unsigned int codePage = 936);
};
