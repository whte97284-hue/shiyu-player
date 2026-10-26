#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "../include/kugou_track_bridge.hpp"
#include "../include/platform_io.hpp"
#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cwctype>
#include <thread>

namespace fs = std::filesystem;

namespace lyric_core {

namespace {

static std::wstring ToLower(const std::wstring& s) {
    std::wstring out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::towlower);
    return out;
}

static bool IsEncryptedExt(const std::wstring& ext) {
    return ext == L".kgma" || ext == L".kgm" || ext == L".kgg";
}

static bool IsTrackExt(const std::wstring& ext) {
    static const wchar_t* const kExts[] = {
        L".kgma", L".kgm", L".kgg", L".flac", L".mp3", L".ogg", L".wav", L".m4a"
    };
    for (const wchar_t* e : kExts) {
        if (ext == e) return true;
    }
    return false;
}

// 不抛异常的存在性检查 (没权限、设备未就绪时返回 false)
static bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

static uint64_t Fnv1a64(const std::wstring& s) {
    uint64_t h = 1469598103934665603ULL;
    for (wchar_t c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

static std::wstring DefaultCacheDir() {
#ifdef _WIN32
    wchar_t local[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, local))) {
        return (fs::path(local) / L"KugouPlayer" / L"decrypted").wstring();
    }
#endif
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) tmp = fs::current_path(ec);
    return (tmp / L"kugou_player_decrypted").wstring();
}

#ifdef _WIN32
static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"";
    return fs::path(buf).parent_path().wstring();
}
#endif

static void ParseMetadata(KuGouTrackEntry& e) {
    ParseOptions po;
    po.load_cover = false;
    AudioTagParser::ParseFile(e.playable_path, e.metadata, po);
}

} // namespace

KuGouTrackBridge::KuGouTrackBridge() {
    cache_dir_ = DefaultCacheDir();
    // 掩码表默认由解密器注册表的 tools 目录决定 (界面程序启动时已设为程序目录\tools);
    // 只有那里找不到时才退到本进程所在目录,不依赖当前工作目录
    auto& reg = DecryptorRegistry::Instance();
    mask_path_ = reg.KgmMaskPath();
#ifdef _WIN32
    if (!Exists(mask_path_)) {
        std::wstring exe = ExeDir();
        if (!exe.empty() && Exists(fs::path(exe) / L"tools" / L"kgm" / L"kgm.mask")) {
            reg.SetToolsDir((fs::path(exe) / L"tools").wstring());
            mask_path_ = reg.KgmMaskPath();
        }
    }
#endif
}

void KuGouTrackBridge::SetMaskPath(const std::wstring& maskPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    mask_path_ = maskPath;
    if (Exists(mask_path_)) {
        DecryptorRegistry::Instance().SetKgmMaskPath(mask_path_);
    }
}

void KuGouTrackBridge::SetCacheOutputDir(const std::wstring& cacheDir) {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_dir_ = cacheDir;
    std::error_code ec;
    fs::create_directories(cache_dir_, ec);
}

void KuGouTrackBridge::SetDecryptIndexFile(const std::wstring& indexPath) {
    std::lock_guard<std::mutex> lock(mutex_);
    index_path_ = indexPath;
}

std::wstring KuGouTrackBridge::SharedProduct(const std::wstring& sourcePath) const {
    std::wstring path;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        path = index_path_;
    }
    if (path.empty()) return L"";
    uint64_t size = 0, mtime = 0;
    if (!platform::GetFileInfo(sourcePath, size, mtime)) return L"";
    DecryptIndex index;
    index.Load(path);
    return index.Lookup(sourcePath, size, mtime);
}

void KuGouTrackBridge::RecordProduct(const std::wstring& sourcePath, const std::wstring& product) {
    std::lock_guard<std::mutex> lock(mutex_); // 同时也串行化对记录文件的读改写
    if (index_path_.empty()) return;
    uint64_t size = 0, mtime = 0;
    if (!platform::GetFileInfo(sourcePath, size, mtime)) return;
    DecryptIndex index;
    index.Load(index_path_);
    index.Put(sourcePath, size, mtime, product, DecryptIndex::Fingerprint(sourcePath, size));
    index.Save(index_path_);
}

std::wstring KuGouTrackBridge::CacheDir() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_dir_;
}

std::wstring KuGouTrackBridge::CacheSlotFor(const std::wstring& sourcePath) const {
    std::error_code ec;
    fs::path abs = fs::absolute(sourcePath, ec);
    if (ec) abs = sourcePath;
    const uint64_t size = fs::file_size(abs, ec);
    const auto mtime = fs::last_write_time(abs, ec).time_since_epoch().count();
    const std::wstring key = ToLower(abs.wstring()) + L"|" + std::to_wstring(ec ? 0 : size) +
                             L"|" + std::to_wstring(static_cast<long long>(mtime));
    wchar_t hex[17] = {0};
    swprintf(hex, 17, L"%016llx", static_cast<unsigned long long>(Fnv1a64(key)));
    return (fs::path(CacheDir()) / hex).wstring();
}

// 缓存子目录里已完成的产物 (解密器先写 .part 再改名,所以跳过 .part 和空文件)
std::wstring KuGouTrackBridge::FindCachedOutput(const std::wstring& slotDir) {
    std::error_code ec;
    fs::directory_iterator it(slotDir, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec)) continue;
        if (ToLower(it->path().extension().wstring()) == L".part") continue;
        if (it->file_size(fec) == 0 || fec) continue;
        return it->path().wstring();
    }
    return L"";
}

void KuGouTrackBridge::FillPlainEntry(const std::wstring& sourcePath, KuGouTrackEntry& e) {
    fs::path sp(sourcePath);
    std::error_code ec;
    e.source_path = sourcePath;
    e.file_name = sp.filename().wstring();
    e.ext = ToLower(sp.extension().wstring());
    e.file_size = fs::file_size(sp, ec);
    if (ec) e.file_size = 0;
    e.is_encrypted = IsEncryptedExt(e.ext);
    // 自动匹配同名或相配的酷狗歌词
    e.matched_krc_path = lyric_manager_.FindMatchingKrcFile(sourcePath, L"", L"");
}

std::vector<std::wstring> KuGouTrackBridge::DiscoverKuGouMusicDirectories() {
    std::vector<std::wstring> candidates;
#ifdef _WIN32
    // 空的光驱 / 读卡器不弹「驱动器中没有磁盘」对话框
    const UINT oldMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);

    // 1. 读取 %APPDATA%\KuGou8\KuGou.ini [DownloadConfigSection] -> DownloadPath
    wchar_t appData[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, appData))) {
        fs::path iniPath = fs::path(appData) / L"KuGou8" / L"KuGou.ini";
        if (Exists(iniPath)) {
            wchar_t buf[MAX_PATH] = {0};
            GetPrivateProfileStringW(L"DownloadConfigSection", L"DownloadPath", L"", buf, MAX_PATH, iniPath.c_str());
            if (buf[0] != L'\0' && Exists(buf)) {
                candidates.push_back(buf);
                fs::path p(buf);
                if (Exists(p / L"KugouMusic")) candidates.push_back((p / L"KugouMusic").wstring());
                if (Exists(p / L"GEDAN" / L"KugouMusic")) candidates.push_back((p / L"GEDAN" / L"KugouMusic").wstring());
                if (Exists(p / L"GEDAN")) candidates.push_back((p / L"GEDAN").wstring());
            }
        }
    }

    // 2. 遍历所有本地驱动器根目录下的 KuGou 目录
    wchar_t drives[512] = {0};
    DWORD len = GetLogicalDriveStringsW(512, drives);
    if (len > 0 && len < 512) {
        wchar_t* p = drives;
        while (*p) {
            UINT driveType = GetDriveTypeW(p);
            if (driveType == DRIVE_FIXED || driveType == DRIVE_REMOVABLE) {
                fs::path root(p);
                fs::path kg = root / L"KuGou";
                if (Exists(kg)) {
                    candidates.push_back(kg.wstring());
                    if (Exists(kg / L"KugouMusic")) candidates.push_back((kg / L"KugouMusic").wstring());
                    if (Exists(kg / L"GEDAN" / L"KugouMusic")) candidates.push_back((kg / L"GEDAN" / L"KugouMusic").wstring());
                }
            }
            p += wcslen(p) + 1;
        }
    }
    SetErrorMode(oldMode);
#endif

    // 去重规范化
    std::vector<std::wstring> uniqueDirs;
    for (const auto& d : candidates) {
        std::error_code ec;
        fs::path canon = fs::canonical(d, ec);
        std::wstring s = ec ? d : canon.wstring();
        if (std::find(uniqueDirs.begin(), uniqueDirs.end(), s) == uniqueDirs.end()) {
            uniqueDirs.push_back(s);
        }
    }
    return uniqueDirs;
}

std::vector<KuGouTrackEntry> KuGouTrackBridge::ScanKuGouLibrary(const std::wstring& customDir) {
    std::vector<std::wstring> dirs;
    if (!customDir.empty() && Exists(customDir)) {
        dirs.push_back(customDir);
    } else {
        dirs = DiscoverKuGouMusicDirectories();
    }

    std::vector<KuGouTrackEntry> entries;
    for (const auto& d : dirs) {
        std::error_code ec;
        fs::directory_iterator it(d, ec), end;
        // 单个文件出错只跳过该文件;只有遍历本身失败才结束这个目录
        for (; !ec && it != end; it.increment(ec)) {
            try {
                std::error_code fec;
                if (!it->is_regular_file(fec)) continue;
                std::wstring ext = ToLower(it->path().extension().wstring());
                if (!IsTrackExt(ext)) continue;

                KuGouTrackEntry te;
                FillPlainEntry(it->path().wstring(), te);
                if (!te.is_encrypted) {
                    // 非加密音频自身即为可播路径
                    te.playable_path = te.source_path;
                    te.is_ready_to_play = true;
                    ParseMetadata(te);
                } else {
                    // 已解密过且源文件没变时直接用产物 (先看播放器的共用记录,再看本地缓存)
                    std::wstring cached = SharedProduct(te.source_path);
                    if (cached.empty()) cached = FindCachedOutput(CacheSlotFor(te.source_path));
                    if (!cached.empty()) {
                        te.playable_path = cached;
                        te.is_ready_to_play = true;
                        ParseMetadata(te);
                    }
                }
                entries.push_back(std::move(te));
            } catch (const std::exception&) {
                // 个别文件名无法转换等异常:跳过该文件
            }
        }
    }
    return entries;
}

bool KuGouTrackBridge::PreparePlayableTrack(const std::wstring& sourcePath, KuGouTrackEntry& outEntry) {
    outEntry = KuGouTrackEntry{};
    std::error_code ec;
    if (!fs::is_regular_file(sourcePath, ec)) return false;

    FillPlainEntry(sourcePath, outEntry);
    if (!outEntry.is_encrypted) {
        outEntry.playable_path = sourcePath;
        outEntry.is_ready_to_play = true;
        ParseMetadata(outEntry);
        return true;
    }

    const std::wstring shared = SharedProduct(sourcePath);
    if (!shared.empty()) {
        outEntry.playable_path = shared;
        outEntry.is_ready_to_play = true;
        ParseMetadata(outEntry);
        return true;
    }

    const std::wstring slot = CacheSlotFor(sourcePath);
    {
        // 同一首歌正在被别的线程解密时等它完成,然后直接用它的产物
        std::unique_lock<std::mutex> lock(mutex_);
        inflight_cv_.wait(lock, [&] { return inflight_.count(slot) == 0; });
        std::wstring cached = FindCachedOutput(slot);
        if (!cached.empty()) {
            lock.unlock();
            outEntry.playable_path = cached;
            outEntry.is_ready_to_play = true;
            ParseMetadata(outEntry);
            return true;
        }
        inflight_.insert(slot);
    }
    struct InflightGuard {
        KuGouTrackBridge* self;
        const std::wstring& slot;
        ~InflightGuard() {
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                self->inflight_.erase(slot);
            }
            self->inflight_cv_.notify_all();
        }
    } guard{this, slot};

    // 解密 (不持锁,不同歌曲可以并行)
    const Decryptor* dec = DecryptorRegistry::Instance().ByExtension(outEntry.ext);
    if (!dec) return false;
    fs::create_directories(slot, ec);

    DecryptOptions opt;
    opt.output_dir = slot;
    DecryptResult dr = dec->Decrypt(sourcePath, opt);
    if (!dr.ok || dr.output.empty() || !Exists(dr.output)) {
        return false;
    }

    RecordProduct(sourcePath, dr.output);
    outEntry.playable_path = dr.output;
    outEntry.is_ready_to_play = true;
    ParseMetadata(outEntry);
    return true;
}

size_t KuGouTrackBridge::BatchPrepare(const std::vector<std::wstring>& sourcePaths,
                                     std::function<void(size_t done, size_t total, const std::wstring& current)> onProgress,
                                     unsigned threads) {
    const size_t total = sourcePaths.size();
    if (total == 0) return 0;
    if (threads == 0) threads = std::min(4u, std::max(1u, std::thread::hardware_concurrency()));
    threads = static_cast<unsigned>(std::min<size_t>(threads, total));

    std::atomic<size_t> next{0};
    std::atomic<size_t> successCount{0};
    size_t done = 0;
    std::mutex progressMutex;

    auto worker = [&] {
        for (size_t i = next++; i < total; i = next++) {
            KuGouTrackEntry entry;
            if (PreparePlayableTrack(sourcePaths[i], entry)) ++successCount;
            std::lock_guard<std::mutex> lock(progressMutex);
            ++done;
            if (onProgress) onProgress(done, total, sourcePaths[i]);
        }
    };

    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (auto& th : pool) th.join();
    return successCount;
}

} // namespace lyric_core
