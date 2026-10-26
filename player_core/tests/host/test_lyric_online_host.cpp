// 歌词内核宿主测试 (在线部分): KRC [language:] 中文翻译 / LRC 解析 / 酷狗在线接口 (模拟 HTTP) 与缓存
// 用法: test_lyric_online_host <work_dir>
#include "../../include/lyric_manager.hpp"
#include "../../include/lyric_provider.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace lyric_core;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

// Python 生成: 两行日文 KRC + [language:] (type 1 中文翻译, type 0 罗马音), zlib 压缩后按酷狗密钥异或, 带 krc1 头
static const unsigned char kKrcBlob[] = {107,114,99,49,56,219,52,231,159,124,182,7,65,176,222,17,73,149,124,222,133,21,9,221,87,134,124,28,64,21,25,0,235,204,24,0,41,240,141,225,216,96,30,250,32,197,145,252,117,149,211,240,88,216,161,224,127,142,243,156,7,85,173,238,40,77,151,14,38,248,173,227,102,248,117,191,216,173,251,57,99,70,120,8,174,205,170,66,15,180,154,59,212,126,249,62,209,23,208,235,176,203,42,253,28,29,53,217,250,142,168,60,174,131,108,223,105,70,121,91,99,129,98,84,141,11,124,193,20,10,217,172,43,53,32,45,55,109,31,86,246,113,98,86,202,57,62,134,132,23,82,21,137,0,214,78,73,90,65,21,106,234,210,141,61,42,13,17,35,40,234,66,101,180,182,161,24,127,254,68,239,47,81,126,182,48,106,86,58,162,75,241,145,190,254,55,148,208,211,105,219,192,77,246,151,116,130,130,123,183,166,7,61,139,125,22,60,8,64,160,105,225,99,34,72,145,7,191,189,36,161,74,196,19,7,231,174,213,36,21,225,57,254,251,49,103,33,122,184,78,125,137,93,177,222,232,94,127,77,33,184,105,71,51,206,56,186,59,93,68,110,153,227,49,96,240,141,227,153,188,92,245,136,97,164,176,53,135,172,172,44,124,20,98,97,221,190,86,139,177,245,208,155,225,184,9,97,60,160,57,212,15,230,201,12,248,174,76,233,64,17,249,189,15,17,209,171,46,231,70,235,24,133,106,4,83,169,37,193,246,213,40,253,68,208,177};

static std::string Base64(const std::string& in) {
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8 | (unsigned char)in[i + 2];
        out += a[v >> 18]; out += a[(v >> 12) & 63]; out += a[(v >> 6) & 63]; out += a[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16;
        out += a[v >> 18]; out += a[(v >> 12) & 63]; out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = (unsigned char)in[i] << 16 | (unsigned char)in[i + 1] << 8;
        out += a[v >> 18]; out += a[(v >> 12) & 63]; out += a[(v >> 6) & 63]; out += '=';
    }
    return out;
}

static void Touch(const fs::path& p, const std::string& data) {
    std::ofstream(p, std::ios::binary) << data;
}

int main(int argc, char** argv) {
    fs::path work = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "lyric_host";
    fs::remove_all(work);
        fs::create_directories(work / "cache");
    const std::string blob(reinterpret_cast<const char*>(kKrcBlob), sizeof(kKrcBlob));

    std::printf("[1] KRC 解密 + [language:] 翻译\n");
    {
        ParsedLyrics l;
        CHECK(KrcDecoder::DecodeMemory(kKrcBlob, sizeof(kKrcBlob), l));
        CHECK(l.lines.size() == 2);
        CHECK(l.title == L"Refrain" && l.artist == L"Aimer");
        if (l.lines.size() == 2) {
            CHECK(l.lines[0].start_ms == 6850 && l.lines[0].full_text == L"夏の午後に");
            CHECK(l.lines[0].syllables.size() == 3 && l.lines[0].syllables[1].start_ms == 7350);
            CHECK(l.lines[0].translation == L"在夏日的午后");
            CHECK(l.lines[1].translation == L"你的声音");
            CHECK(l.lines[0].romaji == L"natsu no");
        }
        CHECK(l.HasTranslation());
        std::string lrc = l.ToStandardLrc(true);
        CHECK(lrc.find("[00:06.85]夏の午後に\r\n[00:06.85]在夏日的午后\r\n") != std::string::npos);
        CHECK(l.ToStandardLrc(false).find("在夏日的午后") == std::string::npos);
    }

    std::printf("[2] LRC 解析 (多时间戳 / 双语同时间戳 / offset)\n");
    {
        ParsedLyrics l;
        std::string lrc = "\xEF\xBB\xBF[ti:梦良衣]\n[ar:ilem]\n[offset:-200]\n[00:04.00]作词：ilem\n"
                          "[00:10.5][01:00.120]副歌\n[00:10.5]chorus\n[00:20.00]\n[00:30.00]结尾\n";
        CHECK(KrcDecoder::ParseLrcText(lrc, l));
        CHECK(l.title == L"梦良衣" && l.artist == L"ilem" && l.offset_ms == -200);
        CHECK(l.lines.size() == 4);
        if (l.lines.size() == 4) {
            CHECK(l.lines[0].start_ms == 4000);
            CHECK(l.lines[1].start_ms == 10500 && l.lines[1].full_text == L"副歌" && l.lines[1].translation == L"chorus");
            CHECK(l.lines[1].duration_ms == 9500); // 到空白行 00:20 结束
            CHECK(l.lines[2].start_ms == 30000);
            CHECK(l.lines[3].start_ms == 60120 && l.lines[3].translation.empty());
        }
    }

    std::printf("[3] 酷狗在线接口 (模拟 HTTP) + 缓存\n");
    {
        int calls = 0;
        std::string lastSearch;
        bool krcFails = false;
        auto http = [&](const std::string& url, std::string& body) -> bool {
            ++calls;
            if (url.find("http://krcs.kugou.com/search?") == 0) {
                lastSearch = url;
                body = R"({"status":200,"errcode":200,"info":"OK","candidates":[)"
                       R"({"id":"1","accesskey":"AAAA","song":"Live","singer":"x","duration":300000},)"
                       R"({"id":116319439,"accesskey":"EA6F1950D581BFD6E9CD96FF5A238F53","song":"梦良衣","singer":"ilem","duration":185000}]})";
                return true;
            }
            if (url.find("http://krcs.kugou.com/download?") == 0) {
                if (url.find("id=116319439") == std::string::npos || url.find("accesskey=EA6F") == std::string::npos) return false;
                if (url.find("fmt=krc") != std::string::npos) {
                    if (krcFails) return false; // 主域名失败 -> 走备用域名, 备用也失败 -> 退回 lrc
                    body = std::string(R"({"status":200,"info":"OK","charset":"utf8","fmt":"krc","content":")") + Base64(blob) + "\"}";
                    return true;
                }
                body = std::string(R"({"status":200,"fmt":"lrc","content":")") + Base64("[ti:梦良衣]\n[00:04.00]作词：ilem\n") + "\"}";
                return true;
            }
            return false;
        };
        LyricManager mgr;
        mgr.SetCacheDirectory((work / "cache").wstring());
        mgr.SetHttpGetter(http);
        ParsedLyrics l;
        CHECK(mgr.FetchOnline(L"/music/a.flac", L"梦良衣", L"ilem", 184600, l));
        CHECK(lastSearch.find("keyword=ilem%20-%20%E6%A2%A6%E8%89%AF%E8%A1%A3") != std::string::npos);
        CHECK(lastSearch.find("duration=184600") != std::string::npos);
        CHECK(l.lines.size() == 2 && l.HasTranslation());
        CHECK(fs::exists(work / "cache" / fs::u8path("ilem - 梦良衣.krc")));
        // 缓存的 .krc 之后由本地匹配直接命中, 不再联网
        int before = calls;
        ParsedLyrics again;
        CHECK(mgr.LoadLyricsForTrack(L"/music/b.flac", L"梦良衣", L"ilem", again) && again.HasTranslation());
        CHECK(calls == before);

        krcFails = true;
        ParsedLyrics lrc;
        CHECK(mgr.FetchOnline(L"/music/c.flac", L"梦良衣", L"", 185000, lrc));
        CHECK(lrc.lines.size() == 1 && lrc.lines[0].full_text == L"作词：ilem");

        // 时长都对不上 -> 不采用, 并在本次运行中不重复请求
        ParsedLyrics none;
        CHECK(!mgr.FetchOnline(L"/music/d.flac", L"梦良衣", L"ilem", 60000, none));
        before = calls;
        CHECK(!mgr.FetchOnline(L"/music/d.flac", L"梦良衣", L"ilem", 60000, none));
        CHECK(calls == before);

        // 没有标签、文件名带曲目序号:按去掉序号的歌名搜索;以数字开头的歌名保持原样
        krcFails = false;
        ParsedLyrics numbered;
        mgr.FetchOnline(L"/music/01梦良衣.flac", L"", L"", 184600, numbered);
        CHECK(lastSearch.find("keyword=%E6%A2%A6%E8%89%AF%E8%A1%A3&") != std::string::npos);
        mgr.FetchOnline(L"/music/03. 梦良衣 (2).flac", L"", L"", 184000, numbered);
        CHECK(lastSearch.find("keyword=%E6%A2%A6%E8%89%AF%E8%A1%A3&") != std::string::npos);
        mgr.FetchOnline(L"/music/2002年的第一场雪.flac", L"", L"", 184000, numbered);
        CHECK(lastSearch.find("keyword=2002%E5%B9%B4") != std::string::npos);
        // 本地 .krc 匹配同样忽略序号 (上面缓存了「ilem - 梦良衣.krc」)
        ParsedLyrics local;
        CHECK(mgr.LoadLyricsForTrack(L"/music/x.flac", L"07 梦良衣", L"ilem", local));
    }

    std::printf("[3b] 歌手是占位符「未知艺术家」:先按歌名查歌手,再用文件夹名\n");
    {
        std::vector<std::string> searches;
        bool songApi = true;
        auto http = [&](const std::string& url, std::string& body) -> bool {
            if (url.find("mobilecdn.kugou.com/api/v3/search/song") != std::string::npos) {
                if (!songApi) return false;
                body = R"({"status":1,"data":{"info":[{"songname":"东风破","singername":"周杰伦","duration":315,"hash":"B24AB4"}]}})";
                return true;
            }
            if (url.find("http://krcs.kugou.com/search?") == 0) {
                searches.push_back(url);
                // 只有带上真实歌手才有候选 (与线上一致:「未知艺术家 - 东风破」「东风破」都是 0 个)
                if (url.find("%E5%91%A8%E6%9D%B0%E4%BC%A6%20-%20") == std::string::npos) {
                    body = R"({"status":200,"candidates":[]})";
                    return true;
                }
                body = R"({"status":200,"candidates":[{"id":"351585921","accesskey":"172C","song":"东风破","singer":"周杰伦","duration":315376}]})";
                return true;
            }
            if (url.find("http://krcs.kugou.com/download?") == 0) {
                body = std::string(R"({"status":200,"fmt":"lrc","content":")") + Base64("[00:01.00]一盏离愁\n") + "\"}";
                return url.find("fmt=lrc") != std::string::npos;
            }
            return false;
        };
        LyricManager mgr;
        mgr.SetHttpGetter(http);
        ParsedLyrics l;
        CHECK(mgr.FetchOnline(L"/m/周杰伦 叶惠美/05东风破.wav", L"05东风破", L"未知艺术家", 315308, l));
        CHECK(!searches.empty() && searches.back().find("hash=B24AB4") != std::string::npos);
        for (const auto& u : searches) CHECK(u.find("%E6%9C%AA%E7%9F%A5") == std::string::npos); // 不再搜「未知…」
        // 歌曲搜索不可用时,用文件夹名「周杰伦 叶惠美」里的「周杰伦」
        songApi = false;
        searches.clear();
        LyricManager mgr2;
        mgr2.SetHttpGetter(http);
        ParsedLyrics l2;
        CHECK(mgr2.FetchOnline(L"/m/周杰伦 叶惠美/05东风破.wav", L"05东风破", L"未知艺术家", 315308, l2));
        CHECK(l2.artist == L"周杰伦" || l2.lines.size() == 1);
    }

    std::printf("[3c] 多源聚合:只采用歌名对得上的候选,缓存 .lrc 后本地直接命中,查不到的不重复请求\n");
    {
        int calls = 0;
        auto http = [&](const std::string& url, std::string& body) -> bool {
            ++calls;
            if (url.find("lrclib.net/api/search") != std::string::npos) {
                // 同歌手的另一首歌排在前面 (时长更接近),不该被选中
                body = R"([{"id":11,"trackName":"晴天","artistName":"周杰伦","albumName":"叶惠美","duration":315.3,"syncedLyrics":"[00:01.00]故事的小黄花"},
                           {"id":12,"trackName":"东风破","artistName":"周杰伦","albumName":"叶惠美","duration":313.0,"syncedLyrics":"[00:02.00]一盏离愁"}])";
                return true;
            }
            if (url.find("lrclib.net/api/get/12") != std::string::npos) {
                body = R"({"syncedLyrics":"[00:02.00]一盏离愁\n[00:05.00]孤单窗前","plainLyrics":"一盏离愁"})";
                return true;
            }
            if (url.find("music.163.com/api/search") != std::string::npos) {
                body = R"({"result":{"songs":[{"id":7,"name":"东风破","artists":[{"name":"周杰伦"}],"album":{"name":"叶惠美"},"duration":313000}]}})";
                return true;
            }
            if (url.find("music.163.com/api/song/lyric") != std::string::npos) {
                body = R"({"lrc":{"lyric":"[00:02.00]一盏离愁\n[00:05.00]孤单窗前"},"tlyric":{"lyric":"[00:02.00]a parting sorrow\n[00:05.01]alone by the window"}})";
                return true;
            }
            return false; // 酷狗不可用
        };
        const fs::path cache = work / "multi_cache";
        fs::create_directories(cache);
        LyricManager mgr;
        mgr.SetCacheDirectory(cache.wstring());
        mgr.SetHttpGetter(http);
        ParsedLyrics l;
        LyricCandidate picked;
        CHECK(mgr.FetchOnlineMulti(L"/m/东风破.flac", L"东风破", L"周杰伦", 315000, l, &picked));
        CHECK(picked.title == L"东风破");
        CHECK(l.lines.size() == 2 && l.lines[0].full_text == L"一盏离愁");
        CHECK(fs::exists(cache / L"周杰伦 - 东风破.lrc"));
        // 缓存的 .lrc 进了本地索引:不联网也能载入
        calls = 0;
        ParsedLyrics again;
        CHECK(mgr.LoadLyricsForTrack(L"/m/东风破.flac", L"东风破", L"周杰伦", again));
        CHECK(again.lines.size() == 2 && calls == 0);
        // 网易云的翻译单独一份,按时间戳并入 (5.00 与 5.01 在容差内)
        RawLyricDoc doc;
        LyricProvider p(http);
        CHECK(p.Fetch(LyricCandidate{"netease", "7"}, doc, nullptr));
        ParsedLyrics nl;
        CHECK(LyricManager::ParseDoc(doc, nl) && nl.lines.size() == 2 && nl.lines[1].translation == L"alone by the window");
        // 纯文本歌词:无时间轴,按行切分
        RawLyricDoc plain;
        plain.kind = RawLyricDoc::Payload::Plain;
        plain.raw_payload = "第一行\r\n\r\n第二行";
        ParsedLyrics pl;
        CHECK(LyricManager::ParseDoc(plain, pl) && pl.lines.size() == 2 && pl.lines[1].full_text == L"第二行");
        // 一个歌名都对不上:不采用,且本次运行内不再请求
        ParsedLyrics none;
        CHECK(!mgr.FetchOnlineMulti(L"/m/七里香.flac", L"七里香", L"周杰伦", 300000, none));
        calls = 0;
        CHECK(!mgr.FetchOnlineMulti(L"/m/七里香.flac", L"七里香", L"周杰伦", 300000, none) && calls == 0);
        // 手动搜索 (loose):关键词不是歌名,候选全部保留
        LyricQuery lq;
        lq.title = L"叶惠美";
        lq.loose = true;
        CHECK(p.SearchAll(lq).candidates.size() >= 2);
    }

    std::printf("[3d] 外语歌补翻译:语种判断 / 时间轴一致性 / 带翻译的候选优先 / 缓存与「确认没有」\n");
    {
        // 本地 (网易云客户端下的同名 lrc):日文、没有翻译,前面有 JSON 署名行
        const std::string localLrc =
            "{\"t\":0,\"c\":[{\"tx\":\"作词: \"},{\"tx\":\"澤野弘之\"}]}\n"
            "[00:20.60]決めつけばかり 自惚れを着た\n[00:30.30]棚に隠した 哀れな\n"
            "[00:39.17]都合の傷だけひけらかして\n[00:48.88]どれだけ磨いでも気はやつれる\n";
        ParsedLyrics local;
        CHECK(KrcDecoder::ParseLrcText(localLrc, local) && local.lines.size() == 4);
        CHECK(LyricManager::IsForeignLyrics(local) && LyricManager::NeedsTranslation(local));
        // 中文歌里夹几句日文、英文:不算外语
        ParsedLyrics zh;
        KrcDecoder::ParseLrcText("[00:01.00]一盏离愁孤单伫立在窗口\n[00:05.00]我在门后假装你人还没走\n[00:09.00]ありがとう\n"
                                 "[00:12.00]旧地如重游月圆更寂寞\n[00:16.00]夜半清醒的烛火不忍苛责我\n", zh);
        CHECK(!LyricManager::IsForeignLyrics(zh));
        ParsedLyrics en;
        KrcDecoder::ParseLrcText("[00:01.00]Yesterday all my troubles seemed so far away\n[00:05.00]Now it looks as though they're here to stay\n", en);
        CHECK(LyricManager::IsForeignLyrics(en));
        // 已带翻译 (双语同时间戳) 的不用再找
        ParsedLyrics bi;
        KrcDecoder::ParseLrcText("[00:20.60]決めつけばかり 自惚れを着た\n[00:20.60]一味固执己见\n[00:30.30]棚に隠した 哀れな\n[00:30.30]藏在架子上\n"
                                 "[00:39.17]都合の傷だけひけらかして\n[00:39.17]只炫耀方便的伤\n", bi);
        CHECK(LyricManager::IsForeignLyrics(bi) && !LyricManager::NeedsTranslation(bi));

        // 时间轴:同一份 (只差全角空格) 完全一致;酷狗式拆行 + 几百毫秒偏差仍算一致;整体错开 3 秒不算
        ParsedLyrics same, split, shifted;
        KrcDecoder::ParseLrcText("[00:20.60]決めつけばかり　自惚れを着た\n[00:30.30]棚に隠した　哀れな\n"
                                 "[00:39.17]都合の傷だけひけらかして\n[00:48.88]どれだけ磨いでも気はやつれる\n", same);
        KrcDecoder::ParseLrcText("[00:20.90]決めつけばかり\n[00:23.10]自惚れを着た\n[00:30.50]棚に隠した 哀れな\n"
                                 "[00:39.40]都合の傷だけひけらかして\n[00:49.20]どれだけ磨いでも気はやつれる\n", split);
        KrcDecoder::ParseLrcText("[00:23.60]決めつけばかり 自惚れを着た\n[00:33.30]棚に隠した 哀れな\n"
                                 "[00:42.17]都合の傷だけひけらかして\n[00:51.88]どれだけ磨いでも気はやつれる\n", shifted);
        CHECK(LyricManager::TimelineAgreement(local, same) == 1.0);
        CHECK(LyricManager::TimelineAgreement(local, split) == 1.0);
        CHECK(LyricManager::TimelineAgreement(local, shifted) == 0.0);

        // 在线:LRCLIB 排第一但没有翻译;酷狗那份带翻译但时间轴错开 3 秒;网易云带翻译且对得上 -> 用网易云
        int calls = 0;
        bool neteaseHasTr = true;
        auto http = [&](const std::string& url, std::string& body) -> bool {
            ++calls;
            if (url.find("lrclib.net/api/search") != std::string::npos) {
                body = R"J([{"id":21,"trackName":"aLIEz","artistName":"瑞葵(mizuki)/SawanoHiroyuki[nZk]","albumName":"A/Z","duration":268.5,"syncedLyrics":"[00:20.60]決めつけばかり"}])J";
                return true;
            }
            if (url.find("lrclib.net/api/get/21") != std::string::npos) {
                body = R"J({"syncedLyrics":"[00:20.60]決めつけばかり 自惚れを着た\n[00:30.30]棚に隠した 哀れな"})J";
                return true;
            }
            if (url.find("music.163.com/api/search") != std::string::npos) {
                body = R"J({"result":{"songs":[{"id":29307041,"name":"aLIEz","artists":[{"name":"瑞葵(mizuki)"},{"name":"SawanoHiroyuki[nZk]"}],"album":{"name":"A/Z"},"duration":268472},
                                               {"id":99,"name":"aLIEz (2016 TOKYO DOME CITY HALL现场)","artists":[{"name":"mizuki"}],"album":{"name":""},"duration":268000}]}})J";
                return true;
            }
            if (url.find("music.163.com/api/song/lyric?id=29307041") != std::string::npos) {
                body = std::string(R"J({"lrc":{"lyric":"[00:20.60]決めつけばかり　自惚れを着た\n[00:30.30]棚に隠した　哀れな\n[00:39.17]都合の傷だけひけらかして\n[00:48.88]どれだけ磨いでも気はやつれる"})J") +
                       (neteaseHasTr ? R"J(,"tlyric":{"lyric":"[00:20.60]一味固执己见\n[00:30.30]藏在架子上\n[00:39.17]只炫耀方便的伤\n[00:48.88]再怎么磨砺也憔悴"}})J" : "}");
                return true;
            }
            if (url.find("music.163.com/api/song/lyric?id=99") != std::string::npos) {
                body = R"J({"lrc":{"lyric":"[00:23.60]決めつけばかり 自惚れを着た\n[00:33.30]棚に隠した 哀れな\n[00:42.17]都合の傷だけひけらかして\n[00:51.88]どれだけ磨いでも気はやつれる"},
                           "tlyric":{"lyric":"[00:23.60]现场版翻译\n[00:33.30]现场版翻译二\n[00:42.17]现场三\n[00:51.88]现场四"}})J";
                return true;
            }
            return false; // 酷狗不可用
        };
        const fs::path root = work / "tr";
        fs::create_directories(root / "lyrics");
        LyricManager mgr;
        mgr.SetCacheDirectory((root / "lyrics").wstring());
        mgr.SetHttpGetter(http);
        ParsedLyrics out;
        LyricCandidate picked;
        CHECK(mgr.FindTranslatedVersion(local, L"/m/aLIEz.flac", L"aLIEz", L"瑞葵(mizuki)/SawanoHiroyuki[nZk]", 268472, out, &picked));
        CHECK(picked.source == "netease" && picked.id == "29307041");
        CHECK(out.lines.size() == 4 && out.lines[0].translation == L"一味固执己见");
        CHECK(fs::exists(root / "lyrics_translated" / fs::u8path("瑞葵(mizuki)、SawanoHiroyuki[nZk] - aLIEz#netease.lrc")));
        // 第二次:翻译缓存直接命中,不联网
        calls = 0;
        ParsedLyrics again;
        LyricCandidate p2;
        CHECK(mgr.FindTranslatedVersion(local, L"/m/aLIEz.flac", L"aLIEz", L"瑞葵(mizuki)/SawanoHiroyuki[nZk]", 268472, again, &p2));
        CHECK(calls == 0 && p2.source == "netease" && again.HasTranslation());
        // 已带翻译 / 中文歌:根本不查
        CHECK(!mgr.FindTranslatedVersion(bi, L"/m/x.flac", L"x", L"y", 0, again) && calls == 0);

        // 没有对得上的翻译 (网易云这份没翻译,现场版错位):本地兜底,并记下「确认没有」,换个管理器 (重启) 也不再查
        neteaseHasTr = false;
        const fs::path root2 = work / "tr2";
        fs::create_directories(root2 / "lyrics");
        {
            LyricManager m2;
            m2.SetCacheDirectory((root2 / "lyrics").wstring());
            m2.SetHttpGetter(http);
            CHECK(!m2.FindTranslatedVersion(local, L"/m/aLIEz.flac", L"aLIEz", L"瑞葵(mizuki)/SawanoHiroyuki[nZk]", 268472, out));
            CHECK(fs::exists(root2 / "lyrics_translated" / "misses.txt"));
        }
        LyricManager m3;
        m3.SetCacheDirectory((root2 / "lyrics").wstring());
        m3.SetHttpGetter(http);
        calls = 0;
        CHECK(!m3.FindTranslatedVersion(local, L"/m/aLIEz.flac", L"aLIEz", L"瑞葵(mizuki)/SawanoHiroyuki[nZk]", 268472, out) && calls == 0);

        // 排名:只差括号附注的歌名算对上;现场版排到录音室版后面;多歌手写法不同也算同一歌手
        LyricQuery rq;
        rq.title = L"aLIEz";
        rq.artist = L"瑞葵(mizuki)/SawanoHiroyuki[nZk]";
        rq.duration_ms = 268472;
        std::vector<LyricCandidate> list(3);
        list[0].source = "kugou"; list[0].id = "1"; list[0].title = L"aLIEz (2016 TOKYO DOME CITY HALL现场)"; list[0].artist = L"mizuki"; list[0].duration_ms = 270003;
        list[1].source = "kugou"; list[1].id = "2"; list[1].title = L"aLIEz (《ALDNOAH.ZERO》TV动画片尾曲)";
        list[1].artist = L"SawanoHiroyuki(nZk) (泽野弘之)/瑞葵 (mizuki)"; list[1].duration_ms = 268638;
        list[2].source = "lrclib"; list[2].id = "3"; list[2].title = L"secret base"; list[2].duration_ms = 268000;
        LyricProvider::RankCandidates(rq, list);
        CHECK(list[0].id == "2" && list[0].title_match);
        CHECK(list[1].id == "1" && list[1].title_match);
        CHECK(!list[2].title_match);
    }

    std::printf("[4] 候选挑选\n");
    {
        std::vector<KuGouLyricCandidate> c(3);
        c[0].duration_ms = 200000; c[1].duration_ms = 185500; c[2].duration_ms = 185100;
        CHECK(KuGouOnlineLyrics::PickCandidate(c, 185000) == 2);
        CHECK(KuGouOnlineLyrics::PickCandidate(c, 0) == 0);
        CHECK(KuGouOnlineLyrics::PickCandidate(c, 100000) == -1);
    }

    std::printf(g_fail ? "lyric online host: %d FAILED\n" : "lyric online host: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
