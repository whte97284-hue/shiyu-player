// 查重宿主测试: 规范化 / 逐字节相同 / 只差标签 (MP3 去标签、FLAC PCM MD5) / 同一首不同音质 / 版本 / 文件名匹配
//               / 自动勾选规则 / 隔离 + 歌词改名 + 撤销 / 废纸篓 / 硬链接 / 永久删除 / 安全检查
// 用法: test_dedupe_host <work_dir>
#include "../../include/audio_tag_parser.hpp"
#include "../../include/library_dedupe.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

using namespace dedupe;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static std::wstring W(const fs::path& p) { return AudioTagParser::Utf8ToWide(p.string()); }
static std::mt19937 g_rng(7);

static std::string Random(size_t n) {
    std::string s(n, '\0');
    for (auto& c : s) c = static_cast<char>(g_rng());
    return s;
}
static void Put(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o << content;
}
static std::string Get(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
}

static std::string Id3v2(const std::string& body) {
    std::string h = "ID3";
    h += '\x03';
    h += '\x00';
    h += '\x00';
    size_t n = body.size();
    h += static_cast<char>((n >> 21) & 0x7F);
    h += static_cast<char>((n >> 14) & 0x7F);
    h += static_cast<char>((n >> 7) & 0x7F);
    h += static_cast<char>(n & 0x7F);
    return h + body;
}
static std::string Id3v1(const std::string& title) {
    std::string t = "TAG" + title;
    t.resize(128, '\0');
    return t;
}
static std::string Flac(const std::string& md5, const std::string& rest) {
    std::string s = "fLaC";
    s += '\x80'; // 最后一个元数据块 + STREAMINFO
    s += '\x00';
    s += '\x00';
    s += '\x22';
    std::string si(34, '\x01');
    si.replace(18, 16, md5);
    return s + si + rest;
}

static DupTrack T(const fs::path& p, const std::wstring& artist, const std::wstring& title, uint32_t ms, bool lossless = false,
                  int kbps = 320, int bits = 0, int rate = 44100) {
    DupTrack t;
    t.path = W(p);
    t.artist = artist;
    t.title = title;
    t.duration_ms = ms;
    t.lossless = lossless;
    t.bitrate_kbps = kbps;
    t.bits_per_sample = bits;
    t.sample_rate = rate;
    return t;
}

static const DupGroup* FindGroup(const FindResult& r, MatchKind k, const std::wstring& anyPath, const std::vector<DupTrack>& tr) {
    for (const auto& g : r.groups) {
        if (g.kind != k) continue;
        for (const auto& m : g.members)
            if (tr[m.track].path == anyPath) return &g;
    }
    return nullptr;
}
static bool Kept(const DupGroup& g, const std::vector<DupTrack>& tr, const std::wstring& path) {
    for (const auto& m : g.members)
        if (tr[m.track].path == path) return m.keep;
    return false;
}
static size_t GroupsContaining(const FindResult& r, const std::vector<DupTrack>& tr, const std::wstring& path, bool skipVersions) {
    size_t n = 0;
    for (const auto& g : r.groups) {
        if (skipVersions && g.kind == MatchKind::Versions) continue;
        for (const auto& m : g.members) n += tr[m.track].path == path;
    }
    return n;
}

static void TestNormalize() {
    std::printf("[1] 规范化\n");
    CHECK(NormalizeArtist(L"周杰伦、费玉清") == NormalizeArtist(L"费玉清 / 周杰伦"));
    CHECK(NormalizeArtist(L"Jay Chou & Ｆei") == NormalizeArtist(L"fei,jay chou"));
    CHECK(NormalizeArtist(L"未知艺术家").empty() && NormalizeArtist(L"").empty() && NormalizeArtist(L"Unknown Artist").empty());
    CHECK(NormalizeTitle(L"01. 东风破 [FLAC]") == NormalizeTitle(L"东风破"));
    CHECK(NormalizeTitle(L"05东风破") == NormalizeTitle(L"东风破"));
    CHECK(NormalizeTitle(L"千里之外 (feat. 费玉清)") == NormalizeTitle(L"千里之外"));
    CHECK(NormalizeTitle(L"Song ft. Someone") == NormalizeTitle(L"song"));
    CHECK(NormalizeTitle(L"东风破 (Live)") != NormalizeTitle(L"东风破"));
    CHECK(NormalizeTitle(L"东风破 - 伴奏") != NormalizeTitle(L"东风破"));
    CHECK(NormalizeTitle(L"ＡＢＣ　Ｄ") == NormalizeTitle(L"abc-d"));
    CHECK(NormalizeTitle(L"17岁") == L"17岁");
}

static void TestFind(const fs::path& work) {
    std::printf("[2] 查找:五种匹配\n");
    const fs::path lib = work / "lib", dl = work / "download";
    const std::string bytes = Random(300 << 10);
    Put(lib / "周杰伦" / "七里香.mp3", bytes);
    Put(dl / "七里香 (1).mp3", bytes);
    Put(dl / "same-size-other.mp3", Random(300 << 10));  // 大小相同、内容不同
    // 只差标签的 MP3
    const std::string frames = Random(200 << 10);
    Put(lib / "a" / "晴天.mp3", Id3v2(std::string(500, 'A')) + frames + Id3v1("Qing Tian"));
    Put(dl / "晴天-tagged.mp3", Id3v2(std::string(2000, 'B')) + frames);
    // PCM MD5 相同的 FLAC (不同压缩参数 → 文件不同)
    const std::string md5 = Random(16);
    Put(lib / "b" / "稻香.flac", Flac(md5, Random(1000)));
    Put(dl / "稻香 (2).flac", Flac(md5, Random(5000)));
    // 同一首不同音质 + 现场版
    Put(lib / "c" / "东风破.flac", Random(4000));
    Put(dl / "东风破.mp3", Random(1500));
    Put(dl / "东风破 Live.mp3", Random(1600));
    Put(dl / "东风破 (Live).flac", Random(1700));
    // 无歌手标签:按文件名
    Put(dl / "x" / "无名歌.wav", Random(2000));
    Put(dl / "y" / "01 无名歌.wav", Random(2100));
    Put(dl / "z" / "别的歌.wav", Random(2200));
    // 时长未知不参与
    Put(dl / "unknown-dur.mp3", Random(900));

    std::vector<DupTrack> tr = {
        T(lib / "周杰伦" / "七里香.mp3", L"周杰伦", L"七里香", 299000),           // 0
        T(dl / "七里香 (1).mp3", L"周杰伦", L"七里香", 299000),                   // 1
        T(dl / "same-size-other.mp3", L"某人", L"别的", 100000),                // 2
        T(lib / "a" / "晴天.mp3", L"周杰伦", L"晴天", 269000),                    // 3
        T(dl / "晴天-tagged.mp3", L"周杰伦", L"晴天", 269000),                    // 4
        T(lib / "b" / "稻香.flac", L"周杰伦", L"稻香", 223000, true, 900, 16),     // 5
        T(dl / "稻香 (2).flac", L"周杰伦", L"稻香", 223000, true, 950, 16),        // 6
        T(lib / "c" / "东风破.flac", L"周杰伦", L"东风破", 315000, true, 1000, 24, 96000),  // 7
        T(dl / "东风破.mp3", L"周杰伦", L"05东风破", 313500, false, 320),           // 8
        T(dl / "东风破 Live.mp3", L"周杰伦", L"东风破", 352000, false, 320),        // 9:标题无版本词但时长差得多
        T(dl / "东风破 (Live).flac", L"周杰伦", L"东风破 (Live)", 353000, true),    // 10:标题带 Live
        T(dl / "x" / "无名歌.wav", L"未知艺术家", L"", 200000, true),               // 11
        T(dl / "y" / "01 无名歌.wav", L"", L"", 201000, true),                      // 12
        T(dl / "z" / "别的歌.wav", L"", L"", 200500, true),                         // 13
        T(dl / "unknown-dur.mp3", L"周杰伦", L"七里香", 0),                       // 14
        T(lib / "周杰伦" / "七里香.mp3", L"周杰伦", L"七里香", 299000),           // 15:重复路径
    };
    tr[14].cue_track = false;
    FindOptions opt;
    opt.preferred_roots = {W(lib)};
    size_t progressCalls = 0;
    FindResult r = FindDuplicates(tr, opt, [&](const FindProgress& p) {
        ++progressCalls;
        CHECK(p.done <= p.total);
    });
    CHECK(!r.cancelled && r.files_hashed >= 3 && progressCalls > 0);

    const DupGroup* id = FindGroup(r, MatchKind::Identical, tr[0].path, tr);
    CHECK(id && id->members.size() == 2 && !id->needs_review);
    CHECK(id && Kept(*id, tr, tr[0].path) && !Kept(*id, tr, tr[1].path));  // 首选目录 + 不像副本
    CHECK(id && id->reclaim_bytes == (300u << 10));
    CHECK(GroupsContaining(r, tr, tr[2].path, false) == 0);
    CHECK(GroupsContaining(r, tr, tr[1].path, false) == 1);  // 被 Identical 认领后不再出现在 SameSong

    const DupGroup* mp3 = FindGroup(r, MatchKind::SameAudio, tr[3].path, tr);
    CHECK(mp3 && mp3->members.size() == 2 && Kept(*mp3, tr, tr[3].path));
    const DupGroup* flac = FindGroup(r, MatchKind::SameAudio, tr[5].path, tr);
    CHECK(flac && flac->members.size() == 2 && Kept(*flac, tr, tr[5].path));

    const DupGroup* song = FindGroup(r, MatchKind::SameSong, tr[7].path, tr);
    CHECK(song && song->members.size() == 2 && song->needs_review);
    CHECK(song && Kept(*song, tr, tr[7].path) && !Kept(*song, tr, tr[8].path));  // 无损 24/96 优先
    CHECK(song && song->members[song->suggested].note.find(L"无损 24bit/96kHz") != std::wstring::npos);
    CHECK(!FindGroup(r, MatchKind::SameSong, tr[10].path, tr));  // 带 Live 的不归为原版
    const DupGroup* ver = FindGroup(r, MatchKind::Versions, tr[9].path, tr);
    CHECK(ver && ver->members.size() == 2 && Kept(*ver, tr, tr[9].path) && Kept(*ver, tr, tr[7].path));
    CHECK(ver && ver->reclaim_bytes == 0);

    const DupGroup* name = FindGroup(r, MatchKind::SameName, tr[11].path, tr);
    CHECK(name && name->members.size() == 2 && name->needs_review);
    CHECK(GroupsContaining(r, tr, tr[13].path, false) == 0);
    CHECK(GroupsContaining(r, tr, tr[14].path, false) == 0);

    // 规则切换
    std::vector<DupGroup> groups = r.groups;
    AutoSelect(groups, tr, KeepRule::Smallest);
    for (const auto& g : groups)
        if (g.kind == MatchKind::SameSong) CHECK(Kept(g, tr, tr[8].path));
    for (const auto& g : groups)
        if (g.kind == MatchKind::Versions)
            for (const auto& m : g.members) CHECK(m.keep);

    // 关掉宽松匹配
    FindOptions strict;
    strict.same_song = strict.same_name = strict.versions = false;
    FindResult rs = FindDuplicates(tr, strict);
    for (const auto& g : rs.groups) CHECK(g.kind == MatchKind::Identical || g.kind == MatchKind::SameAudio);
    std::atomic<bool> cancel{true};
    CHECK(FindDuplicates(tr, opt, nullptr, &cancel).cancelled);
}

static void TestApply(const fs::path& work) {
    std::printf("[3] 批量处理:隔离 + 歌词 + 撤销 / 废纸篓 / 硬链接 / 删除 / 安全检查\n");
    const fs::path lib = work / "lib2", dl = work / "dl2", quar = work / "quarantine";
    Put(lib / "c" / "东风破.flac", Random(4000));
    Put(dl / "album" / "东风破.mp3", Random(1500));
    Put(dl / "album" / "东风破.lrc", "[00:01]歌词");
    Put(dl / "album" / "东风破.zh.krc", "krc");
    Put(dl / "album" / "Thumbs.db", "junk");
    std::vector<DupTrack> tr = {T(lib / "c" / "东风破.flac", L"周杰伦", L"东风破", 315000, true, 1000, 24, 96000),
                                T(dl / "album" / "东风破.mp3", L"周杰伦", L"东风破", 314000)};
    FindResult r = FindDuplicates(tr);
    CHECK(r.groups.size() == 1);
    ApplyOptions ao;
    ao.action = DupAction::MoveToFolder;
    ao.quarantine_root = W(quar);
    ao.protect_roots = {W(dl)};
    const fs::path j1 = work / "dedupe1.log";
    ApplyResult a = ApplySelection(r.groups, tr, ao, W(j1));
    CHECK(a.done == 1 && a.failed == 0 && a.freed_bytes == 1500);
    CHECK(a.changes.size() == 1 && a.changes[0].keeper == tr[0].path && a.changes[0].removed == tr[1].path);
    CHECK(!fs::exists(dl / "album" / "东风破.mp3"));
    CHECK(fs::exists(fs::path(AudioTagParser::WideToUtf8(a.changes[0].moved_to))));
    CHECK(Get(lib / "c" / "东风破.lrc") == "[00:01]歌词");  // 歌词改名给保留的那份
    CHECK(Get(lib / "c" / "东风破.zh.krc") == "krc");
    CHECK(a.lyric_moves.size() == 2);
    CHECK(!fs::exists(dl / "album"));  // 只剩 Thumbs.db 的目录被清理
    CHECK(fs::exists(dl));             // 受保护的根目录保留
    ApplyResult u = UndoDedupe(W(j1));
    CHECK(u.failed == 0 && u.done == 3);
    CHECK(fs::exists(dl / "album" / "东风破.mp3") && Get(dl / "album" / "东风破.lrc") == "[00:01]歌词");
    CHECK(!fs::exists(lib / "c" / "东风破.lrc"));
    CHECK(UndoDedupe(W(j1)).failures.size() == 1);

    // 保留的那份已有歌词:被删那份的歌词跟着进隔离区
    Put(lib / "c" / "东风破.lrc", "keeper lyric");
    r = FindDuplicates(tr);
    a = ApplySelection(r.groups, tr, ao, W(work / "dedupe1b.log"));
    CHECK(a.done == 1 && Get(lib / "c" / "东风破.lrc") == "keeper lyric" && !fs::exists(dl / "album" / "东风破.lrc"));
    CHECK(UndoDedupe(W(work / "dedupe1b.log")).failed == 0 && fs::exists(dl / "album" / "东风破.lrc"));
    fs::remove(lib / "c" / "东风破.lrc");

    // 扫描后文件被改动 → 跳过
    r = FindDuplicates(tr);
    Put(dl / "album" / "东风破.mp3", Random(1501));
    a = ApplySelection(r.groups, tr, ao, W(work / "dedupe2.log"));
    CHECK(a.done == 0 && a.skipped == 1 && fs::exists(dl / "album" / "东风破.mp3"));

    // 一份都不留 → 跳过
    r = FindDuplicates(tr);
    for (auto& g : r.groups)
        for (auto& m : g.members) m.keep = false;
    a = ApplySelection(r.groups, tr, ao, W(work / "dedupe3.log"));
    CHECK(a.done == 0 && a.skipped == 2);

    // 只从曲库移除:磁盘不动
    r = FindDuplicates(tr);
    ApplyOptions lib_only;
    lib_only.action = DupAction::RemoveFromLibrary;
    a = ApplySelection(r.groups, tr, lib_only, W(work / "dedupe4.log"));
    CHECK(a.done == 1 && a.freed_bytes == 0 && fs::exists(dl / "album" / "东风破.mp3") && fs::exists(dl / "album" / "东风破.lrc"));

#ifndef _WIN32
    // 废纸篓 (freedesktop):放进 files/ 并写 .trashinfo,撤销时搬回
    const fs::path trash = work / "xdg";
    setenv("XDG_DATA_HOME", trash.string().c_str(), 1);
    r = FindDuplicates(tr);
    ApplyOptions bin;
    a = ApplySelection(r.groups, tr, bin, W(work / "dedupe5.log"));
    CHECK(a.done == 1 && !fs::exists(dl / "album" / "东风破.mp3"));
    CHECK(fs::exists(trash / "Trash" / "files" / "东风破.mp3"));
    CHECK(Get(trash / "Trash" / "info" / "东风破.mp3.trashinfo").find("Path=") != std::string::npos);
    CHECK(UndoDedupe(W(work / "dedupe5.log")).failed == 0);
    CHECK(fs::exists(dl / "album" / "东风破.mp3") && !fs::exists(trash / "Trash" / "files" / "东风破.mp3"));
    CHECK(!fs::exists(trash / "Trash" / "info" / "东风破.mp3.trashinfo"));
#endif

    // 硬链接:只对逐字节相同的文件
    const std::string bytes = Random(64 << 10);
    Put(lib / "h" / "a.mp3", bytes);
    Put(dl / "h" / "a copy.mp3", bytes);
    std::vector<DupTrack> tr2 = {T(lib / "h" / "a.mp3", L"甲", L"乙", 1000), T(dl / "h" / "a copy.mp3", L"甲", L"乙", 1000)};
    FindOptions fo;
    fo.preferred_roots = {W(lib)};
    FindResult hr = FindDuplicates(tr2, fo);
    CHECK(hr.groups.size() == 1 && hr.groups[0].kind == MatchKind::Identical && Kept(hr.groups[0], tr2, tr2[0].path));
    ApplyOptions link;
    link.action = DupAction::HardLinkToKeeper;
    a = ApplySelection(hr.groups, tr2, link, W(work / "dedupe6.log"));
    CHECK(a.done == 1 && fs::equivalent(lib / "h" / "a.mp3", dl / "h" / "a copy.mp3"));
    CHECK(FindDuplicates(tr2, fo).groups.empty());  // 已是硬链接,不再算重复
    CHECK(UndoDedupe(W(work / "dedupe6.log")).failed == 0);
    CHECK(!fs::equivalent(lib / "h" / "a.mp3", dl / "h" / "a copy.mp3") && Get(dl / "h" / "a copy.mp3") == bytes);
    // 不是逐字节相同的分组不能换硬链接
    a = ApplySelection(FindDuplicates(tr).groups, tr, link, W(work / "dedupe7.log"));
    CHECK(a.done == 0 && a.failed == 1);

    // 永久删除 (歌词保留的那份没有 → 改名过去)
    ApplyOptions del;
    del.action = DupAction::DeletePermanently;
    a = ApplySelection(FindDuplicates(tr).groups, tr, del, W(work / "dedupe8.log"));
    CHECK(a.done == 1 && !fs::exists(dl / "album" / "东风破.mp3") && fs::exists(lib / "c" / "东风破.lrc"));
    ApplyResult ud = UndoDedupe(W(work / "dedupe8.log"));
    CHECK(ud.skipped == 1 && !fs::exists(dl / "album" / "东风破.mp3"));  // 永久删除无法撤销,歌词改名可以
    CHECK(!fs::exists(lib / "c" / "东风破.lrc") && fs::exists(dl / "album" / "东风破.lrc"));

    // 查找只比头尾 + 中间抽样:只差一个未抽到的字节时会归为「完全相同」,
    // 但永久删除 / 换硬链接前逐字节确认,不会动它
    const std::string big = Random(2 << 20);
    std::string bigDiff = big;
    bigDiff[100000] ^= 0x5A;
    Put(lib / "s" / "x.flac", big);
    Put(dl / "s" / "x.flac", bigDiff);
    std::vector<DupTrack> tr3 = {T(lib / "s" / "x.flac", L"A", L"X", 200000, true), T(dl / "s" / "x.flac", L"A", L"X", 200000, true)};
    FindResult sr = FindDuplicates(tr3, fo);
    CHECK(sr.groups.size() == 1 && sr.groups[0].kind == MatchKind::Identical);
    a = ApplySelection(sr.groups, tr3, del, W(work / "dedupe9.log"));
    CHECK(a.done == 0 && a.failed == 1 && fs::exists(dl / "s" / "x.flac"));
    a = ApplySelection(sr.groups, tr3, link, W(work / "dedupe10.log"));
    CHECK(a.done == 0 && a.failed == 1 && Get(dl / "s" / "x.flac") == bigDiff);
    // 抽样到的位置不同 → 不归为完全相同
    bigDiff = big;
    bigDiff[(2 << 20) / 2] ^= 0x5A;
    Put(dl / "s" / "x.flac", bigDiff);
    CHECK(FindDuplicates(tr3, fo).groups.empty() || FindDuplicates(tr3, fo).groups[0].kind != MatchKind::Identical);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <work_dir>\n", argv[0]);
        return 2;
    }
    const fs::path work = fs::absolute(argv[1]);
    fs::remove_all(work);
    fs::create_directories(work);
    TestNormalize();
    TestFind(work);
    TestApply(work);
    if (g_fail) {
        std::printf("dedupe: %d 项失败\n", g_fail);
        return 1;
    }
    std::printf("dedupe: 全部通过\n");
    return 0;
}
