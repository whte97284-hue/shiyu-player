#include "../include/audio_ingestion_pipeline.hpp"
#include "../include/audio_tag_parser.hpp"
#include "../include/cue_sheet.hpp"
#include "../include/decryptor.hpp"
#include "../include/platform_io.hpp"
#include "../include/scan_cache.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace {


std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
    return s;
}

std::wstring ExtLower(const std::wstring& name) {
    size_t d = name.rfind(L'.');
    size_t sep = name.find_last_of(L"\\/");
    if (d == std::wstring::npos || (sep != std::wstring::npos && d < sep)) return L"";
    return Lower(name.substr(d));
}

std::wstring StemOf(const std::wstring& name) {
    size_t sep = name.find_last_of(L"\\/");
    std::wstring base = (sep == std::wstring::npos) ? name : name.substr(sep + 1);
    size_t d = base.rfind(L'.');
    return d == std::wstring::npos ? base : base.substr(0, d);
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? path : path.substr(p + 1);
}

std::wstring ParentOf(const std::wstring& path) {
    size_t p = path.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"" : path.substr(0, p);
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
#ifdef _WIN32
    const wchar_t sep = L'\\';
#else
    const wchar_t sep = L'/';
#endif
    wchar_t last = dir.back();
    if (last == L'\\' || last == L'/') return dir + name;
    return dir + sep + name;
}

bool IsImageExt(const std::wstring& e) { return e == L".jpg" || e == L".jpeg" || e == L".png" || e == L".webp"; }

// 同目录伴生封面择优:cover > folder > front > albumart > 封面 > 目录内唯一图片
std::wstring PickFolderCover(const std::vector<platform::DirEntry>& files) {
    static const wchar_t* const kPriority[] = {L"cover", L"folder", L"front", L"albumartlarge", L"albumart", L"封面", L"album"};
    const platform::DirEntry* best = nullptr;
    int bestRank = 1000;
    size_t imageCount = 0;
    const platform::DirEntry* onlyImage = nullptr;
    for (const auto& f : files) {
        if (!IsImageExt(ExtLower(f.name)) || f.size == 0) continue;
        ++imageCount;
        onlyImage = &f;
        std::wstring stem = Lower(StemOf(f.name));
        for (int r = 0; r < static_cast<int>(sizeof(kPriority) / sizeof(kPriority[0])); ++r) {
            if (stem.compare(0, std::wcslen(kPriority[r]), kPriority[r]) == 0 && r < bestRank) {
                bestRank = r;
                best = &f;
            }
        }
    }
    if (best) return best->path;
    if (imageCount == 1) return onlyImage->path;
    return L"";
}

struct CueBinding {
    std::shared_ptr<CueSheet> sheet;
    size_t file_index = 0;
};

struct WorkItem {
    std::wstring path;
    std::wstring ext;
    uint64_t size = 0;
    uint64_t mtime = 0;
    std::wstring folder_cover;
    CueBinding cue;
};

bool IsEncryptedExt(const std::wstring& e) { return IsEncryptedAudioExtension(e); }

} // namespace

// ===========================================================================
AudioIngestionPipeline::AudioIngestionPipeline() {
    target_lib_dir_ = L"output";
}

AudioIngestionPipeline::~AudioIngestionPipeline() {
    Cancel();
    if (async_worker_thread_.joinable()) async_worker_thread_.join();
}

void AudioIngestionPipeline::SetTargetLibraryDir(const std::wstring& dir) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    target_lib_dir_ = dir;
}

void AudioIngestionPipeline::SetKgmMaskPath(const std::wstring& maskPath) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    kgm_mask_path_ = maskPath;
    DecryptorRegistry::Instance().SetKgmMaskPath(maskPath);
}

void AudioIngestionPipeline::SetDecryptPolicy(DecryptPolicy policy) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    decrypt_policy_ = policy;
}

void AudioIngestionPipeline::SetDecryptIndexFile(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    decrypt_index_path_ = path;
}

void AudioIngestionPipeline::SetThreadCount(size_t threads) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    thread_count_ = threads;
}

void AudioIngestionPipeline::SetLazyCoverExtract(bool lazy) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    lazy_cover_extract_ = lazy;
}

void AudioIngestionPipeline::SetReadId3v1(bool on) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    read_id3v1_ = on;
}

void AudioIngestionPipeline::SetLegacyCodePage(unsigned int codePage) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    code_page_ = codePage;
}

void AudioIngestionPipeline::SetSplitCueSheets(bool split) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    split_cue_ = split;
}

void AudioIngestionPipeline::SetScanCacheFile(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    scan_cache_path_ = path;
}

void AudioIngestionPipeline::AddInputPath(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    input_paths_.push_back(path);
}

void AudioIngestionPipeline::ClearInputPaths() {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    input_paths_.clear();
}

void AudioIngestionPipeline::Cancel() { cancel_requested_.store(true); }

IngestionSummary AudioIngestionPipeline::GetLastSummary() const {
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    return last_summary_;
}

// ===========================================================================
// KGM 解密
// ===========================================================================
void AudioIngestionPipeline::KgmDecryptBlock(uint8_t* data, size_t len, uint64_t fileOffset, const uint8_t key17[17],
                                             const uint8_t* mask, uint64_t maskLen) {
    ::KgmDecryptBlock(data, len, fileOffset, key17, mask, maskLen);
}

// ===========================================================================
// 主流程
// ===========================================================================
std::vector<IngestionItemResult> AudioIngestionPipeline::RunSync(MusicCatalog& catalog) {
    is_running_.store(true);
    cancel_requested_.store(false);
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();

    std::vector<std::wstring> inputs;
    std::wstring targetDir;
    size_t requestedThreads;
    bool lazy, splitCue;
    unsigned int codePage = 936;
    bool readId3v1 = true;
    std::wstring cachePath, indexPath;
    DecryptPolicy policy;
    {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        inputs = input_paths_;
        targetDir = target_lib_dir_;
        requestedThreads = thread_count_;
        lazy = lazy_cover_extract_;
        splitCue = split_cue_;
        codePage = code_page_;
        readId3v1 = read_id3v1_;
        cachePath = scan_cache_path_;
        policy = decrypt_policy_;
        indexPath = decrypt_index_path_;
    }
    DecryptIndex decIndex;
    const bool useIndex = !indexPath.empty();
    if (useIndex) decIndex.Load(indexPath);
    std::atomic<size_t> decryptedCount{0};
    std::atomic<bool> indexDirty{false};
    std::mutex pendingMutex;
    std::vector<EncryptedFileInfo> pending;
    // 增量缓存只保存封面偏移,因此仅在惰性封面模式下启用
    const bool useCache = !cachePath.empty() && lazy;
    ScanCache cache;
    if (useCache) cache.Load(cachePath);

    IngestionSummary summary;

    // ---------------- 1. 枚举 ----------------
    std::vector<WorkItem> items;
    std::unordered_map<std::wstring, std::wstring> dirCover;
    for (const auto& input : inputs) {
        if (cancel_requested_.load()) break;
        if (platform::IsDirectory(input)) {
            platform::EnumerateFilesRecursive(input, [&](const std::wstring& dir, const std::vector<platform::DirEntry>& files) {
                std::wstring cover = PickFolderCover(files);
                if (cover.empty()) {
                    auto it = dirCover.find(ParentOf(dir)); // CD1/CD2 子目录沿用上级目录封面
                    if (it != dirCover.end()) cover = it->second;
                }
                if (!cover.empty()) dirCover[dir] = cover;

                size_t firstIdx = items.size();
                std::unordered_map<std::wstring, size_t> byName, byStem;
                for (const auto& f : files) {
                    std::wstring e = ExtLower(f.name);
                    if (!AudioTagParser::IsSupportedExtension(e)) continue;
                    WorkItem w;
                    w.path = f.path;
                    w.ext = e;
                    w.size = f.size;
                    w.mtime = f.mtime;
                    w.folder_cover = cover;
                    byName[Lower(f.name)] = items.size();
                    byStem[Lower(StemOf(f.name))] = items.size();
                    items.push_back(std::move(w));
                }
                if (splitCue && items.size() > firstIdx) {
                    for (const auto& f : files) {
                        if (ExtLower(f.name) != L".cue") continue;
                        auto sheet = std::make_shared<CueSheet>();
                        if (!CueSheetParser::ParseFile(f.path, *sheet)) continue;
                        for (size_t fi = 0; fi < sheet->files.size(); ++fi) {
                            std::wstring ref = Lower(FileNameOf(sheet->files[fi].file_name));
                            size_t target = SIZE_MAX;
                            auto byNameIt = byName.find(ref);
                            if (byNameIt != byName.end()) {
                                target = byNameIt->second;
                            } else {
                                // CUE 中常写 .wav,实际已转为 .flac/.ape:按主文件名回退匹配
                                auto byStemIt = byStem.find(Lower(StemOf(ref)));
                                if (byStemIt != byStem.end()) target = byStemIt->second;
                                else if (sheet->files.size() == 1 && items.size() - firstIdx == 1) target = firstIdx;
                            }
                            if (target == SIZE_MAX) continue;
                            WorkItem& w = items[target];
                            if (!w.cue.sheet && sheet->files[fi].tracks.size() > 1) {
                                w.cue.sheet = sheet;
                                w.cue.file_index = fi;
                            }
                        }
                    }
                }
                return !cancel_requested_.load();
            });
        } else if (platform::PathExists(input)) {
            WorkItem w;
            w.path = input;
            w.ext = ExtLower(input);
            platform::GetFileInfo(input, w.size, w.mtime);
            items.push_back(std::move(w));
        }
    }
    summary.enumerate_seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    for (const auto& w : items) summary.library_bytes += w.size;

    const size_t total = items.size();
    std::vector<IngestionItemResult> results(total);
    if (total == 0) {
        summary.elapsed_seconds = std::chrono::duration<double>(Clock::now() - t0).count();
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        last_summary_ = summary;
        is_running_.store(false);
        return {};
    }

    const bool anyEncrypted = std::any_of(items.begin(), items.end(), [](const WorkItem& w) { return IsEncryptedExt(w.ext); });
    if (anyEncrypted && policy == DecryptPolicy::Auto && !targetDir.empty()) platform::CreateDirectories(targetDir);

    // ---------------- 2. 存储感知并发度 ----------------
    unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    summary.storage_seek_penalty = inputs.empty() ? -1 : platform::QuerySeekPenalty(inputs.front());
    size_t numThreads = requestedThreads;
    if (numThreads == 0) {
        if (summary.storage_seek_penalty == 1) numThreads = 4;                    // 机械盘:适度队列深度利用 NCQ
        else if (summary.storage_seek_penalty == 0) numThreads = std::min<size_t>(hw * 2, 32); // 固态盘:I/O 并行
        else numThreads = std::min<size_t>(hw, 8);
    }
    numThreads = std::max<size_t>(1, std::min(numThreads, total));
    summary.thread_count = numThreads;

    ParseOptions parseOpt;
    parseOpt.load_cover = !lazy;
    parseOpt.legacy_code_page = codePage;
    parseOpt.read_id3v1 = readId3v1;

    std::atomic<size_t> nextIndex{0};
    std::atomic<uint64_t> bytesRead{0};
    std::atomic<size_t> cueTracks{0}, externalCovers{0}, skippedVideos{0}, cacheHits{0}, cacheMisses{0};
    // 每个条目的最新解析结果 (命中或新解析),运行结束后整体写回缓存;按下标写入,无需加锁
    std::vector<ScanCacheEntry> fresh(useCache ? total : 0);
    std::vector<uint8_t> freshValid(useCache ? total : 0, 0);
    std::mutex catalogMutex, progressMutex;
    // 全部曲目先收集起来,去掉重复后再一次性入库 (解密产物恰好也在扫描目录里时,会被当作普通文件再扫到一次)
    std::vector<TrackItem> collected;
    size_t progressDone = 0;
    auto progressFn = on_progress;

    auto worker = [&]() {
        std::vector<TrackItem> batch;
        batch.reserve(64);
        auto flush = [&]() {
            if (batch.empty()) return;
            std::lock_guard<std::mutex> lock(catalogMutex);
            for (auto& t : batch) collected.push_back(std::move(t));
            batch.clear();
        };
        auto addTrack = [&](const std::wstring& path, const AudioMetadata& meta, const WorkItem& w) {
            // 解密产物记下加密源:专辑归类、隐藏文件夹都按源文件所在目录判断
            TrackItem t = MusicCatalog::BuildTrack(path, meta, path == w.path ? std::wstring() : w.path);
            t.added_time = w.mtime;
            if (!t.has_cover && !w.folder_cover.empty()) {
                t.external_cover_path = w.folder_cover;
                externalCovers.fetch_add(1);
            }
            batch.push_back(std::move(t));
        };

        auto lookup = [&](const WorkItem& w) -> const ScanCacheEntry* {
            return useCache ? cache.Find(w.path, w.size, w.mtime) : nullptr;
        };
        auto remember = [&](size_t idx, const WorkItem& w, bool ok, const AudioMetadata& meta, const std::wstring& target) {
            if (!useCache || w.mtime == 0) return;
            fresh[idx].size = w.size;
            fresh[idx].mtime = w.mtime;
            fresh[idx].parse_ok = ok;
            fresh[idx].meta = meta;
            fresh[idx].target_path = target;
            freshValid[idx] = 1;
        };

        while (!cancel_requested_.load()) {
            size_t idx = nextIndex.fetch_add(1);
            if (idx >= total) break;
            const WorkItem& w = items[idx];
            IngestionItemResult& item = results[idx];
            item.source_path = w.path;

            // 扩展名不是 .kgm 但内容是酷狗加密头 (例如被改名为 .flac),同样走解密
            AudioMetadata probe;
            bool probed = false;
            bool kgmByMagic = false;
            bool countedMiss = false;
            if (!IsEncryptedExt(w.ext) && w.ext != L".ncm" && w.ext != L".kgg") {
                const ScanCacheEntry* hit = lookup(w);
                if (hit && !hit->target_path.empty()) {
                    // 缓存记录表明这是改名的酷狗加密文件:直接交给解密分支,不再探测文件头
                    probed = true;
                    probe.error_code = "encrypted_kgm";
                } else if (hit) {
                    probe = hit->meta;
                    probe.io_read_calls = 0;
                    probe.io_bytes_read = 0;
                    probed = hit->parse_ok;
                    cacheHits.fetch_add(1);
                } else {
                    probed = AudioTagParser::ParseFile(w.path, probe, parseOpt);
                    bytesRead.fetch_add(probe.io_bytes_read);
                    cacheMisses.fetch_add(1);
                    countedMiss = true;
                }
                kgmByMagic = probed && (probe.error_code == "encrypted_kgm" || probe.error_code == "encrypted_vpr");
                if (!kgmByMagic) remember(idx, w, probed, probe, L"");
            }

            const Decryptor* dec = DecryptorRegistry::Instance().ByExtension(w.ext);
            if (!dec && kgmByMagic) dec = DecryptorRegistry::Instance().ById("kgm");
            if (dec) {
                item.was_encrypted = true;
                std::wstring msg;
                const ScanCacheEntry* hit = lookup(w);
                if (hit && hit->parse_ok && !hit->target_path.empty() && platform::PathExists(hit->target_path)) {
                    // 源文件未变且解密产物仍在:跳过解密与解析
                    cacheHits.fetch_add(1);
                    item.target_path = hit->target_path;
                    item.success = true;
                    msg = L"已解密 (增量缓存命中)";
                    addTrack(hit->target_path, hit->meta, w);
                    remember(idx, w, true, hit->meta, hit->target_path);
                } else {
                    // 1) 产物索引  2) 旧版本按文件名放在产物目录的结果  3) Auto 模式现场解密
                    bool moved = false;
                    std::wstring product = useIndex ? decIndex.Lookup(w.path, w.size, w.mtime, &moved) : L"";
                    if (moved) indexDirty.store(true);
                    if (product.empty() && !targetDir.empty()) {
                        for (const wchar_t* e : {L".flac", L".mp3", L".ogg", L".m4a", L".wav"}) {
                            std::wstring c = JoinPath(targetDir, StemOf(w.path) + e);
                            if (platform::PathExists(c)) { product = c; break; }
                        }
                    }
                    if (product.empty() && policy == DecryptPolicy::Auto) {
                        DecryptOptions o;
                        o.output_dir = targetDir;
                        o.cancel = &cancel_requested_;
                        DecryptResult dr = dec->Decrypt(w.path, o);
                        if (dr.ok) {
                            product = dr.output;
                            decryptedCount.fetch_add(1);
                            if (useIndex) decIndex.Put(w.path, w.size, w.mtime, product, DecryptIndex::Fingerprint(w.path, w.size));
                            msg = L"解密成功并持久化落盘";
                        } else {
                            msg = dr.message;
                        }
                    }
                    if (!product.empty()) {
                        if (!countedMiss) cacheMisses.fetch_add(1);
                        AudioMetadata meta;
                        bool ok = AudioTagParser::ParseFile(product, meta, parseOpt);
                        bytesRead.fetch_add(meta.io_bytes_read);
                        item.target_path = product;
                        item.success = ok;
                        if (ok) addTrack(product, meta, w);
                        remember(idx, w, ok, meta, product);
                        if (msg.empty()) msg = L"关联到已解密产物";
                    } else if (policy == DecryptPolicy::Ask) {
                        // 不入库 (播放器无法直接播放加密源),交给界面询问是否解密
                        std::lock_guard<std::mutex> lock(pendingMutex);
                        pending.push_back({w.path, dec->Id(), w.size, w.mtime});
                        item.success = true;
                        msg = L"待解密";
                    } else {
                        item.success = false;
                    }
                }
                item.message = msg;
            } else {
                item.target_path = w.path;
                AudioMetadata& meta = probe;
                if (probed) {
                    item.success = true;
                    if (w.cue.sheet) {
                        // CUE 整轨拆分为独立曲目
                        const CueSheet& sheet = *w.cue.sheet;
                        const CueFileEntry& fe = sheet.files[w.cue.file_index];
                        for (const auto& tr : fe.tracks) {
                            AudioMetadata m = meta;
                            m.title = tr.title;
                            m.artist = !tr.performer.empty() ? tr.performer : (!sheet.performer.empty() ? sheet.performer : meta.artist);
                            m.album = !sheet.title.empty() ? sheet.title : meta.album;
                            if (!sheet.performer.empty()) m.album_artist = sheet.performer;
                            if (!sheet.genre.empty()) m.genre = sheet.genre;
                            if (!sheet.date.empty()) m.year = sheet.date;
                            m.track_number = std::to_wstring(tr.number);
                            uint32_t end = tr.end_ms ? tr.end_ms : meta.duration_ms;
                            m.duration_ms = end > tr.start_ms ? end - tr.start_ms : 0;
                            m.cover_data.clear(); // 封面只保留偏移,不重复复制
                            TrackItem t = MusicCatalog::BuildTrack(w.path, m);
                            t.added_time = w.mtime;
                            t.is_cue_track = true;
                            t.cue_start_ms = tr.start_ms;
                            t.cue_end_ms = tr.end_ms;
                            if (!t.has_cover && !w.folder_cover.empty()) t.external_cover_path = w.folder_cover;
                            batch.push_back(std::move(t));
                        }
                        cueTracks.fetch_add(fe.tracks.size());
                        item.message = L"CUE 分轨入库 " + std::to_wstring(fe.tracks.size()) + L" 首";
                    } else {
                        addTrack(w.path, meta, w);
                        item.message = L"直接入库成功";
                    }
                } else {
                    item.success = false;
                    const std::string& ec = meta.error_code;
                    if (ec == "video_file") {
                        item.message = L"视频文件,已跳过";
                        skippedVideos.fetch_add(1);
                    } else if (ec == "encrypted_qq") {
                        item.message = L"QQ 音乐加密文件 (mflac/mgg),需先解密";
                    } else if (ec == "encrypted_unknown") {
                        item.message = L"文件内容疑似加密,无法识别";
                    } else if (ec == "open_failed") {
                        item.message = L"无法打开文件";
                    } else {
                        item.message = L"元数据解析失败";
                    }
                }
            }

            if (batch.size() >= 64) flush();

            if (progressFn) {
                std::lock_guard<std::mutex> lock(progressMutex);
                IngestionProgress prog;
                prog.current = ++progressDone;
                prog.total = total;
                prog.current_filename = FileNameOf(w.path);
                prog.is_transcoding = item.was_encrypted;
                prog.status_text = item.message;
                progressFn(prog);
            }
        }
        flush();
    };

    std::vector<std::thread> workers;
    workers.reserve(numThreads);
    for (size_t t = 0; t < numThreads; ++t) workers.emplace_back(worker);
    for (auto& th : workers) th.join();

    // ---------------- 3. 去重入库 ----------------
    // 已作为某个加密源的产物入库的文件,不再以普通文件身份重复出现;同一路径同一起点只保留一首
    {
        auto norm = [](const std::wstring& p) {
            std::wstring n = Lower(p);
            for (auto& ch : n) if (ch == L'\\') ch = L'/';
            return n;
        };
        std::unordered_set<std::wstring> products;
        for (const auto& t : collected)
            if (!t.source_path.empty()) products.insert(norm(t.file_path));
        std::unordered_set<std::wstring> seen;
        std::vector<TrackItem> unique;
        unique.reserve(collected.size());
        for (auto& t : collected) {
            std::wstring key = norm(t.file_path);
            if (t.source_path.empty() && products.count(key)) continue;
            if (!seen.insert(key + L"|" + std::to_wstring(t.cue_start_ms)).second) continue;
            unique.push_back(std::move(t));
        }
        collected.clear();
        collected.shrink_to_fit();
        catalog.AddPreparedTracks(std::move(unique));
    }

    // ---------------- 4. 汇总 ----------------
    std::vector<IngestionItemResult> out;
    out.reserve(total);
    for (auto& r : results) {
        if (r.source_path.empty()) continue; // 取消时未处理的条目
        if (r.success) summary.success_count++;
        else summary.failed_count++;
        if (r.was_encrypted) summary.transcoded_count++;
        out.push_back(std::move(r));
    }
    summary.cache_hits = cacheHits.load();
    summary.cache_misses = cacheMisses.load();
    // 写回缓存:只保留本次仍存在的文件 (已删除的文件自然淘汰);被取消时不覆盖旧缓存
    if (useCache && !cancel_requested_.load()) {
        ScanCache next;
        for (size_t i = 0; i < total; ++i)
            if (freshValid[i]) next.Put(items[i].path, std::move(fresh[i]));
        summary.cache_saved = next.Save(cachePath);
    }
    summary.skipped_count = skippedVideos.load();
    summary.failed_count -= summary.skipped_count; // 视频文件属于正常跳过,不计入失败
    summary.total_scanned = out.size();
    summary.cue_tracks = cueTracks.load();
    summary.external_covers = externalCovers.load();
    summary.bytes_read = bytesRead.load();
    summary.tracks_added = catalog.GetAllTracks().size();
    summary.cancelled = cancel_requested_.load();
    summary.decrypted_count = decryptedCount.load();
    std::sort(pending.begin(), pending.end(), [](const EncryptedFileInfo& a, const EncryptedFileInfo& b) { return a.path < b.path; });
    summary.encrypted_pending = std::move(pending);
    if (useIndex && (summary.decrypted_count > 0 || indexDirty.load())) decIndex.Save(indexPath);
    summary.elapsed_seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    {
        std::lock_guard<std::mutex> lock(pipeline_mutex_);
        last_summary_ = summary;
    }
    is_running_.store(false);
    return out;
}

bool AudioIngestionPipeline::StartAsync(MusicCatalog& catalog, std::function<void(const IngestionProgress&)> onProgress,
                                        std::function<void(const IngestionSummary&)> onFinished) {
    if (is_running_.exchange(true)) return false;
    if (async_worker_thread_.joinable()) async_worker_thread_.join();
    cancel_requested_.store(false);
    on_progress = std::move(onProgress); // 在调用线程中设置,避免与工作线程竞争

    async_worker_thread_ = std::thread([this, &catalog, onFinished]() {
        this->RunSync(catalog);
        if (onFinished) onFinished(GetLastSummary());
    });
    return true;
}
