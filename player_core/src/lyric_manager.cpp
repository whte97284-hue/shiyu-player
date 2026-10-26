#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../include/lyric_manager.hpp"
#include "../include/lyric_provider.hpp"
#include "../include/tinf.hpp"
#include "../include/audio_tag_parser.hpp"
#include "../include/platform_io.hpp"
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#include <winhttp.h>
#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#endif
#endif
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cwctype>
#include <cctype>
#include <cstring>
#include <functional>

namespace fs = std::filesystem;

namespace lyric_core {

// 酷狗 16 字节解密异或密钥
const uint8_t KrcDecoder::KRC_KEY[16] = {
    0x40, 0x47, 0x61, 0x77, 0x5E, 0x32, 0x74, 0x47,
    0x51, 0x36, 0x31, 0x2D, 0xCE, 0xD2, 0x6E, 0x69
};

std::vector<std::wstring> KuGouPathFinder::custom_dirs_;
std::mutex KuGouPathFinder::mutex_;

namespace {

std::wstring Utf8ToWide(const std::string& str) { return AudioTagParser::Utf8ToWide(str); }
std::string WideToUtf8(const std::wstring& wstr) { return AudioTagParser::WideToUtf8(wstr); }

bool DirExists(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}
bool DirExists(const std::wstring& p) { return platform::IsDirectory(p); }
#ifdef _WIN32
bool DirExists(const wchar_t* p) { return platform::IsDirectory(p); }
#endif

// 不经 std::filesystem 取文件名主干 (非 Windows 下 wstring -> path 的转换依赖区域设置)
std::wstring FileStem(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"/\\");
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) name.erase(dot);
    return name;
}

void Trim(std::wstring& s) {
    while (!s.empty() && std::iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::iswspace(s[i])) ++i;
    s.erase(0, i);
}

// 匹配用规范化:全角转半角、小写、合并空白、去掉转换器后缀与副本序号
std::wstring Normalize(std::wstring s) {
    for (auto& c : s) {
        if (c >= 0xFF01 && c <= 0xFF5E) c = static_cast<wchar_t>(c - 0xFEE0);
        else if (c == 0x3000) c = L' ';
        c = static_cast<wchar_t>(std::towlower(c));
    }
    static const std::wstring suffixes[] = {L"_kgg-dec", L"_kgm-dec", L"_kgma-dec", L"_vpr-dec", L"_ncm-dec",
                                            L"[hires]", L"[hi-res]", L"(1)", L"(2)", L"(3)"};
    bool stripped = true;
    while (stripped) {
        stripped = false;
        Trim(s);
        for (const auto& suf : suffixes) {
            if (s.size() > suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0) {
                s.erase(s.size() - suf.size());
                stripped = true;
            }
        }
    }
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (std::iswspace(c)) {
            if (!out.empty() && out.back() != L' ') out += L' ';
        } else {
            out += c;
        }
    }
    Trim(out);
    return out;
}

// 去掉转换器后缀与副本序号 (保留原有大小写,用作在线搜索词)
std::wstring StripDecorations(std::wstring s) {
    static const std::wstring suffixes[] = {L"_kgg-dec", L"_kgm-dec", L"_kgma-dec", L"_vpr-dec", L"_ncm-dec",
                                            L"[hires]", L"[hi-res]", L"(1)", L"(2)", L"(3)"};
    bool stripped = true;
    while (stripped) {
        stripped = false;
        Trim(s);
        for (const auto& suf : suffixes) {
            if (s.size() <= suf.size()) continue;
            bool same = true;
            for (size_t i = 0; i < suf.size() && same; ++i)
                same = static_cast<wchar_t>(std::towlower(s[s.size() - suf.size() + i])) == suf[i];
            if (same) {
                s.erase(s.size() - suf.size());
                stripped = true;
            }
        }
    }
    return s;
}

// 去掉文件名 / 标签里的曲目序号:「01东风破」「01. 东风破」「1、东风破」「03 - 东风破」-> 「东风破」。
// 只认 1~3 位数字后跟分隔符,或以 0 开头的两位数字 (01~09) 直接接歌名;
// 「2002年的第一场雪」「1983」这类以数字开头的歌名不受影响
std::wstring StripTrackNumber(const std::wstring& s) {
    size_t i = 0;
    while (i < s.size() && i < 4 && std::iswdigit(s[i])) ++i;
    if (i == 0 || i > 3 || i == s.size()) return s;
    size_t j = i;
    bool sep = false;
    while (j < s.size() && (s[j] == L'.' || s[j] == L'\x3001' || s[j] == L'\xFF0E' || s[j] == L'-' || s[j] == L'_' ||
                            s[j] == L')' || std::iswspace(s[j]))) {
        sep = true;
        ++j;
    }
    if (!sep && !(i == 2 && s[0] == L'0')) return s;
    if (j >= s.size()) return s;
    return s.substr(j);
}

// 「歌手 - 歌名」拆分 (只认带空格的分隔符,避免 A-ha、K-ON 这类名字被拆开)
void SplitArtistTitle(const std::wstring& stem, std::wstring& artist, std::wstring& title) {
    size_t sep = stem.find(L" - ");
    if (sep == std::wstring::npos) {
        artist.clear();
        title = stem;
    } else {
        artist = stem.substr(0, sep);
        title = stem.substr(sep + 3);
    }
}

bool IsHex32(const std::wstring& s, size_t pos) {
    if (pos + 32 > s.size()) return false;
    for (size_t i = pos; i < pos + 32; ++i) {
        if (!std::iswxdigit(s[i])) return false;
    }
    return true;
}

// 酷狗歌词文件名: 「歌手 - 歌名-<32 位 hash>-<id>-<序号>.krc」,去掉 hash 及其后缀
std::wstring StripKrcHashSuffix(const std::wstring& stem) {
    for (size_t pos = stem.rfind(L'-'); pos != std::wstring::npos && pos > 0; pos = stem.rfind(L'-', pos - 1)) {
        if (IsHex32(stem, pos + 1)) return stem.substr(0, pos);
    }
    return stem;
}

// 多歌手拆分:、 , & / ; feat.
std::vector<std::wstring> SplitArtists(const std::wstring& a) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto flush = [&]() {
        // 去掉括号里的别名,如 "aimer (エメ)"
        size_t paren = cur.find(L'(');
        if (paren != std::wstring::npos && paren > 0) cur.erase(paren);
        Trim(cur);
        if (!cur.empty()) out.push_back(cur);
        cur.clear();
    };
    for (wchar_t c : a) {
        if (c == L'\u3001' || c == L',' || c == L'&' || c == L'/' || c == L';' || c == L'\uFF0C') flush();
        else cur += c;
    }
    flush();
    return out;
}

bool ArtistsMatch(const std::wstring& a, const std::wstring& b) {
    for (const auto& x : SplitArtists(a)) {
        for (const auto& y : SplitArtists(b)) {
            if (x == y) return true;
        }
    }
    return false;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) out.push_back(static_cast<char>(cp));
    else if (cp < 0x800) { out.push_back(static_cast<char>(0xC0 | (cp >> 6))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else if (cp < 0x10000) { out.push_back(static_cast<char>(0xE0 | (cp >> 12))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
    else { out.push_back(static_cast<char>(0xF0 | (cp >> 18))); out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F))); out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F))); out.push_back(static_cast<char>(0x80 | (cp & 0x3F))); }
}

// ---------------------------------------------------------------------------
// Base64 (标准字母表, 忽略空白, 容忍缺失的 '=' 填充)
bool Base64Decode(const std::string& in, std::string& out) {
    int8_t table[256];
    bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) table[i] = -1;
        const char* alpha = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) table[static_cast<uint8_t>(alpha[i])] = static_cast<int8_t>(i);
        table[static_cast<uint8_t>('-')] = 62; // URL 安全变体
        table[static_cast<uint8_t>('_')] = 63;
        init = true;
    }
    out.clear();
    out.reserve(in.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char ch : in) {
        if (ch == '=') break;
        if (ch == ' ' || ch == '\r' || ch == '\n' || ch == '\t') continue;
        int8_t v = table[static_cast<uint8_t>(ch)];
        if (v < 0) return false;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return !out.empty();
}

// ---------------------------------------------------------------------------
// 极简 JSON 解析 (只覆盖酷狗接口与 [language:] 扩展所需: 对象 / 数组 / 字符串 / 数字 / 布尔 / null)
struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double num = 0;
    std::string str; // UTF-8
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* Get(const char* key) const {
        if (type != Object) return nullptr;
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    // 数字或数字字符串 ("116319439" / 185000) 统一取值
    double AsNumber() const {
        if (type == Number) return num;
        if (type == String) return std::strtod(str.c_str(), nullptr);
        return 0;
    }
    // 字符串或数字统一转为字符串
    std::string AsString() const {
        if (type == String) return str;
        if (type == Number) {
            std::ostringstream o;
            o << std::setprecision(17) << num;
            return o.str();
        }
        return {};
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    bool Parse(JsonValue& out) {
        SkipWs();
        if (!ParseValue(out, 0)) return false;
        SkipWs();
        return pos_ == s_.size();
    }

private:
    void SkipWs() { while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n')) ++pos_; }
    bool Match(const char* lit) {
        size_t n = std::strlen(lit);
        if (s_.compare(pos_, n, lit) != 0) return false;
        pos_ += n;
        return true;
    }
    bool ParseHex4(uint32_t& v) {
        if (pos_ + 4 > s_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool ParseString(std::string& out) {
        if (pos_ >= s_.size() || s_[pos_] != '"') return false;
        ++pos_;
        while (pos_ < s_.size()) {
            char c = s_[pos_++];
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (pos_ >= s_.size()) return false;
            char e = s_[pos_++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t cp = 0;
                    if (!ParseHex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        size_t save = pos_;
                        pos_ += 2;
                        uint32_t lo = 0;
                        if (ParseHex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else pos_ = save;
                    }
                    AppendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }
    bool ParseValue(JsonValue& v, int depth) {
        if (depth > 64 || pos_ >= s_.size()) return false;
        char c = s_[pos_];
        if (c == '{') {
            v.type = JsonValue::Object;
            ++pos_;
            SkipWs();
            if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
            while (true) {
                SkipWs();
                std::string key;
                if (!ParseString(key)) return false;
                SkipWs();
                if (pos_ >= s_.size() || s_[pos_] != ':') return false;
                ++pos_;
                SkipWs();
                JsonValue child;
                if (!ParseValue(child, depth + 1)) return false;
                v.obj.emplace_back(std::move(key), std::move(child));
                SkipWs();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
                return false;
            }
        }
        if (c == '[') {
            v.type = JsonValue::Array;
            ++pos_;
            SkipWs();
            if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
            while (true) {
                SkipWs();
                JsonValue child;
                if (!ParseValue(child, depth + 1)) return false;
                v.arr.push_back(std::move(child));
                SkipWs();
                if (pos_ < s_.size() && s_[pos_] == ',') { ++pos_; continue; }
                if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = JsonValue::String; return ParseString(v.str); }
        if (Match("true")) { v.type = JsonValue::Bool; v.b = true; return true; }
        if (Match("false")) { v.type = JsonValue::Bool; v.b = false; return true; }
        if (Match("null")) { v.type = JsonValue::Null; return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            const char* begin = s_.c_str() + pos_;
            char* end = nullptr;
            v.type = JsonValue::Number;
            v.num = std::strtod(begin, &end);
            if (end == begin) return false;
            pos_ += static_cast<size_t>(end - begin);
            return true;
        }
        return false;
    }

    const std::string& s_;
    size_t pos_ = 0;
};

bool ParseJson(const std::string& text, JsonValue& out) {
    JsonParser p(text);
    return p.Parse(out);
}

std::string UrlEncode(const std::string& utf8) {
    const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 15]);
        }
    }
    return out;
}

// 文件名非法字符替换, 用于缓存文件名
std::wstring SafeFileName(std::wstring s) {
    for (auto& c : s) {
        if (c == L'\\' || c == L'/') c = L'\u3001'; // 多歌手「A/B」-> 「A、B」, 匹配时仍能拆开
        else if (c == L':' || c == L'*' || c == L'?' || c == L'"' || c == L'<' || c == L'>' || c == L'|' || c < 32) c = L'_';
    }
    Trim(s);
    while (!s.empty() && (s.back() == L'.')) s.pop_back();
    return s;
}

// 把 [language:] 中一行的内容 (字符串数组, 逐字或整行) 拼成一行文本
std::wstring JoinLyricRow(const JsonValue& row) {
    std::string utf8;
    if (row.type == JsonValue::String) utf8 = row.str;
    else if (row.type == JsonValue::Array)
        for (const auto& part : row.arr) if (part.type == JsonValue::String) utf8 += part.str;
    std::wstring w = Utf8ToWide(utf8);
    Trim(w);
    return w;
}

// 解析 KRC 的 [language:Base64(JSON)] 扩展:
// {"content":[{"language":0,"type":1,"lyricContent":[["第一行翻译"],["第二行翻译"],...]},
//             {"language":0,"type":0,"lyricContent":[["na","tsu"," no"],...]}],"version":1}
// type 1 = 中文翻译 (每行一个字符串), type 0 = 音译 / 罗马音 (逐字拆分); 行序与 KRC 时间轴行一一对应
void ApplyLanguageTag(const std::string& base64, ParsedLyrics& lyrics) {
    std::string json;
    if (!Base64Decode(base64, json)) return;
    JsonValue root;
    if (!ParseJson(json, root)) return;
    const JsonValue* content = root.Get("content");
    if (!content || content->type != JsonValue::Array) return;
    for (const auto& block : content->arr) {
        const JsonValue* type = block.Get("type");
        const JsonValue* rows = block.Get("lyricContent");
        if (!type || !rows || rows->type != JsonValue::Array) continue;
        int t = static_cast<int>(type->AsNumber());
        if (t != 0 && t != 1) continue;
        size_t n = std::min(rows->arr.size(), lyrics.lines.size());
        for (size_t i = 0; i < n; ++i) {
            std::wstring text = JoinLyricRow(rows->arr[i]);
            if (t == 1) lyrics.lines[i].translation = std::move(text);
            else lyrics.lines[i].romaji = std::move(text);
        }
    }
}

std::string FormatLrcTime(uint32_t ms) {
    std::ostringstream oss;
    uint32_t totalSec = ms / 1000;
    oss << "[" << std::setfill('0') << std::setw(2) << totalSec / 60 << ":"
        << std::setfill('0') << std::setw(2) << totalSec % 60 << "."
        << std::setfill('0') << std::setw(2) << (ms % 1000) / 10 << "]";
    return oss.str();
}

static bool WriteWholeFile(const std::wstring& path, const std::string& data) {
    const std::wstring tmp = path + L".part";
    {
        platform::WritableFile out;
        if (!out.Create(tmp)) return false;
        const bool ok = out.Write(data.data(), data.size());
        out.Close();
        if (!ok) { platform::RemoveFile(tmp); return false; }
    }
    if (!platform::AtomicReplaceFile(tmp, path)) { platform::RemoveFile(tmp); return false; }
    return true;
}

int64_t NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count() + 1;
}

} // namespace

// ============================================================================
// ParsedLyrics 实用方法
// ============================================================================

bool ParsedLyrics::HasTranslation() const {
    for (const auto& line : lines) if (!line.translation.empty()) return true;
    return false;
}

std::string ParsedLyrics::ToStandardLrc(bool withTranslation) const {
    std::ostringstream oss;
    if (!title.empty()) oss << "[ti:" << WideToUtf8(title) << "]\r\n";
    if (!artist.empty()) oss << "[ar:" << WideToUtf8(artist) << "]\r\n";
    if (!album.empty()) oss << "[al:" << WideToUtf8(album) << "]\r\n";
    if (!by.empty()) oss << "[by:" << WideToUtf8(by) << "]\r\n";
    if (offset_ms != 0) oss << "[offset:" << offset_ms << "]\r\n";

    for (const auto& line : lines) {
        std::string stamp = FormatLrcTime(line.start_ms);
        oss << stamp << WideToUtf8(line.full_text) << "\r\n";
        // 双语 LRC: 翻译行使用相同时间戳, 主流播放器会把它显示为原文下方的副行
        if (withTranslation && !line.translation.empty()) oss << stamp << WideToUtf8(line.translation) << "\r\n";
    }
    return oss.str();
}

int ParsedLyrics::FindLineIndexAt(double seconds) const {
    if (lines.empty() || !(seconds >= 0)) return -1;
    const uint32_t targetMs = seconds >= 4294967.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(seconds * 1000.0);

    int left = 0;
    int right = static_cast<int>(lines.size()) - 1;
    int ans = -1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (lines[mid].start_ms <= targetMs) {
            ans = mid;
            left = mid + 1; // 寻找最靠后的满足条件的行
        } else {
            right = mid - 1;
        }
    }
    return ans;
}

int ParsedLyrics::FindSyllableIndexAt(size_t lineIndex, double seconds) const {
    if (lineIndex >= lines.size()) return -1;
    const auto& line = lines[lineIndex];
    if (line.syllables.empty() || !(seconds >= 0)) return -1;

    const uint32_t targetMs = seconds >= 4294967.0 ? 0xFFFFFFFFu : static_cast<uint32_t>(seconds * 1000.0);
    for (size_t i = 0; i < line.syllables.size(); ++i) {
        uint32_t start = line.syllables[i].start_ms;
        uint32_t end = start + line.syllables[i].duration_ms;
        if (targetMs >= start && targetMs < end) {
            return static_cast<int>(i);
        }
    }
    if (targetMs >= line.syllables.back().start_ms) {
        return static_cast<int>(line.syllables.size()) - 1;
    }
    return -1;
}

// ============================================================================
// KuGouPathFinder 智能嗅探
// ============================================================================

void KuGouPathFinder::AddCustomDirectory(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!dir.empty() && DirExists(dir)) {
        custom_dirs_.push_back(dir);
    }
}

#ifdef _WIN32
bool KuGouPathFinder::ProbeFromKuGouIni(std::vector<std::wstring>& outDirs) {
    // 优先读取 %APPDATA%\KuGou8\KuGou.ini
    wchar_t appData[MAX_PATH] = {0};
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData))) return false;

    fs::path iniPath = fs::path(appData) / L"KuGou8" / L"KuGou.ini";
    std::error_code ec;
    if (!fs::is_regular_file(iniPath, ec)) return false;

    const size_t before = outDirs.size();
    wchar_t buf[MAX_PATH] = {0};
    // 1. 读取 [LyricConfigSection] -> LyricPath
    GetPrivateProfileStringW(L"LyricConfigSection", L"LyricPath", L"", buf, MAX_PATH, iniPath.c_str());
    if (buf[0] != L'\0' && DirExists(buf)) {
        outDirs.push_back(buf);
    }

    // 2. 读取 [DownloadConfigSection] -> DownloadPath 下的 Lyric 子目录
    buf[0] = L'\0';
    GetPrivateProfileStringW(L"DownloadConfigSection", L"DownloadPath", L"", buf, MAX_PATH, iniPath.c_str());
    if (buf[0] != L'\0') {
        fs::path lyr = fs::path(buf) / L"Lyric";
        if (DirExists(lyr)) outDirs.push_back(lyr.wstring());
    }
    return outDirs.size() > before;
}

bool KuGouPathFinder::ProbeFromRegistry(std::vector<std::wstring>& outDirs) {
    // 检查注册表 HKCU\Software\KuGou\Prefetcher
    const size_t before = outDirs.size();
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\KuGou\\Prefetcher", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t pathBuf[MAX_PATH + 1] = {0};
        DWORD bytes = MAX_PATH * sizeof(wchar_t); // 留一个字符给结尾 0 (注册表字符串不保证以 0 结尾)
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"LastDetectedPath", NULL, &type, reinterpret_cast<LPBYTE>(pathBuf), &bytes) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ)) {
            fs::path exePath(pathBuf);
            std::error_code ec;
            if (fs::exists(exePath, ec)) {
                // KGMusic 根目录
                fs::path lyr = exePath.parent_path().parent_path() / L"Lyric";
                if (DirExists(lyr)) outDirs.push_back(lyr.wstring());
            }
        }
        RegCloseKey(hKey);
    }
    return outDirs.size() > before;
}

bool KuGouPathFinder::ProbeFromLogicalDrives(std::vector<std::wstring>& outDirs) {
    // 遍历本地固定磁盘根目录下的 KuGou\Lyric (不碰可移动盘与网络盘,避免无盘光驱 / 掉线网盘卡住)
    wchar_t drives[512] = {0};
    DWORD len = GetLogicalDriveStringsW(511, drives);
    if (len == 0 || len > 511) return false;

    const size_t before = outDirs.size();
    for (wchar_t* p = drives; *p; p += wcslen(p) + 1) {
        if (GetDriveTypeW(p) != DRIVE_FIXED) continue;
        fs::path candidate = fs::path(p) / L"KuGou" / L"Lyric";
        if (DirExists(candidate)) outDirs.push_back(candidate.wstring());
    }
    return outDirs.size() > before;
}

bool KuGouPathFinder::ProbeFromSpecialFolders(std::vector<std::wstring>& outDirs) {
    // 检查标准系统音乐目录 %USERPROFILE%\Music\KuGou\Lyric
    wchar_t musicPath[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_MYMUSIC, NULL, 0, musicPath))) {
        fs::path p = fs::path(musicPath) / L"KuGou" / L"Lyric";
        if (DirExists(p)) {
            outDirs.push_back(p.wstring());
            return true;
        }
    }
    return false;
}
#else
// 非 Windows 平台没有酷狗客户端,只认自定义目录与 ~/Music/KuGou/Lyric
bool KuGouPathFinder::ProbeFromKuGouIni(std::vector<std::wstring>&) { return false; }
bool KuGouPathFinder::ProbeFromRegistry(std::vector<std::wstring>&) { return false; }
bool KuGouPathFinder::ProbeFromLogicalDrives(std::vector<std::wstring>&) { return false; }
bool KuGouPathFinder::ProbeFromSpecialFolders(std::vector<std::wstring>& outDirs) {
    const char* home = std::getenv("HOME");
    if (!home || !*home) return false;
    fs::path p = fs::path(home) / "Music" / "KuGou" / "Lyric";
    if (!DirExists(p)) return false;
    outDirs.push_back(Utf8ToWide(p.string()));
    return true;
}
#endif

std::vector<std::wstring> KuGouPathFinder::DiscoverLyricDirectories() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::wstring> results;

    // 1. 用户自定义目录
    for (const auto& d : custom_dirs_) {
        if (DirExists(d)) results.push_back(d);
    }

    // 2. INI 配置 (最准)
    ProbeFromKuGouIni(results);

    // 3. 注册表
    ProbeFromRegistry(results);

    // 4. 全局逻辑驱动器
    ProbeFromLogicalDrives(results);

    // 5. 用户媒体夹
    ProbeFromSpecialFolders(results);

    // 去重与规范化 (Windows 路径大小写不敏感)
    std::vector<std::wstring> uniqueDirs;
    std::vector<std::wstring> keys;
    for (const auto& dir : results) {
        std::error_code ec;
        fs::path canon = fs::weakly_canonical(dir, ec);
        std::wstring canonStr = ec ? dir : canon.wstring();
        while (canonStr.size() > 1 && (canonStr.back() == L'\\' || canonStr.back() == L'/')) canonStr.pop_back();
        std::wstring key = canonStr;
#ifdef _WIN32
        for (auto& c : key) c = static_cast<wchar_t>(std::towlower(c));
#endif
        if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
            keys.push_back(key);
            uniqueDirs.push_back(canonStr);
        }
    }
    return uniqueDirs;
}

// ============================================================================
// KrcDecoder 解密与逐字解析
// ============================================================================

bool KrcDecoder::DecodeFile(const std::wstring& krcFilePath, ParsedLyrics& outLyrics) {
    platform::ReadOnlyFile f;
    if (!f.Open(krcFilePath, false)) return false;
    const uint64_t size = f.Size();
    if (size <= 4 || size > 10 * 1024 * 1024) return false; // 限制在 10MB 内

    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (f.ReadAt(0, buffer.data(), buffer.size()) != buffer.size()) return false;
    if (buffer[0] == 'k' && buffer[1] == 'r' && buffer[2] == 'c' && buffer[3] == '1')
        return DecodeMemory(buffer.data(), buffer.size(), outLyrics);
    return ParseLrcText(std::string(reinterpret_cast<const char*>(buffer.data()), buffer.size()), outLyrics);
}

bool KrcDecoder::DecodeMemory(const uint8_t* data, size_t size, ParsedLyrics& outLyrics) {
    if (!data || size <= 4) return false;

    // 1. 验证魔数 krc1 (0x6B, 0x72, 0x63, 0x31)
    if (data[0] != 'k' || data[1] != 'r' || data[2] != 'c' || data[3] != '1') {
        return false;
    }

    // 2. 异或混淆还原 (跳过前 4 字节魔数)
    size_t payloadSize = size - 4;
    std::vector<uint8_t> decrypted(payloadSize);
    for (size_t i = 0; i < payloadSize; ++i) {
        decrypted[i] = data[4 + i] ^ KRC_KEY[i % 16];
    }

    // 3. Zlib / Deflate 解压
    std::vector<uint8_t> decompressed;
    if (!ZlibDecompressor::DecompressZlib(decrypted.data(), decrypted.size(), decompressed)) {
        return false;
    }

    // 4. 解析 UTF-8 文本
    std::string rawText(reinterpret_cast<const char*>(decompressed.data()), decompressed.size());
    return ParseRawKrcText(rawText, outLyrics);
}

bool KrcDecoder::ParseRawKrcText(const std::string& rawUtf8Text, ParsedLyrics& outLyrics) {
    outLyrics.lines.clear();
    outLyrics.title.clear();
    outLyrics.artist.clear();
    outLyrics.album.clear();
    outLyrics.by.clear();
    outLyrics.offset_ms = 0;

    std::istringstream iss(rawUtf8Text);
    std::string lineStr;
    std::string languageTag; // [language:...] 等全部时间轴行解析完、排序前再按行序挂载

    while (std::getline(iss, lineStr)) {
        // 去除末尾换行
        while (!lineStr.empty() && (lineStr.back() == '\r' || lineStr.back() == ' ')) {
            lineStr.pop_back();
        }
        if (lineStr.empty()) continue;

        // 去除 UTF-8 BOM
        if (lineStr.size() >= 3 && static_cast<uint8_t>(lineStr[0]) == 0xEF &&
            static_cast<uint8_t>(lineStr[1]) == 0xBB && static_cast<uint8_t>(lineStr[2]) == 0xBF) {
            lineStr = lineStr.substr(3);
        }

        // 解析头部元数据标签: [ar:...], [ti:...], [al:...], [by:...], [offset:...]
        if (lineStr.rfind("[ti:", 0) == 0 && lineStr.back() == ']') {
            outLyrics.title = Utf8ToWide(lineStr.substr(4, lineStr.size() - 5));
            continue;
        }
        if (lineStr.rfind("[ar:", 0) == 0 && lineStr.back() == ']') {
            outLyrics.artist = Utf8ToWide(lineStr.substr(4, lineStr.size() - 5));
            continue;
        }
        if (lineStr.rfind("[al:", 0) == 0 && lineStr.back() == ']') {
            outLyrics.album = Utf8ToWide(lineStr.substr(4, lineStr.size() - 5));
            continue;
        }
        if (lineStr.rfind("[by:", 0) == 0 && lineStr.back() == ']') {
            outLyrics.by = Utf8ToWide(lineStr.substr(4, lineStr.size() - 5));
            continue;
        }
        if (lineStr.rfind("[language:", 0) == 0 && lineStr.back() == ']') {
            languageTag = lineStr.substr(10, lineStr.size() - 11);
            continue;
        }
        if (lineStr.rfind("[offset:", 0) == 0 && lineStr.back() == ']') {
            try {
                outLyrics.offset_ms = std::stoi(lineStr.substr(8, lineStr.size() - 9));
            } catch (...) {}
            continue;
        }

        // 解析时间轴行: [start_ms, duration_ms]<offset1, dur1, 0>word1<offset2, dur2, 0>word2...
        if (lineStr.front() == '[') {
            size_t closeBracket = lineStr.find(']');
            if (closeBracket == std::string::npos) continue;

            std::string timeSeg = lineStr.substr(1, closeBracket - 1);
            size_t comma = timeSeg.find(',');
            if (comma == std::string::npos) continue;

            uint32_t lineStart = 0;
            uint32_t lineDur = 0;
            try {
                lineStart = static_cast<uint32_t>(std::stoul(timeSeg.substr(0, comma)));
                lineDur = static_cast<uint32_t>(std::stoul(timeSeg.substr(comma + 1)));
            } catch (...) {
                continue;
            }

            LyricLine line;
            line.start_ms = lineStart;
            line.duration_ms = lineDur;

            std::string body = lineStr.substr(closeBracket + 1);
            size_t pos = 0;

            while (pos < body.size()) {
                if (body[pos] == '<') {
                    size_t endAngle = body.find('>', pos);
                    if (endAngle == std::string::npos) break;

                    std::string sylTime = body.substr(pos + 1, endAngle - pos - 1);
                    // 形如 "0,528,0" -> 偏移, 时长, 额外参数
                    uint32_t relStart = 0;
                    uint32_t sylDur = 0;
                    size_t c1 = sylTime.find(',');
                    if (c1 != std::string::npos) {
                        size_t c2 = sylTime.find(',', c1 + 1);
                        try {
                            relStart = static_cast<uint32_t>(std::stoul(sylTime.substr(0, c1)));
                            std::string durStr = (c2 != std::string::npos) ?
                                sylTime.substr(c1 + 1, c2 - c1 - 1) : sylTime.substr(c1 + 1);
                            sylDur = static_cast<uint32_t>(std::stoul(durStr));
                        } catch (...) {}
                    }

                    // 提取下一个 '<' 之前的文字
                    size_t nextAngle = body.find('<', endAngle + 1);
                    std::string wordUtf8;
                    if (nextAngle != std::string::npos) {
                        wordUtf8 = body.substr(endAngle + 1, nextAngle - endAngle - 1);
                        pos = nextAngle;
                    } else {
                        wordUtf8 = body.substr(endAngle + 1);
                        pos = body.size();
                    }

                    std::wstring wordW = Utf8ToWide(wordUtf8);
                    line.full_text += wordW;

                    LyricSyllable syl;
                    syl.text = wordW;
                    syl.start_ms = lineStart + relStart;
                    syl.duration_ms = sylDur;
                    line.syllables.push_back(syl);
                } else {
                    // 没有逐字标签的普通行内容
                    line.full_text += Utf8ToWide(body.substr(pos));
                    break;
                }
            }

            if (!line.full_text.empty() || line.duration_ms > 0) {
                outLyrics.lines.push_back(line);
            }
        }
    }
    // 翻译按 KRC 原始行序对应, 必须在排序前挂上
    if (!languageTag.empty()) ApplyLanguageTag(languageTag, outLyrics);
    // 二分定位依赖行按时间有序
    std::stable_sort(outLyrics.lines.begin(), outLyrics.lines.end(),
                     [](const LyricLine& a, const LyricLine& b) { return a.start_ms < b.start_ms; });
    return !outLyrics.lines.empty();
}

bool KrcDecoder::ParseLrcText(const std::string& utf8Text, ParsedLyrics& outLyrics) {
    outLyrics = ParsedLyrics();
    std::string text = utf8Text;
    if (text.size() >= 3 && static_cast<uint8_t>(text[0]) == 0xEF && static_cast<uint8_t>(text[1]) == 0xBB &&
        static_cast<uint8_t>(text[2]) == 0xBF)
        text = text.substr(3);

    std::vector<std::pair<uint32_t, std::wstring>> rows;
    std::istringstream iss(text);
    std::string lineStr;
    while (std::getline(iss, lineStr)) {
        while (!lineStr.empty() && (lineStr.back() == '\r' || lineStr.back() == ' ')) lineStr.pop_back();
        if (lineStr.empty() || lineStr.front() != '[') continue;

        auto tagValue = [&](const char* tag) -> std::wstring {
            size_t n = std::strlen(tag);
            return Utf8ToWide(lineStr.substr(n, lineStr.size() - n - 1));
        };
        if (lineStr.back() == ']') {
            if (lineStr.rfind("[ti:", 0) == 0) { outLyrics.title = tagValue("[ti:"); continue; }
            if (lineStr.rfind("[ar:", 0) == 0) { outLyrics.artist = tagValue("[ar:"); continue; }
            if (lineStr.rfind("[al:", 0) == 0) { outLyrics.album = tagValue("[al:"); continue; }
            if (lineStr.rfind("[by:", 0) == 0) { outLyrics.by = tagValue("[by:"); continue; }
            if (lineStr.rfind("[offset:", 0) == 0) {
                outLyrics.offset_ms = std::atoi(lineStr.substr(8, lineStr.size() - 9).c_str());
                continue;
            }
        }

        // 一行可带多个时间戳: [00:12.30][01:40.00]歌词
        std::vector<uint32_t> times;
        size_t pos = 0;
        while (pos < lineStr.size() && lineStr[pos] == '[') {
            size_t close = lineStr.find(']', pos);
            if (close == std::string::npos) break;
            std::string stamp = lineStr.substr(pos + 1, close - pos - 1);
            size_t colon = stamp.find(':');
            if (colon == std::string::npos || colon == 0) break;
            bool digits = true;
            for (size_t k = 0; k < colon; ++k) if (!std::isdigit(static_cast<unsigned char>(stamp[k]))) digits = false;
            if (!digits) break;
            uint32_t min = static_cast<uint32_t>(std::atoi(stamp.substr(0, colon).c_str()));
            std::string rest = stamp.substr(colon + 1);
            size_t dot = rest.find_first_of(".:");
            uint32_t sec = static_cast<uint32_t>(std::atoi(rest.substr(0, dot).c_str()));
            uint32_t frac = 0;
            if (dot != std::string::npos) {
                std::string f = rest.substr(dot + 1);
                frac = static_cast<uint32_t>(std::atoi(f.c_str()));
                if (f.size() == 1) frac *= 100;
                else if (f.size() == 2) frac *= 10;
            }
            times.push_back(min * 60000 + sec * 1000 + frac);
            pos = close + 1;
        }
        if (times.empty()) continue;
        std::wstring body = Utf8ToWide(lineStr.substr(pos));
        Trim(body);
        for (uint32_t t : times) rows.emplace_back(t, body);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

    for (auto& row : rows) {
        // 双语 LRC: 与上一行时间戳相同的行作为翻译
        if (!outLyrics.lines.empty() && outLyrics.lines.back().start_ms == row.first &&
            outLyrics.lines.back().translation.empty() && !outLyrics.lines.back().full_text.empty()) {
            if (!row.second.empty()) outLyrics.lines.back().translation = row.second;
            continue;
        }
        LyricLine line;
        line.start_ms = row.first;
        line.full_text = row.second;
        outLyrics.lines.push_back(std::move(line));
    }
    for (size_t i = 0; i + 1 < outLyrics.lines.size(); ++i)
        outLyrics.lines[i].duration_ms = outLyrics.lines[i + 1].start_ms - outLyrics.lines[i].start_ms;
    // 去掉空白行 (保留它们的时间作为上一行的结束)
    outLyrics.lines.erase(std::remove_if(outLyrics.lines.begin(), outLyrics.lines.end(),
                                         [](const LyricLine& l) { return l.full_text.empty(); }),
                          outLyrics.lines.end());
    return !outLyrics.lines.empty();
}

// ============================================================================
// KuGouOnlineLyrics 酷狗在线歌词接口
// ============================================================================

KuGouOnlineLyrics::KuGouOnlineLyrics(HttpGetFn http) : http_(http ? std::move(http) : HttpGetFn(&DefaultHttpGet)) {}

bool KuGouOnlineLyrics::DefaultHttpGet(const std::string& url, std::string& body) {
#ifdef _WIN32
    body.clear();
    std::wstring wurl = Utf8ToWide(url);
    URL_COMPONENTS uc = {};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0};
    wchar_t path[4096] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    wchar_t extra[4096] = {0};
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4096;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return false;
    std::wstring object = std::wstring(path) + extra;

    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) KuGou2012", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 4000, 4000, 6000, 8000);
    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, host, uc.nPort, 0);
    if (conn) {
        DWORD flags = uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(conn, L"GET", object.c_str(), NULL, WINHTTP_NO_REFERER,
                                           WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (req) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, NULL)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                    &status, &len, WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    ok = true;
                    for (;;) {
                        DWORD avail = 0;
                        if (!WinHttpQueryDataAvailable(req, &avail)) { ok = false; break; }
                        if (avail == 0) break;
                        size_t old = body.size();
                        body.resize(old + avail);
                        DWORD got = 0;
                        if (!WinHttpReadData(req, &body[old], avail, &got)) { ok = false; break; }
                        body.resize(old + got);
                        if (body.size() > 8 * 1024 * 1024) { ok = false; break; } // 歌词不会这么大
                    }
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    return ok && !body.empty();
#else
    (void)url;
    body.clear();
    return false;
#endif
}

bool KuGouOnlineLyrics::GetWithFallback(const std::string& pathAndQuery, std::string& body) const {
    // krcs.kugou.com 为主, lyrics.kugou.com 为备用域名
    for (const char* host : {"http://krcs.kugou.com", "http://lyrics.kugou.com"}) {
        if (http_(std::string(host) + pathAndQuery, body)) return true;
    }
    return false;
}

bool KuGouOnlineLyrics::Search(const std::wstring& keyword, uint32_t durationMs, const std::string& hash,
                               std::vector<KuGouLyricCandidate>& out) const {
    out.clear();
    std::string q = "/search?ver=1&man=yes&client=pc&keyword=" + UrlEncode(WideToUtf8(keyword));
    q += "&duration=" + (durationMs ? std::to_string(durationMs) : std::string());
    q += "&hash=" + hash;
    std::string body;
    if (!GetWithFallback(q, body)) return false;
    JsonValue root;
    if (!ParseJson(body, root)) return false;
    const JsonValue* status = root.Get("status");
    if (status && static_cast<int>(status->AsNumber()) != 200) return false;
    const JsonValue* cands = root.Get("candidates");
    if (!cands || cands->type != JsonValue::Array) return false;
    for (const auto& c : cands->arr) {
        KuGouLyricCandidate item;
        if (const JsonValue* v = c.Get("id")) item.id = v->AsString();
        if (const JsonValue* v = c.Get("accesskey")) item.accesskey = v->AsString();
        if (const JsonValue* v = c.Get("song")) item.song = Utf8ToWide(v->AsString());
        if (const JsonValue* v = c.Get("singer")) item.singer = Utf8ToWide(v->AsString());
        if (const JsonValue* v = c.Get("duration")) item.duration_ms = static_cast<uint32_t>(v->AsNumber());
        if (!item.id.empty() && !item.accesskey.empty()) out.push_back(std::move(item));
    }
    return !out.empty();
}

bool KuGouOnlineLyrics::Download(const KuGouLyricCandidate& c, const std::string& fmt, std::string& outBytes) const {
    outBytes.clear();
    std::string q = "/download?ver=1&client=pc&id=" + UrlEncode(c.id) + "&accesskey=" + UrlEncode(c.accesskey) +
                    "&fmt=" + fmt + "&charset=utf8";
    std::string body;
    if (!GetWithFallback(q, body)) return false;
    JsonValue root;
    if (!ParseJson(body, root)) return false;
    const JsonValue* status = root.Get("status");
    if (status && static_cast<int>(status->AsNumber()) != 200) return false;
    const JsonValue* content = root.Get("content");
    if (!content || content->type != JsonValue::String) return false;
    return Base64Decode(content->str, outBytes);
}

int KuGouOnlineLyrics::PickCandidate(const std::vector<KuGouLyricCandidate>& list, uint32_t durationMs) {
    if (list.empty()) return -1;
    if (durationMs == 0) return 0;
    // 酷狗按相关度排序; 取前几项中时长最接近的一项, 相差超过 8 秒视为别的版本
    int best = -1;
    int64_t bestDiff = 0;
    for (size_t i = 0; i < list.size() && i < 5; ++i) {
        if (list[i].duration_ms == 0) {
            if (best < 0) { best = static_cast<int>(i); bestDiff = 8000; }
            continue;
        }
        int64_t diff = std::llabs(static_cast<int64_t>(list[i].duration_ms) - static_cast<int64_t>(durationMs));
        if (diff <= 8000 && (best < 0 || diff < bestDiff)) {
            best = static_cast<int>(i);
            bestDiff = diff;
        }
    }
    return best;
}


// ============================================================================
// LyricManager 门面与模糊匹配
// ============================================================================

LyricManager::LyricManager() {
    RefreshDirectories();
}

void LyricManager::RefreshDirectories() {
    auto dirs = KuGouPathFinder::DiscoverLyricDirectories();
    std::lock_guard<std::mutex> lock(mutex_);
    search_dirs_ = std::move(dirs);
    if (!cache_dir_.empty() && std::find(search_dirs_.begin(), search_dirs_.end(), cache_dir_) == search_dirs_.end())
        search_dirs_.push_back(cache_dir_);
    index_.clear();
    index_built_at_ = 0;
}

std::vector<std::wstring> LyricManager::GetLyricDirectories() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return search_dirs_;
}

// 目录本身的修改时间:目录里新增 / 删除 / 改名文件时会变,用来发现酷狗新下载的歌词
std::vector<uint64_t> LyricManager::DirStampsLocked() const {
    std::vector<uint64_t> stamps;
    stamps.reserve(search_dirs_.size());
    for (const auto& dir : search_dirs_) {
        uint64_t size = 0, mtime = 0;
        stamps.push_back(platform::GetFileInfo(dir, size, mtime) ? mtime : 0);
    }
    return stamps;
}

// 歌词库索引:每个 .krc 只解析一次文件名。查不到时 (onMiss) 若歌词目录有变化或索引已超过 10 秒就重建,
// 这样酷狗刚下载的歌词在下一次查找时即可命中
bool LyricManager::EnsureIndexLocked(bool onMiss) {
    const int64_t now = NowSeconds();
    if (index_built_at_ != 0) {
        if (!onMiss) return false;
        if (now - index_built_at_ < 10 && DirStampsLocked() == dir_stamps_) return false;
    }

    index_.clear();
    index_built_at_ = now;
    dir_stamps_ = DirStampsLocked();
    for (const auto& dir : search_dirs_) {
        platform::EnumerateFilesRecursive(dir, [this](const std::wstring&, const std::vector<platform::DirEntry>& files) {
            for (const auto& e : files) {
                const size_t dot = e.name.rfind(L'.');
                if (dot == std::wstring::npos) continue;
                std::wstring ext = e.name.substr(dot);
                for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
                if (ext != L".krc" && ext != L".lrc") continue; // .lrc 是在线多源拉取的缓存

                std::wstring artist, title;
                SplitArtistTitle(StripKrcHashSuffix(e.name.substr(0, dot)), artist, title);
                KrcEntry k;
                k.title = Normalize(title);
                k.artist = Normalize(artist);
                k.path = e.path;
                k.mtime = e.mtime;
                if (!k.title.empty()) index_.push_back(std::move(k));
            }
            return true;
        });
    }
    return true;
}

std::wstring LyricManager::MatchLocked(const std::wstring& title, const std::wstring& artist) const {
    const std::wstring qTitle = Normalize(title);
    const std::wstring qArtist = Normalize(artist);
    if (qTitle.empty()) return {};

    const KrcEntry* best = nullptr;
    int bestScore = 0;
    for (const auto& e : index_) {
        if (e.title != qTitle) continue;
        int score = 1;
        if (!qArtist.empty() && !e.artist.empty()) {
            if (!ArtistsMatch(qArtist, e.artist)) continue; // 同名不同歌手:宁缺毋错
            score = 2;
        }
        // 同一首歌有多份歌词时取最新下载的那份
        if (score > bestScore || (score == bestScore && best && e.mtime > best->mtime)) {
            best = &e;
            bestScore = score;
        }
    }
    return best ? best->path : std::wstring();
}

std::wstring LyricManager::FindMatchingKrcFile(const std::wstring& audioFilePath, const std::wstring& title, const std::wstring& artist) {
    // 候选查询:标签 (歌名 + 歌手),然后是文件名「歌手 - 歌名」
    std::vector<std::pair<std::wstring, std::wstring>> queries;
    if (!title.empty()) queries.emplace_back(title, artist);
    if (!audioFilePath.empty()) {
        std::wstring stem = FileStem(audioFilePath);
        std::wstring fArtist, fTitle;
        SplitArtistTitle(stem, fArtist, fTitle);
        queries.emplace_back(fTitle, fArtist);
    }
    // 再试去掉曲目序号的歌名 (「01东风破」)
    for (size_t i = 0, n = queries.size(); i < n; ++i) {
        std::wstring t = StripTrackNumber(queries[i].first);
        if (t != queries[i].first) queries.emplace_back(t, queries[i].second);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (int pass = 0; pass < 2; ++pass) {
        const bool rebuilt = EnsureIndexLocked(pass == 1);
        if (pass == 1 && !rebuilt) break;
        for (const auto& q : queries) {
            std::wstring hit = MatchLocked(q.first, q.second);
            if (!hit.empty()) return hit;
        }
        if (rebuilt) break; // 索引刚刚建立,无需再建一次
    }
    return {};
}

bool LyricManager::LoadLyricsForTrack(const std::wstring& audioFilePath, const std::wstring& title, const std::wstring& artist, ParsedLyrics& outLyrics) {
    const std::wstring krcPath = FindMatchingKrcFile(audioFilePath, title, artist);
    if (krcPath.empty()) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(krcPath);
        if (it != cache_.end()) {
            outLyrics = it->second;
            return true;
        }
    }

    ParsedLyrics parsed;
    if (!KrcDecoder::DecodeFile(krcPath, parsed)) return false;
    if (parsed.title.empty()) parsed.title = title;
    if (parsed.artist.empty()) parsed.artist = artist;

    std::lock_guard<std::mutex> lock(mutex_);
    if (cache_.size() >= 64) cache_.clear(); // 简单上限,避免长时间播放后无限增长
    cache_[krcPath] = parsed;
    outLyrics = std::move(parsed);
    return true;
}

void LyricManager::SetCacheDirectory(const std::wstring& dir) {
    if (!dir.empty()) platform::CreateDirectories(dir);
    std::lock_guard<std::mutex> lock(mutex_);
    if (!cache_dir_.empty()) search_dirs_.erase(std::remove(search_dirs_.begin(), search_dirs_.end(), cache_dir_), search_dirs_.end());
    cache_dir_ = dir;
    if (!dir.empty() && std::find(search_dirs_.begin(), search_dirs_.end(), dir) == search_dirs_.end()) search_dirs_.push_back(dir);
    index_.clear();
    index_built_at_ = 0;
    tr_misses_.clear();
    tr_misses_loaded_ = false;
}

void LyricManager::SetHttpGetter(KuGouOnlineLyrics::HttpGetFn http) {
    std::lock_guard<std::mutex> lock(mutex_);
    http_ = std::move(http);
}

namespace {
// 曲库在标签缺失时填的占位歌手,不能拿去搜索 (「未知艺术家 - 东风破」搜不到任何结果)
bool IsPlaceholderArtist(const std::wstring& a) {
    std::wstring l;
    for (wchar_t c : a) l += static_cast<wchar_t>(towlower(c));
    return l.empty() || l == L"未知艺术家" || l == L"未知歌手" || l == L"未知" || l == L"unknown" ||
           l == L"unknown artist" || l == L"<unknown>" || l == L"various artists";
}

// 没有歌手时用酷狗歌曲搜索按歌名查出歌手与 hash (只取时长相差 8 秒以内的)
bool GuessSingerOnline(const KuGouOnlineLyrics::HttpGetFn& http, const std::wstring& title, uint32_t durationMs,
                       std::wstring& singer, std::string& hash) {
    std::string body;
    const std::string url = "http://mobilecdn.kugou.com/api/v3/search/song?format=json&page=1&pagesize=10&keyword=" +
                            UrlEncode(WideToUtf8(title));
    if (!http(url, body)) return false;
    JsonValue root;
    if (!ParseJson(body, root)) return false;
    const JsonValue* data = root.Get("data");
    const JsonValue* info = data ? data->Get("info") : nullptr;
    if (!info || info->type != JsonValue::Array) return false;
    const std::wstring want = Normalize(title);
    for (const auto& it : info->arr) {
        const JsonValue* song = it.Get("songname");
        const JsonValue* name = it.Get("singername");
        const JsonValue* dur = it.Get("duration");
        if (!song || !name) continue;
        if (Normalize(Utf8ToWide(song->AsString())) != want) continue;
        const int64_t d = dur ? static_cast<int64_t>(dur->AsNumber()) * 1000 : 0;
        if (durationMs && d && std::llabs(d - static_cast<int64_t>(durationMs)) > 8000) continue;
        singer = Utf8ToWide(name->AsString());
        if (const JsonValue* h = it.Get("hash")) hash = h->AsString();
        return !singer.empty();
    }
    return false;
}
} // namespace

bool LyricManager::FetchOnline(const std::wstring& audioFilePath, const std::wstring& title, const std::wstring& artist,
                               uint32_t durationMs, ParsedLyrics& outLyrics) {
    // 搜索词:标签里的歌名 / 歌手,没有标签时用文件名「歌手 - 歌名」(去掉转换器后缀)
    std::wstring qTitle = title, qArtist = artist;
    if (qTitle.empty() && !audioFilePath.empty()) SplitArtistTitle(StripDecorations(FileStem(audioFilePath)), qArtist, qTitle);
    qTitle = StripTrackNumber(qTitle); // 「01东风破」按「东风破」搜索
    Trim(qTitle);
    Trim(qArtist);
    if (IsPlaceholderArtist(qArtist)) qArtist.clear();
    if (qTitle.empty()) return false;

    const std::wstring key = Normalize(qArtist) + L"\n" + Normalize(qTitle) + L"\n" + std::to_wstring(durationMs / 1000);
    KuGouOnlineLyrics::HttpGetFn http;
    std::wstring cacheDir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (online_misses_.count(key)) return false;
        http = http_;
        cacheDir = cache_dir_;
    }

    KuGouOnlineLyrics api(http);
    std::vector<KuGouLyricCandidate> candidates;
    if (!qArtist.empty()) {
        if (!api.Search(qArtist + L" - " + qTitle, durationMs, std::string(), candidates))
            api.Search(qTitle, durationMs, std::string(), candidates); // 歌手名写法不同时退回只搜歌名
    } else {
        // 没有歌手:只搜歌名常常一个候选都没有。先按歌名查出歌手 (时长对得上),
        // 再试所在文件夹名 (「周杰伦 叶惠美」里的「周杰伦」),最后才只搜歌名
        std::vector<std::wstring> guesses;
        std::wstring singer;
        std::string hash;
        if (GuessSingerOnline(http ? http : KuGouOnlineLyrics::HttpGetFn(&KuGouOnlineLyrics::DefaultHttpGet), qTitle,
                              durationMs, singer, hash)) {
            if (api.Search(singer + L" - " + qTitle, durationMs, hash, candidates)) qArtist = singer;
        }
        if (candidates.empty() && !audioFilePath.empty()) {
            std::wstring dir = audioFilePath;
            size_t cut = dir.find_last_of(L"\\/");
            dir = cut == std::wstring::npos ? std::wstring() : dir.substr(0, cut);
            cut = dir.find_last_of(L"\\/");
            std::wstring folder = cut == std::wstring::npos ? dir : dir.substr(cut + 1);
            size_t sp = folder.find_first_of(L" -_\u3000");
            if (sp != std::wstring::npos && sp > 0) guesses.push_back(folder.substr(0, sp));
            if (!folder.empty()) guesses.push_back(folder);
            for (const auto& g : guesses) {
                // 文件夹名只是猜测:选中的候选歌手必须真的出现在文件夹名里,才算猜对
                if (api.Search(g + L" - " + qTitle, durationMs, std::string(), candidates)) {
                    const int p = KuGouOnlineLyrics::PickCandidate(candidates, durationMs);
                    const std::wstring who = p >= 0 ? Normalize(candidates[static_cast<size_t>(p)].singer) : std::wstring();
                    if (!who.empty() && Normalize(g).find(who) != std::wstring::npos) {
                        qArtist = candidates[static_cast<size_t>(p)].singer;
                        break;
                    }
                }
                candidates.clear();
            }
        }
        if (candidates.empty()) api.Search(qTitle, durationMs, std::string(), candidates);
    }
    const int pick = KuGouOnlineLyrics::PickCandidate(candidates, durationMs);
    if (pick < 0) {
        // 有候选但时长都对不上才记为查不到;没有候选可能是断网,下次播放再试
        if (!candidates.empty()) {
            std::lock_guard<std::mutex> lock(mutex_);
            online_misses_.insert(key);
        }
        return false;
    }
    const KuGouLyricCandidate& c = candidates[static_cast<size_t>(pick)];

    ParsedLyrics parsed;
    std::string bytes;
    bool ok = false;
    if (api.Download(c, "krc", bytes) &&
        KrcDecoder::DecodeMemory(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size(), parsed)) {
        ok = true;
        if (!cacheDir.empty()) {
            const std::wstring name = SafeFileName(qArtist.empty() ? qTitle : qArtist + L" - " + qTitle);
            if (!name.empty()) {
                const std::wstring sep = cacheDir.back() == L'/' || cacheDir.back() == L'\\' ? L"" : L"/";
                if (WriteWholeFile(cacheDir + sep + name + L".krc", bytes)) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    index_built_at_ = 0; // 下次查找时把新缓存收进索引
                }
            }
        }
    } else if (api.Download(c, "lrc", bytes) && KrcDecoder::ParseLrcText(bytes, parsed)) {
        ok = true;
    }
    if (!ok) return false;
    if (parsed.title.empty()) parsed.title = qTitle;
    if (parsed.artist.empty()) parsed.artist = qArtist;
    outLyrics = std::move(parsed);
    return true;
}

bool LyricManager::ParseDoc(const RawLyricDoc& doc, ParsedLyrics& outLyrics) {
    outLyrics = ParsedLyrics();
    switch (doc.kind) {
        case RawLyricDoc::Payload::Krc:
            return KrcDecoder::DecodeMemory(reinterpret_cast<const uint8_t*>(doc.raw_payload.data()),
                                            doc.raw_payload.size(), outLyrics);
        case RawLyricDoc::Payload::Lrc: {
            if (!KrcDecoder::ParseLrcText(doc.lrc_text, outLyrics)) return false;
            // 网易云的翻译单独一份 LRC:按时间戳并入 (容差 50ms,接口偶有取整差)
            ParsedLyrics tr;
            if (!doc.translation_text.empty() && KrcDecoder::ParseLrcText(doc.translation_text, tr)) {
                size_t j = 0;
                for (auto& line : outLyrics.lines) {
                    while (j < tr.lines.size() && tr.lines[j].start_ms + 50 < line.start_ms) ++j;
                    if (j < tr.lines.size() && tr.lines[j].start_ms <= line.start_ms + 50 && line.translation.empty() &&
                        tr.lines[j].full_text != line.full_text)
                        line.translation = tr.lines[j].full_text;
                }
            }
            return true;
        }
        case RawLyricDoc::Payload::Plain: {
            std::istringstream iss(doc.raw_payload.empty() ? doc.lrc_text : doc.raw_payload);
            std::string row;
            while (std::getline(iss, row)) {
                std::wstring text = Utf8ToWide(row);
                Trim(text);
                while (!text.empty() && text.back() == L'\r') text.pop_back();
                if (text.empty()) continue;
                LyricLine line;
                line.full_text = std::move(text);
                outLyrics.lines.push_back(std::move(line));
            }
            return !outLyrics.lines.empty();
        }
    }
    return false;
}

// ============================================================================
// 外语歌的翻译:语种判断、时间轴一致性、在线找带翻译的版本
// ============================================================================

namespace {

// 按字符类别计数:假名 / 谚文 / 汉字 / 拉丁字母
struct ScriptCount {
    size_t kana = 0, hangul = 0, han = 0, latin = 0;
};

ScriptCount CountScripts(const ParsedLyrics& l) {
    ScriptCount n;
    for (const auto& line : l.lines) {
        for (wchar_t c : line.full_text) {
            if ((c >= 0x3041 && c <= 0x30FF && c != 0x30FB && c != 0x30FC) || (c >= 0x31F0 && c <= 0x31FF) ||
                (c >= 0xFF66 && c <= 0xFF9D)) ++n.kana;
            else if ((c >= 0xAC00 && c <= 0xD7AF) || (c >= 0x1100 && c <= 0x11FF) || (c >= 0x3130 && c <= 0x318F)) ++n.hangul;
            else if ((c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF)) ++n.han;
            else if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= 0xC0 && c <= 0x24F && c != 0xD7 && c != 0xF7)) ++n.latin;
        }
    }
    return n;
}

// 比对用的歌词文本:只留字母、数字和 CJK,全角转半角、小写
std::wstring LineKey(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c >= 0xFF01 && c <= 0xFF5E) c = static_cast<wchar_t>(c - 0xFEE0);
        if (std::iswalnum(c) || c >= 0x3040) out.push_back(static_cast<wchar_t>(std::towlower(c)));
    }
    return out;
}

// 两句是不是同一句:短的一方的相邻二字组有六成以上出现在长的一方
// (各家断句不同:酷狗常把网易云的一行拆成两行;繁简 / 新旧字形也会有个别字不同)
bool SameLine(const std::wstring& a, const std::wstring& b) {
    if (a.empty() || b.empty()) return false;
    if (a == b) return true;
    const std::wstring& s = a.size() <= b.size() ? a : b;
    const std::wstring& t = a.size() <= b.size() ? b : a;
    if (s.size() < 2) return t.find(s) == 0;
    size_t hit = 0;
    for (size_t i = 0; i + 1 < s.size(); ++i)
        if (t.find(s.substr(i, 2)) != std::wstring::npos) ++hit;
    return hit * 10 >= (s.size() - 1) * 6;
}

bool Timed(const ParsedLyrics& l) {
    for (const auto& line : l.lines)
        if (line.start_ms > 0) return true;
    return false;
}

int64_t WallSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string ReadSmallFile(const std::wstring& path) {
    platform::ReadOnlyFile f;
    if (!f.Open(path, false)) return {};
    const uint64_t size = f.Size();
    if (size == 0 || size > (1u << 20)) return {};
    std::string data(static_cast<size_t>(size), '\0');
    if (f.ReadAt(0, &data[0], data.size()) != data.size()) return {};
    return data;
}

constexpr int64_t kTranslationMissTtl = 30LL * 24 * 3600; // 确认没有翻译的曲目,30 天后再试 (平台会陆续补上)

} // namespace

bool LyricManager::IsForeignLyrics(const ParsedLyrics& lyrics) {
    const ScriptCount n = CountScripts(lyrics);
    const size_t kh = n.kana + n.hangul;
    // 日 / 韩文:假名或谚文至少 8 个,且占 CJK 字符的两成以上 (中文歌里夹几句日文不算)
    if (kh >= 8 && kh * 5 >= kh + n.han) return true;
    // 英文等:拉丁字母为主,几乎没有汉字
    return kh == 0 && n.latin >= 40 && n.han * 20 <= n.latin;
}

// 「有翻译」按中文译文算:译文行有汉字、没有假名 / 谚文,且至少占正文三成。
// 只精确到秒的 LRC 会把同一秒的两句并成「原文 + 翻译」,罗马音也会被当成翻译,这两种都不算
bool LyricManager::NeedsTranslation(const ParsedLyrics& lyrics) {
    if (lyrics.IsEmpty() || !IsForeignLyrics(lyrics)) return false;
    size_t body = 0, translated = 0;
    for (const auto& line : lyrics.lines) {
        if (LineKey(line.full_text).empty()) continue;
        ++body;
        if (line.translation.empty()) continue;
        ParsedLyrics one;
        one.lines.emplace_back();
        one.lines.back().full_text = line.translation;
        const ScriptCount n = CountScripts(one);
        if (n.han > 0 && n.kana == 0 && n.hangul == 0) ++translated;
    }
    return translated * 10 < body * 3;
}

double LyricManager::TimelineAgreement(const ParsedLyrics& local, const ParsedLyrics& online) {
    std::vector<std::pair<uint32_t, std::wstring>> other;
    other.reserve(online.lines.size());
    for (const auto& o : online.lines) {
        std::wstring k = LineKey(o.full_text);
        if (!k.empty()) other.emplace_back(o.start_ms, std::move(k));
    }
    size_t total = 0, hit = 0, j = 0;
    for (const auto& line : local.lines) {
        const std::wstring k = LineKey(line.full_text);
        if (k.empty()) continue;
        ++total;
        // 两边都按时间有序:窗口 [t-1s, t+1s] 随 t 单调右移
        while (j < other.size() && other[j].first + 1000 < line.start_ms) ++j;
        for (size_t i = j; i < other.size() && other[i].first <= line.start_ms + 1000; ++i) {
            if (SameLine(k, other[i].second)) { ++hit; break; }
        }
    }
    return total ? static_cast<double>(hit) / static_cast<double>(total) : 0.0;
}

// 搜索词:标签缺失时用「歌手 - 歌名」文件名,去掉音轨号与占位歌手
static bool BuildOnlineQuery(const std::wstring& audioFilePath, const std::wstring& title, const std::wstring& artist,
                             std::wstring& qTitle, std::wstring& qArtist) {
    qTitle = title;
    qArtist = artist;
    if (qTitle.empty() && !audioFilePath.empty())
        SplitArtistTitle(StripDecorations(FileStem(audioFilePath)), qArtist, qTitle);
    qTitle = StripTrackNumber(qTitle);
    Trim(qTitle);
    Trim(qArtist);
    if (IsPlaceholderArtist(qArtist)) qArtist.clear();
    return !qTitle.empty();
}

// 多源聚合在线拉取:依次查 LRCLIB / 网易云 / 酷狗,统一排名后按名次拉取,结果落缓存。
// 与旧的 FetchOnline(仅酷狗)相比,冷门和外文曲目也有兜底。
// 外语歌第一名没有翻译时,再往下试几份歌名对得上、时间轴与第一名一致的,有翻译就用它
bool LyricManager::FetchOnlineMulti(const std::wstring& audioFilePath, const std::wstring& title,
                                    const std::wstring& artist, uint32_t durationMs, ParsedLyrics& outLyrics,
                                    LyricCandidate* picked) {
    std::wstring qTitle, qArtist;
    if (!BuildOnlineQuery(audioFilePath, title, artist, qTitle, qArtist)) return false;

    const std::wstring key = L"multi\n" + Normalize(qArtist) + L"\n" + Normalize(qTitle) + L"\n" + std::to_wstring(durationMs / 1000);
    HttpGetFn providerHttp;
    std::wstring cacheDir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (online_misses_.count(key)) return false;
        // 复用 http_ (若外部注入过),否则用 LyricProvider 默认的 WinHTTP
        providerHttp = http_ ? http_ : HttpGetFn(&LyricProvider::DefaultHttpGet);
        cacheDir = cache_dir_;
    }

    LyricProvider provider(providerHttp);
    LyricQuery q;
    q.title = qTitle;
    q.artist = qArtist;
    q.duration_ms = durationMs;

    const LyricSearchResult all = provider.SearchAll(q);
    RawLyricDoc doc;
    LyricCandidate chosen;
    ParsedLyrics parsed;
    bool found = false;
    int tried = 0;
    for (const auto& c : all.candidates) {
        if (!c.title_match) continue; // 自动挑选只认歌名对得上的,宁可没有也不要同一歌手的另一首歌
        if (++tried > 4) break;
        RawLyricDoc d;
        ParsedLyrics p;
        if (!provider.Fetch(c, d) || d.kind == RawLyricDoc::Payload::Plain || !ParseDoc(d, p)) continue;
        if (!found) {
            doc = std::move(d);
            chosen = c;
            parsed = std::move(p);
            found = true;
            if (!NeedsTranslation(parsed)) break;
            continue;
        }
        if (p.HasTranslation() && !NeedsTranslation(p) && TimelineAgreement(parsed, p) >= 0.6) {
            doc = std::move(d);
            chosen = c;
            parsed = std::move(p);
            break;
        }
    }
    if (!found) {
        // 各源有应答却没有歌名对得上的候选,才记为查不到;都没应答多半是断网,下次播放再试
        bool anyMatch = false;
        for (const auto& c : all.candidates) anyMatch = anyMatch || c.title_match;
        if (all.answered > 0 && !anyMatch) {
            std::lock_guard<std::mutex> lock(mutex_);
            online_misses_.insert(key);
        }
        return false;
    }
    if (parsed.title.empty()) parsed.title = qTitle;
    if (parsed.artist.empty()) parsed.artist = qArtist;

    // 写缓存:文件名用查询的歌名 / 歌手 (而非歌词里的标签),这样下次 LoadLyricsForTrack 按同样的名字一定命中。
    // 酷狗 KRC 原样存 .krc;其他源存带翻译的标准 .lrc
    if (!cacheDir.empty()) {
        const std::wstring name = SafeFileName(qArtist.empty() ? qTitle : qArtist + L" - " + qTitle);
        if (!name.empty()) {
            const std::wstring sep = cacheDir.back() == L'/' || cacheDir.back() == L'\\' ? L"" : L"/";
            const bool krc = doc.kind == RawLyricDoc::Payload::Krc;
            const bool wrote = krc ? WriteWholeFile(cacheDir + sep + name + L".krc", doc.raw_payload)
                                   : ExportLrcFile(parsed, cacheDir + sep + name + L".lrc");
            if (wrote) {
                std::lock_guard<std::mutex> lock(mutex_);
                index_built_at_ = 0; // 下次查找时把新缓存收进索引
            }
        }
    }
    if (picked) *picked = chosen;
    outLyrics = std::move(parsed);
    return true;
}

// 翻译缓存放在在线缓存目录旁边的「<缓存目录>_translated」:不进歌词库索引 (文件名里带来源),
// 「<歌手 - 歌名>#<来源>.krc|.lrc」是找到的版本,misses.txt 记着确认没有翻译的曲目与时间
std::wstring LyricManager::TranslationDirLocked() const {
    if (cache_dir_.empty()) return {};
    std::wstring d = cache_dir_;
    while (!d.empty() && (d.back() == L'/' || d.back() == L'\\')) d.pop_back();
    return d + L"_translated";
}

void LyricManager::LoadTranslationMissesLocked() {
    if (tr_misses_loaded_) return;
    tr_misses_loaded_ = true;
    const std::wstring dir = TranslationDirLocked();
    if (dir.empty()) return;
    std::istringstream iss(ReadSmallFile(dir + L"/misses.txt"));
    std::string row;
    const int64_t now = WallSeconds();
    while (std::getline(iss, row)) {
        const size_t tab = row.rfind('\t');
        if (tab == std::string::npos) continue;
        const int64_t at = std::atoll(row.c_str() + tab + 1);
        if (now - at < kTranslationMissTtl) tr_misses_[Utf8ToWide(row.substr(0, tab))] = at;
    }
}

void LyricManager::SaveTranslationMissesLocked() {
    const std::wstring dir = TranslationDirLocked();
    if (dir.empty()) return;
    std::string data;
    for (const auto& kv : tr_misses_) data += WideToUtf8(kv.first) + "\t" + std::to_string(kv.second) + "\n";
    platform::CreateDirectories(dir);
    WriteWholeFile(dir + L"/misses.txt", data);
}

bool LyricManager::FindTranslatedVersion(const ParsedLyrics& local, const std::wstring& audioFilePath,
                                         const std::wstring& title, const std::wstring& artist, uint32_t durationMs,
                                         ParsedLyrics& outLyrics, LyricCandidate* picked, bool allowNetwork) {
    if (!NeedsTranslation(local)) return false;
    std::wstring qTitle, qArtist;
    if (!BuildOnlineQuery(audioFilePath, title, artist, qTitle, qArtist)) return false;
    const bool localTimed = Timed(local);
    // 本地是纯文本 (没有时间轴) 时没法比对,带时间轴、带翻译的就比它好
    auto fits = [&](const ParsedLyrics& p) { return p.HasTranslation() && !NeedsTranslation(p) && (!localTimed || TimelineAgreement(local, p) >= 0.6); };

    const std::wstring missKey = Normalize(qArtist) + L"\x1f" + Normalize(qTitle) + L"\x1f" + std::to_wstring(durationMs / 1000);
    const std::wstring name = SafeFileName(qArtist.empty() ? qTitle : qArtist + L" - " + qTitle);
    HttpGetFn providerHttp;
    std::wstring dir;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        dir = TranslationDirLocked();
        LoadTranslationMissesLocked();
        providerHttp = http_ ? http_ : HttpGetFn(&LyricProvider::DefaultHttpGet);
    }

    // 1. 之前找到过:直接用 (本地歌词改过、对不上了就重新找)
    if (!dir.empty() && !name.empty()) {
        for (const char* src : {"kugou", "netease", "lrclib"}) {
            for (const wchar_t* ext : {L".krc", L".lrc"}) {
                const std::wstring path = dir + L"/" + name + L"#" + Utf8ToWide(src) + ext;
                ParsedLyrics cached;
                if (!platform::PathExists(path) || !KrcDecoder::DecodeFile(path, cached) || !fits(cached)) continue;
                if (picked) {
                    *picked = LyricCandidate();
                    picked->source = src;
                    picked->title = cached.title.empty() ? qTitle : cached.title;
                    picked->artist = cached.artist.empty() ? qArtist : cached.artist;
                }
                outLyrics = std::move(cached);
                return true;
            }
        }
    }
    // 2. 近期确认过没有
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!allowNetwork || tr_misses_.count(missKey)) return false;
    }

    // 3. 多源搜索,按排名试歌名对得上的候选,取第一份带翻译且时间轴对得上的
    LyricProvider provider(providerHttp);
    LyricQuery q;
    q.title = qTitle;
    q.artist = qArtist;
    q.duration_ms = durationMs;
    const LyricSearchResult all = provider.SearchAll(q);
    int tried = 0;
    bool fetchFailed = false;
    for (const auto& c : all.candidates) {
        if (!c.title_match) continue;
        if (++tried > 4) break;
        RawLyricDoc d;
        ParsedLyrics p;
        if (!provider.Fetch(c, d)) { fetchFailed = true; continue; }
        if (d.kind == RawLyricDoc::Payload::Plain || !ParseDoc(d, p) || !fits(p)) continue;
        if (p.title.empty()) p.title = qTitle;
        if (p.artist.empty()) p.artist = qArtist;
        if (!dir.empty() && !name.empty()) {
            platform::CreateDirectories(dir);
            const std::wstring base = dir + L"/" + name + L"#" + Utf8ToWide(c.source);
            if (d.kind == RawLyricDoc::Payload::Krc) WriteWholeFile(base + L".krc", d.raw_payload);
            else ExportLrcFile(p, base + L".lrc");
        }
        if (picked) *picked = c;
        outLyrics = std::move(p);
        return true;
    }
    // 至少两个源有正常应答、拉取也没出错,才算确认没有 (单个源连不上不影响判断);
    // 只有一个源或都没应答多半是断网 / 接口抖动,下次再试
    if (all.answered >= 2 && !fetchFailed) {
        std::lock_guard<std::mutex> lock(mutex_);
        tr_misses_[missKey] = WallSeconds();
        SaveTranslationMissesLocked();
    }
    return false;
}

bool LyricManager::ExportLrcFile(const ParsedLyrics& lyrics, const std::wstring& targetLrcPath) {
    if (lyrics.IsEmpty() || targetLrcPath.empty()) return false;
    return WriteWholeFile(targetLrcPath, "\xEF\xBB\xBF" + lyrics.ToStandardLrc(true));
}

} // namespace lyric_core
