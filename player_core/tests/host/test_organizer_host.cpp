// 整理曲库宿主测试: 命名模板 / 冲突 / 附属文件 / 目录封面 / CUE 整轨 / 交换与链式改名 / 复制与撤销 / 取消 / 空目录清理
// 用法: test_organizer_host <work_dir>
#include "../../include/audio_tag_parser.hpp"
#include "../../include/library_organizer.hpp"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace organizer;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

static std::wstring W(const fs::path& p) { return AudioTagParser::Utf8ToWide(p.string()); }
static fs::path P(const std::wstring& w) { return fs::path(AudioTagParser::WideToUtf8(w)); }

static void Put(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream o(p, std::ios::binary);
    o << content;
}
static std::string Get(const fs::path& p) {
    std::ifstream i(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
}

static OrganizeTrack T(const fs::path& path, const std::wstring& artist, const std::wstring& album, const std::wstring& title,
                       int no, const std::wstring& date = L"") {
    OrganizeTrack t;
    t.path = W(path);
    t.artist = artist;
    t.album = album;
    t.title = title;
    t.track_no = no;
    t.date = date;
    return t;
}

static const OrganizeOp* FindOp(const OrganizePlan& p, const fs::path& from) {
    for (const auto& op : p.ops)
        if (op.from == W(from)) return &op;
    return nullptr;
}

static void TestPattern() {
    std::printf("[1] 命名模板\n");
    OrganizeOptions opt;
    OrganizeTrack t = T("/x/05东风破.wav", L"周杰伦", L"叶惠美", L"东风破", 5, L"2003-07-31");
    CHECK(FormatPattern(opt.pattern, t, opt) == L"周杰伦/2003-07-31 - 叶惠美/05 东风破");
    t.date.clear();
    CHECK(FormatPattern(opt.pattern, t, opt) == L"周杰伦/叶惠美/05 东风破");  // [] 内字段为空 → 整段省略
    t.disc_no = 2;
    t.disc_total = 2;
    CHECK(FormatPattern(opt.pattern, t, opt) == L"周杰伦/叶惠美/2-05 东风破");
    t.disc_total = 1;
    t.disc_no = 1;
    CHECK(FormatPattern(opt.pattern, t, opt) == L"周杰伦/叶惠美/05 东风破");  // 单碟不加「1-」
    t.album_artist = L"群星";
    CHECK(FormatPattern(opt.pattern, t, opt) == L"群星/叶惠美/05 东风破");

    OrganizeTrack bad = T("/x/a.mp3", L"AC/DC", L"Who? Me: \"Live\" <1>|", L"Vol...", 1);
    CHECK(FormatPattern(L"%artist%\\%album%\\%title%", bad, opt) == L"AC_DC/Who_ Me_ _Live_ _1__/Vol");
    opt.illegal = IllegalCharPolicy::FullWidth;
    CHECK(FormatPattern(L"%artist%", bad, opt) == L"AC／DC");
    opt.illegal = IllegalCharPolicy::Underscore;

    OrganizeTrack empty = T("/music/歌手文件夹/无标签.flac", L"", L"", L"", 0);
    CHECK(FormatPattern(opt.pattern, empty, opt) == L"未知/未知/无标签");  // 无标题用文件名,空目录用「未知」
    CHECK(FormatPattern(L"%directoryname%\\%filename%.%ext%", empty, opt) == L"歌手文件夹/无标签.flac");
    CHECK(FormatPattern(L"%artist%", T("/x/a.mp3", L"con", L"", L"", 0), opt) == L"con_");    // 保留名
    CHECK(FormatPattern(L"%artist%", T("/x/a.mp3", L"COM1.x", L"", L"", 0), opt) == L"COM1.x_");
    CHECK(FormatPattern(L"%artist%", T("/x/a.mp3", L"  Space . ", L"", L"", 0), opt) == L"Space");

    OrganizeTrack f = T("/x/a.mp3", L"Beyond", L"", L"海阔天空", 3, L"1993/05");
    CHECK(FormatPattern(L"$upper(%artist%) - $if3(%album%,%genre%,'无专辑')", f, opt) == L"BEYOND - 无专辑");
    CHECK(FormatPattern(L"$left(%artist%,3)$lower(%artist%)", f, opt) == L"Beybeyond");
    CHECK(FormatPattern(L"$replace(%title%,海阔,蓝)$num(%tracknumber%,3)", f, opt) == L"蓝天空003");
    CHECK(FormatPattern(L"%year%['['%genre%']']'it'''", f, opt) == L"1993it'");
    CHECK(FormatPattern(L"$trim($if2(%genre%,  x  ))", f, opt) == L"x");
    CHECK(FormatPattern(L"\\\\%artist%//%title%", f, opt) == L"Beyond/未知/海阔天空");  // 开头的分隔符丢弃,中间的空级用「未知」

    std::wstring err;
    CHECK(!ValidatePattern(L"%foo%", &err) && !err.empty());
    CHECK(!ValidatePattern(L"[%artist%", &err));
    CHECK(!ValidatePattern(L"$nope(%artist%)", &err));
    CHECK(!ValidatePattern(L"$if2(%artist%)", &err));
    CHECK(!ValidatePattern(L"%artist", &err));
    CHECK(!ValidatePattern(L"'abc", &err));
    CHECK(!ValidatePattern(L"a)b", &err));
    CHECK(!ValidatePattern(L"  ", &err));
    CHECK(ValidatePattern(opt.pattern, &err));

    std::wstring longTitle(400, L'长');
    std::wstring r = FormatPattern(L"%title%", T("/x/a.mp3", L"", L"", longTitle, 0), opt);
    CHECK(r.size() == opt.max_component);
}

static void TestMoveUndo(const fs::path& work) {
    std::printf("[2] 移动 + 附属文件 + 封面 + 清理空目录 + 撤销\n");
    const fs::path src = work / "src", lib = work / "lib";
    const fs::path album = src / "周杰伦 叶惠美";
    Put(album / "05东风破.wav", "wav-dongfengpo");
    Put(album / "05东风破.lrc", "lrc");
    Put(album / "05东风破.zh.krc", "krc");
    Put(album / "01以父之名.wav", "wav-yifuzhiming");
    Put(album / "cover.jpg", "jpg");
    Put(album / "Thumbs.db", "junk");
    Put(album / "notes.txt", "unrelated"); // 不是附属文件,留在原处 → 原目录不会被删

    OrganizeOptions opt;
    opt.target_root = W(lib);
    opt.protect_roots = {W(src)};
    std::vector<OrganizeTrack> tracks = {T(album / "05东风破.wav", L"周杰伦", L"叶惠美", L"东风破", 5, L"2003"),
                                         T(album / "01以父之名.wav", L"周杰伦", L"叶惠美", L"以父之名", 1, L"2003")};
    std::atomic<bool> stop{true};
    CHECK(BuildPlan(tracks, opt, nullptr, &stop).cancelled);  // 计算计划也能取消
    size_t planCalls = 0;
    OrganizePlan plan = BuildPlan(tracks, opt, [&](const OrganizeProgress& p) {
        ++planCalls;
        CHECK(p.done <= p.total);
    });
    CHECK(plan.ok() && !plan.cancelled && planCalls > 0);
    CHECK(plan.to_move == 2);
    const fs::path dst = lib / "周杰伦" / "2003 - 叶惠美";
    const OrganizeOp* art = FindOp(plan, album / "cover.jpg");
    CHECK(art && art->action == OpAction::Move && art->to == W(dst / "cover.jpg"));
    const OrganizeOp* lrc = FindOp(plan, album / "05东风破.lrc");
    CHECK(lrc && lrc->to == W(dst / "05 东风破.lrc"));
    const OrganizeOp* krc = FindOp(plan, album / "05东风破.zh.krc");
    CHECK(krc && krc->to == W(dst / "05 东风破.zh.krc"));
    CHECK(!FindOp(plan, album / "notes.txt"));

    const fs::path journal = work / "journal1.log";
    size_t progressCalls = 0;
    OrganizeResult res = Execute(plan, opt, W(journal), [&](const OrganizeProgress& p) {
        ++progressCalls;
        CHECK(p.done <= p.total);
    });
    CHECK(res.failed == 0 && res.moved == 5 && !res.cancelled);
    CHECK(progressCalls >= 5);
    CHECK(Get(dst / "05 东风破.wav") == "wav-dongfengpo");
    CHECK(Get(dst / "01 以父之名.wav") == "wav-yifuzhiming");
    CHECK(Get(dst / "05 东风破.lrc") == "lrc");
    CHECK(fs::exists(dst / "cover.jpg"));
    CHECK(!fs::exists(album / "05东风破.wav"));
    CHECK(fs::exists(album / "notes.txt")); // 目录里还有别的文件,不删
    CHECK(res.removed_dirs.empty());

    JournalInfo ji = InspectJournal(W(journal));
    CHECK(ji.exists && ji.finished && !ji.undone && ji.planned == 5 && ji.completed == 5);

    // 再算一次计划:全部已在目标位置
    std::vector<OrganizeTrack> again = {T(dst / "05 东风破.wav", L"周杰伦", L"叶惠美", L"东风破", 5, L"2003"),
                                        T(dst / "01 以父之名.wav", L"周杰伦", L"叶惠美", L"以父之名", 1, L"2003")};
    OrganizePlan plan2 = BuildPlan(again, opt);
    CHECK(plan2.unchanged == 2 && plan2.to_move == 0);
    for (const auto& op : plan2.ops) CHECK(op.action == OpAction::Skip);

    OrganizeResult u = Undo(W(journal));
    CHECK(u.failed == 0 && u.moved == 5);
    CHECK(Get(album / "05东风破.wav") == "wav-dongfengpo");
    CHECK(Get(album / "05东风破.zh.krc") == "krc");
    CHECK(fs::exists(album / "cover.jpg"));
    CHECK(!fs::exists(lib / "周杰伦")); // 整理时新建的目录已清掉
    CHECK(InspectJournal(W(journal)).undone);
    CHECK(Undo(W(journal)).failures.size() == 1); // 不能撤销两次

    // 去掉无关文件后再整理一次:原目录 (只剩 Thumbs.db) 被清理,受保护的 src 根目录保留
    fs::remove(album / "notes.txt");
    OrganizeResult res2 = Execute(BuildPlan(tracks, opt), opt, W(work / "journal1b.log"));
    CHECK(res2.failed == 0 && res2.moved == 5);
    CHECK(!fs::exists(album));
    CHECK(fs::exists(src));
    CHECK(res2.removed_dirs.size() == 1);
    OrganizeResult u2 = Undo(W(work / "journal1b.log"));
    CHECK(u2.failed == 0 && fs::exists(album / "cover.jpg") && fs::exists(album / "01以父之名.wav"));
}

static void TestFolderArtSplit(const fs::path& work) {
    std::printf("[3] 目录拆分到多处时封面复制\n");
    const fs::path d = work / "mixed";
    Put(d / "a.mp3", "A");
    Put(d / "b.mp3", "B");
    Put(d / "folder.jpg", "art");
    OrganizeOptions opt;
    opt.target_root = W(work / "lib3");
    OrganizePlan plan = BuildPlan({T(d / "a.mp3", L"甲", L"专辑一", L"a", 1), T(d / "b.mp3", L"乙", L"专辑二", L"b", 1)}, opt);
    size_t copies = 0;
    for (const auto& op : plan.ops)
        if (op.kind == OpKind::FolderArt) {
            CHECK(op.action == OpAction::Copy);
            ++copies;
        }
    CHECK(copies == 2);
    OrganizeResult r = Execute(plan, opt, W(work / "j3.log"));
    CHECK(r.failed == 0);
    CHECK(Get(work / "lib3" / "甲" / "专辑一" / "folder.jpg") == "art");
    CHECK(Get(work / "lib3" / "乙" / "专辑二" / "folder.jpg") == "art");
    CHECK(fs::exists(d / "folder.jpg")); // 原处保留
    CHECK(fs::exists(d));                // 还有 folder.jpg,目录不删

    // 目录里有一首不参与整理的歌:封面也不能带走
    const fs::path e = work / "partial";
    Put(e / "a.mp3", "A2");
    Put(e / "other.flac", "not in library");
    Put(e / "cover.png", "png");
    OrganizePlan p2 = BuildPlan({T(e / "a.mp3", L"甲", L"专辑三", L"a", 1)}, opt);
    const OrganizeOp* art = FindOp(p2, e / "cover.png");
    CHECK(art && art->action == OpAction::Copy);
}

static void TestConflicts(const fs::path& work) {
    std::printf("[4] 重复 / 重名 / 目标已存在\n");
    const fs::path d = work / "dups";
    Put(d / "x1.mp3", "SAME");
    Put(d / "x2.mp3", "SAME");
    Put(d / "x3.mp3", "DIFFERENT");
    const fs::path lib = work / "lib4";
    Put(lib / "歌手" / "专辑" / "02 已有.mp3", "EXISTING");
    Put(lib / "歌手" / "专辑" / "03 相同.mp3", "SAMEOLD");
    Put(d / "x4.mp3", "EXISTING-OTHER");
    Put(d / "x5.mp3", "SAMEOLD");
    OrganizeOptions opt;
    opt.target_root = W(lib);
    std::vector<OrganizeTrack> tr = {T(d / "x1.mp3", L"歌手", L"专辑", L"歌", 1), T(d / "x2.mp3", L"歌手", L"专辑", L"歌", 1),
                                     T(d / "x3.mp3", L"歌手", L"专辑", L"歌", 1), T(d / "x4.mp3", L"歌手", L"专辑", L"已有", 2),
                                     T(d / "x5.mp3", L"歌手", L"专辑", L"相同", 3), T(d / "x1.mp3", L"歌手", L"专辑", L"歌", 1),
                                     T(d / "missing.mp3", L"歌手", L"专辑", L"不存在", 9)};
    OrganizePlan plan = BuildPlan(tr, opt);
    const fs::path a = lib / "歌手" / "专辑";
    CHECK(FindOp(plan, d / "x1.mp3")->to == W(a / "01 歌.mp3"));
    CHECK(FindOp(plan, d / "x2.mp3")->state == OpState::Duplicate);
    CHECK(FindOp(plan, d / "x3.mp3")->to == W(a / "01 歌 (2).mp3"));
    CHECK(FindOp(plan, d / "x3.mp3")->state == OpState::Renamed);
    CHECK(FindOp(plan, d / "x4.mp3")->to == W(a / "02 已有 (2).mp3"));
    CHECK(FindOp(plan, d / "x5.mp3")->state == OpState::Duplicate);
    CHECK(FindOp(plan, d / "missing.mp3")->state == OpState::Error);
    CHECK(plan.duplicates == 2 && plan.renamed == 2 && plan.errors == 1 && plan.to_move == 3);
    size_t x1 = 0;
    for (const auto& op : plan.ops) x1 += op.from == W(d / "x1.mp3");
    CHECK(x1 == 1); // 同一文件只计划一次

    opt.duplicates = DuplicatePolicy::KeepBoth;
    OrganizePlan keep = BuildPlan(tr, opt);
    CHECK(FindOp(keep, d / "x2.mp3")->to == W(a / "01 歌 (2).mp3"));
    CHECK(FindOp(keep, d / "x3.mp3")->to == W(a / "01 歌 (3).mp3"));
    opt.duplicates = DuplicatePolicy::Skip;

    // 计划之后、执行之前目标处冒出同名文件:执行时自动换名,不覆盖
    Put(a / "01 歌.mp3", "RACE");
    OrganizeResult r = Execute(plan, opt, W(work / "j4.log"));
    CHECK(r.failed == 0 && r.moved == 3);
    CHECK(Get(a / "01 歌.mp3") == "RACE");
    CHECK(Get(a / "02 已有.mp3") == "EXISTING");
    CHECK(fs::exists(d / "x2.mp3")); // 重复的留在原处
    bool racedMoved = false;
    for (const auto& c : r.changes)
        if (c.from == W(d / "x1.mp3")) racedMoved = Get(P(c.to)) == "SAME" && c.to != W(a / "01 歌.mp3");
    CHECK(racedMoved);
}

static void TestSwapAndChain(const fs::path& work) {
    std::printf("[5] 原地改名:交换 / 链式 / 附属文件跟随\n");
    const fs::path d = work / "swap";
    Put(d / "A.mp3", "content-of-A");
    Put(d / "B.mp3", "content-of-B");
    Put(d / "A.lrc", "lrc-A");
    Put(d / "C.mp3", "content-of-C");
    Put(d / "D.mp3", "content-of-D");
    OrganizeOptions opt;
    opt.mode = Mode::RenameInPlace;
    opt.pattern = L"%title%";
    opt.protect_roots = {W(work)};
    // A ↔ B 交换;C → D,D → E (链式)
    OrganizePlan plan = BuildPlan({T(d / "A.mp3", L"", L"", L"B", 0), T(d / "B.mp3", L"", L"", L"A", 0),
                                   T(d / "C.mp3", L"", L"", L"D", 0), T(d / "D.mp3", L"", L"", L"E", 0)},
                                  opt);
    CHECK(FindOp(plan, d / "A.mp3")->via_temp && FindOp(plan, d / "B.mp3")->via_temp);
    CHECK(FindOp(plan, d / "C.mp3")->via_temp && !FindOp(plan, d / "D.mp3")->via_temp);
    CHECK(FindOp(plan, d / "A.mp3")->to == W(d / "B.mp3"));
    OrganizeResult r = Execute(plan, opt, W(work / "j5.log"));
    CHECK(r.failed == 0);
    CHECK(Get(d / "B.mp3") == "content-of-A");
    CHECK(Get(d / "A.mp3") == "content-of-B");
    CHECK(Get(d / "D.mp3") == "content-of-C");
    CHECK(Get(d / "E.mp3") == "content-of-D");
    CHECK(Get(d / "B.lrc") == "lrc-A");
    size_t temps = 0;
    for (auto& e : fs::directory_iterator(d)) temps += e.path().filename().string().rfind(".kporg-", 0) == 0;
    CHECK(temps == 0);
    JournalInfo ji = InspectJournal(W(work / "j5.log"));
    CHECK(ji.completed == 5 && ji.planned == 5);
    OrganizeResult u = Undo(W(work / "j5.log"));
    CHECK(u.failed == 0);
    CHECK(Get(d / "A.mp3") == "content-of-A" && Get(d / "B.mp3") == "content-of-B");
    CHECK(Get(d / "C.mp3") == "content-of-C" && Get(d / "D.mp3") == "content-of-D" && !fs::exists(d / "E.mp3"));
    CHECK(Get(d / "A.lrc") == "lrc-A" && !fs::exists(d / "B.lrc"));
}

static void TestCueImage(const fs::path& work) {
    std::printf("[6] 整轨 + CUE\n");
    const fs::path d = work / "cue";
    Put(d / "CDImage.flac", "flac-image");
    Put(d / "专辑.cue", "PERFORMER \"歌手\"\nTITLE \"专辑\"\nFILE \"CDImage.flac\" WAVE\n  TRACK 01 AUDIO\n    INDEX 01 00:00:00\n");
    Put(d / "CDImage.log", "eac log");
    OrganizeOptions opt;
    opt.target_root = W(work / "lib6");
    OrganizeTrack t = T(d / "CDImage.flac", L"歌手", L"专辑", L"", 0, L"1999");
    t.cue_image = true;
    OrganizePlan plan = BuildPlan({t, t}, opt);
    const fs::path dst = work / "lib6" / "歌手" / "1999 - 专辑";
    CHECK(FindOp(plan, d / "CDImage.flac")->to == W(dst / "CDImage.flac"));
    const OrganizeOp* cue = FindOp(plan, d / "专辑.cue");
    CHECK(cue && cue->to == W(dst / "专辑.cue"));
    const OrganizeOp* log = FindOp(plan, d / "CDImage.log");
    CHECK(log && log->to == W(dst / "CDImage.log"));
    OrganizeResult r = Execute(plan, opt, W(work / "j6.log"));
    CHECK(r.failed == 0 && r.moved == 3);
    CHECK(fs::exists(dst / "专辑.cue") && fs::exists(dst / "CDImage.flac"));
}

static void TestCopyCancelExclude(const fs::path& work) {
    std::printf("[7] 复制模式 / 撤销复制 / 取消 / 排除 / 路径长度\n");
    const fs::path d = work / "copy";
    Put(d / "s.mp3", std::string(3 << 20, 'x')); // 3MB:复制要分块
    Put(d / "s.lrc", "l");
    Put(d / "skip" / "t.mp3", "t");
    OrganizeOptions opt;
    opt.mode = Mode::Copy;
    opt.target_root = W(work / "lib7");
    opt.exclude_paths = {W(d / "skip")};
    std::vector<OrganizeTrack> tr = {T(d / "s.mp3", L"甲", L"乙", L"丙", 1), T(d / "skip" / "t.mp3", L"甲", L"乙", L"丁", 2)};
    OrganizePlan plan = BuildPlan(tr, opt);
    CHECK(plan.to_copy == 1 && plan.excluded == 1);
    CHECK(!plan.volumes.empty() && plan.volumes[0].bytes >= (3u << 20));
    OrganizeResult r = Execute(plan, opt, W(work / "j7.log"));
    CHECK(r.copied == 2 && r.failed == 0);
    const fs::path out = work / "lib7" / "甲" / "乙" / "01 丙.mp3";
    CHECK(fs::file_size(out) == (3u << 20) && fs::exists(d / "s.mp3"));
    CHECK(fs::last_write_time(out) == fs::last_write_time(d / "s.mp3"));
    for (const auto& c : r.changes) CHECK(c.copied);
    CHECK(fs::exists(d / "skip" / "t.mp3"));
    OrganizeResult u = Undo(W(work / "j7.log"));
    CHECK(u.failed == 0 && !fs::exists(out) && fs::exists(d / "s.mp3") && !fs::exists(work / "lib7" / "甲"));

    std::atomic<bool> cancel{true};
    OrganizeResult c = Execute(BuildPlan(tr, opt), opt, W(work / "j7c.log"), nullptr, &cancel);
    CHECK(c.cancelled && c.copied == 0 && !fs::exists(out));
    JournalInfo ji = InspectJournal(W(work / "j7c.log"));
    CHECK(ji.exists && ji.completed == 0);

    // 路径长度限制:先缩短文件名,再缩短目录
    opt.max_path = W(work / "lib7").size() + 40;
    OrganizePlan lp = BuildPlan({T(d / "s.mp3", std::wstring(60, L'甲'), std::wstring(60, L'乙'), std::wstring(80, L'丙'), 1)}, opt);
    const OrganizeOp* op = FindOp(lp, d / "s.mp3");
    CHECK(op && op->state != OpState::Error && op->to.size() <= opt.max_path);
    CHECK(!lp.warnings.empty());
    opt.max_path = 10;
    CHECK(FindOp(BuildPlan(tr, opt), d / "s.mp3")->state == OpState::Error);

    OrganizeOptions none;
    none.mode = Mode::Move;
    CHECK(BuildPlan(tr, none).ops.empty()); // 没有目标目录
    none.target_root = W(work / "x");
    none.pattern = L"%bogus%";
    OrganizePlan bp = BuildPlan(tr, none);
    CHECK(bp.errors == 2 && !bp.warnings.empty());
}

static void TestCancelResume(const fs::path& work) {
    std::printf("[8] 中途取消 → 继续整理 / 撤销\n");
    const fs::path d = work / "resume";
    std::vector<OrganizeTrack> tr;
    for (int i = 1; i <= 6; ++i) {
        const fs::path f = d / ("t" + std::to_string(i) + ".mp3");
        Put(f, "song" + std::to_string(i));
        tr.push_back(T(f, L"歌手", L"专辑", L"歌" + std::to_wstring(i), i));
    }
    OrganizeOptions opt;
    opt.target_root = W(work / "lib8");
    opt.protect_roots = {W(work)};
    std::atomic<bool> cancel{false};
    OrganizeResult r = Execute(BuildPlan(tr, opt), opt, W(work / "j8.log"), [&](const OrganizeProgress& p) {
        if (p.done >= 2) cancel = true;
    }, &cancel);
    CHECK(r.cancelled && r.moved == 2);
    JournalInfo ji = InspectJournal(W(work / "j8.log"));
    CHECK(ji.planned == 6 && ji.completed == 2);
    // 再算一次计划:已经搬过去的两首不在原处 (错误:源文件不存在),其余四首继续
    OrganizePlan rest = BuildPlan(tr, opt);
    CHECK(rest.to_move == 4 && rest.errors == 2);
    OrganizeResult r2 = Execute(rest, opt, W(work / "j8b.log"));
    CHECK(r2.moved == 4 && r2.failed == 0 && !fs::exists(d));
    // 两段分别撤销 (后做的先撤)
    CHECK(Undo(W(work / "j8b.log")).failed == 0);
    CHECK(Undo(W(work / "j8.log")).failed == 0);
    for (int i = 1; i <= 6; ++i) CHECK(Get(d / ("t" + std::to_string(i) + ".mp3")) == "song" + std::to_string(i));
    CHECK(!fs::exists(work / "lib8" / "歌手"));
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: %s <work_dir>\n", argv[0]);
        return 2;
    }
    const fs::path work = fs::absolute(argv[1]);
    fs::remove_all(work);
    fs::create_directories(work);
    TestPattern();
    TestMoveUndo(work);
    TestFolderArtSplit(work);
    TestConflicts(work);
    TestSwapAndChain(work);
    TestCueImage(work);
    TestCopyCancelExclude(work);
    TestCancelResume(work);
    if (g_fail) {
        std::printf("organizer: %d 项失败\n", g_fail);
        return 1;
    }
    std::printf("organizer: 全部通过\n");
    return 0;
}
