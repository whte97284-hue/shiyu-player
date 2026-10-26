// 多容器封面写入:MP3 / WAV / DSF / DSDIFF 共用一份 ID3v2.3 数据,只是摆放位置不同
#include "cover_writer.hpp"

#include "cover_fetcher.hpp"
#include "platform_io.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

namespace cover_core {

namespace {

constexpr uint32_t kMaxImage = 16u << 20; // 16MB,超过认为异常

uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
uint64_t Le64(const uint8_t* p) { return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32); }
uint32_t Be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint64_t Be64(const uint8_t* p) { return (static_cast<uint64_t>(Be32(p)) << 32) | Be32(p + 4); }

void PutLe32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}
void PutLe32At(std::vector<uint8_t>& v, size_t at, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<uint8_t>(x >> (8 * i));
}
void PutLe64At(std::vector<uint8_t>& v, size_t at, uint64_t x) {
    for (int i = 0; i < 8; ++i) v[at + i] = static_cast<uint8_t>(x >> (8 * i));
}
void PutBe64At(std::vector<uint8_t>& v, size_t at, uint64_t x) {
    for (int i = 0; i < 8; ++i) v[at + i] = static_cast<uint8_t>(x >> (8 * (7 - i)));
}

std::wstring ExtOf(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    const size_t sep = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (sep != std::wstring::npos && dot < sep)) return L"";
    std::wstring e = path.substr(dot);
    for (wchar_t& c : e) c = static_cast<wchar_t>(::towlower(c));
    return e;
}

// ID3v2 的 syncsafe 28 位整数
uint32_t Syncsafe(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0] & 0x7F) << 21) | (static_cast<uint32_t>(p[1] & 0x7F) << 14) |
           (static_cast<uint32_t>(p[2] & 0x7F) << 7) | (static_cast<uint32_t>(p[3] & 0x7F));
}

// APIC 帧内容:编码(1) + MIME\0 + 图片类型(1) + 描述\0 + 图片
std::string ApicBody(const std::string& image, const std::string& mime) {
    std::string b;
    b.push_back(0); // ISO-8859-1,描述留空
    b += mime;
    b.push_back(0);
    b.push_back(3); // Cover (front)
    b.push_back(0); // 空描述
    b += image;
    return b;
}

// 一个 ID3v2 帧:v2.3 用普通 BE32 长度,v2.4 用 syncsafe
void AppendFrame(std::string& out, const char id[4], const std::string& body, bool v24) {
    out.append(id, 4);
    const uint32_t n = static_cast<uint32_t>(body.size());
    if (v24) {
        out.push_back(static_cast<char>((n >> 21) & 0x7F));
        out.push_back(static_cast<char>((n >> 14) & 0x7F));
        out.push_back(static_cast<char>((n >> 7) & 0x7F));
        out.push_back(static_cast<char>(n & 0x7F));
    } else {
        out.push_back(static_cast<char>(n >> 24));
        out.push_back(static_cast<char>(n >> 16));
        out.push_back(static_cast<char>(n >> 8));
        out.push_back(static_cast<char>(n));
    }
    out.push_back(0);
    out.push_back(0);
    out += body;
}

std::string WrapId3v23(const std::string& frames) {
    std::string out("ID3", 3);
    out.push_back(3);
    out.push_back(0);
    out.push_back(0); // 无 unsynchronisation / 扩展头
    const uint32_t n = static_cast<uint32_t>(frames.size());
    out.push_back(static_cast<char>((n >> 21) & 0x7F));
    out.push_back(static_cast<char>((n >> 14) & 0x7F));
    out.push_back(static_cast<char>((n >> 7) & 0x7F));
    out.push_back(static_cast<char>(n & 0x7F));
    out += frames;
    return out;
}

// 整文件读入(封面写入都是"小改大搬",一次性读最省事;WAV/DSF 也就几十 MB)
bool Slurp(const std::wstring& path, std::vector<uint8_t>& out) {
    platform::ReadOnlyFile f;
    if (!f.Open(path, false)) return false;
    out.resize(static_cast<size_t>(f.Size()));
    if (out.empty()) return true;
    return f.ReadAt(0, out.data(), out.size()) == out.size();
}

// 先写 .tmp 再原子替换;失败不碰原文件
bool Commit(const std::wstring& path, const std::vector<uint8_t>& data, std::string* error) {
    const std::wstring tmp = path + L".cover.tmp";
    {
        platform::WritableFile w;
        if (!w.Create(tmp)) {
            if (error) *error = "无法创建临时文件";
            return false;
        }
        if (!data.empty() && !w.Write(data.data(), data.size())) {
            w.Close();
            platform::RemoveFile(tmp);
            if (error) *error = "写入失败";
            return false;
        }
    }
    if (!platform::AtomicReplaceFile(tmp, path)) {
        platform::RemoveFile(tmp);
        if (error) *error = "替换失败";
        return false;
    }
    return true;
}

} // namespace

// ===========================================================================
//  ID3v2 —— 构造 / 换 APIC
// ===========================================================================
std::string BuildId3v23Apic(const std::string& image, const std::string& mime) {
    std::string frames;
    AppendFrame(frames, "APIC", ApicBody(image, mime), false);
    return WrapId3v23(frames);
}

size_t Id3v2TotalSize(const std::string& d) {
    if (d.size() < 10 || std::memcmp(d.data(), "ID3", 3) != 0) return 0;
    const auto* p = reinterpret_cast<const uint8_t*>(d.data());
    return 10 + Syncsafe(p + 6);
}

std::string ReplaceId3v2Apic(const std::string& oldTag, const std::string& image, const std::string& mime) {
    const std::string apic = ApicBody(image, mime);
    if (oldTag.size() < 10 || std::memcmp(oldTag.data(), "ID3", 3) != 0) return BuildId3v23Apic(image, mime);

    const auto* p = reinterpret_cast<const uint8_t*>(oldTag.data());
    const uint8_t version = p[3];
    const uint8_t flags = p[5];

    // v2.2 用 3 字节帧号,处理起来收益极低 —— 直接用新标签(只丢文本帧,封面能上)
    if (version < 3) return BuildId3v23Apic(image, mime);

    const bool v24 = (version == 4);
    size_t pos = 10;
    // 扩展头(v2.3:4 字节大小;v2.4:syncsafe)整体跳过
    if (flags & 0x40) {
        if (pos + 4 > oldTag.size()) return BuildId3v23Apic(image, mime);
        const size_t ext = v24 ? Syncsafe(p + pos) : (Be32(p + pos) + 4);
        pos += ext;
    }

    std::string frames;
    while (pos + 10 <= oldTag.size()) {
        // padding:帧号首字节为 0
        if (p[pos] == 0) break;
        char id[5] = {0};
        std::memcpy(id, p + pos, 4);
        // 帧号应全是 ASCII 大写字母/数字
        bool valid = true;
        for (int i = 0; i < 4; ++i) {
            const char c = id[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) { valid = false; break; }
        }
        if (!valid) break;
        const size_t fsz = v24 ? Syncsafe(p + pos + 4) : Be32(p + pos + 4);
        if (fsz == 0 || pos + 10 + fsz > oldTag.size()) break;
        // 丢掉旧的 APIC 与那些"图片"帧(PIC 是 v2.2 的)
        const bool isPic = (std::memcmp(id, "APIC", 4) == 0);
        if (!isPic) frames.append(oldTag, pos, 10 + fsz);
        pos += 10 + fsz;
    }
    AppendFrame(frames, "APIC", apic, false); // 统一输出成 v2.3
    return WrapId3v23(frames);
}

// ===========================================================================
//  MP3 —— ID3v2 放在文件最前面,换标签后原样接上音频
// ===========================================================================
bool EmbedMp3(const std::wstring& path, const std::string& image, const std::string& mime, std::string* error) {
    platform::ReadOnlyFile f;
    if (!f.Open(path, false)) {
        if (error) *error = "无法打开文件";
        return false;
    }
    const uint64_t size = f.Size();
    if (size < 16) {
        if (error) *error = "文件过小";
        return false;
    }
    // 只需读出原标签(最多 16MB),不用把整首歌载进来
    uint8_t head[10];
    if (f.ReadAt(0, head, 10) != 10) {
        if (error) *error = "读取失败";
        return false;
    }
    uint64_t tagLen = 0;
    std::string oldTag;
    if (std::memcmp(head, "ID3", 3) == 0) {
        tagLen = 10ull + Syncsafe(head + 6);
        if (tagLen > size || tagLen > (1u << 24)) {
            if (error) *error = "ID3 头损坏";
            return false;
        }
        oldTag.resize(static_cast<size_t>(tagLen));
        if (f.ReadAt(0, &oldTag[0], oldTag.size()) != oldTag.size()) {
            if (error) *error = "读取标签失败";
            return false;
        }
    }
    const std::string newTag = ReplaceId3v2Apic(oldTag, image, mime);

    const std::wstring tmp = path + L".cover.tmp";
    {
        platform::WritableFile w;
        if (!w.Create(tmp)) {
            if (error) *error = "无法创建临时文件";
            return false;
        }
        bool ok = w.Write(newTag.data(), newTag.size());
        std::vector<uint8_t> buf(1 << 20);
        uint64_t off = tagLen;
        while (ok && off < size) {
            const size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), size - off));
            const size_t got = f.ReadAt(off, buf.data(), want);
            if (got == 0) { ok = false; break; }
            ok = w.Write(buf.data(), got);
            off += got;
        }
        w.Close();
        if (!ok) {
            platform::RemoveFile(tmp);
            if (error) *error = "写入失败";
            return false;
        }
    }
    // ★ 必须先关掉源文件句柄:Windows 下带着未关闭的读句柄无法替换它自己
    f.Close();
    if (!platform::AtomicReplaceFile(tmp, path)) {
        platform::RemoveFile(tmp);
        if (error) *error = "替换失败";
        return false;
    }
    return true;
}

// ===========================================================================
//  WAV —— RIFF 里追加 "id3 " 块,回填 RIFF 大小
// ===========================================================================
bool EmbedWav(const std::wstring& path, const std::string& image, const std::string& mime, std::string* error) {
    std::vector<uint8_t> d;
    if (!Slurp(path, d)) {
        if (error) *error = "无法读取文件";
        return false;
    }
    if (d.size() < 12 || std::memcmp(d.data(), "RIFF", 4) != 0 || std::memcmp(d.data() + 8, "WAVE", 4) != 0) {
        if (error) *error = "不是 WAV";
        return false;
    }
    if (std::memcmp(d.data() + 4, "\xFF\xFF\xFF\xFF", 4) == 0) {
        if (error) *error = "RF64 暂不支持";
        return false;
    }

    // 逐块复制,顺手丢掉旧的 "id3 " 块
    std::vector<uint8_t> out(d.begin(), d.begin() + 12);
    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        const uint8_t* ch = d.data() + pos;
        uint64_t len = Le32(ch + 4);
        const uint64_t body = pos + 8;
        if (body + len > d.size()) len = d.size() - body; // 截断文件:按实际长度收
        const bool isId3 = (std::memcmp(ch, "id3 ", 4) == 0 || std::memcmp(ch, "ID3 ", 4) == 0);
        if (!isId3) {
            const size_t take = static_cast<size_t>(8 + len + (len & 1));
            const size_t avail = std::min(take, d.size() - pos);
            out.insert(out.end(), d.begin() + pos, d.begin() + pos + avail);
            if (avail < take) out.push_back(0); // 补 pad
        }
        pos = static_cast<size_t>(body + len + (len & 1));
    }

    const std::string tag = BuildId3v23Apic(image, mime);
    out.insert(out.end(), {'i', 'd', '3', ' '});
    PutLe32(out, static_cast<uint32_t>(tag.size()));
    out.insert(out.end(), tag.begin(), tag.end());
    if (tag.size() & 1) out.push_back(0);

    if (out.size() < 8) {
        if (error) *error = "结果异常";
        return false;
    }
    PutLe32At(out, 4, static_cast<uint32_t>(out.size() - 8)); // RIFF 大小
    return Commit(path, out, error);
}

// ===========================================================================
//  DSF —— ID3v2 在文件尾,回填头部两处 64 位字段
// ===========================================================================
bool EmbedDsf(const std::wstring& path, const std::string& image, const std::string& mime, std::string* error) {
    std::vector<uint8_t> d;
    if (!Slurp(path, d)) {
        if (error) *error = "无法读取文件";
        return false;
    }
    if (d.size() < 92 || std::memcmp(d.data(), "DSD ", 4) != 0) {
        if (error) *error = "不是 DSF";
        return false;
    }
    const uint64_t metaPtr = Le64(d.data() + 20);
    // 有旧元数据就砍掉,重新追加
    if (metaPtr != 0 && metaPtr < d.size()) d.resize(static_cast<size_t>(metaPtr));

    const std::string tag = BuildId3v23Apic(image, mime);
    const uint64_t audioEnd = d.size();
    d.insert(d.end(), tag.begin(), tag.end());

    PutLe64At(d, 12, d.size());   // 文件总大小
    PutLe64At(d, 20, audioEnd);   // 元数据偏移
    return Commit(path, d, error);
}

// ===========================================================================
//  DSDIFF —— "ID3 " 块,大端 64 位长度,回填 FRM8 大小
// ===========================================================================
bool EmbedDff(const std::wstring& path, const std::string& image, const std::string& mime, std::string* error) {
    std::vector<uint8_t> d;
    if (!Slurp(path, d)) {
        if (error) *error = "无法读取文件";
        return false;
    }
    if (d.size() < 16 || std::memcmp(d.data(), "FRM8", 4) != 0) {
        if (error) *error = "不是 DSDIFF";
        return false;
    }
    // FRM8 头 12 字节(4CC+BE64 长度) 之后是 4 字节格式类型 "DSD "
    std::vector<uint8_t> out(d.begin(), d.begin() + 12);
    size_t pos = 12;
    while (pos + 12 <= d.size()) {
        const uint8_t* ch = d.data() + pos;
        uint64_t len = Be64(ch + 4);
        const uint64_t body = pos + 12;
        if (body + len > d.size()) len = d.size() - body;
        const bool isId3 = (std::memcmp(ch, "ID3 ", 4) == 0 || std::memcmp(ch, "id3 ", 4) == 0);
        if (!isId3) {
            const size_t take = static_cast<size_t>(12 + len + (len & 1));
            const size_t avail = std::min(take, d.size() - pos);
            out.insert(out.end(), d.begin() + pos, d.begin() + pos + avail);
            if (avail < take) out.push_back(0);
        }
        pos = static_cast<size_t>(body + len + (len & 1));
    }

    const std::string tag = BuildId3v23Apic(image, mime);
    out.insert(out.end(), {'I', 'D', '3', ' '});
    const uint64_t n = tag.size();
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(n >> (8 * (7 - i))));
    out.insert(out.end(), tag.begin(), tag.end());
    if (tag.size() & 1) out.push_back(0);

    if (out.size() < 12) {
        if (error) *error = "结果异常";
        return false;
    }
    PutBe64At(out, 4, out.size() - 12); // FRM8 大小 = 之后全部内容
    return Commit(path, out, error);
}

// ===========================================================================
//  分派
// ===========================================================================
bool CanEmbedCover(const std::wstring& path) {
    const std::wstring e = ExtOf(path);
    return e == L".flac" || e == L".mp3" || e == L".wav" || e == L".dsf" || e == L".dff" || e == L".ogg";
}

bool EmbedCover(const std::wstring& path, const std::string& image, const std::string& mime,
                std::string* error) {
    if (image.size() < 16) {
        if (error) *error = "图片过小";
        return false;
    }
    if (image.size() > kMaxImage) {
        if (error) *error = "封面过大 " + std::to_string(image.size() / 1024) + "KB";
        return false;
    }
    const char* sniffed = SniffImage(image);
    if (!sniffed) {
        if (error) *error = "图片格式不认识";
        return false;
    }
    const std::string m = mime.empty() ? std::string(sniffed) : mime;
    const std::wstring e = ExtOf(path);
    if (e == L".flac") return EmbedFlacPicture(path, image, 0, 0, error, m);
    if (e == L".mp3") return EmbedMp3(path, image, m, error);
    if (e == L".wav") return EmbedWav(path, image, m, error);
    if (e == L".dsf") return EmbedDsf(path, image, m, error);
    if (e == L".dff") return EmbedDff(path, image, m, error);
    if (e == L".ogg") return EmbedOgg(path, image, m, error);
    if (error) *error = "该格式暂不支持写入封面";
    return false;
}

} // namespace cover_core
