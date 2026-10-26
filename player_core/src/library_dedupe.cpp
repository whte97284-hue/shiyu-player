#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../include/library_dedupe.hpp"
#include "../include/audio_tag_parser.hpp"
#include "../include/platform_io.hpp"
#include "../include/song_cleaner.hpp"
#include "../include/xxhash64.hpp"
#include "../include/file_sampling.hpp"

#include <algorithm>
#include <atomic>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace dedupe {

namespace {

#ifdef _WIN32
constexpr wchar_t kSep = L'\\';
#else
constexpr wchar_t kSep = L'/';
#endif

// ---------------------------------------------------------------- 路径工具
fs::path FsPath(const std::wstring& p) {
#ifdef _WIN32
    return fs::path(platform::LongPath(p));
#else
    return fs::path(platform::NativePath(p));
#endif
}

std::wstring FromFs(const fs::path& p) {
#ifdef _WIN32
    std::wstring s = p.wstring();
    if (s.compare(0, 8, L"\\\\?\\UNC\\") == 0) return L"\\\\" + s.substr(8);
    if (s.compare(0, 4, L"\\\\?\\") == 0) return s.substr(4);
    return s;
#else
    return AudioTagParser::Utf8ToWide(p.string());
#endif
}

bool IsSep(wchar_t c) { return c == L'\\' || c == L'/'; }

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring Key(std::wstring p) {
    for (auto& c : p)
        if (IsSep(c)) c = kSep;
    while (p.size() > 1 && p.back() == kSep) p.pop_back();
#ifdef _WIN32
    p = Lower(p);
#endif
    return p;
}

bool IsUnder(const std::wstring& pathKey, const std::wstring& rootKey) {
    if (rootKey.empty()) return false;
    if (pathKey == rootKey) return true;
    std::wstring r = rootKey;
    if (r.back() != kSep) r += kSep;
    return pathKey.compare(0, r.size(), r) == 0;
}

std::wstring ParentOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    if (s == std::wstring::npos) return L"";
    if (s == 0) return p.substr(0, 1);
    if (s == 2 && p[1] == L':') return p.substr(0, 3);
    return p.substr(0, s);
}

std::wstring NameOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

void SplitName(const std::wstring& name, std::wstring& stem, std::wstring& ext) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        stem = name;
        ext.clear();
    } else {
        stem = name.substr(0, dot);
        ext = name.substr(dot);
    }
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    return IsSep(dir.back()) ? dir + name : dir + kSep + name;
}

bool Exists(const std::wstring& p) {
    std::error_code ec;
    return fs::exists(FsPath(p), ec);
}

bool SameFileObject(const std::wstring& a, const std::wstring& b) {
    std::error_code ec;
    bool r = fs::equivalent(FsPath(a), FsPath(b), ec);
    return r && !ec;
}

// 文件身份 (卷 + 文件编号):判断两个路径是不是同一个文件 (硬链接)。
// fs::equivalent 在 Windows 上每次比较都要打开两个文件,查重时按需取一次缓存起来
struct FileId {
    uint64_t volume = 0, index = 0;
    bool ok = false;
    bool operator==(const FileId& o) const { return ok && o.ok && volume == o.volume && index == o.index; }
};

FileId GetFileId(const std::wstring& p) {
    FileId id;
#ifdef _WIN32
    HANDLE h = CreateFileW(platform::LongPath(p).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return id;
    BY_HANDLE_FILE_INFORMATION fi;
    if (GetFileInformationByHandle(h, &fi)) {
        id.volume = fi.dwVolumeSerialNumber;
        id.index = (static_cast<uint64_t>(fi.nFileIndexHigh) << 32) | fi.nFileIndexLow;
        id.ok = true;
    }
    CloseHandle(h);
#else
    struct stat st;
    if (::stat(platform::NativePath(p).c_str(), &st) == 0) {
        id.volume = static_cast<uint64_t>(st.st_dev);
        id.index = static_cast<uint64_t>(st.st_ino);
        id.ok = true;
    }
#endif
    return id;
}

bool Stat(const std::wstring& p, uint64_t& size, int64_t& mtime) {
    uint64_t s = 0, m = 0;
    if (!platform::GetFileInfo(p, s, m)) return false;
    size = s;
    mtime = static_cast<int64_t>(m);
    return true;
}

std::wstring UniqueFree(const std::wstring& path) {
    if (!Exists(path)) return path;
    std::wstring stem, ext;
    SplitName(NameOf(path), stem, ext);
    const std::wstring dir = ParentOf(path);
    for (int n = 2; n < 100000; ++n) {
        std::wstring c = Join(dir, stem + L" (" + std::to_wstring(n) + L")" + ext);
        if (!Exists(c)) return c;
    }
    return path + L".dup";
}

// ---------------------------------------------------------------- 哈希与比对
std::vector<uint8_t>& ReadBuffer() {
    thread_local std::vector<uint8_t> buf(1 << 20);
    return buf;
}

bool HashOpen(const platform::ReadOnlyFile& f, uint64_t begin, uint64_t end, kp::Xxh64& h, const std::atomic<bool>* cancel,
              const std::function<void(uint64_t)>& onBytes) {
    if (end > f.Size()) return false;
    std::vector<uint8_t>& buf = ReadBuffer();
    for (uint64_t off = begin; off < end;) {
        if (cancel && cancel->load()) return false;
        size_t want = static_cast<size_t>(std::min<uint64_t>(buf.size(), end - off));
        size_t n = f.ReadAt(off, buf.data(), want);
        if (n == 0) return false;
        h.Add(buf.data(), n);
        off += n;
        if (onBytes) onBytes(n);
    }
    return true;
}

// 快速指纹:范围的头尾各 64KB (只打开一次文件)。范围小于 128KB 时就是整段
bool QuickHash(const std::wstring& path, uint64_t begin, uint64_t end, uint64_t& out) {
    platform::ReadOnlyFile f;
    if (!f.Open(path, true) || end > f.Size() || end < begin) return false;
    const uint64_t q = 64 << 10;
    kp::Xxh64 h(0x9E3779B97F4A7C15ull);
    if (end - begin <= 2 * q) {
        if (!HashOpen(f, begin, end, h, nullptr, nullptr)) return false;
    } else if (!HashOpen(f, begin, begin + q, h, nullptr, nullptr) || !HashOpen(f, end - q, end, h, nullptr, nullptr)) {
        return false;
    }
    out = h.Digest();
    return true;
}

// 抽样指纹:头尾之间再均匀取 5 段 64KB (加上 QuickHash 的头尾共 7 段,约 448KB)。
// 范围不大于 7 段时直接哈希整段。查找阶段用它代替整文件哈希:同样大小、7 处内容都一样的不同文件
// 在音乐库里几乎不可能出现;永久删除 / 换硬链接前还会逐字节确认
constexpr int kInteriorSamples = 5;
bool SampleHash(const std::wstring& path, uint64_t begin, uint64_t end, uint64_t& out, bool& whole) {
    platform::ReadOnlyFile f;
    if (!f.Open(path, true) || end > f.Size() || end < begin) return false;
    const uint64_t q = 64 << 10, len = end - begin;
    kp::Xxh64 h(0xC2B2AE3D27D4EB4Full);
    whole = len <= (kInteriorSamples + 2) * q;
    if (whole) {
        if (!HashOpen(f, begin, end, h, nullptr, nullptr)) return false;
    } else {
        for (int j = 1; j <= kInteriorSamples; ++j) {
            const uint64_t off = begin + (len - q) * j / (kInteriorSamples + 1);
            if (!HashOpen(f, off, off + q, h, nullptr, nullptr)) return false;
        }
    }
    out = h.Digest();
    return true;
}

using kp::IoThreads;
using kp::ParallelFor;

bool SameRange(const std::wstring& a, uint64_t ab, const std::wstring& b, uint64_t bb, uint64_t len) {
    platform::ReadOnlyFile fa, fb;
    if (!fa.Open(a, false) || !fb.Open(b, false)) return false;
    if (ab + len > fa.Size() || bb + len > fb.Size()) return false;
    // 每次各读 4MB 再比:机械盘上两个文件之间来回寻道的次数是 1MB 时的四分之一
    constexpr size_t kChunk = 4 << 20;
    std::vector<uint8_t>& x = kp::ScratchBuffer(0, kChunk);
    std::vector<uint8_t>& y = kp::ScratchBuffer(1, kChunk);
    for (uint64_t off = 0; off < len;) {
        size_t want = static_cast<size_t>(std::min<uint64_t>(kChunk, len - off));
        size_t na = fa.ReadAt(ab + off, x.data(), want);
        size_t nb = fb.ReadAt(bb + off, y.data(), want);
        if (na != want || nb != want || std::memcmp(x.data(), y.data(), want) != 0) return false;
        off += want;
    }
    return true;
}

bool SameContent(const std::wstring& a, const std::wstring& b) {
    uint64_t sa = 0, sb = 0;
    int64_t m = 0;
    if (!Stat(a, sa, m) || !Stat(b, sb, m) || sa != sb) return false;
    return SameRange(a, 0, b, 0, sa);
}

// MP3:去掉 ID3v2 (头) / APEv2 与 ID3v1 (尾) 之后的音频帧范围
bool Mp3Payload(const std::wstring& path, uint64_t& begin, uint64_t& end) {
    platform::ReadOnlyFile f;
    if (!f.Open(path, true)) return false;
    const uint64_t size = f.Size();
    begin = 0;
    end = size;
    uint8_t h[10];
    while (begin + 10 <= end && f.ReadAt(begin, h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3') {
        uint64_t len = (uint64_t(h[6] & 0x7F) << 21) | (uint64_t(h[7] & 0x7F) << 14) | (uint64_t(h[8] & 0x7F) << 7) | (h[9] & 0x7F);
        begin += 10 + len + ((h[5] & 0x10) ? 10 : 0);
    }
    for (int round = 0; round < 3; ++round) {
        uint8_t t[32];
        if (end >= begin + 128 && f.ReadAt(end - 128, t, 3) == 3 && t[0] == 'T' && t[1] == 'A' && t[2] == 'G') {
            end -= 128;
            continue;
        }
        if (end >= begin + 32 && f.ReadAt(end - 32, t, 32) == 32 && std::memcmp(t, "APETAGEX", 8) == 0) {
            uint32_t len = t[12] | (t[13] << 8) | (t[14] << 16) | (uint32_t(t[15]) << 24);
            uint32_t flags = t[20] | (t[21] << 8) | (t[22] << 16) | (uint32_t(t[23]) << 24);
            uint64_t total = uint64_t(len) + ((flags & 0x80000000u) ? 32 : 0);
            if (total > end - begin) break;
            end -= total;
            continue;
        }
        break;
    }
    return end > begin + 1024; // 太短的不当作音频
}

// FLAC:STREAMINFO 里记录的原始 PCM 的 MD5 (编码器写入,改标签不变)
bool FlacPcmMd5(const std::wstring& path, std::string& md5) {
    platform::ReadOnlyFile f;
    if (!f.Open(path, true)) return false;
    uint64_t off = 0;
    uint8_t h[10];
    if (f.ReadAt(0, h, 10) == 10 && h[0] == 'I' && h[1] == 'D' && h[2] == '3')
        off = 10 + ((uint64_t(h[6] & 0x7F) << 21) | (uint64_t(h[7] & 0x7F) << 14) | (uint64_t(h[8] & 0x7F) << 7) | (h[9] & 0x7F));
    uint8_t b[42];
    if (f.ReadAt(off, b, 42) != 42 || std::memcmp(b, "fLaC", 4) != 0 || (b[4] & 0x7F) != 0) return false;
    bool nonzero = false;
    for (int i = 26; i < 42; ++i) nonzero = nonzero || b[i] != 0;
    if (!nonzero) return false; // 编码器没写 MD5
    md5.assign(reinterpret_cast<const char*>(b + 26), 16);
    return true;
}

// ---------------------------------------------------------------- 规范化
std::wstring HalfWidth(std::wstring s) {
    for (auto& c : s) {
        if (c >= 0xFF01 && c <= 0xFF5E) c = static_cast<wchar_t>(c - 0xFEE0);
        else if (c == 0x3000) c = L' ';
    }
    return s;
}

bool IsWordChar(wchar_t c) {
    return std::iswalnum(static_cast<wint_t>(c)) || (c >= 0x3040 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) ||
           (c >= 0xF900 && c <= 0xFAFF);
}

bool IsPlaceholderArtist(const std::wstring& a) {
    std::wstring l = Lower(SongCleaner::Trim(a));
    return l.empty() || l == L"未知艺术家" || l == L"未知歌手" || l == L"未知" || l == L"unknown" ||
           l == L"unknown artist" || l == L"<unknown>" || l == L"various artists" || l == L"群星";
}

std::wstring Squash(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : HalfWidth(s))
        if (IsWordChar(c)) out += static_cast<wchar_t>(towlower(c));
    return out;
}

// 去掉 (feat. xxx) / (ft. xxx) 这类括号:同一首歌是否标了合唱者不影响查重
std::wstring DropFeat(const std::wstring& t) {
    std::wstring s = t;
    for (;;) {
        std::wstring l = Lower(s);
        size_t hit = std::wstring::npos;
        for (const wchar_t* kw : {L"feat.", L"feat ", L"ft.", L"featuring"}) {
            size_t p = l.find(kw);
            if (p != std::wstring::npos && (hit == std::wstring::npos || p < hit)) hit = p;
        }
        if (hit == std::wstring::npos) return s;
        size_t open = s.find_last_of(L"([【（", hit);
        if (open != std::wstring::npos && open < hit) {
            size_t close = s.find_first_of(L")]】）", hit);
            s.erase(open, close == std::wstring::npos ? std::wstring::npos : close - open + 1);
        } else {
            s.erase(hit); // "歌名 feat. xxx" 没有括号:截掉后半
        }
    }
}

std::wstring FormatLabel(const std::wstring& path) {
    std::wstring stem, ext;
    SplitName(NameOf(path), stem, ext);
    std::wstring e = ext.empty() ? L"" : ext.substr(1);
    for (auto& c : e) c = static_cast<wchar_t>(towupper(c));
    return e;
}

bool LooksLikeCopy(const std::wstring& path) {
    std::wstring stem, ext;
    SplitName(NameOf(path), stem, ext);
    std::wstring l = Lower(stem);
    if (l.find(L"副本") != std::wstring::npos || l.find(L"copy") != std::wstring::npos ||
        l.find(L"复制") != std::wstring::npos)
        return true;
    // 结尾是 " (2)" / "(1)" / "_1" / "-2"
    size_t i = l.size();
    if (i > 0 && l[i - 1] == L')') {
        size_t j = i - 1;
        while (j > 0 && std::iswdigit(l[j - 1])) --j;
        if (j < i - 1 && j > 0 && l[j - 1] == L'(') return true;
    }
    size_t j = i;
    while (j > 0 && std::iswdigit(l[j - 1])) --j;
    return j < i && i - j <= 2 && j > 1 && (l[j - 1] == L'_' || l[j - 1] == L'-') && l[j - 2] != L' ';
}

int TagScore(const DupTrack& t) {
    return (!t.title.empty()) + (!IsPlaceholderArtist(t.artist)) + (!t.album.empty() && t.album != L"未知专辑") +
           (t.track_no > 0) + (t.has_cover ? 1 : 0);
}

bool InRoots(const std::wstring& path, const std::vector<std::wstring>& roots) {
    const std::wstring k = Key(path);
    for (const auto& r : roots)
        if (IsUnder(k, Key(r))) return true;
    return false;
}

using Rank = std::tuple<int, int, int, int, int, int, int, int64_t, int64_t>;

Rank QualityRank(const DupTrack& t, const DupMember& m, const std::vector<std::wstring>& roots) {
    return Rank(t.lossless ? 1 : 0, t.bits_per_sample, std::min(t.sample_rate, 768000), t.bitrate_kbps, TagScore(t),
                InRoots(t.path, roots) ? 1 : 0, LooksLikeCopy(t.path) ? 0 : 1, -static_cast<int64_t>(t.path.size()), -m.mtime);
}

Rank PathRank(const DupTrack& t, const DupMember& m, const std::vector<std::wstring>& roots) {
    return Rank(InRoots(t.path, roots) ? 1 : 0, LooksLikeCopy(t.path) ? 0 : 1, TagScore(t), t.lossless ? 1 : 0, 0, 0, 0,
                -static_cast<int64_t>(t.path.size()), -m.mtime);
}

std::wstring MemberNote(const DupTrack& t, const std::vector<std::wstring>& roots) {
    std::wstring n = FormatLabel(t.path);
    if (t.lossless) {
        n += L" 无损";
        if (t.bits_per_sample > 0 && t.sample_rate > 0)
            n += L" " + std::to_wstring(t.bits_per_sample) + L"bit/" +
                 (t.sample_rate % 1000 == 0 ? std::to_wstring(t.sample_rate / 1000)
                                            : std::to_wstring(t.sample_rate / 1000) + L"." + std::to_wstring(t.sample_rate % 1000 / 100)) +
                 L"kHz";
    } else if (t.bitrate_kbps > 0) {
        n += L" " + std::to_wstring(t.bitrate_kbps) + L"kbps";
    }
    if (InRoots(t.path, roots)) n += L" · 在首选目录";
    if (LooksLikeCopy(t.path)) n += L" · 文件名像副本";
    return n;
}

} // namespace

// ============================================================================
std::wstring NormalizeTitle(const std::wstring& title) {
    std::wstring t = SongCleaner::StripTrackPrefix(SongCleaner::CleanTitle(HalfWidth(title)));
    return Squash(DropFeat(t));
}

std::wstring NormalizeArtist(const std::wstring& artist) {
    if (IsPlaceholderArtist(artist)) return L"";
    std::wstring a = DropFeat(HalfWidth(artist));
    std::vector<std::wstring> parts;
    std::wstring cur;
    auto flush = [&]() {
        std::wstring s = Squash(cur);
        if (!s.empty()) parts.push_back(s);
        cur.clear();
    };
    for (size_t i = 0; i < a.size(); ++i) {
        wchar_t c = a[i];
        if (c == L'、' || c == L'/' || c == L'&' || c == L',' || c == L'，' || c == L';' || c == L'；' || c == L'×' ||
            c == L'+') {
            flush();
        } else if (c == L' ' && i + 2 < a.size() && (a.compare(i, 3, L" x ") == 0 || a.compare(i, 3, L" X ") == 0)) {
            flush();
            i += 1;
        } else {
            cur += c;
        }
    }
    flush();
    std::sort(parts.begin(), parts.end());
    parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
    std::wstring out;
    for (const auto& p : parts) out += (out.empty() ? L"" : L"|") + p;
    return out;
}

void UpdateReclaim(std::vector<DupGroup>& groups) {
    for (auto& g : groups) {
        g.reclaim_bytes = 0;
        for (const auto& m : g.members)
            if (!m.keep) g.reclaim_bytes += m.size;
    }
}

void AutoSelect(std::vector<DupGroup>& groups, const std::vector<DupTrack>& tracks, KeepRule rule,
                const std::vector<std::wstring>& roots) {
    for (auto& g : groups) {
        if (g.members.empty()) continue;
        if (g.kind == MatchKind::Versions) {
            for (auto& m : g.members) m.keep = true;
            continue;
        }
        KeepRule r = rule;
        if (r == KeepRule::Auto)
            r = (g.kind == MatchKind::Identical || g.kind == MatchKind::SameAudio) ? KeepRule::PreferredPath : KeepRule::BestQuality;
        size_t best = 0;
        for (size_t i = 1; i < g.members.size(); ++i) {
            const DupMember& a = g.members[i];
            const DupMember& b = g.members[best];
            const DupTrack& ta = tracks[a.track];
            const DupTrack& tb = tracks[b.track];
            bool better = false;
            switch (r) {
                case KeepRule::BestQuality: better = QualityRank(ta, a, roots) > QualityRank(tb, b, roots); break;
                case KeepRule::PreferredPath: better = PathRank(ta, a, roots) > PathRank(tb, b, roots); break;
                case KeepRule::Oldest: better = a.mtime < b.mtime; break;
                case KeepRule::Newest: better = a.mtime > b.mtime; break;
                case KeepRule::Largest: better = a.size > b.size; break;
                case KeepRule::Smallest: better = a.size < b.size; break;
                default: break;
            }
            if (better) best = i;
        }
        g.suggested = best;
        for (size_t i = 0; i < g.members.size(); ++i) g.members[i].keep = i == best;
    }
    UpdateReclaim(groups);
}

FindResult FindDuplicates(const std::vector<DupTrack>& tracks, const FindOptions& opt,
                          std::function<void(const FindProgress&)> onProgress, const std::atomic<bool>* cancel) {
    FindResult res;
    auto cancelled = [&]() { return cancel && cancel->load(); };
    FindProgress prog;
    // 并行阶段的计数 (工作线程累加,调用线程汇报)
    std::atomic<size_t> doneCount{0};
    auto tick = [&]() {
        prog.done = doneCount.load();
        if (onProgress) onProgress(prog);
    };

    // ---- 0. 去掉重复路径、CUE 分轨;大小优先用扫描时记下的 (不逐个查询文件),修改时间只给最终分组里的成员取
    struct Info {
        uint64_t size = 0;
        int64_t mtime = 0;
        bool ok = false;
        bool statted = false;
    };
    std::vector<Info> info(tracks.size());
    std::vector<size_t> live;
    std::unordered_set<std::wstring> seen;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const DupTrack& t = tracks[i];
        if (t.cue_track || t.path.empty() || !seen.insert(Key(t.path)).second) continue;
        if (t.size > 0) {
            info[i].size = t.size;
        } else {
            if (!Stat(t.path, info[i].size, info[i].mtime) || info[i].size == 0) continue;
            info[i].statted = true;
        }
        info[i].ok = true;
        live.push_back(i);
    }
    std::vector<char> claimed(tracks.size(), 0); // 已被更严格的分组处理掉 (非保留成员)

    // 文件身份:只给进入候选的文件取,缓存
    std::unordered_map<size_t, FileId> ids;
    auto sameFile = [&](size_t a, size_t b) {
        if (info[a].size != info[b].size) return false;
        auto get = [&](size_t i) -> const FileId& {
            auto it = ids.find(i);
            if (it == ids.end()) it = ids.emplace(i, GetFileId(tracks[i].path)).first;
            return it->second;
        };
        return get(a) == get(b);
    };

    auto makeMember = [&](size_t i) {
        if (!info[i].statted) {
            uint64_t size = 0;
            if (Stat(tracks[i].path, size, info[i].mtime)) info[i].size = size;
            info[i].statted = true;
        }
        DupMember m;
        m.track = i;
        m.size = info[i].size;
        m.mtime = info[i].mtime;
        m.note = MemberNote(tracks[i], opt.preferred_roots);
        return m;
    };
    auto addGroup = [&](MatchKind kind, const std::vector<size_t>& all) {
        // 硬链接 (同一个文件的多个路径) 只算一份:它们不占额外空间,删哪个都腾不出空间
        std::vector<size_t> idx;
        for (size_t i : all) {
            bool linked = false;
            for (size_t j : idx)
                if (sameFile(i, j)) linked = true;
            if (!linked) idx.push_back(i);
        }
        if (idx.size() < 2) return false;
        DupGroup g;
        g.kind = kind;
        g.needs_review = kind == MatchKind::SameSong || kind == MatchKind::SameName || kind == MatchKind::Versions;
        for (size_t i : idx) g.members.push_back(makeMember(i));
        std::vector<DupGroup> one{g};
        AutoSelect(one, tracks, KeepRule::Auto, opt.preferred_roots);
        if (kind != MatchKind::Versions)
            for (const auto& m : one[0].members)
                if (!m.keep) claimed[m.track] = 1;
        res.groups.push_back(std::move(one[0]));
        return true;
    };

    // 范围 [b, e) 的内容指纹:先比头尾 64KB,相同的再比中间 5 处抽样 (都是小块随机读,不再整文件读取:
    // 曲库里真重复很多时,整文件哈希要把几十 GB 读一遍)。分组键里带上范围长度。
    // 指纹判定的「完全相同」在永久删除 / 换硬链接前会逐字节再确认。
    // 进度:每个候选文件算 2 步 (头尾、抽样),头尾不同的直接算完
    struct Range {
        size_t i;
        uint64_t b, e;
    };
    auto groupByContent = [&](std::vector<Range> cand) {
        std::vector<std::vector<size_t>> out;
        if (cand.size() < 2) {
            doneCount += 2 * cand.size();
            return out;
        }
        std::vector<uint64_t> quick(cand.size(), 0);
        std::vector<char> qok(cand.size(), 0);
        ParallelFor(cand.size(), IoThreads(), [&](size_t k) {
            qok[k] = QuickHash(tracks[cand[k].i].path, cand[k].b, cand[k].e, quick[k]);
            ++doneCount;
        }, tick, cancel);
        std::map<std::pair<uint64_t, uint64_t>, std::vector<size_t>> byQuick;
        for (size_t k = 0; k < cand.size(); ++k)
            if (qok[k]) byQuick[{cand[k].e - cand[k].b, quick[k]}].push_back(k);
        std::vector<size_t> needSample;
        for (auto& q : byQuick)
            if (q.second.size() >= 2) needSample.insert(needSample.end(), q.second.begin(), q.second.end());
        doneCount += cand.size() - needSample.size();
        std::vector<uint64_t> sample(cand.size(), 0);
        std::vector<char> sok(cand.size(), 0);
        ParallelFor(needSample.size(), IoThreads(), [&](size_t n) {
            const size_t k = needSample[n];
            const Range& r = cand[k];
            if (r.e - r.b <= 2 * (64 << 10)) {
                sok[k] = 1; // 头尾指纹已经覆盖整段
            } else {
                bool whole = false;
                sok[k] = SampleHash(tracks[r.i].path, r.b, r.e, sample[k], whole);
            }
            ++doneCount;
        }, tick, cancel);
        std::map<std::tuple<uint64_t, uint64_t, uint64_t>, std::vector<size_t>> bySample;
        for (size_t k : needSample) {
            if (!sok[k]) continue;
            ++res.files_hashed;
            bySample[{cand[k].e - cand[k].b, quick[k], sample[k]}].push_back(cand[k].i);
        }
        for (auto& f : bySample)
            if (f.second.size() >= 2) out.push_back(std::move(f.second));
        return out;
    };

    // ---- 1. Identical:大小相同 → 头尾指纹 → 中间抽样指纹
    if (opt.identical) {
        std::map<uint64_t, std::vector<size_t>> bySize;
        for (size_t i : live) bySize[info[i].size].push_back(i);
        std::vector<Range> cand;
        for (auto& kv : bySize)
            if (kv.second.size() >= 2)
                for (size_t i : kv.second) {
                    cand.push_back({i, 0, kv.first});
                    prog.total += 2;
                }
        tick();
        // 不同大小的文件哈希值碰巧相同也不会混到一组:分组键里再带上大小
        std::map<uint64_t, std::vector<Range>> sized;
        for (const Range& r : cand) sized[r.e].push_back(r);
        std::vector<Range> flat;
        for (auto& kv : sized) flat.insert(flat.end(), kv.second.begin(), kv.second.end());
        for (auto& grp : groupByContent(flat)) {
            if (cancelled()) break;
            std::map<uint64_t, std::vector<size_t>> bySz;
            for (size_t i : grp) bySz[info[i].size].push_back(i);
            for (auto& kv : bySz)
                if (kv.second.size() >= 2) addGroup(MatchKind::Identical, kv.second);
        }
    }

    // ---- 2. SameAudio:FLAC PCM MD5;MP3 去标签后的音频帧
    if (opt.same_audio && !cancelled()) {
        // FLAC:同样的 PCM 采样数必然相同,时长 (由采样数算出) 也就相同;只读时长撞车的那些文件的头
        std::vector<size_t> flacs, mp3s;
        std::map<std::pair<uint32_t, int>, std::vector<size_t>> flacByDur;
        for (size_t i : live) {
            if (claimed[i]) continue;
            std::wstring ext = Lower(FormatLabel(tracks[i].path));
            if (ext == L"flac") flacByDur[{tracks[i].duration_ms, tracks[i].sample_rate}].push_back(i);
            else if (ext == L"mp3") mp3s.push_back(i);
        }
        for (auto& kv : flacByDur)
            if (kv.second.size() >= 2 || kv.first.first == 0) flacs.insert(flacs.end(), kv.second.begin(), kv.second.end());
        prog.total += flacs.size() + mp3s.size();
        tick();

        std::vector<std::string> md5(flacs.size());
        std::vector<char> mok(flacs.size(), 0);
        ParallelFor(flacs.size(), IoThreads(), [&](size_t k) {
            mok[k] = FlacPcmMd5(tracks[flacs[k]].path, md5[k]);
            ++doneCount;
        }, tick, cancel);
        std::map<std::string, std::vector<size_t>> byMd5;
        for (size_t k = 0; k < flacs.size(); ++k)
            if (mok[k]) byMd5[md5[k]].push_back(flacs[k]);
        for (auto& kv : byMd5) {
            if (kv.second.size() < 2) continue;
            // 硬链接只算一份
            std::vector<size_t> distinct;
            for (size_t i : kv.second) {
                bool linked = false;
                for (size_t j : distinct) linked = linked || sameFile(i, j);
                if (!linked) distinct.push_back(i);
            }
            if (distinct.size() >= 2) addGroup(MatchKind::SameAudio, distinct);
        }

        // MP3:读出去掉标签后的音频范围 (并行),范围长度相同的再比内容
        std::vector<Range> ranges(mp3s.size());
        std::vector<char> rok(mp3s.size(), 0);
        ParallelFor(mp3s.size(), IoThreads(), [&](size_t k) {
            ranges[k].i = mp3s[k];
            rok[k] = Mp3Payload(tracks[mp3s[k]].path, ranges[k].b, ranges[k].e);
            ++doneCount;
        }, tick, cancel);
        std::map<uint64_t, std::vector<Range>> byPayloadSize;
        for (size_t k = 0; k < mp3s.size(); ++k)
            if (rok[k]) byPayloadSize[ranges[k].e - ranges[k].b].push_back(ranges[k]);
        std::vector<Range> cand;
        for (auto& kv : byPayloadSize)
            if (kv.second.size() >= 2) cand.insert(cand.end(), kv.second.begin(), kv.second.end());
        prog.total += 2 * cand.size();
        tick();
        for (auto& grp : groupByContent(cand)) {
            if (cancelled()) break;
            std::vector<size_t> distinct;
            for (size_t i : grp) {
                bool linked = false;
                for (size_t j : distinct) linked = linked || sameFile(i, j);
                if (!linked) distinct.push_back(i);
            }
            if (distinct.size() >= 2) addGroup(MatchKind::SameAudio, distinct);
        }
    }
    tick();

    // ---- 3. SameSong / SameName / Versions:规范化的歌手 + 歌名,再按时长聚类
    if ((opt.same_song || opt.same_name || opt.versions) && !cancelled()) {
        std::map<std::wstring, std::vector<size_t>> byKey;
        std::map<std::wstring, bool> keyIsName;
        for (size_t i : live) {
            if (claimed[i] || tracks[i].duration_ms == 0) continue;
            const DupTrack& t = tracks[i];
            std::wstring artist = NormalizeArtist(t.artist);
            std::wstring stem, ext;
            SplitName(NameOf(t.path), stem, ext);
            std::wstring title = NormalizeTitle(t.title.empty() ? stem : t.title);
            if (title.empty()) continue;
            std::wstring key;
            bool byName = artist.empty();
            if (byName) {
                if (!opt.same_name) continue;
                key = L"N\x1f" + title;
            } else {
                key = L"S\x1f" + artist + L"\x1f" + title;
            }
            byKey[key].push_back(i);
            keyIsName[key] = byName;
        }
        for (auto& kv : byKey) {
            if (kv.second.size() < 2) continue;
            auto& v = kv.second;
            std::sort(v.begin(), v.end(), [&](size_t a, size_t b) { return tracks[a].duration_ms < tracks[b].duration_ms; });
            std::vector<std::vector<size_t>> clusters;
            for (size_t i : v) {
                if (clusters.empty() || tracks[i].duration_ms - tracks[clusters.back().front()].duration_ms > opt.duration_tolerance_ms)
                    clusters.push_back({});
                clusters.back().push_back(i);
            }
            const bool byName = keyIsName[kv.first];
            std::vector<size_t> reps;
            for (auto& c : clusters) {
                if (c.size() >= 2 && (byName ? opt.same_name : opt.same_song)) {
                    if (addGroup(byName ? MatchKind::SameName : MatchKind::SameSong, c)) {
                        const DupGroup& g = res.groups.back();
                        reps.push_back(g.members[g.suggested].track);
                    } else {
                        reps.push_back(c[0]);
                    }
                } else {
                    // 不按同一首处理时,时长相近的也作为各自的版本列出
                    for (size_t i : c) reps.push_back(i);
                    if (c.size() >= 2) reps.resize(reps.size() - c.size() + 1);
                }
            }
            if (opt.versions && !byName && reps.size() >= 2 && clusters.size() >= 2) addGroup(MatchKind::Versions, reps);
        }
    }

    res.cancelled = cancelled();
    // 能腾出空间多的排前面;同一类型放一起 (严格的在前)
    std::stable_sort(res.groups.begin(), res.groups.end(), [](const DupGroup& a, const DupGroup& b) {
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.reclaim_bytes > b.reclaim_bytes;
    });
    return res;
}

// ============================================================================
namespace {

struct Journal {
    FILE* f = nullptr;
    bool Open(const std::wstring& path, bool append) {
        if (path.empty()) return true;
        platform::CreateDirectories(ParentOf(path));
#ifdef _WIN32
        f = _wfopen(platform::LongPath(path).c_str(), append ? L"ab" : L"wb");
#else
        f = std::fopen(platform::NativePath(path).c_str(), append ? "ab" : "wb");
#endif
        return f != nullptr;
    }
    void Line(const char* kind, const std::wstring& a = L"", const std::wstring& b = L"", const std::wstring& c = L"") {
        if (!f) return;
        std::string s = kind;
        for (const std::wstring* x : {&a, &b, &c})
            if (!x->empty()) s += "\t" + AudioTagParser::WideToUtf8(*x);
        s += "\n";
        std::fwrite(s.data(), 1, s.size(), f);
        std::fflush(f);
    }
    ~Journal() {
        if (f) std::fclose(f);
    }
};

std::vector<std::vector<std::wstring>> ReadJournal(const std::wstring& path) {
    std::vector<std::vector<std::wstring>> rows;
    platform::ReadOnlyFile f;
    if (!f.Open(path, false)) return rows;
    std::string data(static_cast<size_t>(f.Size()), '\0');
    if (f.ReadAt(0, data.data(), data.size()) != data.size()) return rows;
    std::istringstream in(data);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::wstring> cols;
        size_t start = 0;
        for (;;) {
            size_t tab = line.find('\t', start);
            cols.push_back(AudioTagParser::Utf8ToWide(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start)));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        rows.push_back(std::move(cols));
    }
    return rows;
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty() || Exists(dir)) return true;
    std::error_code ec;
    fs::create_directories(FsPath(dir), ec);
    return Exists(dir);
}

// 移动文件:先改名,跨盘时复制 + 校验 + 删源
bool MoveOne(const std::wstring& from, const std::wstring& to, std::wstring& err) {
    if (!EnsureDir(ParentOf(to))) {
        err = L"无法创建目录 " + ParentOf(to);
        return false;
    }
    std::error_code ec;
    fs::rename(FsPath(from), FsPath(to), ec);
    if (!ec) return true;
    const std::wstring part = to + L".part";
    fs::copy_file(FsPath(from), FsPath(part), fs::copy_options::overwrite_existing, ec);
    if (ec || !SameContent(from, part)) {
        fs::remove(FsPath(part), ec);
        err = L"复制失败 (磁盘已满或设备断开?)";
        return false;
    }
    auto t = fs::last_write_time(FsPath(from), ec);
    if (!ec) fs::last_write_time(FsPath(part), t, ec);
    ec.clear();
    fs::rename(FsPath(part), FsPath(to), ec);
    if (ec) {
        fs::remove(FsPath(part), ec);
        err = L"无法完成改名";
        return false;
    }
    if (!fs::remove(FsPath(from), ec)) {
        fs::permissions(FsPath(from), fs::perms::owner_write, fs::perm_options::add, ec);
        if (!fs::remove(FsPath(from), ec)) {
            fs::remove(FsPath(to), ec);
            err = L"原文件删不掉 (可能正被占用)";
            return false;
        }
    }
    return true;
}

bool DeleteOne(const std::wstring& p) {
    std::error_code ec;
    if (fs::remove(FsPath(p), ec)) return true;
    fs::permissions(FsPath(p), fs::perms::owner_write, fs::perm_options::add, ec);
    return fs::remove(FsPath(p), ec);
}

// 隔离文件夹内保留原来的完整路径:F:\Music\a.mp3 → <root>\F\Music\a.mp3
std::wstring QuarantinePath(const std::wstring& root, const std::wstring& path) {
    std::wstring rel;
    for (wchar_t c : path) {
        if (c == L':') continue;
        rel += IsSep(c) ? kSep : c;
    }
    while (!rel.empty() && rel.front() == kSep) rel.erase(0, 1);
    return UniqueFree(Join(root, rel));
}

#ifndef _WIN32
std::string UrlEncodePath(const std::string& p) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : p) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::wstring HomeTrash() {
#ifdef __APPLE__
    const char* home = std::getenv("HOME");
    return home ? AudioTagParser::Utf8ToWide(std::string(home) + "/.Trash") : L"";
#else
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x) return AudioTagParser::Utf8ToWide(std::string(x) + "/Trash");
    const char* home = std::getenv("HOME");
    return home ? AudioTagParser::Utf8ToWide(std::string(home) + "/.local/share/Trash") : L"";
#endif
}

// 文件所在挂载点 (st_dev 变化处)
std::wstring MountRoot(const std::wstring& path) {
    struct stat st;
    if (::stat(platform::NativePath(path).c_str(), &st) != 0) return L"";
    dev_t dev = st.st_dev;
    std::wstring cur = ParentOf(path);
    for (;;) {
        std::wstring parent = ParentOf(cur);
        struct stat ps;
        if (parent.empty() || parent == cur || ::stat(platform::NativePath(parent).c_str(), &ps) != 0 || ps.st_dev != dev)
            return cur;
        cur = parent;
    }
}

// freedesktop 废纸篓:文件放进 files/,同名 .trashinfo 写原路径与删除时间,文件管理器可以还原
bool TrashInto(const std::wstring& trash, const std::wstring& path, std::wstring& movedTo, std::wstring& infoPath) {
    const std::wstring files = Join(trash, L"files"), infos = Join(trash, L"info");
    if (!EnsureDir(files) || !EnsureDir(infos)) return false;
    std::wstring name = NameOf(path), stem, ext;
    SplitName(name, stem, ext);
    for (int n = 1; n < 100000; ++n) {
        std::wstring cand = n == 1 ? name : stem + L"." + std::to_wstring(n) + ext;
        std::wstring info = Join(infos, cand + L".trashinfo");
        // O_EXCL 创建 .trashinfo 先占住名字 (规范要求的做法,避免与别的程序冲突)
        FILE* f = std::fopen(platform::NativePath(info).c_str(), "wx");
        if (!f) continue;
        if (Exists(Join(files, cand))) {
            std::fclose(f);
            std::remove(platform::NativePath(info).c_str());
            continue;
        }
        char date[32];
        std::time_t now = std::time(nullptr);
        std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", std::localtime(&now));
        std::string body = "[Trash Info]\nPath=" + UrlEncodePath(platform::NativePath(path)) + "\nDeletionDate=" + date + "\n";
        std::fwrite(body.data(), 1, body.size(), f);
        std::fclose(f);
        if (::rename(platform::NativePath(path).c_str(), platform::NativePath(Join(files, cand)).c_str()) != 0) {
            std::remove(platform::NativePath(info).c_str());
            return false;
        }
        movedTo = Join(files, cand);
        infoPath = info;
        return true;
    }
    return false;
}
#endif

// 移到回收站 / 废纸篓。movedTo 为空表示由系统管理 (Windows 回收站)
bool Recycle(const std::wstring& path, std::wstring& movedTo, std::wstring& infoPath, std::wstring& err) {
#ifdef _WIN32
    if (path.size() >= MAX_PATH) {
        err = L"路径太长,系统回收站不支持";
        return false;
    }
    std::wstring from = path;
    for (auto& c : from)
        if (c == L'/') c = L'\\';
    from.push_back(L'\0'); // 双 0 结尾
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    int r = SHFileOperationW(&op);
    if (r != 0 || op.fAnyOperationsAborted || Exists(path)) {
        err = L"无法移到回收站 (该盘可能没有回收站,或文件正被占用)";
        return false;
    }
    movedTo.clear();
    infoPath.clear();
    return true;
#else
    std::wstring home = HomeTrash();
    if (!home.empty() && TrashInto(home, path, movedTo, infoPath)) return true;
#ifndef __APPLE__
    // 不在家目录所在的盘 (U 盘、移动硬盘):用该盘根目录下的 .Trash-<uid>
    std::wstring root = MountRoot(path);
    if (!root.empty() && TrashInto(Join(root, L".Trash-" + std::to_wstring(::getuid())), path, movedTo, infoPath)) return true;
#endif
    err = L"无法移到废纸篓";
    return false;
#endif
}

bool IsLyricExt(const std::wstring& lowerExt) {
    return lowerExt == L".lrc" || lowerExt == L".krc" || lowerExt == L".qrc" || lowerExt == L".trc";
}

// 与歌曲同名的歌词文件 (「歌名.lrc」「歌名.zh.lrc」)
std::vector<std::wstring> LyricsOf(const std::wstring& audio) {
    std::vector<std::wstring> out;
    std::wstring stem, ext;
    SplitName(NameOf(audio), stem, ext);
    const std::wstring ls = Lower(stem);
    std::error_code ec;
    for (fs::directory_iterator di(FsPath(ParentOf(audio)), ec), end; !ec && di != end; di.increment(ec)) {
        std::error_code fec;
        if (!di->is_regular_file(fec)) continue;
        std::wstring name = FromFs(di->path().filename());
        std::wstring ln = Lower(name), s, e;
        SplitName(name, s, e);
        if (ln.size() > ls.size() && ln.compare(0, ls.size(), ls) == 0 && ln[ls.size()] == L'.' && IsLyricExt(Lower(e)))
            out.push_back(Join(ParentOf(audio), name));
    }
    return out;
}

bool IsJunkLeftover(const std::wstring& lowerName) {
    return lowerName == L"thumbs.db" || lowerName == L"desktop.ini" || lowerName == L".ds_store";
}

void RemoveEmptyDirs(std::wstring dir, const std::vector<std::wstring>& protectRoots, Journal& journal) {
    std::vector<std::wstring> keys;
    for (const auto& r : protectRoots) keys.push_back(Key(r));
    for (int depth = 0; depth < 64 && !dir.empty(); ++depth) {
        const std::wstring k = Key(dir);
        bool inside = false;
        for (const auto& r : keys) {
            if (k == r) return;
            if (IsUnder(k, r)) inside = true;
        }
        if (!inside) return;
        std::error_code ec;
        std::vector<fs::path> junk;
        bool empty = true;
        for (fs::directory_iterator di(FsPath(dir), ec), end; !ec && di != end; di.increment(ec)) {
            std::error_code fec;
            if (di->is_regular_file(fec) && IsJunkLeftover(Lower(FromFs(di->path().filename())))) junk.push_back(di->path());
            else empty = false;
        }
        if (ec || !empty) return;
        for (const auto& j : junk) fs::remove(j, ec);
        if (!fs::remove(FsPath(dir), ec) || ec) return;
        journal.Line("X", dir);
        dir = ParentOf(dir);
    }
}

} // namespace

ApplyResult ApplySelection(const std::vector<DupGroup>& groups, const std::vector<DupTrack>& tracks, const ApplyOptions& opt,
                           const std::wstring& journalPath, std::function<void(const FindProgress&)> onProgress,
                           const std::atomic<bool>* cancel) {
    ApplyResult res;
    res.journal = journalPath;
    if (opt.action == DupAction::MoveToFolder && opt.quarantine_root.empty()) {
        res.failures.push_back({L"", L"没有指定隔离文件夹"});
        return res;
    }
    Journal journal;
    if (!journal.Open(journalPath, false)) {
        res.failures.push_back({journalPath, L"无法写入日志,未执行"});
        return res;
    }
    journal.Line("KPDUP1");

    struct Task {
        const DupMember* m;
        std::wstring keeper;
        MatchKind kind;
    };
    std::vector<Task> tasks;
    std::unordered_set<std::wstring> queued;
    for (const auto& g : groups) {
        std::vector<const DupMember*> keep, drop;
        for (const auto& m : g.members) (m.keep ? keep : drop).push_back(&m);
        if (drop.empty()) continue;
        if (keep.empty() && !opt.allow_remove_all) {
            for (const auto* m : drop) {
                ++res.skipped;
                res.failures.push_back({tracks[m->track].path, L"这一组一份都没保留,已跳过"});
            }
            continue;
        }
        std::wstring keeper;
        if (!keep.empty()) {
            const DupMember* k = keep[0];
            if (g.suggested < g.members.size() && g.members[g.suggested].keep) k = &g.members[g.suggested];
            keeper = tracks[k->track].path;
        }
        for (const auto* m : drop)
            if (queued.insert(Key(tracks[m->track].path)).second) tasks.push_back({m, keeper, g.kind});
    }

    FindProgress prog;
    prog.total = tasks.size();
    for (const auto& t : tasks) prog.bytes_total += t.m->size;
    std::unordered_map<std::wstring, std::wstring> removedTo; // 被处理的文件 → 保留文件 (用于解析链式保留)
    std::vector<std::wstring> touchedDirs;
    const bool disk = opt.action != DupAction::RemoveFromLibrary;

    for (const auto& task : tasks) {
        if (cancel && cancel->load()) {
            res.cancelled = true;
            break;
        }
        const std::wstring& path = tracks[task.m->track].path;
        prog.current = path;
        auto finish = [&]() {
            ++prog.done;
            prog.bytes_done += task.m->size;
            if (onProgress) onProgress(prog);
        };
        uint64_t size = 0;
        int64_t mtime = 0;
        if (!Stat(path, size, mtime)) {
            ++res.skipped;
            res.failures.push_back({path, L"文件已不在"});
            finish();
            continue;
        }
        if (opt.verify_unchanged && (size != task.m->size || mtime != task.m->mtime)) {
            ++res.skipped;
            res.failures.push_back({path, L"扫描后文件有改动,已跳过"});
            finish();
            continue;
        }
        if (disk && !task.keeper.empty() && (!Exists(task.keeper) || SameFileObject(path, task.keeper))) {
            ++res.skipped;
            res.failures.push_back({path, Exists(task.keeper) ? L"与保留文件是同一个文件" : L"保留的那份已不在,未处理"});
            finish();
            continue;
        }

        // 歌词:保留的那份没有时改名给它,否则跟着被处理
        std::vector<std::wstring> lyricsToDispose;
        if (disk && opt.action != DupAction::HardLinkToKeeper) {
            std::wstring rs, re, ks, ke;
            SplitName(NameOf(path), rs, re);
            SplitName(NameOf(task.keeper), ks, ke);
            // 「歌.mp3」与「歌.flac」在同一目录:歌词本来就同时属于两者,不动
            const bool sameStem = !task.keeper.empty() && Key(Join(ParentOf(path), rs)) == Key(Join(ParentOf(task.keeper), ks));
            if (!sameStem) {
                for (const auto& lyr : LyricsOf(path)) {
                    std::wstring target;
                    if (opt.carry_lyrics && !task.keeper.empty()) {
                        target = Join(ParentOf(task.keeper), ks + NameOf(lyr).substr(rs.size()));
                    }
                    std::wstring err;
                    if (!target.empty() && !Exists(target) && MoveOne(lyr, target, err)) {
                        journal.Line("Y", lyr, target);
                        res.lyric_moves.push_back(target);
                    } else {
                        lyricsToDispose.push_back(lyr);
                    }
                }
            }
        }

        std::wstring err, movedTo, info;
        bool ok = false;
        switch (opt.action) {
            case DupAction::RemoveFromLibrary:
                journal.Line("R", path);
                ok = true;
                break;
            case DupAction::DeletePermanently:
                // 查找时按指纹判定「完全相同」;永久删除无法撤销,删之前逐字节再确认一次
                if (task.kind == MatchKind::Identical && !task.keeper.empty() && !SameContent(path, task.keeper)) {
                    err = L"与保留的那份内容不一致,未删除";
                    break;
                }
                ok = DeleteOne(path);
                if (ok) journal.Line("D", path);
                else err = L"删除失败 (可能正被占用)";
                break;
            case DupAction::MoveToFolder:
                movedTo = QuarantinePath(opt.quarantine_root, path);
                ok = MoveOne(path, movedTo, err);
                if (ok) journal.Line("Q", path, movedTo);
                break;
            case DupAction::RecycleBin:
                ok = Recycle(path, movedTo, info, err);
                if (ok) journal.Line("B", path, movedTo, info);
                break;
            case DupAction::HardLinkToKeeper: {
                if (task.kind != MatchKind::Identical || !SameContent(path, task.keeper)) {
                    err = L"只有内容完全相同的文件才能换成硬链接";
                    break;
                }
                const std::wstring tmp = UniqueFree(path + L".kpdup-link");
                std::error_code ec;
                fs::create_hard_link(FsPath(task.keeper), FsPath(tmp), ec);
                if (ec) {
                    err = L"无法创建硬链接 (不在同一个盘或文件系统不支持)";
                    break;
                }
                fs::rename(FsPath(tmp), FsPath(path), ec);
                if (ec) {
                    fs::remove(FsPath(tmp), ec);
                    err = L"无法替换为硬链接";
                    break;
                }
                journal.Line("L", path, task.keeper);
                ok = true;
                break;
            }
        }
        if (!ok) {
            ++res.failed;
            res.failures.push_back({path, err});
            finish();
            continue;
        }
        for (const auto& lyr : lyricsToDispose) {
            std::wstring e2, mt, inf;
            if (opt.action == DupAction::DeletePermanently) {
                if (DeleteOne(lyr)) journal.Line("D", lyr);
            } else if (opt.action == DupAction::MoveToFolder) {
                mt = QuarantinePath(opt.quarantine_root, lyr);
                if (MoveOne(lyr, mt, e2)) journal.Line("Q", lyr, mt);
            } else if (opt.action == DupAction::RecycleBin) {
                if (Recycle(lyr, mt, inf, e2)) journal.Line("B", lyr, mt, inf);
            }
        }
        ++res.done;
        if (opt.action != DupAction::RemoveFromLibrary) res.freed_bytes += task.m->size;
        res.changes.push_back({path, task.keeper, movedTo});
        removedTo[Key(path)] = task.keeper;
        if (disk && opt.action != DupAction::HardLinkToKeeper) touchedDirs.push_back(ParentOf(path));
        finish();
    }
    // 保留文件本身又在别的组里被处理掉时,顺着找到最终保留的那份
    for (auto& c : res.changes) {
        for (int hop = 0; hop < 16 && !c.keeper.empty(); ++hop) {
            auto it = removedTo.find(Key(c.keeper));
            if (it == removedTo.end() || it->second == c.keeper) break;
            c.keeper = it->second;
        }
    }
    if (!opt.protect_roots.empty()) {
        std::sort(touchedDirs.begin(), touchedDirs.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
        touchedDirs.erase(std::unique(touchedDirs.begin(), touchedDirs.end()), touchedDirs.end());
        for (const auto& d : touchedDirs) RemoveEmptyDirs(d, opt.protect_roots, journal);
    }
    journal.Line(res.cancelled ? "CANCELLED" : "F");
    return res;
}

ApplyResult UndoDedupe(const std::wstring& journalPath) {
    ApplyResult res;
    res.journal = journalPath;
    auto rows = ReadJournal(journalPath);
    if (rows.empty() || rows[0][0] != L"KPDUP1") {
        res.failures.push_back({journalPath, L"没有可撤销的记录"});
        return res;
    }
    for (const auto& r : rows)
        if (r[0] == L"U") {
            res.failures.push_back({journalPath, L"已经撤销过了"});
            return res;
        }
    Journal journal;
    journal.Open(journalPath, true);
    // 先恢复被删掉的空目录 (按删除顺序逆序重建),再逆序还原文件
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const auto& r = *it;
        const std::wstring& kind = r[0];
        if (kind == L"X" && r.size() >= 2) {
            EnsureDir(r[1]);
        } else if ((kind == L"Q" || kind == L"Y" || (kind == L"B" && r.size() >= 3)) && r.size() >= 3) {
            const std::wstring& orig = r[1];
            const std::wstring& now = r[2];
            std::wstring err;
            if (Exists(orig)) {
                ++res.failed;
                res.failures.push_back({orig, L"原位置已有文件,无法还原"});
            } else if (!Exists(now)) {
                ++res.failed;
                res.failures.push_back({now, L"文件已不在 (可能已清空回收站)"});
            } else if (MoveOne(now, orig, err)) {
                ++res.done;
                res.changes.push_back({now, L"", orig});
                if (kind == L"B" && r.size() >= 4) {
                    std::error_code ec;
                    fs::remove(FsPath(r[3]), ec); // 对应的 .trashinfo
                }
            } else {
                ++res.failed;
                res.failures.push_back({now, err});
            }
        } else if (kind == L"B") {
            ++res.skipped;
            res.failures.push_back({r.size() > 1 ? r[1] : L"", L"在系统回收站里,请从回收站还原"});
        } else if (kind == L"L" && r.size() >= 3) {
            // 硬链接 → 独立文件:复制一份再替换
            const std::wstring& path = r[1];
            const std::wstring tmp = UniqueFree(path + L".kpdup-undo");
            std::error_code ec;
            fs::copy_file(FsPath(path), FsPath(tmp), ec);
            if (!ec) {
                auto t = fs::last_write_time(FsPath(path), ec);
                if (!ec) fs::last_write_time(FsPath(tmp), t, ec);
                ec.clear();
                fs::rename(FsPath(tmp), FsPath(path), ec);
            }
            if (ec) {
                fs::remove(FsPath(tmp), ec);
                ++res.failed;
                res.failures.push_back({path, L"无法换回独立文件"});
            } else {
                ++res.done;
            }
        } else if (kind == L"D") {
            ++res.skipped;
            res.failures.push_back({r.size() > 1 ? r[1] : L"", L"已永久删除,无法还原"});
        } else if (kind == L"R") {
            ++res.done; // 只从曲库移除:文件一直在,调用方重新入库即可
            res.changes.push_back({r.size() > 1 ? r[1] : L"", L"", r.size() > 1 ? r[1] : L""});
        }
    }
    journal.Line("U");
    return res;
}

} // namespace dedupe
