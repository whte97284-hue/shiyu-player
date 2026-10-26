#pragma once
// 解析内核内部共享设施 (不对外暴露)
#include "../include/audio_tag_parser.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

namespace tagcore {

// ---------------------------------------------------------------------------
// 字节源:文件或内存的统一随机访问接口
// ---------------------------------------------------------------------------
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual uint64_t Size() const = 0;
    virtual size_t ReadAt(uint64_t off, void* dst, size_t n) = 0;
    virtual bool IsMemory() const { return false; }
};

// 内存字节源:头部缓冲 + 可选尾部缓冲,逻辑大小可大于实际持有的数据 (兼容旧 ParseMemory 语义)
class MemorySource : public ByteSource {
public:
    MemorySource(const uint8_t* head, size_t headSize, uint64_t logicalSize,
                 const uint8_t* tail = nullptr, size_t tailSize = 0);
    uint64_t Size() const override { return logical_size_; }
    size_t ReadAt(uint64_t off, void* dst, size_t n) override;
    bool IsMemory() const override { return true; }

private:
    const uint8_t* head_;
    size_t head_size_;
    uint64_t logical_size_;
    const uint8_t* tail_;
    size_t tail_size_;
};

// ---------------------------------------------------------------------------
// Reader:带滑动窗口缓存、全程越界校验的读取器,并统计真实 I/O
// ---------------------------------------------------------------------------
class Reader {
public:
    static constexpr size_t kWindow = 64 * 1024;

    explicit Reader(ByteSource& src);

    uint64_t Size() const { return size_; }

    // 精确读取 n 字节,不足则返回 false
    bool Read(uint64_t off, void* dst, size_t n);
    // 读取 n 字节到 vector (n 上限由调用方控制)
    bool ReadVec(uint64_t off, size_t n, std::vector<uint8_t>& out);
    // 尽力读取,返回实际字节数
    size_t ReadSome(uint64_t off, void* dst, size_t n);

    bool U8(uint64_t off, uint8_t& v);
    bool U16BE(uint64_t off, uint16_t& v);
    bool U16LE(uint64_t off, uint16_t& v);
    bool U24BE(uint64_t off, uint32_t& v);
    bool U32BE(uint64_t off, uint32_t& v);
    bool U32LE(uint64_t off, uint32_t& v);
    bool U64BE(uint64_t off, uint64_t& v);
    bool U64LE(uint64_t off, uint64_t& v);
    bool Match(uint64_t off, const char* magic, size_t n);

    uint32_t io_calls = 0;
    uint64_t io_bytes = 0;

private:
    bool Fill(uint64_t off, size_t want);

    ByteSource& src_;
    uint64_t size_;
    std::vector<uint8_t> win_;
    uint64_t win_off_ = 0;
    size_t win_len_ = 0;
};

// ---------------------------------------------------------------------------
// 字节序工具
// ---------------------------------------------------------------------------
inline uint16_t BE16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint16_t LE16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t BE24(const uint8_t* p) { return (uint32_t(p[0]) << 16) | (uint32_t(p[1]) << 8) | p[2]; }
inline uint32_t BE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint32_t LE32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline uint64_t BE64(const uint8_t* p) { return (uint64_t(BE32(p)) << 32) | BE32(p + 4); }
inline uint64_t LE64(const uint8_t* p) { return uint64_t(LE32(p)) | (uint64_t(LE32(p + 4)) << 32); }
inline uint32_t SyncSafe32(const uint8_t* p) {
    return (uint32_t(p[0] & 0x7F) << 21) | (uint32_t(p[1] & 0x7F) << 14) | (uint32_t(p[2] & 0x7F) << 7) | (p[3] & 0x7F);
}

// ---------------------------------------------------------------------------
// 文本编解码
// ---------------------------------------------------------------------------
void AppendCodepoint(std::wstring& out, uint32_t cp);
bool IsValidUtf8(const uint8_t* p, size_t n);
bool IsAscii(const uint8_t* p, size_t n);
std::wstring DecodeUtf8(const uint8_t* p, size_t n);
std::wstring DecodeUtf16(const uint8_t* p, size_t n, bool bigEndianDefault);
std::wstring DecodeLatin1(const uint8_t* p, size_t n);
std::wstring DecodeCodePage(const uint8_t* p, size_t n, unsigned int codePage);
// 非 Unicode 遗留编码智能解码:合法 UTF-8 优先,其次按代码页 (默认 GBK),最后 Latin-1
std::wstring DecodeLegacy(const uint8_t* p, size_t n, unsigned int codePage);
// 去除首尾空白与 NUL
std::wstring TrimTag(const std::wstring& s);

// ---------------------------------------------------------------------------
// 解析上下文
// ---------------------------------------------------------------------------
struct Ctx {
    AudioMetadata& m;
    const ParseOptions& opt;
    Reader& r;
};

// 仅在目标字段为空时写入
inline void SetIfEmpty(std::wstring& field, const std::wstring& value) {
    if (field.empty()) {
        std::wstring v = TrimTag(value);
        if (!v.empty()) field = v;
    }
}

// 提交一张候选封面 (自动按 正面封面 > 其他 的优先级择优),被采纳返回 true
//  offset/size:封面字节在文件中的物理位置 (offsetReliable = false 时表示无法直接定位)
//  inlineData :若已在内存中 (例如 base64 解码、反同步后),直接传入
bool OfferCover(Ctx& c, uint8_t pictureType, const std::string& mime, uint64_t offset, uint64_t size,
                bool offsetReliable, const uint8_t* inlineData);

// 从图片字节探测 MIME 与宽高
std::string SniffImageMime(const uint8_t* p, size_t n);
void ProbeImageSize(const uint8_t* p, size_t n, uint32_t& w, uint32_t& h);

// Vorbis Comment (FLAC / Ogg Vorbis / Opus / Ogg FLAC 共用)
void ApplyVorbisComments(Ctx& c, const uint8_t* p, size_t n);
// FLAC METADATA_BLOCK_PICTURE 结构 (内存版)
bool ParseFlacPictureStruct(Ctx& c, const uint8_t* p, size_t n, uint64_t fileOffset, bool offsetReliable);
std::vector<uint8_t> Base64Decode(const uint8_t* p, size_t n);
// 各标签格式共用的扩展字段:REPLAYGAIN_* 与歌词 (LYRICS / UNSYNCEDLYRICS);已处理返回 true
bool ApplyExtraTag(Ctx& c, const std::string& key, const std::wstring& value);
// AES-128-ECB 原地解密 (n 必须是 16 的倍数);NCM 元数据与密钥块共用
void Aes128EcbDecrypt(const uint8_t key[16], uint8_t* data, size_t n);

const wchar_t* Id3v1GenreName(unsigned idx);

// ---------------------------------------------------------------------------
// 各格式解析器
// ---------------------------------------------------------------------------
// ID3v2:解析位于 base 的标签,返回标签总长度 (含头尾);不是 ID3v2 返回 0
uint64_t ParseId3v2(Ctx& c, uint64_t base);
// 计算 base 处 ID3v2 标签的总长度 (不解析)
uint64_t Id3v2TotalSize(Reader& r, uint64_t base);
bool ParseId3v1AndApeTail(Ctx& c, bool* outHasId3v1, uint64_t* outApeBytes);
bool ParseApeTagAt(Ctx& c, uint64_t footerOffset, uint64_t* outTagBytes);

bool ParseMpegAudio(Ctx& c, uint64_t start);
bool ParseFlac(Ctx& c, uint64_t base);
bool ParseOgg(Ctx& c);
bool ParseWav(Ctx& c);
bool ParseAiff(Ctx& c);
bool ParseMp4(Ctx& c);
bool ParseDsf(Ctx& c);
bool ParseDff(Ctx& c);
bool ParseApe(Ctx& c, uint64_t base);
bool ParseNcm(Ctx& c);

} // namespace tagcore
