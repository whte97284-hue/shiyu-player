#include "../include/cue_sheet.hpp"
#include "../include/audio_tag_parser.hpp"
#include "../include/platform_io.hpp"
#include <cwctype>

namespace {

// 拆分一行:关键字 + 参数 (支持双引号包裹的含空格参数)
std::vector<std::wstring> Tokenize(const std::wstring& line) {
    std::vector<std::wstring> out;
    size_t i = 0, n = line.size();
    while (i < n) {
        while (i < n && std::iswspace(static_cast<wint_t>(line[i]))) ++i;
        if (i >= n) break;
        std::wstring tok;
        if (line[i] == L'"') {
            ++i;
            while (i < n && line[i] != L'"') tok += line[i++];
            if (i < n) ++i;
        } else {
            while (i < n && !std::iswspace(static_cast<wint_t>(line[i]))) tok += line[i++];
        }
        out.push_back(tok);
    }
    return out;
}

std::wstring Upper(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towupper(static_cast<wint_t>(ch)));
    return s;
}

// "mm:ss:ff" (ff = 1/75 秒) -> 毫秒
bool ParseMsf(const std::wstring& s, uint32_t& ms) {
    unsigned m = 0, sec = 0, f = 0;
    int part = 0;
    unsigned* fields[3] = {&m, &sec, &f};
    bool any = false;
    for (wchar_t ch : s) {
        if (ch == L':') {
            if (++part > 2) return false;
            continue;
        }
        if (ch < L'0' || ch > L'9') return false;
        *fields[part] = *fields[part] * 10 + static_cast<unsigned>(ch - L'0');
        any = true;
    }
    if (!any || part != 2 || sec >= 60 || f >= 75) return false;
    ms = static_cast<uint32_t>((static_cast<uint64_t>(m) * 60 * 75 + sec * 75 + f) * 1000 / 75);
    return true;
}

std::wstring Rest(const std::vector<std::wstring>& t, size_t from) {
    std::wstring s;
    for (size_t i = from; i < t.size(); ++i) {
        if (i > from) s += L' ';
        s += t[i];
    }
    return s;
}

} // namespace

bool CueSheetParser::ParseFile(const std::wstring& path, CueSheet& out) {
    platform::ReadOnlyFile f;
    if (!f.Open(path, false) || f.Size() == 0 || f.Size() > (4u << 20)) return false;
    std::string bytes(static_cast<size_t>(f.Size()), '\0');
    if (f.ReadAt(0, &bytes[0], bytes.size()) != bytes.size()) return false;
    return ParseText(bytes, out);
}

bool CueSheetParser::ParseText(const std::string& bytes, CueSheet& out) {
    std::wstring text = AudioTagParser::DecodeTextAuto(bytes);
    CueFileEntry* curFile = nullptr;
    CueTrack* curTrack = nullptr;

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find_first_of(L"\r\n", pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (line.empty()) continue;
        std::vector<std::wstring> t = Tokenize(line);
        if (t.empty()) continue;
        std::wstring kw = Upper(t[0]);

        if (kw == L"FILE" && t.size() >= 2) {
            out.files.emplace_back();
            curFile = &out.files.back();
            curFile->file_name = t[1];
            curTrack = nullptr;
        } else if (kw == L"TRACK" && t.size() >= 2 && curFile) {
            CueTrack tr;
            tr.number = std::wcstol(t[1].c_str(), nullptr, 10);
            curFile->tracks.push_back(tr);
            curTrack = &curFile->tracks.back();
        } else if (kw == L"TITLE" && t.size() >= 2) {
            (curTrack ? curTrack->title : out.title) = Rest(t, 1);
        } else if (kw == L"PERFORMER" && t.size() >= 2) {
            (curTrack ? curTrack->performer : out.performer) = Rest(t, 1);
        } else if (kw == L"ISRC" && t.size() >= 2 && curTrack) {
            curTrack->isrc = t[1];
        } else if (kw == L"INDEX" && t.size() >= 3 && curTrack) {
            uint32_t ms = 0;
            if (std::wcstol(t[1].c_str(), nullptr, 10) == 1 && ParseMsf(t[2], ms)) curTrack->start_ms = ms;
        } else if (kw == L"REM" && t.size() >= 3) {
            std::wstring sub = Upper(t[1]);
            if (sub == L"GENRE") out.genre = Rest(t, 2);
            else if (sub == L"DATE") out.date = Rest(t, 2);
        }
    }

    // 计算每轨结束时间 = 同文件下一轨的起点
    size_t total = 0;
    for (auto& f : out.files) {
        for (size_t i = 0; i < f.tracks.size(); ++i) {
            f.tracks[i].end_ms = (i + 1 < f.tracks.size()) ? f.tracks[i + 1].start_ms : 0;
        }
        total += f.tracks.size();
    }
    return total > 0;
}
