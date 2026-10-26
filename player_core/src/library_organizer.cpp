#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../include/library_organizer.hpp"
#include "../include/audio_tag_parser.hpp"
#include "../include/cue_sheet.hpp"
#include "../include/decryptor.hpp"
#include "../include/xxhash64.hpp"
#include "../include/file_sampling.hpp"
#include "../include/platform_io.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace organizer {

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

std::wstring Native(std::wstring p) {
    for (auto& c : p)
        if (IsSep(c)) c = kSep;
    return p;
}

std::wstring StripTrailingSep(std::wstring p) {
    while (p.size() > 1 && IsSep(p.back()) && !(p.size() == 3 && p[1] == L':')) p.pop_back();
    return p;
}

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (IsSep(dir.back())) return dir + name;
    return dir + kSep + name;
}

std::wstring ParentOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    if (s == std::wstring::npos) return L"";
    if (s == 0) return p.substr(0, 1);
    if (s == 2 && p.size() > 1 && p[1] == L':') return p.substr(0, 3);
    return p.substr(0, s);
}

std::wstring NameOf(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

// 主文件名与扩展名 (扩展名含点;以点开头的隐藏文件没有扩展名)
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

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

// 比较用的键:Windows 不分大小写,分隔符统一
std::wstring Key(const std::wstring& p) {
    std::wstring k = StripTrailingSep(Native(p));
#ifdef _WIN32
    k = Lower(k);
#endif
    return k;
}

bool IsUnder(const std::wstring& pathKey, const std::wstring& rootKey) {
    if (rootKey.empty()) return false;
    if (pathKey == rootKey) return true;
    std::wstring r = rootKey;
    if (!IsSep(r.back())) r += kSep;
    return pathKey.compare(0, r.size(), r) == 0;
}

bool Exists(const std::wstring& p) {
    std::error_code ec;
    return fs::exists(FsPath(p), ec);
}

bool SameFileObject(const std::wstring& a, const std::wstring& b) {
    std::error_code ec;
    return fs::equivalent(FsPath(a), FsPath(b), ec) && !ec;
}

uint64_t SizeOf(const std::wstring& p) {
    uint64_t size = 0, mtime = 0;
    platform::GetFileInfo(p, size, mtime);
    return size;
}

// 文件内容的 XXH64 是否等于给定值 (复制后校验目标文件)
bool SameHash(const std::wstring& path, uint64_t expected) {
    platform::ReadOnlyFile f;
    if (!f.Open(path, false)) return false;
    std::vector<uint8_t>& buf = kp::ScratchBuffer(0, 4 << 20);
    kp::Xxh64 h;
    for (uint64_t off = 0; off < f.Size();) {
        size_t n = f.ReadAt(off, buf.data(), 4 << 20);
        if (n == 0) return false;
        h.Add(buf.data(), n);
        off += n;
    }
    return h.Digest() == expected;
}

// 所在卷:用来判断能否直接改名 (同盘) 以及统计每个盘需要多少空间
std::wstring ExistingAncestor(std::wstring p) {
    while (!p.empty() && !Exists(p)) {
        std::wstring parent = ParentOf(p);
        if (parent == p) break;
        p = parent;
    }
    return p;
}

std::wstring VolumeOf(const std::wstring& path) {
    std::wstring p = ExistingAncestor(path);
    if (p.empty()) return L"";
#ifdef _WIN32
    wchar_t vol[MAX_PATH + 1] = {0};
    if (GetVolumePathNameW(p.c_str(), vol, MAX_PATH)) {
        wchar_t guid[64] = {0};
        if (GetVolumeNameForVolumeMountPointW(vol, guid, 64)) return Lower(guid);
        return Lower(vol);
    }
    return Lower(p.substr(0, std::min<size_t>(p.size(), 3)));
#else
    struct stat st;
    if (::stat(platform::NativePath(p).c_str(), &st) != 0) return L"";
    return std::to_wstring(static_cast<unsigned long long>(st.st_dev));
#endif
}

std::wstring VolumeRootOf(const std::wstring& path) {
    std::wstring p = ExistingAncestor(path);
#ifdef _WIN32
    wchar_t vol[MAX_PATH + 1] = {0};
    if (!p.empty() && GetVolumePathNameW(p.c_str(), vol, MAX_PATH)) return vol;
    return p.substr(0, std::min<size_t>(p.size(), 3));
#else
    // 向上找到设备号变化的地方即为挂载点
    std::wstring dev = VolumeOf(p), cur = p;
    for (;;) {
        std::wstring parent = ParentOf(cur);
        if (parent.empty() || parent == cur || VolumeOf(parent) != dev) return cur;
        cur = parent;
    }
#endif
}

uint64_t AvailableBytes(const std::wstring& path) {
    std::error_code ec;
    fs::space_info si = fs::space(FsPath(ExistingAncestor(path)), ec);
    return ec ? 0 : static_cast<uint64_t>(si.available);
}

bool IsAudioExt(const std::wstring& lowerExt) {
    return AudioTagParser::IsSupportedExtension(lowerExt) || IsEncryptedAudioExtension(lowerExt) || lowerExt == L".kgg";
}

bool IsCompanionExt(const std::wstring& e) {
    static const wchar_t* const kExts[] = {L".lrc", L".krc", L".qrc", L".trc", L".cue", L".jpg", L".jpeg",
                                           L".png", L".webp", L".bmp", L".txt", L".nfo", L".log", L".accurip"};
    for (const wchar_t* x : kExts)
        if (e == x) return true;
    return false;
}

bool IsImageExt(const std::wstring& e) {
    return e == L".jpg" || e == L".jpeg" || e == L".png" || e == L".webp" || e == L".bmp" || e == L".gif";
}

bool IsFolderArtName(const std::wstring& lowerStem) {
    static const wchar_t* const kNames[] = {L"cover", L"folder", L"front", L"albumart", L"album", L"albumartsmall"};
    for (const wchar_t* n : kNames)
        if (lowerStem == n) return true;
    return lowerStem.compare(0, 9, L"albumart_") == 0; // Windows Media Player 的 AlbumArt_{...}_Large
}

bool IsJunkLeftover(const std::wstring& lowerName) {
    return lowerName == L"thumbs.db" || lowerName == L"desktop.ini" || lowerName == L".ds_store" ||
           lowerName == L"ehthumbs.db";
}

// 截断到至多 n 个 UTF-16 单元,不把代理对拆开
std::wstring Truncate(const std::wstring& s, size_t n) {
    if (s.size() <= n) return s;
    size_t cut = n;
    if (sizeof(wchar_t) == 2 && cut > 0 && s[cut - 1] >= 0xD800 && s[cut - 1] <= 0xDBFF) --cut;
    std::wstring r = s.substr(0, cut);
    while (!r.empty() && (r.back() == L' ' || r.back() == L'.')) r.pop_back();
    return r;
}

// ---------------------------------------------------------------- 模板
constexpr wchar_t kDirMark = L'\x01'; // 模板里的目录分隔符 (字段值里的 \ / 会被替换掉,不会与之混淆)

std::wstring Sanitize(const std::wstring& v, IllegalCharPolicy pol) {
    std::wstring out;
    out.reserve(v.size());
    for (wchar_t c : v) {
        if (c < 0x20 || c == 0x7F) {
            out += L' ';
            continue;
        }
        const wchar_t* bad = L"\\/:*?\"<>|";
        const wchar_t* full = L"＼／：＊？＂＜＞｜";
        const wchar_t* hit = std::wcschr(bad, c);
        if (hit) out += pol == IllegalCharPolicy::FullWidth ? full[hit - bad] : L'_';
        else out += c;
    }
    return out;
}

std::wstring Pad(int n, int width) {
    std::wstring s = std::to_wstring(n);
    while (static_cast<int>(s.size()) < width) s = L"0" + s;
    return s;
}

struct Value {
    std::wstring text;
    bool truth = false; // 引用的字段有值 / 函数结果非空
};

class PatternEval {
public:
    PatternEval(const std::wstring& pat, const OrganizeTrack* t, const OrganizeOptions* opt)
        : p_(pat), t_(t), opt_(opt) {}

    bool Run(std::wstring& out, std::wstring* err) {
        pos_ = 0;
        Value v = Sequence(L"");
        if (!error_.empty() || pos_ != p_.size()) {
            if (error_.empty()) error_ = L"模板里有多余的 " + std::wstring(1, p_[pos_]);
            if (err) *err = error_;
            return false;
        }
        out = v.text;
        return true;
    }

private:
    // 读到 stops 中的任一字符 (不消费) 或结尾为止
    Value Sequence(const wchar_t* stops) {
        Value acc;
        while (pos_ < p_.size() && error_.empty()) {
            wchar_t c = p_[pos_];
            if (std::wcschr(stops, c) && c != 0) break;
            if (c == L'%') {
                Value f = Field();
                acc.text += f.text;
                acc.truth = acc.truth || f.truth;
            } else if (c == L'[') {
                ++pos_;
                Value inner = Sequence(L"]");
                if (pos_ >= p_.size()) {
                    error_ = L"[ 没有对应的 ]";
                    break;
                }
                ++pos_;
                if (inner.truth) acc.text += inner.text; // 方括号里至少有一个字段有值才显示 (与 foobar2000 一致)
            } else if (c == L'$') {
                Value f = Function();
                acc.text += f.text;
                acc.truth = acc.truth || f.truth;
            } else if (c == L'\'') {
                size_t end = p_.find(L'\'', pos_ + 1);
                if (end == std::wstring::npos) {
                    error_ = L"引号没有闭合";
                    break;
                }
                if (end == pos_ + 1) acc.text += L'\''; // '' 表示单引号本身
                else acc.text += Sanitize(p_.substr(pos_ + 1, end - pos_ - 1), opt_->illegal);
                pos_ = end + 1;
            } else if (IsSep(c)) {
                acc.text += kDirMark;
                ++pos_;
            } else if (c == L']' || c == L')' || c == L',') {
                if (std::wcschr(stops, c)) break;
                error_ = L"多余的 " + std::wstring(1, c);
                break;
            } else {
                acc.text += Sanitize(std::wstring(1, c), opt_->illegal);
                ++pos_;
            }
        }
        return acc;
    }

    Value Field() {
        size_t end = p_.find(L'%', pos_ + 1);
        if (end == std::wstring::npos) {
            error_ = L"% 没有闭合";
            return {};
        }
        std::wstring name = Lower(p_.substr(pos_ + 1, end - pos_ - 1));
        pos_ = end + 1;
        bool known = true;
        std::wstring v = Lookup(name, known);
        if (!known) {
            error_ = L"不认识的字段 %" + name + L"%";
            return {};
        }
        Value r;
        r.text = Sanitize(v, opt_->illegal);
        r.truth = !v.empty();
        return r;
    }

    std::wstring Lookup(const std::wstring& n, bool& known) const {
        static const wchar_t* const kFields[] = {L"artist", L"album artist", L"albumartist", L"album", L"title",
                                                 L"tracknumber", L"discnumber", L"date", L"year", L"genre",
                                                 L"codec", L"format", L"filename", L"ext", L"directoryname"};
        known = false;
        for (const wchar_t* f : kFields)
            if (n == f) known = true;
        if (!known || !t_) return L"x"; // 没有歌曲时只检查语法
        const OrganizeTrack& t = *t_;
        std::wstring stem, ext;
        SplitName(NameOf(t.path), stem, ext);
        if (n == L"artist") return t.artist;
        if (n == L"album artist" || n == L"albumartist") return !t.album_artist.empty() ? t.album_artist : t.artist;
        if (n == L"album") return t.album;
        if (n == L"title") return !t.title.empty() ? t.title : stem; // 没有标题时用文件名 (与 foobar2000 一致)
        if (n == L"tracknumber") return t.track_no > 0 ? Pad(t.track_no, 2) : L"";
        if (n == L"discnumber")
            return t.disc_no > 0 && (t.disc_total > 1 || t.disc_no > 1) ? std::to_wstring(t.disc_no) : L"";
        if (n == L"date") return t.date;
        if (n == L"year") {
            std::wstring y;
            for (wchar_t c : t.date) {
                if (c >= L'0' && c <= L'9') y += c;
                else if (!y.empty()) break;
                if (y.size() == 4) break;
            }
            return y.size() == 4 ? y : L"";
        }
        if (n == L"genre") return t.genre;
        if (n == L"codec") return t.codec;
        if (n == L"format") return t.format;
        if (n == L"filename") return stem;
        if (n == L"ext") return ext.empty() ? L"" : Lower(ext.substr(1));
        return NameOf(ParentOf(t.path)); // directoryname
    }

    Value Function() {
        size_t open = p_.find(L'(', pos_);
        if (open == std::wstring::npos) {
            error_ = L"函数缺少 (";
            return {};
        }
        std::wstring name = Lower(p_.substr(pos_ + 1, open - pos_ - 1));
        pos_ = open + 1;
        std::vector<Value> args;
        for (;;) {
            args.push_back(Sequence(L",)"));
            if (!error_.empty()) return {};
            if (pos_ >= p_.size()) {
                error_ = L"$" + name + L"( 没有闭合";
                return {};
            }
            if (p_[pos_++] == L')') break;
        }
        auto need = [&](size_t lo, size_t hi) {
            if (args.size() < lo || args.size() > hi) error_ = L"$" + name + L" 的参数个数不对";
            return error_.empty();
        };
        auto num = [](const std::wstring& s) {
            int v = 0;
            bool any = false;
            for (wchar_t c : s) {
                if (c < L'0' || c > L'9') break;
                v = v * 10 + (c - L'0');
                any = true;
                if (v > 100000) break;
            }
            return any ? v : 0;
        };
        Value r;
        if (name == L"if2") {
            if (!need(2, 2)) return {};
            r = args[0].truth ? args[0] : args[1];
        } else if (name == L"if3") {
            if (!need(2, 64)) return {};
            r = args.back();
            for (size_t i = 0; i + 1 < args.size(); ++i)
                if (args[i].truth) {
                    r = args[i];
                    break;
                }
        } else if (name == L"num") {
            if (!need(2, 2)) return {};
            r.truth = args[0].truth;
            r.text = args[0].text.empty() ? L"" : Pad(num(args[0].text), num(args[1].text));
        } else if (name == L"left") {
            if (!need(2, 2)) return {};
            r.text = Truncate(args[0].text, static_cast<size_t>(num(args[1].text)));
            r.truth = args[0].truth;
        } else if (name == L"upper" || name == L"lower") {
            if (!need(1, 1)) return {};
            r = args[0];
            for (auto& c : r.text) c = name == L"upper" ? static_cast<wchar_t>(towupper(c)) : static_cast<wchar_t>(towlower(c));
        } else if (name == L"trim") {
            if (!need(1, 1)) return {};
            r = args[0];
            size_t a = r.text.find_first_not_of(L' '), b = r.text.find_last_not_of(L' ');
            r.text = a == std::wstring::npos ? L"" : r.text.substr(a, b - a + 1);
        } else if (name == L"replace") {
            if (!need(3, 64) || args.size() % 2 == 0) {
                if (error_.empty()) error_ = L"$replace 的参数个数不对";
                return {};
            }
            r = args[0];
            for (size_t i = 1; i + 1 < args.size(); i += 2) {
                const std::wstring& a = args[i].text;
                if (a.empty()) continue;
                std::wstring out;
                for (size_t k = 0; k < r.text.size();) {
                    if (r.text.compare(k, a.size(), a) == 0) {
                        out += args[i + 1].text;
                        k += a.size();
                    } else {
                        out += r.text[k++];
                    }
                }
                r.text = out;
            }
        } else {
            error_ = L"不认识的函数 $" + name;
            return {};
        }
        r.truth = r.truth && !r.text.empty();
        return r;
    }

    const std::wstring& p_;
    const OrganizeTrack* t_;
    const OrganizeOptions* opt_;
    size_t pos_ = 0;
    std::wstring error_;
};

bool IsReservedName(const std::wstring& name) {
    std::wstring base = name.substr(0, name.find(L'.'));
    std::wstring u;
    for (wchar_t c : base) u += static_cast<wchar_t>(towupper(c));
    while (!u.empty() && u.back() == L' ') u.pop_back();
    if (u == L"CON" || u == L"PRN" || u == L"AUX" || u == L"NUL" || u == L"CONIN$" || u == L"CONOUT$") return true;
    if (u.size() == 4 && (u.compare(0, 3, L"COM") == 0 || u.compare(0, 3, L"LPT") == 0) && u[3] >= L'0' && u[3] <= L'9')
        return true;
    return false;
}

std::wstring CleanComponent(std::wstring c, const OrganizeOptions& opt, bool* truncated) {
    size_t a = c.find_first_not_of(L' ');
    c = a == std::wstring::npos ? L"" : c.substr(a);
    while (!c.empty() && (c.back() == L' ' || c.back() == L'.')) c.pop_back(); // Windows 不允许以点或空格结尾
    if (c.empty() || c == L"." || c == L"..") c = opt.unknown_value;
    if (opt.max_component && c.size() > opt.max_component) {
        c = Truncate(c, opt.max_component);
        if (truncated) *truncated = true;
    }
    if (IsReservedName(c)) c += L"_";
    return c;
}

// 渲染为若干级名称 (最后一级是主文件名)
bool RenderComponents(const OrganizeTrack& t, const OrganizeOptions& opt, std::vector<std::wstring>& comps,
                      std::wstring* err, bool* truncated) {
    std::wstring raw;
    PatternEval ev(opt.pattern, &t, &opt);
    if (!ev.Run(raw, err)) return false;
    comps.clear();
    std::wstring cur;
    for (wchar_t c : raw) {
        if (c == kDirMark) {
            comps.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    comps.push_back(cur);
    // 模板开头的分隔符 (写成了绝对路径的样子) 产生的空级丢弃;字段为空造成的空级用「未知」
    size_t leading = 0;
    while (leading < opt.pattern.size() && IsSep(opt.pattern[leading])) ++leading;
    std::vector<std::wstring> clean;
    for (size_t i = 0; i < comps.size(); ++i) {
        if (i < leading && i + 1 < comps.size()) continue;
        clean.push_back(CleanComponent(comps[i], opt, truncated));
    }
    comps = clean;
    if (comps.empty()) {
        if (err) *err = L"模板渲染结果为空";
        return false;
    }
    return true;
}

// 把路径压到 max_path 以内:先缩短文件名,再均匀缩短各级目录
bool FitPath(std::wstring& dir, std::vector<std::wstring>& relDirs, const std::wstring& root, std::wstring& stem,
             const std::wstring& ext, size_t maxPath, bool* truncated) {
    if (!maxPath) return true;
    auto build = [&]() {
        std::wstring d = root;
        for (const auto& c : relDirs) d = Join(d, c);
        return d;
    };
    dir = build();
    auto total = [&]() { return Join(dir, stem + ext).size(); };
    if (total() <= maxPath) return true;
    *truncated = true;
    size_t over = total() - maxPath;
    if (stem.size() > 12) {
        size_t cut = std::min(over, stem.size() - 12);
        stem = Truncate(stem, stem.size() - cut);
    }
    for (int round = 0; round < 64 && total() > maxPath; ++round) {
        size_t longest = 0;
        for (size_t i = 1; i < relDirs.size(); ++i)
            if (relDirs[i].size() > relDirs[longest].size()) longest = i;
        if (relDirs.empty() || relDirs[longest].size() <= 8) break;
        relDirs[longest] = Truncate(relDirs[longest], relDirs[longest].size() - std::min<size_t>(total() - maxPath, relDirs[longest].size() - 8));
        dir = build();
    }
    return total() <= maxPath;
}

std::wstring WithSuffix(const std::wstring& dir, const std::wstring& stem, const std::wstring& ext, int n) {
    return Join(dir, n <= 1 ? stem + ext : stem + L" (" + std::to_wstring(n) + L")" + ext);
}

// ---------------------------------------------------------------- 目录内容缓存
// 每个目录只列一次:文件名与大小都从目录列表里拿 (Windows 上一次 FindNextFile 就带回大小),
// 不再对每个源文件、每个候选目标路径单独查询文件系统
struct DirListing {
    std::vector<std::wstring> names;                  // 仅文件
    std::unordered_map<std::wstring, uint64_t> sizes; // Key(文件名) → 大小
};

const DirListing& ListDir(std::unordered_map<std::wstring, DirListing>& cache, const std::wstring& dir) {
    auto it = cache.find(Key(dir));
    if (it != cache.end()) return it->second;
    DirListing l;
    std::error_code ec;
    for (fs::directory_iterator di(FsPath(dir), ec), end; !ec && di != end; di.increment(ec)) {
        std::error_code fec;
        if (!di->is_regular_file(fec)) continue;
        const std::wstring name = FromFs(di->path().filename());
        const uintmax_t size = di->file_size(fec);
        l.names.push_back(name);
        l.sizes[Key(name)] = fec ? 0 : static_cast<uint64_t>(size);
    }
    return cache.emplace(Key(dir), std::move(l)).first->second;
}

// 路径对应的文件是否在 (缓存的) 目录列表里;size 可选带回大小
bool Listed(std::unordered_map<std::wstring, DirListing>& cache, const std::wstring& path, uint64_t* size = nullptr) {
    const DirListing& l = ListDir(cache, ParentOf(path));
    auto it = l.sizes.find(Key(NameOf(path)));
    if (it == l.sizes.end()) return false;
    if (size) *size = it->second;
    return true;
}

// ---------------------------------------------------------------- 日志
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
    void Line(const std::string& kind, const std::wstring& a = L"", const std::wstring& b = L"", const std::wstring& c = L"") {
        if (!f) return;
        std::string s = kind;
        for (const std::wstring* x : {&a, &b, &c})
            if (!x->empty()) s += "\t" + AudioTagParser::WideToUtf8(*x);
        s += "\n";
        std::fwrite(s.data(), 1, s.size(), f);
        std::fflush(f); // 每一步立即落盘:断电后日志与实际状态最多差一个文件
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

// ---------------------------------------------------------------- 文件操作
struct Mover {
    const OrganizeOptions& opt;
    Journal& journal;
    const std::atomic<bool>* cancel;
    std::function<void(uint64_t)> onBytes;
    std::set<std::wstring> createdDirs; // 本次新建的目录 (撤销时清理)

    bool Cancelled() const { return cancel && cancel->load(); }

    bool EnsureDir(const std::wstring& dir, std::wstring& err) {
        if (dir.empty() || Exists(dir)) return true;
        std::vector<std::wstring> missing;
        for (std::wstring d = dir; !d.empty() && !Exists(d); d = ParentOf(d)) {
            missing.push_back(d);
            if (ParentOf(d) == d) break;
        }
        std::error_code ec;
        fs::create_directories(FsPath(dir), ec);
        if (ec || !Exists(dir)) {
            err = L"无法创建目录 " + dir;
            return false;
        }
        for (auto it = missing.rbegin(); it != missing.rend(); ++it) {
            createdDirs.insert(*it);
            journal.Line("K", *it);
        }
        return true;
    }

    // 复制到 to.part → 校验 → 改名为 to,保留修改时间
    bool CopyVerified(const std::wstring& from, const std::wstring& to, std::wstring& err) {
        const std::wstring part = to + L".part";
        platform::ReadOnlyFile in;
        if (!in.Open(from, false)) {
            err = L"无法读取源文件";
            return false;
        }
        bool ok = true;
        kp::Xxh64 srcHash; // 边复制边算源文件指纹,校验时只需再读一遍目标文件
        {
            platform::WritableFile out;
            if (!out.Create(part)) {
                err = L"无法写入目标位置";
                return false;
            }
            std::vector<uint8_t>& buf = kp::ScratchBuffer(0, 4 << 20);
            for (uint64_t off = 0; off < in.Size();) {
                if (Cancelled()) {
                    err = L"已取消";
                    ok = false;
                    break;
                }
                size_t n = in.ReadAt(off, buf.data(), 4 << 20);
                if (n == 0 || !out.Write(buf.data(), n)) {
                    err = L"复制时读写失败 (磁盘已满或设备断开?)";
                    ok = false;
                    break;
                }
                srcHash.Add(buf.data(), n);
                off += n;
                if (onBytes) onBytes(n);
            }
        }
        if (ok && SizeOf(part) != in.Size()) {
            err = L"复制后大小不一致";
            ok = false;
        }
        in.Close();
        if (ok && opt.verify_copy && !SameHash(part, srcHash.Digest())) {
            err = L"复制后校验不一致";
            ok = false;
        }
        std::error_code ec;
        if (ok) {
            auto t = fs::last_write_time(FsPath(from), ec);
            if (!ec) fs::last_write_time(FsPath(part), t, ec);
            ec.clear();
            fs::rename(FsPath(part), FsPath(to), ec);
            if (ec) {
                err = L"无法完成改名";
                ok = false;
            }
        }
        if (!ok) fs::remove(FsPath(part), ec);
        return ok;
    }

    bool RemoveSource(const std::wstring& p) {
        std::error_code ec;
        fs::path fp = FsPath(p);
        if (fs::remove(fp, ec)) return true;
        // 只读文件:先去掉只读属性再删
        fs::permissions(fp, fs::perms::owner_write, fs::perm_options::add, ec);
        ec.clear();
        return fs::remove(fp, ec);
    }

    // 移动或复制一个文件;返回值 0 = 失败,1 = 已移动,2 = 已复制 (含跨盘移动时源文件删不掉)
    int Transfer(const std::wstring& from, const std::wstring& to, bool copyOnly, std::wstring& err, std::wstring& note) {
        if (!EnsureDir(ParentOf(to), err)) return 0;
        if (!copyOnly) {
            std::error_code ec;
            fs::rename(FsPath(from), FsPath(to), ec);
            if (!ec) {
                journal.Line("M", from, to);
                if (onBytes) onBytes(SizeOf(to));
                return 1;
            }
            // 跨盘或其他原因不能直接改名:复制 + 删除
        }
        if (!CopyVerified(from, to, err)) return 0;
        if (copyOnly) {
            journal.Line("C", from, to);
            return 2;
        }
        if (!RemoveSource(from)) {
            journal.Line("C", from, to);
            note = L"已复制到新位置,但原文件删不掉 (可能正被占用),两处都保留";
            return 2;
        }
        journal.Line("M", from, to);
        return 1;
    }
};

std::wstring UniqueFree(const std::wstring& path) {
    std::wstring stem, ext;
    SplitName(NameOf(path), stem, ext);
    const std::wstring dir = ParentOf(path);
    for (int n = 2; n < 10000; ++n) {
        std::wstring c = WithSuffix(dir, stem, ext, n);
        if (!Exists(c)) return c;
    }
    return path + L".dup";
}

std::wstring TempNameFor(const std::wstring& target) {
    const std::wstring dir = ParentOf(target);
    for (int n = 1; n < 100000; ++n) {
        std::wstring c = Join(dir, L".kporg-" + std::to_wstring(n) + L".tmp");
        if (!Exists(c)) return c;
    }
    return target + L".kporg.tmp";
}

void RemoveEmptyDirsUpward(std::wstring dir, const std::vector<std::wstring>& protectKeys, Journal& journal,
                           std::vector<std::wstring>& removed) {
    for (int depth = 0; depth < 64 && !dir.empty(); ++depth) {
        const std::wstring k = Key(dir);
        // 只清理受保护根目录 (曲库根) 之下的目录,不删根目录本身
        bool inside = false;
        for (const auto& r : protectKeys) {
            if (k == r) return;
            if (IsUnder(k, r)) inside = true;
        }
        if (!inside && depth > 0) return;
        std::error_code ec;
        std::vector<fs::path> junk;
        bool empty = true;
        for (fs::directory_iterator di(FsPath(dir), ec), end; !ec && di != end; di.increment(ec)) {
            std::wstring name = Lower(FromFs(di->path().filename()));
            std::error_code fec;
            if (di->is_regular_file(fec) && IsJunkLeftover(name)) junk.push_back(di->path());
            else empty = false;
        }
        if (ec || !empty) return;
        for (const auto& j : junk) fs::remove(j, ec);
        if (!fs::remove(FsPath(dir), ec) || ec) return;
        journal.Line("R", dir);
        removed.push_back(dir);
        if (!inside) return; // 没有给出曲库根时只清理文件原来所在的那一级
        dir = ParentOf(dir);
    }
}

} // namespace

// ============================================================================
bool OrganizePlan::ok() const {
    for (const auto& v : volumes)
        if (!v.enough) return false;
    return true;
}

bool ValidatePattern(const std::wstring& pattern, std::wstring* error) {
    if (pattern.find_first_not_of(L" ") == std::wstring::npos) {
        if (error) *error = L"模板为空";
        return false;
    }
    OrganizeOptions opt;
    std::wstring out;
    PatternEval ev(pattern, nullptr, &opt);
    return ev.Run(out, error);
}

std::wstring FormatPattern(const std::wstring& pattern, const OrganizeTrack& t, const OrganizeOptions& optIn, std::wstring* error) {
    OrganizeOptions opt = optIn;
    opt.pattern = pattern;
    std::vector<std::wstring> comps;
    if (!RenderComponents(t, opt, comps, error, nullptr)) return L"";
    std::wstring out;
    for (const auto& c : comps) out = out.empty() ? c : out + kSep + c;
    return out;
}

OrganizePlan BuildPlan(const std::vector<OrganizeTrack>& tracks, const OrganizeOptions& opt,
                       std::function<void(const OrganizeProgress&)> onProgress, const std::atomic<bool>* cancel) {
    OrganizePlan plan;
    auto cancelled = [&]() { return cancel && cancel->load(); };
    OrganizeProgress prog;
    prog.total = tracks.size();
    const bool copyMode = opt.mode == Mode::Copy;
    const bool inPlace = opt.mode == Mode::RenameInPlace;
    std::wstring patErr;
    if (!ValidatePattern(opt.pattern, &patErr)) {
        plan.warnings.push_back(L"模板有误:" + patErr);
        plan.errors = tracks.size();
        for (size_t i = 0; i < tracks.size(); ++i) {
            OrganizeOp op;
            op.from = tracks[i].path;
            op.track = i;
            op.action = OpAction::Skip;
            op.state = OpState::Error;
            op.note = patErr;
            plan.ops.push_back(op);
        }
        return plan;
    }
    if (!inPlace && opt.target_root.empty()) {
        plan.warnings.push_back(L"没有指定目标文件夹");
        return plan;
    }
    const std::wstring root = StripTrailingSep(Native(opt.target_root));

    std::vector<std::wstring> excludeKeys;
    for (const auto& e : opt.exclude_paths) excludeKeys.push_back(Key(e));
    auto excluded = [&](const std::wstring& k) {
        for (const auto& e : excludeKeys)
            if (IsUnder(k, e)) return true;
        return false;
    };

    std::unordered_map<std::wstring, DirListing> dirs;
    std::unordered_map<std::wstring, size_t> claimed;        // 目标键 → 占用它的操作
    std::unordered_map<std::wstring, size_t> movingSources;  // 会被搬走的源文件键 → 操作 (移动模式)
    std::unordered_set<std::wstring> seenSources;
    std::vector<size_t> audioOps;
    size_t truncatedCount = 0, untagged = 0;

    // ---- 1. 每首歌的目标路径
    for (size_t i = 0; i < tracks.size(); ++i) {
        if (cancelled()) {
            plan.cancelled = true;
            return plan;
        }
        const OrganizeTrack& t = tracks[i];
        if (onProgress && (i & 31) == 0) {
            prog.done = i;
            prog.current = t.path;
            onProgress(prog);
        }
        OrganizeOp op;
        op.kind = OpKind::Audio;
        op.track = i;
        op.from = Native(t.path);
        op.action = copyMode ? OpAction::Copy : OpAction::Move;
        const std::wstring fromKey = Key(op.from);
        if (!seenSources.insert(fromKey).second) continue; // 同一个文件出现两次 (整轨的多首 CUE 分轨)
        uint64_t mtime = 0;
        if (!Listed(dirs, op.from, &op.size) && !platform::GetFileInfo(op.from, op.size, mtime)) {
            op.action = OpAction::Skip;
            op.state = OpState::Error;
            op.note = L"源文件不存在";
            plan.ops.push_back(op);
            continue;
        }
        if (excluded(fromKey)) {
            op.action = OpAction::Skip;
            op.state = OpState::Excluded;
            op.note = L"在排除列表里";
            plan.ops.push_back(op);
            continue;
        }
        std::vector<std::wstring> comps;
        std::wstring err;
        bool truncated = false;
        if (!RenderComponents(t, opt, comps, &err, &truncated)) {
            op.action = OpAction::Skip;
            op.state = OpState::Error;
            op.note = err;
            plan.ops.push_back(op);
            continue;
        }
        if (t.title.empty() && t.artist.empty() && t.album.empty()) ++untagged;
        std::wstring srcStem, ext;
        SplitName(NameOf(op.from), srcStem, ext);
        std::wstring stem = t.cue_image ? srcStem : comps.back();
        std::vector<std::wstring> relDirs(comps.begin(), comps.end() - 1);
        std::wstring dir;
        if (inPlace) {
            dir = ParentOf(op.from);
            relDirs.clear();
            if (!FitPath(dir, relDirs, dir, stem, ext, opt.max_path, &truncated)) {
                op.action = OpAction::Skip;
                op.state = OpState::Error;
                op.note = L"路径过长";
                plan.ops.push_back(op);
                continue;
            }
        } else if (!FitPath(dir, relDirs, root, stem, ext, opt.max_path, &truncated)) {
            op.action = OpAction::Skip;
            op.state = OpState::Error;
            op.note = L"路径过长,缩短后仍超过限制";
            plan.ops.push_back(op);
            continue;
        }
        if (truncated) ++truncatedCount;
        op.to = Join(dir, stem + ext);
        if (!copyMode) movingSources[fromKey] = plan.ops.size();
        audioOps.push_back(plan.ops.size());
        plan.ops.push_back(op);
    }

    // ---- 1b. 内容比对:撞到同一个目标、或目标处已有同大小文件的,先并行算好抽样指纹。
    // 判成重复的只是「不搬 / 不复制」,文件留在原处,计划列表里看得到,所以不必逐字节整读
    // (曲库重复多时整读要读几十 GB,机械盘上要等几十分钟)
    std::unordered_map<std::wstring, uint64_t> prints; // Key(路径) → 指纹 (算不出的不在表里)
    {
        std::vector<std::wstring> need;
        std::unordered_set<std::wstring> needKeys;
        auto want = [&](const std::wstring& p) {
            if (needKeys.insert(Key(p)).second) need.push_back(p);
        };
        std::unordered_map<std::wstring, std::vector<size_t>> byTarget;
        for (size_t idx : audioOps) byTarget[Key(plan.ops[idx].to)].push_back(idx);
        if (opt.duplicates == DuplicatePolicy::Skip) {
            for (auto& kv : byTarget) {
                std::map<uint64_t, std::vector<size_t>> bySize;
                for (size_t idx : kv.second) bySize[plan.ops[idx].size].push_back(idx);
                uint64_t existing = 0;
                const bool exists = Listed(dirs, plan.ops[kv.second[0]].to, &existing);
                for (auto& b : bySize) {
                    const bool hitsExisting = exists && existing == b.first;
                    if (b.second.size() < 2 && !hitsExisting) continue;
                    for (size_t idx : b.second) want(plan.ops[idx].from);
                    if (hitsExisting) want(plan.ops[kv.second[0]].to);
                }
            }
        }
        std::vector<uint64_t> fp(need.size(), 0);
        std::vector<char> ok(need.size(), 0);
        std::atomic<size_t> done{0};
        prog.stage = 1;
        prog.done = 0;
        prog.total = need.size();
        prog.current.clear();
        kp::ParallelFor(need.size(), kp::IoThreads(), [&](size_t k) {
            ok[k] = kp::SampledFingerprint(need[k], fp[k]);
            ++done;
        }, [&]() {
            if (!onProgress) return;
            prog.done = done.load();
            onProgress(prog);
        }, cancel);
        if (cancelled()) {
            plan.cancelled = true;
            return plan;
        }
        for (size_t k = 0; k < need.size(); ++k)
            if (ok[k]) prints[Key(need[k])] = fp[k];
    }
    auto printOf = [&](const std::wstring& p, uint64_t& out) {
        auto it = prints.find(Key(p));
        if (it == prints.end()) {
            if (!kp::SampledFingerprint(p, out)) return false;
            prints.emplace(Key(p), out);
            return true;
        }
        out = it->second;
        return true;
    };
    // 调用方已确认两边大小相同
    auto sameContent = [&](const std::wstring& a, const std::wstring& b) {
        uint64_t x = 0, y = 0;
        return printOf(a, x) && printOf(b, y) && x == y;
    };

    // ---- 2. 冲突处理 (按顺序,先到先得)
    for (size_t idx : audioOps) {
        OrganizeOp& op = plan.ops[idx];
        const std::wstring fromKey = Key(op.from);
        std::wstring toKey = Key(op.to);
        if (op.from == op.to || (toKey == fromKey && SameFileObject(op.from, op.to) && op.from == op.to)) {
            op.action = OpAction::Skip;
            op.state = OpState::Unchanged;
            movingSources.erase(fromKey);
            claimed[toKey] = idx;
            continue;
        }
        if (toKey == fromKey) {
            // 只有大小写不同 (Windows):经临时名两步改名
            if (copyMode) {
                op.action = OpAction::Skip;
                op.state = OpState::Unchanged;
            } else {
                op.via_temp = true;
            }
            claimed[toKey] = idx;
            continue;
        }
        std::wstring stem, ext;
        SplitName(NameOf(op.to), stem, ext);
        const std::wstring dir = ParentOf(op.to);
        for (int n = 1;; ++n) {
            const std::wstring cand = WithSuffix(dir, stem, ext, n);
            const std::wstring ck = Key(cand);
            auto c = claimed.find(ck);
            if (c != claimed.end()) {
                const OrganizeOp& other = plan.ops[c->second];
                if (n == 1 && opt.duplicates == DuplicatePolicy::Skip && other.size == op.size && sameContent(op.from, other.from)) {
                    op.action = OpAction::Skip;
                    op.state = OpState::Duplicate;
                    op.note = L"与 " + other.from + L" 内容相同";
                    movingSources.erase(fromKey);
                    break;
                }
                continue;
            }
            uint64_t candSize = 0;
            if (Listed(dirs, cand, &candSize)) {
                auto mv = movingSources.find(ck);
                if (mv != movingSources.end() && mv->second != idx) {
                    // 目标此刻被另一首要搬走的歌占着:先搬到临时名,最后再改成正式名字
                    op.to = cand;
                    op.via_temp = true;
                    if (n > 1) op.state = OpState::Renamed;
                    claimed[ck] = idx;
                    break;
                }
                if (n == 1 && opt.duplicates == DuplicatePolicy::Skip && candSize == op.size && sameContent(op.from, cand)) {
                    op.action = OpAction::Skip;
                    op.state = OpState::Duplicate;
                    op.to = cand;
                    op.note = L"目标位置已有内容相同的文件";
                    movingSources.erase(fromKey);
                    break;
                }
                continue;
            }
            op.to = cand;
            if (n > 1) {
                op.state = OpState::Renamed;
                op.note = L"目标重名,已追加序号";
            }
            claimed[ck] = idx;
            break;
        }
    }

    // ---- 3. 附属文件与目录封面
    std::map<std::wstring, std::vector<size_t>> bySourceDir; // 源目录键 → 该目录里参与整理的音频操作
    std::map<std::wstring, std::wstring> sourceDirPath;
    const size_t audioCount = plan.ops.size();
    for (size_t idx = 0; idx < audioCount; ++idx) {
        const OrganizeOp& op = plan.ops[idx];
        if (op.kind != OpKind::Audio || op.state == OpState::Error || op.state == OpState::Excluded) continue;
        const std::wstring d = ParentOf(op.from);
        bySourceDir[Key(d)].push_back(idx);
        sourceDirPath[Key(d)] = d;
    }
    auto addCompanion = [&](OpKind kind, const std::wstring& from, const std::wstring& to, OpAction action, size_t track,
                            const std::wstring& note) {
        OrganizeOp c;
        c.kind = kind;
        c.from = from;
        c.to = to;
        c.track = track;
        c.action = action;
        if (!Listed(dirs, from, &c.size)) c.size = SizeOf(from);
        c.note = note;
        const std::wstring tk = Key(to);
        if (Key(from) == tk && from == to) {
            c.action = OpAction::Skip;
            c.state = OpState::Unchanged;
        } else if (claimed.count(tk) || (Listed(dirs, to) && !SameFileObject(from, to))) {
            uint64_t toSize = 0;
            if (Listed(dirs, to, &toSize) && toSize == c.size && sameContent(from, to)) {
                c.action = OpAction::Skip;
                c.state = OpState::Duplicate;
                c.note = L"目标位置已有相同文件";
            } else {
                c.action = OpAction::Skip;
                c.state = OpState::Error;
                c.note = L"目标位置已有同名的不同文件,保留在原处";
            }
        } else if (Key(from) == tk) {
            c.via_temp = action == OpAction::Move; // 仅大小写变化
        }
        if (c.action != OpAction::Skip) claimed[tk] = plan.ops.size();
        plan.ops.push_back(c);
    };

    std::unordered_set<std::wstring> companionTaken; // 已作为附属文件处理的源文件
    for (auto& kv : bySourceDir) {
        const std::wstring& dirPath = sourceDirPath[kv.first];
        const DirListing& listing = ListDir(dirs, dirPath);
        std::set<std::wstring> audioInPlanKeys;
        for (size_t idx : kv.second) audioInPlanKeys.insert(Key(plan.ops[idx].from));

        if (opt.move_companions) {
            for (size_t idx : kv.second) {
                const OrganizeOp a = plan.ops[idx]; // 复制一份:下面会往 plan.ops 追加
                if (a.action == OpAction::Skip) continue;
                const OrganizeTrack& t = tracks[a.track];
                std::wstring aStem, aExt, nStem, nExt;
                SplitName(NameOf(a.from), aStem, aExt);
                SplitName(NameOf(a.to), nStem, nExt);
                const std::wstring lowStem = Lower(aStem);
                const std::wstring toDir = ParentOf(a.to);
                // 引用这个整轨的 CUE (文件名可能和整轨不同)
                std::set<std::wstring> cueRefs;
                if (t.cue_image) {
                    for (const auto& name : listing.names) {
                        std::wstring s, e;
                        SplitName(name, s, e);
                        if (Lower(e) != L".cue") continue;
                        CueSheet sheet;
                        if (!CueSheetParser::ParseFile(Join(dirPath, name), sheet)) continue;
                        for (const auto& f : sheet.files)
                            if (Lower(NameOf(f.file_name)) == Lower(NameOf(a.from)) || Lower(StripTrailingSep(f.file_name)) == lowStem + Lower(aExt))
                                cueRefs.insert(Lower(name));
                    }
                }
                for (const auto& name : listing.names) {
                    const std::wstring full = Join(dirPath, name);
                    const std::wstring fk = Key(full);
                    if (audioInPlanKeys.count(fk) || companionTaken.count(fk)) continue;
                    std::wstring lname = Lower(name);
                    std::wstring s, e;
                    SplitName(name, s, e);
                    const std::wstring le = Lower(e);
                    if (IsAudioExt(le)) continue;
                    // 同名附属文件:「歌名.lrc」「歌名.zh.lrc」都算 (主文件名之后紧跟一个点)
                    bool sameStem = lname.size() > lowStem.size() && lname.compare(0, lowStem.size(), lowStem) == 0 &&
                                    lname[lowStem.size()] == L'.';
                    bool isCue = cueRefs.count(lname) > 0;
                    if (!isCue && !(sameStem && IsCompanionExt(le))) continue;
                    if (IsFolderArtName(Lower(s)) && !sameStem) continue;
                    companionTaken.insert(fk);
                    std::wstring newName = (t.cue_image || isCue) ? name : nStem + name.substr(aStem.size());
                    addCompanion(OpKind::Companion, full, Join(toDir, newName), a.action, a.track, L"");
                }
            }
        }

        if (opt.move_folder_art && !inPlace) {
            // 目录里的歌全都去往同一个目录时,封面跟着搬;否则复制给每个去处,原处保留
            std::set<std::wstring> targets;
            std::map<std::wstring, std::wstring> targetPath;
            bool allMoving = true;
            for (size_t idx : kv.second) {
                const OrganizeOp& a = plan.ops[idx];
                if (a.action == OpAction::Skip) {
                    allMoving = false;
                    continue;
                }
                targets.insert(Key(ParentOf(a.to)));
                targetPath[Key(ParentOf(a.to))] = ParentOf(a.to);
            }
            // 目录里还有不参与整理的音频 (不在曲库 / 被排除) 时也不能把封面带走
            for (const auto& name : listing.names) {
                std::wstring s, e;
                SplitName(name, s, e);
                if (IsAudioExt(Lower(e)) && !audioInPlanKeys.count(Key(Join(dirPath, name)))) allMoving = false;
            }
            if (!targets.empty()) {
                const bool move = !copyMode && allMoving && targets.size() == 1;
                for (const auto& name : listing.names) {
                    std::wstring s, e;
                    SplitName(name, s, e);
                    if (!IsImageExt(Lower(e)) || !IsFolderArtName(Lower(s))) continue;
                    const std::wstring full = Join(dirPath, name);
                    if (companionTaken.count(Key(full))) continue;
                    companionTaken.insert(Key(full));
                    for (const auto& tk : targets)
                        addCompanion(OpKind::FolderArt, full, Join(targetPath[tk], name), move ? OpAction::Move : OpAction::Copy,
                                     SIZE_MAX, move ? L"" : L"目录拆分到多处,封面复制一份");
                }
            }
        }
    }

    // ---- 4. 跨盘判断、空间统计、汇总
    std::map<std::wstring, VolumeNeed> needs;
    std::unordered_map<std::wstring, std::wstring> volCache;
    auto vol = [&](const std::wstring& p) {
        const std::wstring d = ParentOf(p);
        auto it = volCache.find(Key(d));
        if (it != volCache.end()) return it->second;
        std::wstring v = VolumeOf(d);
        volCache[Key(d)] = v;
        return v;
    };
    for (auto& op : plan.ops) {
        if (op.action == OpAction::Skip) continue;
        const std::wstring tv = vol(op.to);
        op.cross_volume = op.action == OpAction::Move && tv != vol(op.from);
        if (op.action == OpAction::Copy || op.cross_volume) {
            VolumeNeed& n = needs[tv];
            if (n.root.empty()) {
                n.root = VolumeRootOf(ParentOf(op.to));
                n.available = AvailableBytes(ParentOf(op.to));
            }
            n.bytes += op.size;
        }
    }
    for (auto& kv : needs) {
        // 留 64MB 余量;同一时刻最多只有一个 .part 文件
        kv.second.enough = kv.second.available >= kv.second.bytes + (64ull << 20);
        if (!kv.second.enough) plan.warnings.push_back(L"目标盘 " + kv.second.root + L" 空间不足");
        plan.volumes.push_back(kv.second);
    }
    for (const auto& op : plan.ops) {
        if (op.kind != OpKind::Audio) continue;
        switch (op.state) {
            case OpState::Unchanged: ++plan.unchanged; break;
            case OpState::Duplicate: ++plan.duplicates; break;
            case OpState::Error: ++plan.errors; break;
            case OpState::Excluded: ++plan.excluded; break;
            case OpState::Renamed: ++plan.renamed; break;
            default: break;
        }
        if (op.action == OpAction::Move) ++plan.to_move;
        if (op.action == OpAction::Copy) ++plan.to_copy;
    }
    if (truncatedCount) plan.warnings.push_back(std::to_wstring(truncatedCount) + L" 首的路径过长,已截短");
    if (untagged) plan.warnings.push_back(std::to_wstring(untagged) + L" 首没有歌名、歌手和专辑标签,会归到「" + opt.unknown_value + L"」");
    return plan;
}

// ============================================================================
OrganizeResult Execute(const OrganizePlan& plan, const OrganizeOptions& opt, const std::wstring& journalPath,
                       std::function<void(const OrganizeProgress&)> onProgress, const std::atomic<bool>* cancel) {
    OrganizeResult res;
    res.journal = journalPath;
    if (!plan.ok()) {
        res.failures.push_back({L"", L"目标盘空间不足,未执行"});
        return res;
    }
    Journal journal;
    if (!journal.Open(journalPath, false)) {
        res.failures.push_back({journalPath, L"无法写入整理日志,未执行"});
        return res;
    }
    size_t planned = 0;
    OrganizeProgress prog;
    for (const auto& op : plan.ops) {
        if (op.action == OpAction::Skip) {
            ++res.skipped;
            continue;
        }
        ++planned;
        prog.bytes_total += op.size;
    }
    prog.total = planned;
    journal.Line("KPORG1", std::to_wstring(planned));

    Mover mover{opt, journal, cancel, nullptr, {}};
    mover.onBytes = [&](uint64_t n) {
        prog.bytes_done += n;
        if (onProgress) onProgress(prog);
    };

    struct Pending {
        size_t op;
        std::wstring temp;
    };
    std::vector<Pending> temps;
    std::vector<std::wstring> touchedSourceDirs;

    auto finishOne = [&](const OrganizeOp& op, int r, const std::wstring& to, const std::wstring& err) {
        if (r == 0) {
            ++res.failed;
            res.failures.push_back({op.from, err});
        } else {
            if (r == 1) ++res.moved;
            else ++res.copied;
            res.changes.push_back({op.from, to, r == 2});
            if (r == 1) touchedSourceDirs.push_back(ParentOf(op.from));
        }
        ++prog.done;
        if (onProgress) onProgress(prog);
    };

    // 阶段 1:需要临时名的先搬到临时名,腾出它们原来的位置
    for (size_t i = 0; i < plan.ops.size(); ++i) {
        const OrganizeOp& op = plan.ops[i];
        if (op.action == OpAction::Skip || !op.via_temp) continue;
        if (mover.Cancelled()) break;
        prog.current = op.from;
        std::wstring err, note;
        const std::wstring temp = TempNameFor(op.to);
        int r = mover.Transfer(op.from, temp, op.action == OpAction::Copy, err, note);
        if (r == 0) finishOne(op, 0, L"", err);
        else temps.push_back({i, temp});
    }
    // 阶段 2:其余文件直接搬到目标
    for (size_t i = 0; i < plan.ops.size(); ++i) {
        const OrganizeOp& op = plan.ops[i];
        if (op.action == OpAction::Skip || op.via_temp) continue;
        if (mover.Cancelled()) {
            res.cancelled = true;
            break;
        }
        prog.current = op.from;
        std::wstring err, note;
        std::wstring to = op.to;
        if (!Exists(op.from)) {
            finishOne(op, 0, L"", L"源文件已不存在");
            continue;
        }
        if (Exists(to) && !SameFileObject(op.from, to)) to = UniqueFree(to); // 计划之后目标处又出现了文件
        int r = mover.Transfer(op.from, to, op.action == OpAction::Copy, err, note);
        if (!note.empty()) res.failures.push_back({op.from, note});
        finishOne(op, r, to, err);
    }
    // 阶段 3:临时名改为正式名 (即使已取消也要做完,不能留下临时文件)
    for (const auto& p : temps) {
        const OrganizeOp& op = plan.ops[p.op];
        std::wstring to = op.to;
        if (Exists(to) && !SameFileObject(p.temp, to)) to = UniqueFree(to);
        std::error_code ec;
        fs::rename(FsPath(p.temp), FsPath(to), ec);
        if (ec) {
            // 改不成正式名:文件还在临时名下,日志里有记录,撤销可以还原
            finishOne(op, 0, L"", L"无法从临时名改为 " + to + L",文件暂存为 " + p.temp);
            continue;
        }
        journal.Line("M", p.temp, to);
        finishOne(op, op.action == OpAction::Copy ? 2 : 1, to, L"");
    }
    if (mover.Cancelled()) res.cancelled = true;

    if (opt.remove_empty_dirs && opt.mode != Mode::Copy) {
        std::vector<std::wstring> protectKeys;
        for (const auto& r : opt.protect_roots) protectKeys.push_back(Key(r));
        if (!opt.target_root.empty()) protectKeys.push_back(Key(opt.target_root));
        std::sort(touchedSourceDirs.begin(), touchedSourceDirs.end(),
                  [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); }); // 先深后浅
        touchedSourceDirs.erase(std::unique(touchedSourceDirs.begin(), touchedSourceDirs.end()), touchedSourceDirs.end());
        for (const auto& d : touchedSourceDirs) RemoveEmptyDirsUpward(d, protectKeys, journal, res.removed_dirs);
    }
    journal.Line(res.cancelled ? "CANCELLED" : "F");
    return res;
}

JournalInfo InspectJournal(const std::wstring& journalPath) {
    JournalInfo info;
    auto rows = ReadJournal(journalPath);
    if (rows.empty() || rows[0].empty() || rows[0][0] != L"KPORG1") return info;
    info.exists = true;
    if (rows[0].size() > 1) info.planned = static_cast<size_t>(std::wcstoul(rows[0][1].c_str(), nullptr, 10));
    std::unordered_set<std::wstring> temps;
    for (const auto& r : rows) {
        if (r[0] == L"F" || r[0] == L"CANCELLED") info.finished = true;
        else if (r[0] == L"U") info.undone = true;
        else if ((r[0] == L"M" || r[0] == L"C") && r.size() >= 3) {
            // 经临时名的文件记两行,只算一次
            if (temps.erase(Key(r[1]))) continue;
            if (NameOf(r[2]).compare(0, 7, L".kporg-") == 0) temps.insert(Key(r[2]));
            ++info.completed;
        }
    }
    return info;
}

OrganizeResult Undo(const std::wstring& journalPath, std::function<void(const OrganizeProgress&)> onProgress) {
    OrganizeResult res;
    res.journal = journalPath;
    auto rows = ReadJournal(journalPath);
    if (rows.empty() || rows[0].empty() || rows[0][0] != L"KPORG1") {
        res.failures.push_back({journalPath, L"没有可撤销的整理记录"});
        return res;
    }
    for (const auto& r : rows)
        if (r[0] == L"U") {
            res.failures.push_back({journalPath, L"这次整理已经撤销过了"});
            return res;
        }
    std::vector<const std::vector<std::wstring>*> ops;
    std::vector<std::wstring> created;
    for (const auto& r : rows) {
        if ((r[0] == L"M" || r[0] == L"C") && r.size() >= 3) ops.push_back(&r);
        else if (r[0] == L"K" && r.size() >= 2) created.push_back(r[1]);
    }
    Journal journal;
    journal.Open(journalPath, true);
    OrganizeOptions opt;
    Mover mover{opt, journal, nullptr, nullptr, {}};
    OrganizeProgress prog;
    prog.total = ops.size();
    for (auto it = ops.rbegin(); it != ops.rend(); ++it) {
        const auto& r = **it;
        const std::wstring& from = r[1];
        const std::wstring& to = r[2];
        prog.current = to;
        if (r[0] == L"C") {
            // 复制出来的副本:删掉 (内容已被改过就不删,避免误删)
            std::error_code ec;
            if (Exists(to) && Exists(from) && SizeOf(to) == SizeOf(from)) {
                if (fs::remove(FsPath(to), ec)) {
                    ++res.moved;
                    res.changes.push_back({to, L"", false});
                } else {
                    ++res.failed;
                    res.failures.push_back({to, L"无法删除副本"});
                }
            } else if (Exists(to)) {
                ++res.skipped;
                res.failures.push_back({to, L"原文件不在了或副本已改动,保留副本"});
            }
        } else {
            if (!Exists(to)) {
                ++res.failed;
                res.failures.push_back({to, L"文件已不在整理后的位置"});
            } else if (Exists(from)) {
                ++res.failed;
                res.failures.push_back({from, L"原位置已有文件,无法搬回"});
            } else {
                std::wstring err, note;
                int r2 = mover.Transfer(to, from, false, err, note);
                if (r2 == 1) {
                    ++res.moved;
                    res.changes.push_back({to, from, false});
                } else {
                    ++res.failed;
                    res.failures.push_back({to, err.empty() ? note : err});
                }
            }
        }
        ++prog.done;
        if (onProgress) onProgress(prog);
    }
    // 清理整理时新建、现在已空的目录 (先深后浅)
    std::sort(created.begin(), created.end(), [](const std::wstring& a, const std::wstring& b) { return a.size() > b.size(); });
    for (const auto& d : created) {
        std::error_code ec;
        if (fs::is_empty(FsPath(d), ec) && !ec && fs::remove(FsPath(d), ec)) res.removed_dirs.push_back(d);
    }
    journal.Line("U");
    return res;
}

} // namespace organizer
