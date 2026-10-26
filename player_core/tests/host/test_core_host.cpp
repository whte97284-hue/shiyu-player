// 跨平台集成测试:入库流水线 + 曲库 + 拼音检索 + CUE + 封面 + KGM 解密内核
// 用法: test_core_host <fixture_dir> <work_dir> [bench_copies]
#include "../../include/audio_ingestion_pipeline.hpp"
#include "../../include/decryptor.hpp"
#include "../../include/audio_tag_parser.hpp"
#include "../../include/cue_sheet.hpp"
#include "../../include/music_catalog.hpp"
#include "../../include/pinyin_table.hpp"
#include "../../include/platform_io.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace fs = std::filesystem;
static int g_pass = 0, g_fail = 0;

static void Check(const char* name, bool cond, const std::string& detail = "") {
    (cond ? g_pass : g_fail)++;
    std::printf("  [%s] %s%s%s\n", cond ? "PASS" : "FAIL", name, detail.empty() ? "" : "  -- ", detail.c_str());
}

static std::string U8(const std::wstring& w) { return AudioTagParser::WideToUtf8(w); }
static std::wstring W(const std::string& s) { return AudioTagParser::Utf8ToWide(s); }

static const TrackItem* FindCue(const MusicCatalog& c, const std::wstring& title) {
    for (const auto& t : c.GetAllTracks())
        if (t.is_cue_track && t.title == title) return &t;
    return nullptr;
}

static std::vector<uint8_t> ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <fixture_dir> <work_dir> [bench_copies]\n", argv[0]);
        return 2;
    }
    fs::path fx = argv[1], work = argv[2];
    int benchCopies = argc > 3 ? std::atoi(argv[3]) : 0;
    fs::remove_all(work);
    fs::create_directories(work);

    // ------------------------------------------------------------------
    std::printf("\n[1] KGM 解密内核:化简公式与原始半字节算法逐值等价\n");
    {
        bool same = true;
        for (int c = 0; c < 256 && same; ++c)
            for (int k = 0; k < 256 && same; k += 3)
                for (int m = 0; m < 256; ++m) {
                    uint8_t t = static_cast<uint8_t>(c ^ k);
                    uint8_t low = static_cast<uint8_t>((m & 0x0F) ^ (t & 0x0F));
                    uint8_t high = static_cast<uint8_t>((t >> 4) ^ (m >> 4) ^ low);
                    uint8_t ref = static_cast<uint8_t>(((high & 0x0F) << 4) | (low & 0x0F));
                    uint8_t key17[17] = {0};
                    key17[0] = static_cast<uint8_t>(k);
                    uint8_t mask[1] = {static_cast<uint8_t>(m)};
                    uint8_t v = static_cast<uint8_t>(c);
                    AudioIngestionPipeline::KgmDecryptBlock(&v, 1, 0, key17, mask, 1);
                    if (v != ref) { same = false; break; }
                }
        Check("x ^ (x << 4) 与原算法在全部 (c,k,m) 组合上结果一致", same);
    }

    // ------------------------------------------------------------------
    std::printf("\n[2] 构建测试曲库 (含 KGMA 加密样本、CUE 整轨、伴生封面、坏文件)\n");
    fs::path lib = work / "library";
    fs::copy(fx / "library_extras", lib, fs::copy_options::recursive);
    fs::create_directories(lib / "formats");
    for (const auto& e : fs::directory_iterator(fx))
        if (e.is_regular_file() && e.path().extension() != ".json") fs::copy_file(e.path(), lib / "formats" / e.path().filename());

    // 用随机掩码表加密一个 FLAC 生成 .kgma,验证流水线解密产物逐字节还原
    fs::path maskPath = work / "test.mask";
    std::vector<uint8_t> mask(1 << 16);
    std::mt19937 rng(7);
    for (auto& b : mask) b = static_cast<uint8_t>(rng());
    std::ofstream(maskPath, std::ios::binary).write(reinterpret_cast<const char*>(mask.data()), mask.size());
    std::vector<uint8_t> plain = ReadAll(fx / "flac_24_96_bigcover_first.flac");
    {
        std::vector<uint8_t> enc = plain;
        uint8_t key17[17];
        for (int i = 0; i < 16; ++i) key17[i] = static_cast<uint8_t>(rng());
        key17[16] = 0;
        // 变换是对合的:f(f(x)) = x,因此加密 = f(明文) ^ key ^ mask
        for (size_t i = 0; i < enc.size(); ++i) {
            uint8_t x = enc[i];
            enc[i] = static_cast<uint8_t>((x ^ (x << 4)) ^ key17[i % 17] ^ mask[i % mask.size()]);
        }
        std::vector<uint8_t> hdr(1024, 0);
        static const uint8_t kKgmMagic[16] = {0x7C, 0xD5, 0x32, 0xEB, 0x86, 0x02, 0x7F, 0x4B,
                                              0xA8, 0xAF, 0xA6, 0x8E, 0x0F, 0xFF, 0x99, 0x14};
        std::memcpy(hdr.data(), kKgmMagic, 16);
        uint32_t hlen = 1024;
        std::memcpy(&hdr[16], &hlen, 4);
        std::memcpy(&hdr[28], key17, 16);
        fs::create_directories(lib / "kugou");
        std::ofstream o(lib / "kugou" / "放課後ティータイム - Listen!!.kgma", std::ios::binary);
        o.write(reinterpret_cast<const char*>(hdr.data()), hdr.size());
        o.write(reinterpret_cast<const char*>(enc.data()), enc.size());
        o.close();
        // 同样的密文,但扩展名被改成 .flac (真实库中「卫兰」一类文件)
        fs::copy_file(lib / "kugou" / "放課後ティータイム - Listen!!.kgma", lib / "kugou" / "伪装成flac的酷狗文件.flac");
    }

    MusicCatalog catalog;
    AudioIngestionPipeline pipe;
    pipe.SetTargetLibraryDir(W((work / "decoded").string()));
    pipe.SetKgmMaskPath(W(maskPath.string()));
    pipe.SetLazyCoverExtract(true);
    pipe.SetThreadCount(4);
    pipe.AddInputPath(W(lib.string()));
    size_t lastProgress = 0;
    bool monotonic = true;
    pipe.on_progress = [&](const IngestionProgress& p) {
        if (p.current != lastProgress + 1) monotonic = false;
        lastProgress = p.current;
    };
    auto results = pipe.RunSync(catalog);
    IngestionSummary sum = pipe.GetLastSummary();
    std::printf("  扫描 %zu 文件 / 入库 %zu 曲目 / 成功 %zu / 失败 %zu / CUE 分轨 %zu / 伴生封面 %zu / 读盘 %llu KB / %.3fs\n",
                sum.total_scanned, sum.tracks_added, sum.success_count, sum.failed_count, sum.cue_tracks,
                sum.external_covers, static_cast<unsigned long long>(sum.bytes_read / 1024), sum.elapsed_seconds);

    Check("进度回调串行且严格单调递增", monotonic && lastProgress == results.size());
    Check("非音频文件 (readme.txt) 不进入扫描", std::none_of(results.begin(), results.end(), [](const IngestionItemResult& r) {
              return r.source_path.find(L"readme.txt") != std::wstring::npos;
          }));
    auto resultOf = [&](const wchar_t* needle) -> const IngestionItemResult* {
        for (const auto& r : results)
            if (r.source_path.find(needle) != std::wstring::npos) return &r;
        return nullptr;
    };
    Check("__MACOSX 目录与 ._ AppleDouble 文件被过滤", !resultOf(L"__MACOSX") && !resultOf(L"._hidden"));
    {
        const IngestionItemResult* v = resultOf(L"ep10 [1080P].mp4");
        Check("MP4 视频识别为视频并跳过,不计入失败", v && !v->success && sum.skipped_count == 1,
              v ? U8(v->message) : "未扫描");
        const IngestionItemResult* q = resultOf(L"qq_mflac.flac");
        Check("QQ 音乐加密 (尾部 QTag) 给出明确原因", q && !q->success && q->message.find(L"QQ") != std::wstring::npos,
              q ? U8(q->message) : "");
        const IngestionItemResult* e = resultOf(L"random_encrypted.flac");
        Check("高熵未知加密给出明确原因", e && !e->success && e->message.find(L"加密") != std::wstring::npos,
              e ? U8(e->message) : "");
        const IngestionItemResult* d = resultOf(L"伪装成flac的酷狗文件.flac");
        Check("改名为 .flac 的 KGM 文件按魔数识别并解密", d && d->success && d->was_encrypted, d ? U8(d->message) : "");
    }
    Check("失败数 = broken.flac + QQ 加密 + 未知加密", sum.failed_count == 3, std::to_string(sum.failed_count));
    {
        const TrackItem* lemon = nullptr;
        for (const auto& t : catalog.GetAllTracks())
            if (t.file_name == L"02. Lemon.wav") lemon = &t;
        Check("无标签 WAV:专辑取上级目录 (跳过 CD1)、曲号取文件名、标题去掉序号",
              lemon && lemon->album == L"YANKEE (Deluxe)" && lemon->track_no == 2 && lemon->title == L"Lemon",
              lemon ? U8(lemon->album + L" / " + lemon->title) + " #" + std::to_string(lemon->track_no) : "");
    }

    {
        AudioMetadata none;
        TrackItem d = MusicCatalog::BuildTrack(L"F:/ZiYuan/Music/周杰伦 叶惠美/05东风破.wav", none);
        Check("无标签 \"周杰伦 叶惠美/05东风破.wav\":歌手 / 专辑取自目录,标题去掉补零曲号",
              d.artist == L"周杰伦" && d.album == L"叶惠美" && d.title == L"东风破" && d.track_no == 5,
              U8(d.artist + L" / " + d.album + L" / " + d.title) + " #" + std::to_string(d.track_no));
        TrackItem e = MusicCatalog::BuildTrack(L"D:/Music/Taylor Swift/1989.flac", none);
        TrackItem f = MusicCatalog::BuildTrack(L"D:/Music/2003 - 叶惠美/17岁.flac", none);
        TrackItem g = MusicCatalog::BuildTrack(L"D:/Music/Beyond - 海阔天空/2002年的第一场雪.mp3", none);
        Check("目录推断不误伤:英文空格目录 / 年份目录 / 数字开头的歌名",
              e.artist == L"未知艺术家" && e.title == L"1989" && f.artist == L"未知艺术家" && f.title == L"17岁" &&
              g.artist == L"Beyond" && g.album == L"海阔天空" && g.title == L"2002年的第一场雪" && g.track_no == 0,
              U8(e.artist + L"|" + f.artist + L"|" + f.title + L"|" + g.artist + L"|" + g.title));
        AudioMetadata tagged;
        tagged.artist = L"周杰伦";
        tagged.album = L"叶惠美";
        TrackItem h = MusicCatalog::BuildTrack(L"D:/Music/群星 合集/03 晴天.flac", tagged);
        Check("有歌手标签时不看目录", h.artist == L"周杰伦" && h.album == L"叶惠美", U8(h.artist));
    }

    // KGMA
    const IngestionItemResult* kg = nullptr;
    for (const auto& r : results)
        if (r.was_encrypted && r.source_path.find(L".kgma") != std::wstring::npos) kg = &r;
    Check("KGMA 解密成功", kg && kg->success, kg ? U8(kg->message) : "未找到");
    if (kg && kg->success) {
        Check("KGMA 解密产物与原始 FLAC 逐字节一致", ReadAll(fs::path(U8(kg->target_path))) == plain);
        Check("KGMA 产物入库并解析出 24bit/96k", [&] {
            const TrackItem* t = catalog.FindByPath(kg->target_path);
            return t && t->sample_rate == 96000 && t->bits_per_sample == 24 && t->is_hires;
        }());
    }

    // NCM:原生解密 + 标签回写
    {
        const IngestionItemResult* n = resultOf(L"netease.ncm");
        Check("NCM 原生解密成功", n && n->success && n->was_encrypted, n ? U8(n->message) : "未找到");
        if (n && n->success) {
            std::vector<uint8_t> prod = ReadAll(fs::path(U8(n->target_path)));
            std::vector<uint8_t> src = ReadAll(fx / "ncm_source" / "plain.flac");
            // 跳过原始 FLAC 的元数据块,比较音频帧
            size_t pos = 4;
            for (;;) {
                size_t len = (size_t(src[pos + 1]) << 16) | (size_t(src[pos + 2]) << 8) | src[pos + 3];
                bool lastBlock = src[pos] & 0x80;
                pos += 4 + len;
                if (lastBlock) break;
            }
            size_t frames = src.size() - pos;
            Check("NCM 产物音频帧与原始 FLAC 逐字节一致",
                  prod.size() > frames && std::equal(src.begin() + pos, src.end(), prod.end() - frames));
            AudioMetadata pm;
            ParseOptions po;
            po.load_cover = true;
            bool ok = AudioTagParser::ParseFile(W(U8(n->target_path)), pm, po);
            Check("NCM 产物写回标题 / 歌手 / 专辑 / 封面",
                  ok && pm.title == L"起风了" && pm.artist.find(L"林俊杰") != std::wstring::npos && pm.album == L"起风了" &&
                      pm.has_cover,
                  U8(pm.title + L" / " + pm.artist));
        }
        const Decryptor* d = DecryptorRegistry::Instance().Detect(W((lib / "kugou" / "伪装成flac的酷狗文件.flac").string()));
        Check("解密工具注册表:按文件头识别改名的 KGM", d && std::string(d->Id()) == "kgm");
        Check("解密工具注册表:三个内置工具 (kgm / ncm / kgg)",
              DecryptorRegistry::Instance().ById("kgm") && DecryptorRegistry::Instance().ById("ncm") &&
                  DecryptorRegistry::Instance().ById("kgg") && DecryptorRegistry::Instance().All().size() == 3);
    }

    // ReplayGain + 内嵌歌词
    {
        AudioMetadata a, b, c;
        ParseOptions lo;
        lo.load_lyrics = true;
        AudioTagParser::ParseFile(W((fx / "tag_extras" / "rg.flac").string()), a, lo);
        Check("FLAC ReplayGain:曲目 -6.50 dB / 峰值 0.95,专辑 -5.00 dB",
              a.has_rg_track && std::fabs(a.rg_track_gain + 6.5f) < 1e-3 && std::fabs(a.rg_track_peak - 0.95f) < 1e-3 &&
                  a.has_rg_album && std::fabs(a.rg_album_gain + 5.0f) < 1e-3);
        Check("FLAC 内嵌歌词 (LYRICS)", a.lyrics.find(L"第二句") != std::wstring::npos);
        AudioTagParser::ParseFile(W((fx / "tag_extras" / "rg.mp3").string()), b, lo);
        Check("MP3 TXXX ReplayGain +2.10 dB 与 USLT 歌词",
              b.has_rg_track && std::fabs(b.rg_track_gain - 2.1f) < 1e-3 && b.lyrics.find(L"世界") != std::wstring::npos,
              U8(b.lyrics));
        AudioTagParser::ParseFile(W((fx / "tag_extras" / "rg.flac").string()), c);
        Check("未请求时不读取歌词 (入库省内存)", c.lyrics.empty() && c.has_rg_track);
    }

    // 询问模式:扫描只收集加密源,不解密、不入库;解密并写入索引后,再次扫描直接复用产物
    {
        fs::path idx = work / "decrypt.index";
        auto runAsk = [&](MusicCatalog& cat) {
            AudioIngestionPipeline p;
            p.SetTargetLibraryDir(W((work / "decoded_ask").string()));
            p.SetKgmMaskPath(W(maskPath.string()));
            p.SetLazyCoverExtract(true);
            p.SetDecryptPolicy(DecryptPolicy::Ask);
            p.SetDecryptIndexFile(W(idx.string()));
            p.AddInputPath(W(lib.string()));
            p.RunSync(cat);
            return p.GetLastSummary();
        };
        MusicCatalog c1;
        IngestionSummary s1 = runAsk(c1);
        size_t kinds[3] = {0, 0, 0};
        for (const auto& e : s1.encrypted_pending) kinds[e.kind == "kgm" ? 0 : e.kind == "ncm" ? 1 : 2]++;
        Check("询问模式收集到 2 个 KGM (含改名) + 1 个 NCM,且未解密", s1.encrypted_pending.size() == 3 && kinds[0] == 2 &&
                                                                              kinds[1] == 1 && s1.decrypted_count == 0 &&
                                                                              !fs::exists(work / "decoded_ask"),
              std::to_string(s1.encrypted_pending.size()));
        Check("询问模式下加密源不入库、不计为失败", c1.GetAllTracks().size() + 3 == catalog.GetAllTracks().size() &&
                                                    s1.failed_count == sum.failed_count,
              std::to_string(c1.GetAllTracks().size()) + " vs " + std::to_string(catalog.GetAllTracks().size()));
        DecryptIndex index;
        index.Load(W(idx.string()));
        size_t ok = 0;
        for (const auto& e : s1.encrypted_pending) {
            DecryptOptions o;
            o.output_dir = W((work / "decoded_ask").string());
            DecryptResult r = DecryptorRegistry::Instance().ById(e.kind)->Decrypt(e.path, o);
            if (r.ok) {
                ++ok;
                index.Put(e.path, e.size, e.mtime, r.output, DecryptIndex::Fingerprint(e.path, e.size));
            }
        }
        index.Save(W(idx.string()));
        Check("通过注册表逐个解密待处理文件", ok == 3);
        MusicCatalog c2;
        IngestionSummary s2 = runAsk(c2);
        Check("再次扫描:按索引直接入库产物,不再提示", s2.encrypted_pending.empty() && c2.GetAllTracks().size() == catalog.GetAllTracks().size(),
              std::to_string(s2.encrypted_pending.size()));
        size_t withSource = 0;
        for (const auto& t : c2.GetAllTracks())
            if (!t.source_path.empty()) ++withSource;
        Check("产物入库时记下加密源 (按源文件夹归类 / 隐藏)", withSource == 3, std::to_string(withSource));

        // 产物目录本身也在扫描范围内:同一首歌只出现一次
        {
            AudioIngestionPipeline p;
            p.SetLazyCoverExtract(true);
            p.SetDecryptPolicy(DecryptPolicy::Ask);
            p.SetDecryptIndexFile(W(idx.string()));
            p.AddInputPath(W(lib.string()));
            p.AddInputPath(W((work / "decoded_ask").string()));
            MusicCatalog c3;
            p.RunSync(c3);
            size_t src = 0;
            for (const auto& t : c3.GetAllTracks())
                if (!t.source_path.empty()) ++src;
            Check("产物目录也被扫描时不重复入库", c3.GetAllTracks().size() == c2.GetAllTracks().size() && src == 3,
                  std::to_string(c3.GetAllTracks().size()) + " vs " + std::to_string(c2.GetAllTracks().size()));
        }

        // 源文件被移动 / 改名:按内容指纹认出,不再提示转换
        {
            const std::wstring first = s1.encrypted_pending.front().path;
            fs::path movedDir = work / "moved";
            fs::create_directories(movedDir);
            fs::path moved = movedDir / ("改名后" + fs::path(first).extension().string());
            fs::copy_file(fs::path(first), moved, fs::copy_options::overwrite_existing);
            AudioIngestionPipeline p;
            p.SetLazyCoverExtract(true);
            p.SetDecryptPolicy(DecryptPolicy::Ask);
            p.SetDecryptIndexFile(W(idx.string()));
            p.AddInputPath(W(movedDir.string()));
            MusicCatalog c4;
            p.RunSync(c4);
            IngestionSummary s4 = p.GetLastSummary();
            DecryptIndex reloaded;
            reloaded.Load(W(idx.string()));
            uint64_t sz = 0, mt = 0;
            platform::GetFileInfo(W(moved.string()), sz, mt);
            Check("移动 / 改名的加密源按指纹复用产物", s4.encrypted_pending.empty() && c4.GetAllTracks().size() == 1 &&
                                                         !reloaded.Find(W(moved.string()), sz, mt).empty(),
                  std::to_string(s4.encrypted_pending.size()));
        }
    }

    // CUE
    const TrackItem* t1 = FindCue(catalog, L"十年");
    const TrackItem* t2 = FindCue(catalog, L"明年今日");
    const TrackItem* t3 = FindCue(catalog, L"黑白灰");
    Check("CUE (GBK, FILE 写 .wav 实为 .flac) 拆分出 3 首", sum.cue_tracks == 3 && t1 && t2 && t3);
    if (t1 && t2 && t3) {
        Check("CUE 第 2 轨起止 = 10.000s ~ 20.493s", t2->cue_start_ms == 10000 && t2->cue_end_ms == 20493,
              std::to_string(t2->cue_start_ms) + "~" + std::to_string(t2->cue_end_ms));
        Check("CUE 末轨时长 = 文件时长 - 起点", t3->duration_ms == 30000 - 20493, std::to_string(t3->duration_ms));
        Check("CUE 专辑 / 歌手 / 流派 继承自表头", t2->album == L"黑白灰" && t2->artist == L"陈奕迅" && t2->genre == L"Pop");
        Check("CUE 分轨关联 folder.jpg 伴生封面", !t1->external_cover_path.empty());
    }

    // 伴生封面继承
    const TrackItem* hires = nullptr;
    for (const auto& t : catalog.GetAllTracks())
        if (t.file_name == L"黑夜不再来_hires.flac") hires = &t;
    Check("CD1 子目录继承上级目录 cover.jpg", hires && hires->external_cover_path.find(L"cover.jpg") != std::wstring::npos);

    // 惰性封面
    const TrackItem* bigCover = nullptr;
    for (const auto& t : catalog.GetAllTracks())
        if (t.file_name == L"mp3_cbr320_id3v23_utf16_apic.mp3") bigCover = &t;
    Check("惰性模式入库不在内存中持有封面", bigCover && bigCover->has_cover && bigCover->cover_data.empty() && bigCover->cover_offset > 0);
    if (bigCover) {
        std::vector<uint8_t> img;
        std::string mime;
        bool ok = MusicCatalog::LoadCover(*bigCover, img, mime);
        Check("LoadCover 按物理偏移一次读出完整封面", ok && img.size() == bigCover->cover_size && mime == "image/jpeg",
              std::to_string(img.size()) + " bytes " + mime);
    }

    // ------------------------------------------------------------------
    std::printf("\n[3] 检索:拼音 / 首字母 / 简繁同音 / 假名罗马音 / 相关度排序 / 去重择优\n");
    auto has = [&](const std::vector<const TrackItem*>& v, const std::wstring& title) {
        return std::any_of(v.begin(), v.end(), [&](const TrackItem* t) { return t->title == title; });
    };
    Check("'陈奕迅' 命中繁体标签 '陳奕迅' 的曲目", has(catalog.Search(L"陈奕迅"), L"黑夜不再来"));
    Check("'cyx' 首字母命中", has(catalog.Search(L"cyx"), L"黑夜不再来"));
    Check("'chenyixun' 全拼命中", has(catalog.Search(L"chenyixun"), L"黑夜不再来"));
    Check("'heiyebuzai' 全拼前缀片段命中", has(catalog.Search(L"heiyebuzai"), L"黑夜不再来"));
    Check("'adamasu' 罗马音命中 'アダマス'", has(catalog.Search(L"adamasu"), L"アダマス"));
    Check("'shinian' 命中 CUE 分轨 '十年'", has(catalog.Search(L"shinian"), L"十年"));
    {
        auto r = catalog.Search(L"黑白灰");
        Check("标题精确匹配排在专辑匹配之前", !r.empty() && r.front()->title == L"黑白灰");
    }
    {
        auto groups = catalog.SearchGrouped(L"黑夜不再来");
        bool ok = groups.size() == 1 && groups[0].primary && groups[0].primary->sample_rate == 96000 &&
                  groups[0].duplicates.size() == 2;
        Check("三个版本折叠为一组,24bit/96k 版本被选为主版本", ok);
    }
    Check("拼音全表覆盖生僻字 '喆' / 繁体 '陳'", PinyinHelper::GetFullPinyin(L"陶喆陳") == "taozhechen");
    Check("拗音/促音罗马音 'きょう' -> kyou, 'ちょっと' -> chotto",
          PinyinHelper::GetFullPinyin(L"きょう") == "kyou" && PinyinHelper::GetFullPinyin(L"ちょっと") == "chotto",
          PinyinHelper::GetFullPinyin(L"きょう") + " " + PinyinHelper::GetFullPinyin(L"ちょっと"));

    auto st = catalog.GetStats();
    std::printf("  曲库统计: %zu 首 / 无损 %zu / Hi-Res %zu / 有封面 %zu / 歌手 %zu / 专辑 %zu\n", st.total_tracks,
                st.lossless_count, st.hires_count, st.with_cover_count, st.total_artists, st.total_albums);

    // ------------------------------------------------------------------
    std::printf("\n[4] 增量扫描缓存\n");
    {
        std::wstring cacheFile = W((work / "scan.cache").string());
        auto scan = [&](MusicCatalog& cat) {
            AudioIngestionPipeline p;
            p.SetTargetLibraryDir(W((work / "decoded").string()));
            p.SetKgmMaskPath(W(maskPath.string()));
            p.SetLazyCoverExtract(true);
            p.SetScanCacheFile(cacheFile);
            p.AddInputPath(W(lib.string()));
            p.RunSync(cat);
            return p.GetLastSummary();
        };
        auto fingerprint = [](const MusicCatalog& cat) {
            std::vector<std::wstring> v;
            for (const auto& t : cat.GetAllTracks())
                v.push_back(t.file_path + L"|" + t.title + L"|" + t.artist + L"|" + t.album + L"|" +
                            std::to_wstring(t.duration_ms) + L"|" + std::to_wstring(t.sample_rate) + L"|" +
                            std::to_wstring(t.cover_offset) + L"|" + t.external_cover_path);
            std::sort(v.begin(), v.end());
            return v;
        };
        MusicCatalog c1, c2, c3, c4;
        IngestionSummary s1 = scan(c1);
        IngestionSummary s2 = scan(c2);
        std::printf("  首次: 解析 %zu / 命中 %zu / 读盘 %llu KB;再次: 解析 %zu / 命中 %zu / 读盘 %llu KB\n",
                    s1.cache_misses, s1.cache_hits, static_cast<unsigned long long>(s1.bytes_read / 1024),
                    s2.cache_misses, s2.cache_hits, static_cast<unsigned long long>(s2.bytes_read / 1024));
        Check("首次扫描写入缓存", s1.cache_saved && s1.cache_misses > 0 && s1.cache_hits == 0);
        Check("再次扫描全部命中缓存、解析阶段零读盘", s2.cache_misses == 0 && s2.cache_hits == s1.cache_misses && s2.bytes_read == 0);
        Check("缓存命中构建的曲库与首次完全一致 (含 CUE、封面偏移、伴生封面)", fingerprint(c1) == fingerprint(c2) &&
              c1.GetAllTracks().size() == c2.GetAllTracks().size());

        // 修改一个文件 (追加字节 -> 大小变化),只应重新解析这一个
        fs::path changed = lib / "formats" / "mp3_raw_cbr_noxing.mp3";
        { std::ofstream(changed, std::ios::binary | std::ios::app).write("\0\0\0\0", 4); }
        IngestionSummary s3 = scan(c3);
        Check("文件变化后仅重新解析变化的文件", s3.cache_misses == 1 && s3.cache_hits == s1.cache_misses - 1,
              std::to_string(s3.cache_misses) + " miss");

        // 缓存文件损坏:安全退化为全量扫描
        { std::fstream f(work / "scan.cache", std::ios::in | std::ios::out | std::ios::binary); f.seekp(40); f.write("garbage!", 8); }
        { std::ofstream f(work / "scan.cache", std::ios::binary | std::ios::app); }
        fs::resize_file(work / "scan.cache", fs::file_size(work / "scan.cache") / 2);
        IngestionSummary s4 = scan(c4);
        Check("缓存损坏时安全退化为全量扫描,结果仍正确", s4.cache_hits == 0 && fingerprint(c4).size() == fingerprint(c1).size());
    }

    // ------------------------------------------------------------------
    if (benchCopies > 0) {
        std::printf("\n[5] 吞吐压测:复制 %d 份样本库\n", benchCopies);
        fs::path bench = work / "bench";
        for (int i = 0; i < benchCopies; ++i) {
            fs::path d = bench / ("disc_" + std::to_string(i));
            fs::create_directories(d);
            for (const auto& e : fs::directory_iterator(lib / "formats")) {
                std::error_code ec;
                fs::create_hard_link(e.path(), d / e.path().filename(), ec);
                if (ec) fs::copy_file(e.path(), d / e.path().filename());
            }
        }
        MusicCatalog bc;
        AudioIngestionPipeline bp;
        bp.SetLazyCoverExtract(true);
        bp.AddInputPath(W(bench.string()));
        auto t0 = std::chrono::steady_clock::now();
        bp.RunSync(bc);
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        IngestionSummary bs = bp.GetLastSummary();
        std::printf("  %zu 文件 (%.1f MB) -> %zu 曲目, %.3fs, %.0f 首/秒, 读盘 %.1f MB (占库 %.2f%%), 线程 %zu\n",
                    bs.total_scanned, bs.library_bytes / 1048576.0, bs.tracks_added, sec, bs.total_scanned / sec,
                    bs.bytes_read / 1048576.0, 100.0 * bs.bytes_read / std::max<uint64_t>(1, bs.library_bytes), bs.thread_count);
        auto q0 = std::chrono::steady_clock::now();
        auto hits = bc.Search(L"chenyixun");
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - q0).count();
        std::printf("  检索 %zu 首曲库: %.3f ms (%zu 命中)\n", bc.GetAllTracks().size(), ms, hits.size());
        Check("吞吐 >= 500 首/秒 (页缓存热数据,衡量 CPU 开销)", bs.total_scanned / sec >= 500);
        Check("惰性入库读盘量 < 曲库体积的 15%", bs.bytes_read * 100 < bs.library_bytes * 15);
    }

    std::printf("\n==== host core tests: %d passed / %d failed ====\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
