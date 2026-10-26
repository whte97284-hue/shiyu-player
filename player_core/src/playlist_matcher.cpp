#include "../include/playlist_matcher.hpp"

#include <algorithm>
#include <cwctype>
#include <tuple>
#include <unordered_map>

#include "../include/library_dedupe.hpp"
#include "../include/song_cleaner.hpp"

namespace playlist_core {

namespace {

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

bool HasVersionWord(const std::wstring& s) {
    const std::wstring l = Lower(s);
    for (const wchar_t* kw : {L"live", L"remix", L"inst", L"karaoke", L"acoustic", L"demo", L"cover", L"dj",
                              L"伴奏", L"纯音乐", L"现场", L"演唱会", L"版", L"remaster", L"edit", L"翻唱", L"钢琴", L"piano"})
        if (l.find(kw) != std::wstring::npos) return true;
    return false;
}

std::vector<std::wstring> SplitParts(const std::wstring& joined) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : joined) {
        if (c == L'|') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

struct Keys {
    std::wstring full, base;
    std::vector<std::wstring> artists;  // 已排序
};

Keys MakeKeys(const std::wstring& title, const std::wstring& artist) {
    Keys k;
    k.full = dedupe::NormalizeTitle(title);
    k.base = dedupe::NormalizeTitle(StripNoteBrackets(title));
    if (k.base.empty()) k.base = k.full;
    k.artists = SplitParts(dedupe::NormalizeArtist(artist));
    return k;
}

// 3 相同 / 2 有交集 / 1 有一边没有歌手 / 0 不同
int ArtistRelation(const std::vector<std::wstring>& a, const std::vector<std::wstring>& b) {
    if (a.empty() || b.empty()) return 1;
    if (a == b) return 3;
    for (const auto& x : a)
        if (std::binary_search(b.begin(), b.end(), x)) return 2;
    return 0;
}

using Quality = std::tuple<int, int, int, int>;
Quality QualityOf(const LocalTrack& t) {
    return Quality(t.lossless ? 1 : 0, t.bits_per_sample, std::min(t.sample_rate, 768000), t.bitrate_kbps);
}

} // namespace

std::wstring StripNoteBrackets(const std::wstring& title) {
    static const std::wstring kOpen = L"([【（《<", kClose = L")]】）》>";
    std::wstring out;
    size_t i = 0;
    while (i < title.size()) {
        const size_t o = kOpen.find(title[i]);
        if (o == std::wstring::npos) {
            out += title[i++];
            continue;
        }
        // 找配对的右括号 (按层数配对;各种右括号都接受,平台常混用全半角)
        size_t j = i + 1;
        for (int depth = 1; j < title.size(); ++j) {
            if (kOpen.find(title[j]) != std::wstring::npos) ++depth;
            else if (kClose.find(title[j]) != std::wstring::npos && --depth == 0) break;
        }
        if (j >= title.size()) {
            out += title.substr(i);
            break;
        }
        const std::wstring inner = title.substr(i + 1, j - i - 1);
        if (HasVersionWord(inner)) out += title.substr(i, j - i + 1);
        i = j + 1;
    }
    // 「歌名 - 电视剧《xx》插曲」:破折号后面是注释时去掉
    for (const wchar_t* sep : {L" - ", L" – ", L" — "}) {
        const size_t p = out.find(sep);
        if (p != std::wstring::npos && p > 0 && !HasVersionWord(out.substr(p))) out.erase(p);
    }
    return SongCleaner::Trim(out);
}

std::vector<TrackMatch> MatchToLibrary(const std::vector<PlaylistTrack>& tracks, const std::vector<LocalTrack>& library,
                                       const MatchOptions& opt, MatchSummary* summary) {
    std::vector<Keys> localKeys(library.size());
    std::unordered_map<std::wstring, std::vector<size_t>> byFull, byBase;
    for (size_t i = 0; i < library.size(); ++i) {
        const LocalTrack& t = library[i];
        std::wstring title = t.title, artist = t.artist;
        if (title.empty() && !t.file_name.empty()) {
            std::wstring ga, gt;
            if (SongCleaner::GuessFromFileName(t.file_name, ga, gt)) {
                title = gt;
                if (artist.empty()) artist = ga;
            } else {
                title = t.file_name;
                const size_t dot = title.rfind(L'.');
                if (dot != std::wstring::npos && dot > 0) title.erase(dot);
            }
        }
        localKeys[i] = MakeKeys(title, artist);
        if (localKeys[i].full.empty()) continue;
        byFull[localKeys[i].full].push_back(i);
        if (localKeys[i].base != localKeys[i].full) byBase[localKeys[i].base].push_back(i);
    }

    std::vector<TrackMatch> out(tracks.size());
    MatchSummary sum;
    for (size_t n = 0; n < tracks.size(); ++n) {
        const PlaylistTrack& p = tracks[n];
        const Keys pk = MakeKeys(p.title, p.artist);
        std::vector<size_t> cand;
        auto collect = [&](const std::unordered_map<std::wstring, std::vector<size_t>>& m, const std::wstring& key) {
            auto it = m.find(key);
            if (it != m.end()) cand.insert(cand.end(), it->second.begin(), it->second.end());
        };
        if (!pk.full.empty()) {
            collect(byFull, pk.full);
            collect(byFull, pk.base);
            collect(byBase, pk.base);
        }
        std::sort(cand.begin(), cand.end());
        cand.erase(std::unique(cand.begin(), cand.end()), cand.end());

        // 歌名相同但歌手全不同的:只有一份、时长几乎一样时才当作「可能」(中英文名的歌手)
        size_t foreignArtist = 0;
        for (size_t i : cand)
            if (ArtistRelation(pk.artists, localKeys[i].artists) == 0) ++foreignArtist;

        TrackMatch best;
        int bestRel = -1;
        Quality bestQ{};
        for (size_t i : cand) {
            const LocalTrack& l = library[i];
            const int rel = ArtistRelation(pk.artists, localKeys[i].artists);
            const bool durKnown = p.duration_ms > 0 && l.duration_ms > 0;
            const int diff = durKnown ? static_cast<int>(p.duration_ms > l.duration_ms ? p.duration_ms - l.duration_ms
                                                                                       : l.duration_ms - p.duration_ms)
                                      : -1;
            MatchLevel level = MatchLevel::Missing;
            const bool close = !durKnown || diff <= static_cast<int>(opt.tolerance_ms);
            const bool loose = durKnown && diff <= static_cast<int>(opt.loose_tolerance_ms);
            if (rel >= 2) level = close ? MatchLevel::Exact : loose ? MatchLevel::Likely : MatchLevel::Missing;
            else if (rel == 1) level = (close || loose) ? MatchLevel::Likely : MatchLevel::Missing;
            else if (foreignArtist == cand.size() && foreignArtist == 1 && durKnown && diff <= 1500)
                level = MatchLevel::Likely;
            if (level == MatchLevel::Missing) continue;

            const Quality q = QualityOf(l);
            const bool better = best.level == MatchLevel::Missing ||
                                std::make_tuple(static_cast<int>(level), rel, q, -diff) >
                                    std::make_tuple(static_cast<int>(best.level), bestRel, bestQ, -best.duration_diff_ms);
            if (better) {
                best.level = level;
                best.local = i;
                best.duration_diff_ms = diff;
                bestRel = rel;
                bestQ = q;
            }
        }
        out[n] = best;
        if (best.level == MatchLevel::Exact) ++sum.exact;
        else if (best.level == MatchLevel::Likely) ++sum.likely;
        else ++sum.missing;
    }
    if (summary) *summary = sum;
    return out;
}

} // namespace playlist_core
