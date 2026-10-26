// OGG 封面写入:Vorbis Comment 里加 METADATA_BLOCK_PICTURE(值是 FLAC PICTURE 块体的 base64)
//
// 为什么麻烦:OGG 是"页"结构,每个包按 255 字节切成 segment,一页最多 255 个 segment。
// comment 头加上几十万字节的图片后会撑爆原来那一页,必须重新分页;
// 而插入新页会让后面所有音频页的序号变化,每页的 CRC32 也得重算。
#include "cover_writer.hpp"

#include "cover_fetcher.hpp"
#include "platform_io.hpp"

#include <algorithm>
#include <cstring>

namespace cover_core {

namespace {

constexpr size_t kMaxOggSegment = 255;
constexpr size_t kMaxPageSegments = 255;

// ---- OGG 用的 CRC32 (多项式 0x04C11DB7,与 zlib 的不同:不反转) ----
struct OggCrc {
    uint32_t t[256];
    OggCrc() {
        for (int i = 0; i < 256; ++i) {
            uint32_t c = static_cast<uint32_t>(i) << 24;
            for (int k = 0; k < 8; ++k)
                c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
            t[i] = c;
        }
    }
    uint32_t Calc(const uint8_t* p, size_t n) const {
        uint32_t c = 0;
        for (size_t i = 0; i < n; ++i) c = (c << 8) ^ t[((c >> 24) ^ p[i]) & 0xFF];
        return c;
    }
};
const OggCrc& Crc() {
    static const OggCrc c;
    return c;
}

struct Page {
    uint8_t type = 0;
    int64_t granule = 0;
    uint32_t serial = 0;
    uint32_t seq = 0;
    std::vector<uint8_t> segs;
    std::vector<uint8_t> body;
};

uint32_t Le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
int64_t Le64s(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | p[i];
    return static_cast<int64_t>(v);
}
void PutLe32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}
void PutLe64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 0; i < 8; ++i) v.push_back(static_cast<uint8_t>(x >> (8 * i)));
}

// 解析整个文件的页面
bool ParsePages(const std::vector<uint8_t>& d, std::vector<Page>& out) {
    size_t pos = 0;
    while (pos + 27 <= d.size()) {
        if (std::memcmp(d.data() + pos, "OggS", 4) != 0) return false;
        Page p;
        p.type = d[pos + 5];
        p.granule = Le64s(d.data() + pos + 6);
        p.serial = Le32(d.data() + pos + 14);
        p.seq = Le32(d.data() + pos + 18);
        const uint8_t nseg = d[pos + 26];
        if (pos + 27 + nseg > d.size()) return false;
        p.segs.assign(d.begin() + pos + 27, d.begin() + pos + 27 + nseg);
        size_t blen = 0;
        for (uint8_t s : p.segs) blen += s;
        const size_t body = pos + 27 + nseg;
        if (body + blen > d.size()) return false;
        p.body.assign(d.begin() + body, d.begin() + body + blen);
        out.push_back(std::move(p));
        pos = body + blen;
    }
    return !out.empty();
}

// 序列化一页(自动算 CRC)
void EmitPage(std::vector<uint8_t>& out, const Page& p) {
    std::vector<uint8_t> hdr;
    hdr.insert(hdr.end(), {'O', 'g', 'g', 'S'});
    hdr.push_back(0);          // version
    hdr.push_back(p.type);
    PutLe64(hdr, static_cast<uint64_t>(p.granule));
    PutLe32(hdr, p.serial);
    PutLe32(hdr, p.seq);
    PutLe32(hdr, 0);           // CRC 占位
    hdr.push_back(static_cast<uint8_t>(p.segs.size()));
    hdr.insert(hdr.end(), p.segs.begin(), p.segs.end());

    std::vector<uint8_t> whole = hdr;
    whole.insert(whole.end(), p.body.begin(), p.body.end());
    const uint32_t crc = Crc().Calc(whole.data(), whole.size());
    for (int i = 0; i < 4; ++i) whole[22 + i] = static_cast<uint8_t>(crc >> (8 * i));
    out.insert(out.end(), whole.begin(), whole.end());
}

// 把一串包写成页。做法:先把所有包展平成一串 segment(记录它属于哪个包、包内偏移),
// 再按每页 255 个 segment 切;页面边界若落在包中间,下一页打 continued 标志。
// 返回写入的页数(供后面重排音频页序号用)。
struct Seg {
    uint8_t val;
    size_t pkt;   // 包索引
    size_t off;   // 包内字节偏移
};

uint32_t PackPackets(std::vector<uint8_t>& out, const std::vector<std::vector<uint8_t>>& packets, uint8_t firstType,
                     int64_t firstGranule, uint32_t serial, uint32_t startSeq) {
    std::vector<Seg> segs;
    for (size_t pi = 0; pi < packets.size(); ++pi) {
        const size_t n = packets[pi].size();
        size_t i = 0;
        while (n - i >= kMaxOggSegment) {           // 满 255 的段
            segs.push_back({static_cast<uint8_t>(kMaxOggSegment), pi, i});
            i += kMaxOggSegment;
        }
        segs.push_back({static_cast<uint8_t>(n - i), pi, i}); // 收尾段(可能是 0)
    }

    uint32_t seq = startSeq;
    uint32_t pages = 0;
    size_t si = 0;
    while (si < segs.size()) {
        Page page;
        // 上一页最后一段是满 255,说明同一个包还没结束 → 本页是延续页
        const bool continued = (si > 0 && segs[si - 1].val == kMaxOggSegment);
        page.type = (si == 0) ? firstType : (continued ? 1 : 0);
        page.granule = (si == 0) ? firstGranule : -1;  // 非首页不含包结尾
        page.serial = serial;
        page.seq = seq++;

        const size_t cnt = std::min<size_t>(kMaxPageSegments, segs.size() - si);
        for (size_t k = 0; k < cnt; ++k) {
            const Seg& sg = segs[si + k];
            page.segs.push_back(sg.val);
            if (sg.val) page.body.insert(page.body.end(), packets[sg.pkt].begin() + sg.off,
                                          packets[sg.pkt].begin() + sg.off + sg.val);
        }
        si += cnt;
        EmitPage(out, page);
        ++pages;
    }
    return pages;
}

// base64(标准字母表,带 = 填充)
std::string Base64(const std::vector<uint8_t>& in) {
    static const char* kA = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8) | in[i + 2];
        out.push_back(kA[(v >> 18) & 63]);
        out.push_back(kA[(v >> 12) & 63]);
        out.push_back(kA[(v >> 6) & 63]);
        out.push_back(kA[v & 63]);
        i += 3;
    }
    const size_t rest = in.size() - i;
    if (rest == 1) {
        const uint32_t v = static_cast<uint32_t>(in[i]) << 16;
        out.push_back(kA[(v >> 18) & 63]);
        out.push_back(kA[(v >> 12) & 63]);
        out += "==";
    } else if (rest == 2) {
        const uint32_t v = (static_cast<uint32_t>(in[i]) << 16) | (static_cast<uint32_t>(in[i + 1]) << 8);
        out.push_back(kA[(v >> 18) & 63]);
        out.push_back(kA[(v >> 12) & 63]);
        out.push_back(kA[(v >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

// FLAC PICTURE 块体(与 FLAC 里的格式完全一致,只是整块做 base64)
std::vector<uint8_t> PictureBlockBody(const std::string& image, const std::string& mime) {
    std::vector<uint8_t> b;
    auto be32 = [&b](uint32_t v) {
        b.push_back(static_cast<uint8_t>(v >> 24));
        b.push_back(static_cast<uint8_t>(v >> 16));
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v));
    };
    be32(3); // Cover (front)
    be32(static_cast<uint32_t>(mime.size()));
    b.insert(b.end(), mime.begin(), mime.end());
    be32(0); // 空描述
    be32(0); // 宽 (未知)
    be32(0); // 高
    be32(24);
    be32(0);
    be32(static_cast<uint32_t>(image.size()));
    b.insert(b.end(), image.begin(), image.end());
    return b;
}

bool Slurp(const std::wstring& path, std::vector<uint8_t>& out) {
    platform::ReadOnlyFile f;
    if (!f.Open(path, false)) return false;
    out.resize(static_cast<size_t>(f.Size()));
    if (out.empty()) return true;
    return f.ReadAt(0, out.data(), out.size()) == out.size();
}

} // namespace

bool EmbedOgg(const std::wstring& path, const std::string& image, const std::string& mime, std::string* error) {
    std::vector<uint8_t> d;
    if (!Slurp(path, d)) {
        if (error) *error = "无法读取文件";
        return false;
    }
    std::vector<Page> pages;
    if (!ParsePages(d, pages)) {
        if (error) *error = "OGG 页结构损坏";
        return false;
    }
    if (pages.size() < 2) {
        if (error) *error = "OGG 头部不全";
        return false;
    }
    // page0 必须是 Vorbis identification,page1 是 comment + setup
    if (pages[0].body.size() < 7 || pages[0].body[0] != 1 || std::memcmp(pages[0].body.data() + 1, "vorbis", 6) != 0) {
        if (error) *error = "不是 Vorbis(Opus/Speex 暂不支持)";
        return false;
    }
    if (pages[1].body.size() < 7 || pages[1].body[0] != 3 || std::memcmp(pages[1].body.data() + 1, "vorbis", 6) != 0) {
        if (error) *error = "comment 头不是 Vorbis comment";
        return false;
    }

    // 把 page1 里的包拆开(comment + setup)
    std::vector<std::vector<uint8_t>> pkts;
    {
        std::vector<uint8_t> cur;
        size_t off = 0;
        for (uint8_t s : pages[1].segs) {
            if (off + s > pages[1].body.size()) break;
            cur.insert(cur.end(), pages[1].body.begin() + off, pages[1].body.begin() + off + s);
            off += s;
            if (s < kMaxOggSegment) {
                pkts.push_back(cur);
                cur.clear();
            }
        }
        if (!cur.empty()) pkts.push_back(cur);
    }
    if (pkts.empty()) {
        if (error) *error = "comment 包解析失败";
        return false;
    }

    // 重建 comment 包:保留原有字段,替换/追加 METADATA_BLOCK_PICTURE
    const std::vector<uint8_t>& old = pkts[0];
    std::vector<uint8_t> vendor;
    std::vector<std::vector<uint8_t>> fields;
    {
        if (old.size() < 11) {
            if (error) *error = "comment 包过短";
            return false;
        }
        const uint32_t vlen = Le32(old.data() + 7);
        if (7 + 4 + vlen + 4 > old.size()) {
            if (error) *error = "comment vendor 长度越界";
            return false;
        }
        vendor.assign(old.begin() + 11, old.begin() + 11 + vlen);
        size_t o2 = 11 + vlen;
        const uint32_t cnt = Le32(old.data() + o2);
        o2 += 4;
        for (uint32_t i = 0; i < cnt && o2 + 4 <= old.size(); ++i) {
            const uint32_t fl = Le32(old.data() + o2);
            o2 += 4;
            if (o2 + fl > old.size()) break;
            std::vector<uint8_t> kv(old.begin() + o2, old.begin() + o2 + fl);
            o2 += fl;
            // 丢掉旧的封面字段,后面统一加新的
            const std::string key(reinterpret_cast<const char*>(kv.data()), std::min<size_t>(kv.size(), 22));
            if (key.compare(0, 22, "METADATA_BLOCK_PICTURE") == 0) continue;
            fields.push_back(std::move(kv));
        }
    }
    const std::string b64 = Base64(PictureBlockBody(image, mime));
    {
        std::string kv = "METADATA_BLOCK_PICTURE=" + b64;
        fields.push_back(std::vector<uint8_t>(kv.begin(), kv.end()));
    }

    std::vector<uint8_t> newComment;
    newComment.insert(newComment.end(), {'\x03', 'v', 'o', 'r', 'b', 'i', 's'});
    PutLe32(newComment, static_cast<uint32_t>(vendor.size()));
    newComment.insert(newComment.end(), vendor.begin(), vendor.end());
    PutLe32(newComment, static_cast<uint32_t>(fields.size()));
    for (const auto& f : fields) {
        PutLe32(newComment, static_cast<uint32_t>(f.size()));
        newComment.insert(newComment.end(), f.begin(), f.end());
    }
    // Vorbis 要求 comment 包以 1 位结束标记收尾
    newComment.push_back(1);
    pkts[0] = std::move(newComment);

    // 输出:page0 原样;然后重排 [comment, setup...];其余音频页跟着改序号
    std::vector<uint8_t> out;
    EmitPage(out, pages[0]);
    const uint32_t newHeaderPages =
        PackPackets(out, pkts, pages[1].type, pages[1].granule, pages[1].serial, pages[1].seq);

    // 音频页:原 seq 从 pages[1].seq+1 开始,新 seq 从 pages[1].seq+newHeaderPages 开始
    const uint32_t oldNext = pages[1].seq + 1;
    const uint32_t newNext = pages[1].seq + newHeaderPages;
    const int64_t delta = static_cast<int64_t>(newNext) - static_cast<int64_t>(oldNext);
    for (size_t i = 2; i < pages.size(); ++i) {
        Page p = pages[i];
        p.seq = static_cast<uint32_t>(static_cast<int64_t>(p.seq) + delta);
        EmitPage(out, p);
    }

    const std::wstring tmp = path + L".cover.tmp";
    {
        platform::WritableFile w;
        if (!w.Create(tmp)) {
            if (error) *error = "无法创建临时文件";
            return false;
        }
        if (!w.Write(out.data(), out.size())) {
            w.Close();
            platform::RemoveFile(tmp);
            if (error) *error = "写入失败";
            return false;
        }
        w.Close();
    }
    if (!platform::AtomicReplaceFile(tmp, path)) {
        platform::RemoveFile(tmp);
        if (error) *error = "替换失败";
        return false;
    }
    return true;
}

} // namespace cover_core
