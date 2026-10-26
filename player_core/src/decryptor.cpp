#include "../include/decryptor.hpp"

#include "../include/audio_tag_parser.hpp"
#include "../include/cover_fetcher.hpp"
#include "../include/platform_io.hpp"
#include "tag_internal.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <cwctype>
#include <filesystem>
#include <sstream>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

constexpr size_t kChunk = 1u << 20;

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    return s;
}

std::wstring ExtLower(const std::wstring& name) {
    size_t d = name.rfind(L'.');
    size_t sep = name.find_last_of(L"\\/");
    if (d == std::wstring::npos || (sep != std::wstring::npos && d < sep)) return L"";
    return Lower(name.substr(d));
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring StemOf(const std::wstring& path) {
    std::wstring base = FileNameOf(path);
    size_t d = base.rfind(L'.');
    return d == std::wstring::npos ? base : base.substr(0, d);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
#ifdef _WIN32
    const wchar_t sep = L'\\';
#else
    const wchar_t sep = L'/';
#endif
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + sep + name;
}

std::wstring SniffExt(const uint8_t* buf, size_t len) {
    if (len >= 4 && !std::memcmp(buf, "fLaC", 4)) return L".flac";
    if (len >= 3 && !std::memcmp(buf, "ID3", 3)) return L".mp3";
    if (len >= 2 && buf[0] == 0xFF && (buf[1] & 0xE0) == 0xE0) return L".mp3";
    if (len >= 4 && !std::memcmp(buf, "OggS", 4)) return L".ogg";
    if (len >= 12 && !std::memcmp(buf, "RIFF", 4) && !std::memcmp(buf + 8, "WAVE", 4)) return L".wav";
    if (len >= 8 && !std::memcmp(buf + 4, "ftyp", 4)) return L".m4a";
    return L"";
}

// 产物路径:同名文件已存在时追加 (2)、(3)…,绝不覆盖用户已有的文件
std::wstring UniqueOutput(const std::wstring& dir, const std::wstring& stem, const std::wstring& ext) {
    std::wstring p = JoinPath(dir, stem + ext);
    for (int n = 2; platform::PathExists(p) && n < 1000; ++n) p = JoinPath(dir, stem + L" (" + std::to_wstring(n) + L")" + ext);
    return p;
}

bool Cancelled(const DecryptOptions& o) { return o.cancel && o.cancel->load(); }

// 写入 <目标>.part,完成后改名;失败或取消时删除临时文件,目录里不会留下半截产物
class PartWriter {
public:
    bool Create(const std::wstring& finalPath) {
        final_ = finalPath;
        part_ = finalPath + L".part";
        return f_.Create(part_);
    }
    bool Write(const void* p, size_t n) { return ok_ = ok_ && f_.Write(p, n); }
    bool Commit() {
        f_.Close();
        if (!ok_ || !platform::AtomicReplaceFile(part_, final_)) {
            platform::RemoveFile(part_);
            return false;
        }
        return true;
    }
    void Abort() {
        f_.Close();
        platform::RemoveFile(part_);
    }

private:
    platform::WritableFile f_;
    std::wstring final_, part_;
    bool ok_ = true;
};

void PutBE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x));
}
void PutLE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x));
    v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 24));
}

// ---------------------------------------------------------------------------
// 标签回写:FLAC 追加 VORBIS_COMMENT / PICTURE 元数据块;MP3 前置 ID3v2.3
// ---------------------------------------------------------------------------
struct TagSet {
    std::wstring title, artist, album;
    std::vector<uint8_t> cover;
    std::string mime;
    bool Empty() const { return title.empty() && artist.empty() && album.empty() && cover.empty(); }
};

std::string MimeOf(const std::vector<uint8_t>& img, const std::string& hint) {
    if (!hint.empty()) return hint;
    if (img.size() > 3 && img[0] == 0x89 && img[1] == 'P') return "image/png";
    return "image/jpeg";
}

// 返回新的 FLAC 头部 (fLaC + 全部元数据块),metaEnd 输出原头部长度;无法解析时返回空
std::vector<uint8_t> RebuildFlacHeader(const uint8_t* buf, size_t n, const TagSet& tags, size_t& metaEnd) {
    std::vector<uint8_t> out;
    if (n < 8 || std::memcmp(buf, "fLaC", 4) != 0) return out;
    struct Block {
        uint8_t type;
        size_t off, len;
    };
    std::vector<Block> blocks;
    size_t pos = 4;
    bool hasPic = false;
    for (;;) {
        if (pos + 4 > n) return {};
        uint8_t h = buf[pos];
        size_t len = (size_t(buf[pos + 1]) << 16) | (size_t(buf[pos + 2]) << 8) | buf[pos + 3];
        if (pos + 4 + len > n) return {};
        uint8_t type = h & 0x7F;
        hasPic = hasPic || type == 6;
        blocks.push_back({type, pos + 4, len});
        pos += 4 + len;
        if (h & 0x80) break;
    }
    metaEnd = pos;

    // VORBIS_COMMENT:已有块时保留原有字段与厂商串,只补上缺失的标题 / 歌手 / 专辑
    std::vector<std::string> items;
    std::string vendor = "kugou-player-core";
    int vcIndex = -1;
    for (size_t i = 0; i < blocks.size(); ++i) {
        if (blocks[i].type != 4) continue;
        vcIndex = int(i);
        const uint8_t* q = buf + blocks[i].off;
        size_t len = blocks[i].len, pos2 = 0;
        auto u32 = [&](uint32_t& v) {
            if (pos2 + 4 > len) return false;
            v = uint32_t(q[pos2]) | uint32_t(q[pos2 + 1]) << 8 | uint32_t(q[pos2 + 2]) << 16 | uint32_t(q[pos2 + 3]) << 24;
            pos2 += 4;
            return true;
        };
        uint32_t vl = 0, cnt = 0;
        if (!u32(vl) || pos2 + vl > len) break;
        vendor.assign(reinterpret_cast<const char*>(q + pos2), vl);
        pos2 += vl;
        if (!u32(cnt)) break;
        for (uint32_t k = 0; k < cnt; ++k) {
            uint32_t cl = 0;
            if (!u32(cl) || pos2 + cl > len) break;
            items.emplace_back(reinterpret_cast<const char*>(q + pos2), cl);
            pos2 += cl;
        }
        break;
    }
    auto hasKey = [&](const char* key) {
        size_t kl = std::strlen(key);
        for (const auto& it : items)
            if (it.size() > kl && it[kl] == '=' && std::equal(key, key + kl, it.begin(), [](char a, char b) {
                    return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
                }))
                return true;
        return false;
    };
    size_t before = items.size();
    auto add = [&](const char* k, const std::wstring& v) {
        if (!v.empty()) items.push_back(std::string(k) + "=" + AudioTagParser::WideToUtf8(v));
    };
    if (!hasKey("TITLE")) add("TITLE", tags.title);
    if (!hasKey("ARTIST")) {
        // 多位歌手分别写一条 ARTIST,播放器可正确拆分
        const std::wstring& a = tags.artist;
        size_t st = 0;
        while (!a.empty() && st <= a.size()) {
            size_t e = a.find(L'/', st);
            add("ARTIST", a.substr(st, e == std::wstring::npos ? std::wstring::npos : e - st));
            if (e == std::wstring::npos) break;
            st = e + 1;
        }
    }
    if (!hasKey("ALBUM")) add("ALBUM", tags.album);
    std::vector<uint8_t> vc;
    const bool vcChanged = items.size() != before;
    if (vcChanged) {
        PutLE32(vc, uint32_t(vendor.size()));
        vc.insert(vc.end(), vendor.begin(), vendor.end());
        PutLE32(vc, uint32_t(items.size()));
        for (const auto& it : items) {
            PutLE32(vc, uint32_t(it.size()));
            vc.insert(vc.end(), it.begin(), it.end());
        }
    }

    std::vector<std::pair<uint8_t, std::vector<uint8_t>>> extra;
    if (vcChanged && vcIndex < 0) extra.emplace_back(4, vc);
    if (!hasPic && !tags.cover.empty() && tags.cover.size() < (1u << 24) - 256) {
        std::vector<uint8_t> pic;
        std::string mime = MimeOf(tags.cover, tags.mime);
        PutBE32(pic, 3);
        PutBE32(pic, uint32_t(mime.size()));
        pic.insert(pic.end(), mime.begin(), mime.end());
        PutBE32(pic, 0);                      // 描述
        for (int i = 0; i < 4; ++i) PutBE32(pic, 0); // 宽 / 高 / 色深 / 索引色 (未知)
        PutBE32(pic, uint32_t(tags.cover.size()));
        pic.insert(pic.end(), tags.cover.begin(), tags.cover.end());
        extra.emplace_back(6, std::move(pic));
    }
    if (extra.empty() && !(vcChanged && vcIndex >= 0)) return {};

    out.insert(out.end(), buf, buf + 4);
    auto header = [&](uint8_t type, size_t len, bool last) {
        out.push_back(uint8_t((last ? 0x80 : 0) | type));
        out.push_back(uint8_t(len >> 16));
        out.push_back(uint8_t(len >> 8));
        out.push_back(uint8_t(len));
    };
    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        bool lastOne = extra.empty() && i + 1 == blocks.size();
        if (int(i) == vcIndex && vcChanged) {
            header(4, vc.size(), lastOne);
            out.insert(out.end(), vc.begin(), vc.end());
        } else {
            header(b.type, b.len, lastOne);
            out.insert(out.end(), buf + b.off, buf + b.off + b.len);
        }
    }
    for (size_t i = 0; i < extra.size(); ++i) {
        header(extra[i].first, extra[i].second.size(), i + 1 == extra.size());
        out.insert(out.end(), extra[i].second.begin(), extra[i].second.end());
    }
    return out;
}

std::vector<uint8_t> BuildId3v23(const TagSet& tags) {
    std::vector<uint8_t> frames;
    auto frame = [&](const char id[4], const std::vector<uint8_t>& body) {
        frames.insert(frames.end(), id, id + 4);
        PutBE32(frames, uint32_t(body.size()));
        frames.push_back(0);
        frames.push_back(0);
        frames.insert(frames.end(), body.begin(), body.end());
    };
    auto text = [&](const char id[4], const std::wstring& v) {
        if (v.empty()) return;
        std::vector<uint8_t> b{1, 0xFF, 0xFE}; // UTF-16 LE 带 BOM
        for (wchar_t ch : v) {
            uint32_t c = static_cast<uint32_t>(ch);
            if (c > 0xFFFF) { // UTF-32 平台上的增补平面字符
                c -= 0x10000;
                uint16_t hi = uint16_t(0xD800 + (c >> 10)), lo = uint16_t(0xDC00 + (c & 0x3FF));
                b.push_back(uint8_t(hi)), b.push_back(uint8_t(hi >> 8)), b.push_back(uint8_t(lo)), b.push_back(uint8_t(lo >> 8));
            } else {
                b.push_back(uint8_t(c)), b.push_back(uint8_t(c >> 8));
            }
        }
        frame(id, b);
    };
    text("TIT2", tags.title);
    text("TPE1", tags.artist);
    text("TALB", tags.album);
    if (!tags.cover.empty()) {
        std::vector<uint8_t> b{0};
        std::string mime = MimeOf(tags.cover, tags.mime);
        b.insert(b.end(), mime.begin(), mime.end());
        b.push_back(0);
        b.push_back(3); // 封面 (正面)
        b.push_back(0); // 空描述
        b.insert(b.end(), tags.cover.begin(), tags.cover.end());
        frame("APIC", b);
    }
    if (frames.empty()) return {};
    std::vector<uint8_t> out{'I', 'D', '3', 3, 0, 0};
    uint32_t sz = uint32_t(frames.size());
    out.push_back(uint8_t((sz >> 21) & 0x7F));
    out.push_back(uint8_t((sz >> 14) & 0x7F));
    out.push_back(uint8_t((sz >> 7) & 0x7F));
    out.push_back(uint8_t(sz & 0x7F));
    out.insert(out.end(), frames.begin(), frames.end());
    return out;
}

// ---------------------------------------------------------------------------
// KGM 掩码表:按路径映射一次,进程内共享
// ---------------------------------------------------------------------------
std::shared_ptr<platform::MappedFile> MaskFor(const std::wstring& path) {
    static std::mutex m;
    static std::wstring loadedPath;
    static std::shared_ptr<platform::MappedFile> mask;
    std::lock_guard<std::mutex> lock(m);
    if (mask && loadedPath == path) return mask;
    auto f = std::make_shared<platform::MappedFile>();
    if (!f->Open(path) || f->Size() == 0) return nullptr;
    mask = f;
    loadedPath = path;
    return mask;
}

#pragma pack(push, 1)
struct KgmHeaderRaw {
    uint8_t magic[16];
    uint32_t header_len;
    uint32_t crypto_ver;
    uint32_t key_derive;
    uint8_t file_key[16];
};
#pragma pack(pop)

const uint8_t kKgmMagic[16] = {0x7C, 0xD5, 0x32, 0xEB, 0x86, 0x02, 0x7F, 0x4B, 0xA8, 0xAF, 0xA6, 0x8E, 0x0F, 0xFF, 0x99, 0x14};
const uint8_t kVprMagic[16] = {0x05, 0x28, 0xBC, 0x96, 0xE9, 0xE4, 0x5A, 0x43, 0x91, 0xAA, 0xBD, 0xD0, 0x7A, 0xF5, 0x36, 0x31};

// ===========================================================================
class KgmDecryptor : public Decryptor {
public:
    EncryptedKind Kind() const override { return EncryptedKind::Kgm; }
    const char* Id() const override { return "kgm"; }
    std::wstring DisplayName() const override { return L"酷狗 KGM"; } // 酷狗 KGM
    std::vector<std::wstring> Extensions() const override { return {L".kgm", L".kgma", L".vpr"}; }
    bool Native() const override { return true; }

    // 掩码已内置 (见 KgmDecryptBlockBuiltin),外部 kgm.mask 只是可选覆盖
    bool IsAvailable(std::wstring*) const override { return true; }
    bool Probe(const uint8_t* h, size_t n) const override {
        return n >= 16 && (!std::memcmp(h, kKgmMagic, 16) || !std::memcmp(h, kVprMagic, 16));
    }

    DecryptResult Decrypt(const std::wstring& src, const DecryptOptions& opt) const override {
        DecryptResult r;
        // 显式提供了 kgm.mask 时用它,否则用内置算法生成的掩码 (两者逐字节一致)
        auto mask = MaskFor(DecryptorRegistry::Instance().KgmMaskPath());
        platform::ReadOnlyFile in;
        if (!in.Open(src, false)) return Fail(r, L"无法打开源文件"); // 无法打开源文件
        KgmHeaderRaw header;
        if (in.ReadAt(0, &header, sizeof(header)) != sizeof(header) || header.header_len < sizeof(header) ||
            header.header_len >= in.Size())
            return Fail(r, L"KGM 文件头损坏"); // KGM 文件头损坏
        uint8_t key[17];
        std::memcpy(key, header.file_key, 16);
        key[16] = 0;
        auto decryptBlock = [&](uint8_t* p, size_t n, uint64_t at) {
            if (mask) KgmDecryptBlock(p, n, at, key, mask->Data(), mask->Size());
            else KgmDecryptBlockBuiltin(p, n, at, key);
        };

        std::vector<uint8_t> buf(kChunk);
        const uint64_t payload = in.Size() - header.header_len;
        size_t first = in.ReadAt(header.header_len, buf.data(), size_t(std::min<uint64_t>(kChunk, payload)));
        if (first == 0) return Fail(r, L"KGM 负载为空");
        decryptBlock(buf.data(), first, 0);
        std::wstring ext = SniffExt(buf.data(), first);
        if (ext.empty()) return Fail(r, L"解密结果不是音频 (掩码表不匹配?)"); // 解密结果不是音频 (掩码表不匹配?)

        if (!platform::IsDirectory(opt.output_dir)) platform::CreateDirectories(opt.output_dir);
        r.output = UniqueOutput(opt.output_dir, StemOf(src), ext);
        PartWriter out;
        if (!out.Create(r.output)) return Fail(r, L"无法创建输出文件"); // 无法创建输出文件
        out.Write(buf.data(), first);
        uint64_t off = first;
        while (off < payload && !Cancelled(opt)) {
            size_t n = in.ReadAt(header.header_len + off, buf.data(), size_t(std::min<uint64_t>(kChunk, payload - off)));
            if (n == 0) break;
            decryptBlock(buf.data(), n, off);
            if (!out.Write(buf.data(), n)) break;
            off += n;
            if (opt.progress) opt.progress(off, payload);
        }
        if (off != payload) {
            out.Abort();
            return Fail(r, Cancelled(opt) ? L"已取消" : L"写入失败");
        }
        if (!out.Commit()) return Fail(r, L"写入失败");
        r.ok = true;
        return r;
    }

private:
    static DecryptResult& Fail(DecryptResult& r, const std::wstring& msg) {
        r.ok = false;
        r.message = msg;
        r.output.clear();
        return r;
    }
};

// ===========================================================================
class NcmDecryptor : public Decryptor {
public:
    EncryptedKind Kind() const override { return EncryptedKind::Ncm; }
    const char* Id() const override { return "ncm"; }
    std::wstring DisplayName() const override { return L"网易云 NCM"; } // 网易云 NCM
    std::vector<std::wstring> Extensions() const override { return {L".ncm"}; }
    bool Native() const override { return true; }
    bool IsAvailable(std::wstring*) const override { return true; }
    bool Probe(const uint8_t* h, size_t n) const override { return n >= 8 && !std::memcmp(h, "CTENFDAM", 8); }

    DecryptResult Decrypt(const std::wstring& src, const DecryptOptions& opt) const override {
        DecryptResult r;
        auto fail = [&](const wchar_t* m) {
            r.ok = false;
            r.message = m;
            r.output.clear();
            return r;
        };
        platform::ReadOnlyFile in;
        if (!in.Open(src, false)) return fail(L"无法打开源文件");
        const uint64_t size = in.Size();
        uint8_t head[14];
        if (in.ReadAt(0, head, 14) != 14 || std::memcmp(head, "CTENFDAM", 8) != 0) return fail(L"不是 NCM 文件");
        const uint32_t keyLen = uint32_t(head[10]) | uint32_t(head[11]) << 8 | uint32_t(head[12]) << 16 | uint32_t(head[13]) << 24;
        if (keyLen == 0 || keyLen % 16 != 0 || keyLen > 4096 || 14ull + keyLen + 4 > size) return fail(L"NCM 密钥块损坏");

        // 1. 密钥块:逐字节 ^0x64 → AES-128-ECB (core key) → 去 PKCS7 → 去掉 "neteasecloudmusic" 前缀 = RC4 密钥
        std::vector<uint8_t> kb(keyLen);
        in.ReadAt(14, kb.data(), keyLen);
        for (auto& b : kb) b ^= 0x64;
        static const uint8_t kCoreKey[16] = {0x68, 0x7A, 0x48, 0x52, 0x41, 0x6D, 0x73, 0x6F,
                                             0x35, 0x6B, 0x49, 0x6E, 0x62, 0x61, 0x78, 0x57};
        tagcore::Aes128EcbDecrypt(kCoreKey, kb.data(), kb.size());
        uint8_t pad = kb.back();
        if (pad < 1 || pad > 16 || pad > kb.size()) return fail(L"NCM 密钥解密失败");
        kb.resize(kb.size() - pad);
        static const char kPrefix[] = "neteasecloudmusic";
        if (kb.size() <= 17 || std::memcmp(kb.data(), kPrefix, 17) != 0) return fail(L"NCM 密钥解密失败");
        const uint8_t* key = kb.data() + 17;
        const size_t klen = kb.size() - 17;

        // 2. RC4 变体:KSA 生成置换盒,密钥流只与 (i & 0xFF) 有关,预先展开成 256 字节表
        uint8_t box[256];
        for (int i = 0; i < 256; ++i) box[i] = uint8_t(i);
        uint8_t last = 0;
        size_t ko = 0;
        for (int i = 0; i < 256; ++i) {
            uint8_t sw = box[i];
            uint8_t c = uint8_t(sw + last + key[ko]);
            if (++ko >= klen) ko = 0;
            box[i] = box[c];
            box[c] = sw;
            last = c;
        }
        uint8_t stream[256];
        for (int i = 0; i < 256; ++i) {
            uint8_t j = uint8_t(i + 1);
            stream[i] = box[uint8_t(box[j] + box[uint8_t(box[j] + j)])];
        }
        auto xorStream = [&](uint8_t* p, size_t n, uint64_t pos) {
            for (size_t i = 0; i < n; ++i) p[i] ^= stream[(pos + i) & 0xFF];
        };

        // 3. 定位音频:元数据块之后是 CRC32(4) + 间隙(5),再是封面;新旧两种封面布局都试,以解出的文件头确认
        uint8_t lb[4];
        uint64_t metaLenOff = 14ull + keyLen;
        in.ReadAt(metaLenOff, lb, 4);
        uint64_t metaLen = uint32_t(lb[0]) | uint32_t(lb[1]) << 8 | uint32_t(lb[2]) << 16 | uint32_t(lb[3]) << 24;
        uint64_t p = metaLenOff + 4 + metaLen + 9;
        uint8_t ab[8] = {};
        in.ReadAt(p, ab, 8);
        uint64_t a = uint32_t(ab[0]) | uint32_t(ab[1]) << 8 | uint32_t(ab[2]) << 16 | uint32_t(ab[3]) << 24;
        uint64_t b = uint32_t(ab[4]) | uint32_t(ab[5]) << 8 | uint32_t(ab[6]) << 16 | uint32_t(ab[7]) << 24;
        std::vector<uint64_t> candidates = {p + 8 + a, p + 4 + a, p + 8 + b, p + 8, p + 4};
        uint64_t audio = 0;
        std::wstring ext;
        for (uint64_t c : candidates) {
            uint8_t probe[16];
            if (c + 16 > size || in.ReadAt(c, probe, 16) != 16) continue;
            xorStream(probe, 16, 0);
            ext = SniffExt(probe, 16);
            if (!ext.empty()) {
                audio = c;
                break;
            }
        }
        if (ext.empty()) return fail(L"未找到音频数据"); // 未找到音频数据

        // 4. 元数据与封面 (由解析内核解出),写回产物
        TagSet tags;
        if (opt.write_tags) {
            AudioMetadata meta;
            ParseOptions po;
            po.load_cover = true;
            if (AudioTagParser::ParseFile(src, meta, po)) {
                tags.title = meta.title;
                tags.artist = meta.artist;
                tags.album = meta.album;
                tags.cover = std::move(meta.cover_data);
                tags.mime = meta.cover_mime_type;
            }
        }

        if (!platform::IsDirectory(opt.output_dir)) platform::CreateDirectories(opt.output_dir);
        r.output = UniqueOutput(opt.output_dir, StemOf(src), ext);
        PartWriter out;
        if (!out.Create(r.output)) return fail(L"无法创建输出文件");

        const uint64_t payload = size - audio;
        std::vector<uint8_t> buf(kChunk);
        size_t first = in.ReadAt(audio, buf.data(), size_t(std::min<uint64_t>(kChunk, payload)));
        xorStream(buf.data(), first, 0);
        size_t skip = 0;
        if (!tags.Empty()) {
            if (ext == L".flac") {
                size_t metaEnd = 0;
                std::vector<uint8_t> hdr = RebuildFlacHeader(buf.data(), first, tags, metaEnd);
                if (!hdr.empty()) {
                    out.Write(hdr.data(), hdr.size());
                    skip = metaEnd;
                }
            } else if (ext == L".mp3" && std::memcmp(buf.data(), "ID3", 3) != 0) {
                std::vector<uint8_t> id3 = BuildId3v23(tags);
                if (!id3.empty()) out.Write(id3.data(), id3.size());
            }
        }
        out.Write(buf.data() + skip, first - skip);
        uint64_t off = first;
        while (off < payload && !Cancelled(opt)) {
            size_t n = in.ReadAt(audio + off, buf.data(), size_t(std::min<uint64_t>(kChunk, payload - off)));
            if (n == 0) break;
            xorStream(buf.data(), n, off);
            if (!out.Write(buf.data(), n)) break;
            off += n;
            if (opt.progress) opt.progress(off, payload);
        }
        if (off != payload) {
            out.Abort();
            return fail(Cancelled(opt) ? L"已取消" : L"写入失败");
        }
        if (!out.Commit()) return fail(L"写入失败");
        r.ok = true;
        return r;
    }
};

// ===========================================================================
// KGG:调用外部 kgg-dec.exe (依赖同目录 infra.dll 与本机酷狗客户端的密钥库)
// ===========================================================================
class KggDecryptor : public Decryptor {
public:
    EncryptedKind Kind() const override { return EncryptedKind::Kgg; }
    const char* Id() const override { return "kgg"; }
    std::wstring DisplayName() const override { return L"酷狗 KGG"; } // 酷狗 KGG
    std::vector<std::wstring> Extensions() const override { return {L".kgg"}; }
    bool Native() const override { return false; }

    static std::wstring Dir() { return JoinPath(DecryptorRegistry::Instance().ToolsDir(), L"kgg"); }
    static std::wstring Exe() { return JoinPath(Dir(), L"kgg-dec.exe"); }

    bool IsAvailable(std::wstring* reason) const override {
#ifdef _WIN32
        if (!platform::PathExists(Exe()) || !platform::PathExists(JoinPath(Dir(), L"infra.dll"))) {
            if (reason) *reason = L"缺少 kgg-dec.exe / infra.dll"; // 缺少 kgg-dec.exe / infra.dll
            return false;
        }
        return true;
#else
        if (reason) *reason = L"需要 Windows"; // 需要 Windows
        return false;
#endif
    }
    bool Probe(const uint8_t*, size_t) const override { return false; } // 无公开魔数,只按扩展名识别

    DecryptResult Decrypt(const std::wstring& src, const DecryptOptions& opt) const override {
        DecryptResult r;
        std::wstring why;
        if (!IsAvailable(&why)) {
            r.message = why;
            return r;
        }
#ifdef _WIN32
        static std::atomic<unsigned> counter{0};
        if (!platform::IsDirectory(opt.output_dir)) platform::CreateDirectories(opt.output_dir);
        // 在产物目录下的私有工作目录里处理副本:源目录 (可能是只读曲库) 里不产生任何文件
        std::wstring work = JoinPath(opt.output_dir, L".kgg-work-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                                         std::to_wstring(counter.fetch_add(1)));
        std::error_code ec;
        fs::remove_all(fs::path(work), ec);
        platform::CreateDirectories(work);
        auto cleanup = [&]() { fs::remove_all(fs::path(work), ec); };
        const std::wstring copy = JoinPath(work, FileNameOf(src));
        {
            platform::ReadOnlyFile in;
            PartWriter w;
            if (!in.Open(src, false) || !w.Create(copy)) {
                cleanup();
                r.message = L"无法复制源文件";
                return r;
            }
            std::vector<uint8_t> buf(kChunk);
            uint64_t off = 0;
            while (off < in.Size()) {
                size_t n = in.ReadAt(off, buf.data(), size_t(std::min<uint64_t>(kChunk, in.Size() - off)));
                if (n == 0 || !w.Write(buf.data(), n)) break;
                off += n;
            }
            if (off != in.Size() || !w.Commit()) {
                cleanup();
                r.message = L"无法复制源文件";
                return r;
            }
        }

        std::wstring cmd = L"\"" + Exe() + L"\" \"" + work + L"\"";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring dir = Dir();
        if (!CreateProcessW(Exe().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
            cleanup();
            r.message = L"无法启动 kgg-dec.exe";
            return r;
        }
        DWORD waited = 0;
        bool killed = false;
        while (WaitForSingleObject(pi.hProcess, 200) == WAIT_TIMEOUT) {
            waited += 200;
            if (Cancelled(opt) || waited > 10 * 60 * 1000) {
                TerminateProcess(pi.hProcess, 1);
                WaitForSingleObject(pi.hProcess, 2000);
                killed = true;
                break;
            }
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (killed) {
            cleanup();
            r.message = Cancelled(opt) ? L"已取消" : L"kgg-dec.exe 超时";
            return r;
        }

        // 工作目录里出现的音频文件即为产物
        std::wstring produced, ext;
        platform::EnumerateFilesRecursive(work, [&](const std::wstring&, const std::vector<platform::DirEntry>& files) {
            for (const auto& f : files) {
                std::wstring e = ExtLower(f.name);
                if (e == L".kgg" || e == L".part" || f.size == 0) continue;
                platform::ReadOnlyFile pf;
                uint8_t h[16] = {};
                if (!pf.Open(f.path, true) || pf.ReadAt(0, h, 16) < 4) continue;
                std::wstring sniff = SniffExt(h, 16);
                if (!sniff.empty()) {
                    produced = f.path;
                    ext = sniff;
                    return false;
                }
            }
            return true;
        });
        if (produced.empty()) {
            cleanup();
            r.message = L"kgg-dec.exe 未产生音频 (本机酷狗密钥库缺失?)"; // 未产生音频 (本机酷狗密钥库缺失?)
            return r;
        }
        r.output = UniqueOutput(opt.output_dir, StemOf(src), ext);
        if (!platform::AtomicReplaceFile(produced, r.output)) {
            cleanup();
            r.output.clear();
            r.message = L"写入失败";
            return r;
        }
        cleanup();
        // KGG 解出的 FLAC 没有 PICTURE 块 (kgg-dec 只做纯字节流解密)。
        // 这里补一次封面:本地算大小/MD5 对酷狗 sqhash,严格匹配优先,歌名兜底。
        // 失败**不影响**解密结果 —— 拿不到封面就算了,音频本身是好的。
        {
            std::string coverErr, coverSrc;
            cover_core::EnsureCover(r.output, &coverErr, &coverSrc);
        }
        r.ok = true;
        return r;
#else
        (void)src;
        (void)opt;
        return r;
#endif
    }
};

} // namespace

// ===========================================================================
void KgmDecryptBlock(uint8_t* data, size_t len, uint64_t fileOffset, const uint8_t key17[17], const uint8_t* mask,
                     uint64_t maskLen) {
    // 原始逐半字节算法:t = c ^ k; low = (m ^ t) & 0xF; high = ((t >> 4) ^ (m >> 4) ^ low) & 0xF
    // 代数化简后等价于:x = c ^ k ^ m; out = x ^ (x << 4)  (取低 8 位)
    size_t ki = static_cast<size_t>(fileOffset % 17);
    uint64_t mi = fileOffset % maskLen;
    for (size_t i = 0; i < len; ++i) {
        uint8_t x = static_cast<uint8_t>(data[i] ^ key17[ki] ^ mask[mi]);
        data[i] = static_cast<uint8_t>(x ^ (x << 4));
        if (++ki == 17) ki = 0;
        if (++mi == maskLen) mi = 0;
    }
}

// 内置 KGM 掩码:原 kgm.mask (81,979,072 字节) 的每个字节都满足
//   mask[i] = f(slot[i % 16] ^ pad[i % 17] ^ collapse(i)),  f(y) = y ^ (y << 4)
// slot 为酷狗 v3 槽位密钥 {0x6C,0x2C,0x2F,0x27} 的 MD5 (每 2 字节倒序,同 unlock-music),
// pad 为 17 字节固定序列,collapse(i) 为偏移 4 个字节的异或。f 是线性的对合变换,所以
//   f(c ^ k ^ mask) = f(c ^ k) ^ slot ^ pad ^ collapse(i)
// 不再需要 80MB 的掩码文件,且偏移超过原文件长度时也正确 (原文件会回绕)
namespace {
const uint8_t kKgmSlotBox[16] = {0x14, 0xE3, 0x10, 0xB1, 0x0D, 0x3B, 0x6F, 0x41, 0x85, 0x6B, 0x79, 0x27, 0x8B, 0xFD, 0x61, 0x85};
const uint8_t kKgmPad[17] = {0x2C, 0x67, 0xFF, 0x20, 0x70, 0x61, 0x91, 0x0A, 0x3E,
                             0x61, 0x12, 0x6D, 0x91, 0x50, 0x72, 0xCD, 0xDB};
struct KgmBuiltinTable {
    uint8_t t[272]; // 16 × 17
    KgmBuiltinTable() {
        for (int i = 0; i < 272; ++i) t[i] = static_cast<uint8_t>(kKgmSlotBox[i % 16] ^ kKgmPad[i % 17]);
    }
};
} // namespace

void KgmDecryptBlockBuiltin(uint8_t* data, size_t len, uint64_t fileOffset, const uint8_t key17[17]) {
    static const KgmBuiltinTable table;
    size_t ki = static_cast<size_t>(fileOffset % 17);
    size_t ti = static_cast<size_t>(fileOffset % 272);
    for (size_t i = 0; i < len; ++i) {
        const uint32_t pos = static_cast<uint32_t>(fileOffset + i);
        const uint8_t collapse = static_cast<uint8_t>(pos ^ (pos >> 8) ^ (pos >> 16) ^ (pos >> 24));
        const uint8_t x = static_cast<uint8_t>(data[i] ^ key17[ki]);
        data[i] = static_cast<uint8_t>(x ^ (x << 4) ^ table.t[ti] ^ collapse);
        if (++ki == 17) ki = 0;
        if (++ti == 272) ti = 0;
    }
}

bool IsEncryptedAudioExtension(const std::wstring& e) {
    return DecryptorRegistry::Instance().ByExtension(e) != nullptr;
}

DecryptorRegistry::DecryptorRegistry() : tools_dir_(L"tools") {
    list_.push_back(std::make_unique<KgmDecryptor>());
    list_.push_back(std::make_unique<NcmDecryptor>());
    list_.push_back(std::make_unique<KggDecryptor>());
}

DecryptorRegistry& DecryptorRegistry::Instance() {
    static DecryptorRegistry r;
    return r;
}

void DecryptorRegistry::Register(std::unique_ptr<Decryptor> d) {
    std::lock_guard<std::mutex> lock(mtx_);
    // 同 id 后注册者覆盖
    for (auto& e : list_)
        if (std::strcmp(e->Id(), d->Id()) == 0) {
            e = std::move(d);
            return;
        }
    list_.push_back(std::move(d));
}

const Decryptor* DecryptorRegistry::ById(const std::string& id) const {
    for (const auto& d : list_)
        if (id == d->Id()) return d.get();
    return nullptr;
}

const Decryptor* DecryptorRegistry::ByExtension(const std::wstring& ext) const {
    for (const auto& d : list_)
        for (const auto& e : d->Extensions())
            if (e == ext) return d.get();
    return nullptr;
}

const Decryptor* DecryptorRegistry::Detect(const std::wstring& path) const {
    platform::ReadOnlyFile f;
    uint8_t h[16] = {};
    if (f.Open(path, true)) {
        size_t n = f.ReadAt(0, h, 16);
        for (const auto& d : list_)
            if (d->Probe(h, n)) return d.get();
    }
    return ByExtension(ExtLower(path));
}

void DecryptorRegistry::SetToolsDir(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(mtx_);
    tools_dir_ = dir;
}
std::wstring DecryptorRegistry::ToolsDir() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return tools_dir_;
}
void DecryptorRegistry::SetKgmMaskPath(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(mtx_);
    kgm_mask_ = path;
}
std::wstring DecryptorRegistry::KgmMaskPath() const {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!kgm_mask_.empty()) return kgm_mask_;
    return JoinPath(JoinPath(tools_dir_, L"kgm"), L"kgm.mask");
}

// ===========================================================================
bool DecryptIndex::Load(const std::wstring& file) {
    std::lock_guard<std::mutex> lock(mtx_);
    map_.clear();
    byPrint_.clear();
    platform::ReadOnlyFile f;
    if (!f.Open(file, false) || f.Size() > (256u << 20)) return false;
    std::string data(size_t(f.Size()), '\0');
    if (f.ReadAt(0, data.data(), data.size()) != data.size()) return false;
    std::istringstream in(data);
    std::string line;
    // KPDI1:大小 \t 修改时间 \t 源 \t 产物;KPDI2 在末尾多一列十六进制指纹
    if (!std::getline(in, line) || (line != "KPDI1" && line != "KPDI2")) return false;
    const bool v2 = line == "KPDI2";
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string size, mtime, src, product, print;
        if (!std::getline(ss, size, '\t') || !std::getline(ss, mtime, '\t') || !std::getline(ss, src, '\t') ||
            !std::getline(ss, product, v2 ? '\t' : '\n'))
            continue;
        if (v2) std::getline(ss, print);
        Entry e;
        e.size = std::strtoull(size.c_str(), nullptr, 10);
        e.mtime = std::strtoull(mtime.c_str(), nullptr, 10);
        e.product = AudioTagParser::Utf8ToWide(product);
        e.fingerprint = std::strtoull(print.c_str(), nullptr, 16);
        if (e.fingerprint) byPrint_[e.fingerprint] = e.product;
        map_[AudioTagParser::Utf8ToWide(src)] = std::move(e);
    }
    return true;
}

bool DecryptIndex::Save(const std::wstring& file) const {
    std::string data = "KPDI2\n";
    {
        std::lock_guard<std::mutex> lock(mtx_);
        char hex[17];
        for (const auto& kv : map_) {
            std::snprintf(hex, sizeof hex, "%llx", static_cast<unsigned long long>(kv.second.fingerprint));
            data += std::to_string(kv.second.size) + '\t' + std::to_string(kv.second.mtime) + '\t' +
                    AudioTagParser::WideToUtf8(kv.first) + '\t' + AudioTagParser::WideToUtf8(kv.second.product) + '\t' +
                    hex + '\n';
        }
    }
    std::wstring tmp = file + L".tmp";
    {
        platform::WritableFile f;
        if (!f.Create(tmp) || !f.Write(data.data(), data.size())) return false;
    }
    return platform::AtomicReplaceFile(tmp, file);
}

std::wstring DecryptIndex::Find(const std::wstring& src, uint64_t size, uint64_t mtime) const {
    std::wstring product;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = map_.find(src);
        if (it == map_.end() || it->second.size != size || it->second.mtime != mtime) return L"";
        product = it->second.product;
    }
    return platform::PathExists(product) ? product : L"";
}

std::wstring DecryptIndex::FindByFingerprint(uint64_t fingerprint) const {
    if (!fingerprint) return L"";
    std::wstring product;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = byPrint_.find(fingerprint);
        if (it == byPrint_.end()) return L"";
        product = it->second;
    }
    return platform::PathExists(product) ? product : L"";
}

std::wstring DecryptIndex::Lookup(const std::wstring& src, uint64_t size, uint64_t mtime, bool* changed) {
    if (changed) *changed = false;
    std::wstring product = Find(src, size, mtime);
    if (!product.empty()) return product;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (byPrint_.empty()) return L""; // 没有任何指纹记录 (旧索引):不必读文件
    }
    const uint64_t fp = Fingerprint(src, size);
    product = FindByFingerprint(fp);
    if (!product.empty()) {
        Put(src, size, mtime, product, fp);
        if (changed) *changed = true;
    }
    return product;
}

void DecryptIndex::Put(const std::wstring& src, uint64_t size, uint64_t mtime, const std::wstring& product, uint64_t fingerprint) {
    std::lock_guard<std::mutex> lock(mtx_);
    map_[src] = Entry{size, mtime, product, fingerprint};
    if (fingerprint) byPrint_[fingerprint] = product;
}

uint64_t DecryptIndex::Fingerprint(const std::wstring& src, uint64_t size) {
    platform::ReadOnlyFile f;
    if (!f.Open(src, false)) return 0;
    uint8_t buf[16384];
    size_t n = f.ReadAt(0, buf, sizeof buf);
    if (n == 0) return 0;
    uint64_t h = 1469598103934665603ULL ^ size;
    for (size_t i = 0; i < n; ++i) {
        h ^= buf[i];
        h *= 1099511628211ULL;
    }
    return h ? h : 1;
}

size_t DecryptIndex::Size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return map_.size();
}
