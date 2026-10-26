#include "library_service.h"
#include "scan_cache.hpp"

#include <QClipboard>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <unordered_set>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>

#include "audio_ingestion_pipeline.hpp"
#include "audio_tag_parser.hpp"
#include "library_dedupe.hpp"
#include "folder_watcher.h"
#include "xxhash64.hpp"
#include "kugou_track_bridge.hpp"
#ifdef Q_OS_WIN
#include <malloc.h>
#include <windows.h>
#endif
#include "pinyin_table.hpp"

namespace {
LibraryService* g_instance = nullptr;

QString Q(const std::wstring& w) { return QString::fromStdWString(w); }

std::wstring ParentDir(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s);
}

// CD1/Disc 2 这类子目录归并到上级目录,使多碟专辑成为一张
std::wstring AlbumDirKey(const std::wstring& path) {
    std::wstring dir = ParentDir(path);
    size_t s = dir.find_last_of(L"\\/");
    std::wstring name = s == std::wstring::npos ? dir : dir.substr(s + 1);
    std::wstring l;
    for (wchar_t c : name) l += static_cast<wchar_t>(towlower(c));
    for (const wchar_t* pre : {L"cd", L"disc", L"disk"}) {
        size_t n = wcslen(pre);
        if (l.compare(0, n, pre) == 0) {
            size_t i = n;
            while (i < l.size() && (l[i] == L' ' || l[i] == L'_' || l[i] == L'-' || l[i] == L'.')) ++i;
            bool digits = i < l.size();
            for (size_t k = i; k < l.size(); ++k) digits = digits && iswdigit(l[k]);
            if (digits) return ParentDir(dir);
        }
    }
    return dir;
}
} // namespace

QString SortKeyOf(const std::wstring& text) {
    return QString::fromStdString(PinyinHelper::GetFullPinyin(text)) + QString::fromStdWString(text).toLower();
}

QString LetterOf(const QString& key) {
    for (QChar c : key) {
        if (c.isSpace()) continue;
        char l = c.toLatin1();
        if (l >= 'a' && l <= 'z') return QString(QChar(l - 32));
        if (l >= 'A' && l <= 'Z') return QString(QChar(l));
        return QStringLiteral("#");
    }
    return QStringLiteral("#");
}

QString TrackKeyOf(const TrackItem& t) {
    return QString::fromStdWString(t.file_path) + QLatin1Char('#') + QString::number(t.is_cue_track ? t.cue_start_ms : 0);
}

QString PathKeyOf(const QString& path) {
    QString k = QDir::cleanPath(QDir::fromNativeSeparators(path));
#ifdef Q_OS_WIN
    k = k.toLower();
#endif
    return k;
}

QString RemapTrackKey(const QString& key, const QHash<QString, QString>& pathMap) {
    const int hash = key.lastIndexOf(QLatin1Char('#'));
    if (hash < 0) return {};
    auto it = pathMap.constFind(PathKeyOf(key.left(hash)));
    if (it == pathMap.constEnd()) return {};
    return QDir::toNativeSeparators(it.value()) + key.mid(hash);
}

namespace {
// 「A / B」「A、B」「A feat. B」拆为多位歌手;「&」「,」常见于组合名,不拆
std::vector<std::wstring> SplitArtists(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    auto push = [&]() {
        size_t a = cur.find_first_not_of(L" \t");
        size_t b = cur.find_last_not_of(L" \t");
        if (a != std::wstring::npos) out.push_back(cur.substr(a, b - a + 1));
        cur.clear();
    };
    std::wstring lower;
    for (wchar_t c : s) lower += static_cast<wchar_t>(towlower(c));
    for (size_t i = 0; i < s.size(); ++i) {
        wchar_t c = s[i];
        if (c == L'/' || c == L'\u3001' || c == L';' || c == L'\uff1b' || c == L'\uff0f') {
            push();
            continue;
        }
        bool feat = false;
        for (const wchar_t* f : {L" feat. ", L" ft. ", L" feat ", L" featuring "}) {
            size_t n = wcslen(f);
            if (lower.compare(i, n, f) == 0) {
                push();
                i += n - 1;
                feat = true;
                break;
            }
        }
        if (!feat) cur += c;
    }
    push();
    return out;
}
} // namespace

// 后台扫描作业:持有流水线与新曲库,完成后交给主线程
class ScanJob {
public:
    AudioIngestionPipeline pipeline;
    std::unique_ptr<MusicCatalog> catalog = std::make_unique<MusicCatalog>();
    std::atomic<qint64> lastPost{0};
    QElapsedTimer clock;
    QStringList hidden;   // 本次聚合时要隐藏的文件夹
    QStringList excluded; // 只从曲库移除的单个文件
    QStringList folders;  // 显示去重时,同等音质优先保留排在前面的文件夹里的
    bool collapseCopies = false;
};

void LibraryService::carryScanCache(const std::vector<std::pair<std::wstring, std::wstring>>& moves, bool keepSource) {
    if (moves.empty() || scanning()) return;
    const std::wstring path = (dataDir() + "/scan.cache").toStdWString();
    ScanCache cache;
    if (!cache.Load(path)) return;
    if (cache.Carry(moves, keepSource) > 0) cache.Save(path);
}

QString LibraryService::dataDir() {
    QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(d);
    return d;
}

LibraryService::LibraryService(QObject* parent) : QObject(parent) {
    g_instance = this;
    pending_ = std::make_shared<std::vector<EncryptedFileInfo>>();
    snap_ = std::make_shared<LibrarySnapshot>();
    QSettings s;
    folders_ = s.value("library/folders").toStringList();
    hiddenFolders_ = s.value("library/hiddenFolders").toStringList();
    extraFiles_ = s.value("library/files").toStringList();
    excludedFiles_ = s.value("library/excludedFiles").toStringList();
    folderNotes_ = s.value("library/folderNotes").toMap();
    kugouFolders_ = s.value("library/kugouFolders").toStringList();
    watch_ = s.value("library/watch", true).toBool();
    displayDedupe_ = s.value("library/displayDedupe", false).toBool();
    // 文件夹有变化后等 3 秒:期间又有变化就重新计时,酷狗还在下载 (文件不断变大) 时不会去扫半截文件
    watchDebounce_ = new QTimer(this);
    watchDebounce_->setSingleShot(true);
    watchDebounce_->setInterval(3000);
    connect(watchDebounce_, &QTimer::timeout, this, &LibraryService::backgroundRescan);
    watcher_ = new FolderWatcher(this);
    connect(watcher_, &FolderWatcher::changed, watchDebounce_, qOverload<>(&QTimer::start));
    updateWatcher();
}

QString LibraryService::decryptOutputDir() {
    QString d = QSettings().value("decrypt/outputDir").toString();
    if (d.isEmpty()) d = QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/Decrypted");
    return d;
}

void LibraryService::setWatchFolders(bool on) {
    if (on == watch_) return;
    watch_ = on;
    QSettings().setValue("library/watch", on);
    updateWatcher();
    emit watchFoldersChanged();
}

void LibraryService::updateWatcher() {
    QStringList roots;
    if (watch_)
        for (const QString& f : folders_)
            if (QFileInfo(f).isDir()) roots << f;
    watcher_->setRoots(roots);
}

int LibraryService::addFiles(const QStringList& paths) {
    int added = 0;
    for (const QString& p : paths) {
        QString n = QDir::toNativeSeparators(QDir::cleanPath(p));
        if (n.isEmpty() || !QFileInfo(n).isFile()) continue;
        if (extraFiles_.contains(n, Qt::CaseInsensitive)) continue;
        extraFiles_.append(n);
        ++added;
    }
    if (added) {
        saveFolders();
        rescan();
    }
    return added;
}

void LibraryService::clearExtraFiles() {
    if (extraFiles_.isEmpty()) return;
    extraFiles_.clear();
    saveFolders();
    if (folders_.isEmpty()) finishScan(std::make_shared<LibrarySnapshot>(), IngestionSummary{});
    else rescan();
}

QStringList LibraryService::kugouCandidates() const {
    QStringList all;
    for (const std::wstring& d : lyric_core::KuGouTrackBridge::DiscoverKuGouMusicDirectories())
        all << QDir::toNativeSeparators(QDir::cleanPath(Q(d)));
    auto norm = [](const QString& p) { return QDir::fromNativeSeparators(p).toLower(); };
    const QStringList dismissed = QSettings().value("library/kugouDismissed").toStringList();
    QStringList out;
    for (const QString& c : all) {
        const QString nc = norm(c);
        // 有更具体的子目录 (KuGou\KugouMusic) 时不列父目录,避免把缓存、歌词目录也扫进来
        bool hasChild = false;
        for (const QString& o : all)
            if (norm(o).startsWith(nc + "/")) hasChild = true;
        if (hasChild) continue;
        bool covered = false;
        for (const QString& f : folders_) {
            const QString nf = norm(f);
            if (nc == nf || nc.startsWith(nf + "/")) covered = true;
        }
        if (covered || dismissed.contains(c, Qt::CaseInsensitive) || out.contains(c, Qt::CaseInsensitive)) continue;
        out << c;
    }
    return out;
}

int LibraryService::addKugouFolders(const QStringList& paths) {
    for (const QString& p : paths)
        if (!kugouFolders_.contains(p, Qt::CaseInsensitive)) kugouFolders_ << p;
    QSettings().setValue("library/kugouFolders", kugouFolders_);
    QVariantList urls;
    for (const QString& p : paths) urls << p;
    return addFolderUrls(urls);
}

void LibraryService::dismissKugouCandidates(const QStringList& paths) {
    QSettings s;
    QStringList d = s.value("library/kugouDismissed").toStringList();
    for (const QString& p : paths)
        if (!d.contains(p, Qt::CaseInsensitive)) d << p;
    s.setValue("library/kugouDismissed", d);
}

bool LibraryService::isKugouFolder(const QString& path) const {
    if (kugouFolders_.contains(path, Qt::CaseInsensitive)) return true;
    // 手动加入的酷狗目录也认:路径里有一级目录就叫 KuGou / KugouMusic / KuGou8
    for (const QString& part : QDir::fromNativeSeparators(path).split('/', Qt::SkipEmptyParts)) {
        const QString l = part.toLower();
        if (l == QLatin1String("kugou") || l == QLatin1String("kugoumusic") || l == QLatin1String("kugou8")) return true;
    }
    return false;
}

LibraryService::~LibraryService() {
    if (job_) job_->pipeline.Cancel();
    job_.reset(); // 流水线析构会等待后台线程结束
    g_instance = nullptr;
}

LibraryService* LibraryService::instance() { return g_instance; }

int LibraryService::trackCount() const { return static_cast<int>(snapshot()->songs.size()); } // 不含隐藏文件夹
int LibraryService::albumCount() const { return static_cast<int>(snapshot()->albums.size()); }

std::shared_ptr<const LibrarySnapshot> LibraryService::snapshot() const {
    std::lock_guard<std::mutex> lock(snap_mutex_);
    return snap_;
}

void LibraryService::saveFolders() {
    QSettings().setValue("library/folders", folders_);
    QSettings().setValue("library/hiddenFolders", hiddenFolders_);
    QSettings().setValue("library/files", extraFiles_);
    QSettings().setValue("library/excludedFiles", excludedFiles_);
    updateWatcher();
    emit foldersChanged();
}

void LibraryService::setDisplayDedupe(bool on) {
    if (on == displayDedupe_) return;
    displayDedupe_ = on;
    QSettings().setValue("library/displayDedupe", on);
    emit displayDedupeChanged();
    // 和隐藏文件夹一样:增量扫描几乎全部命中缓存,只是重新聚合
    rescan();
}

void LibraryService::setFolderNote(const QString& path, const QString& note) {
    if (!folders_.contains(path)) return;
    const QString n = note.simplified();
    if (folderNotes_.value(path).toString() == n) return;
    if (n.isEmpty()) folderNotes_.remove(path);
    else folderNotes_.insert(path, n);
    QSettings().setValue("library/folderNotes", folderNotes_);
    emit foldersChanged();
}

void LibraryService::setFolderHidden(const QString& path, bool hidden) {
    if (!folders_.contains(path) || hiddenFolders_.contains(path) == hidden) return;
    if (hidden) hiddenFolders_.append(path);
    else hiddenFolders_.removeOne(path);
    saveFolders();
    // 增量扫描几乎全部命中缓存,只是按新的隐藏列表重新聚合专辑 / 歌手 / 歌曲
    rescan();
}

void LibraryService::addFolder(const QString& path) {
    if (addFolderUrls({path}) == 0) rescan(); // 已存在的文件夹:当作一次手动重扫
}

int LibraryService::addFolderUrls(const QVariantList& urls) {
    int added = 0;
    QStringList files;
    for (const QVariant& v : urls) {
        QUrl u = v.toUrl();
        QString local = u.isLocalFile() ? u.toLocalFile() : v.toString();
        if (local.isEmpty()) continue;
        QFileInfo fi(local);
        // 拖进来的是单个音频文件:只加这一首,不把整个所在文件夹都加进来
        if (fi.isFile()) {
            if (AudioTagParser::IsSupportedExtension(("." + fi.suffix().toLower()).toStdWString())) files << fi.absoluteFilePath();
            continue;
        }
        if (!fi.isDir()) continue;
        QString p = QDir::toNativeSeparators(QDir::cleanPath(fi.absoluteFilePath()));
        // 已被某个父文件夹覆盖的子文件夹不重复添加;新加的父文件夹吸收已有的子文件夹
        bool covered = false;
        for (const QString& f : folders_) {
            QString fp = QDir::fromNativeSeparators(f), np = QDir::fromNativeSeparators(p);
            if (np.compare(fp, Qt::CaseInsensitive) == 0 || np.startsWith(fp + "/", Qt::CaseInsensitive)) covered = true;
        }
        if (covered) continue;
        QString np = QDir::fromNativeSeparators(p);
        folders_.erase(std::remove_if(folders_.begin(), folders_.end(), [&](const QString& f) {
                           return QDir::fromNativeSeparators(f).startsWith(np + "/", Qt::CaseInsensitive);
                       }),
                       folders_.end());
        folders_.append(p);
        ++added;
    }
    if (added) {
        saveFolders();
        rescan();
    }
    if (!files.isEmpty()) added += addFiles(files);
    return added;
}

void LibraryService::removeFolder(const QString& path) {
    if (!folders_.removeOne(path)) return;
    hiddenFolders_.removeOne(path);
    if (folderNotes_.remove(path)) QSettings().setValue("library/folderNotes", folderNotes_);
    saveFolders();
    if (scanning_) {
        rescanPending_ = true;
        cancelScan();
    } else if (folders_.isEmpty() && extraFiles_.isEmpty()) {
        // 最后一个文件夹被移除:清空曲库
        finishScan(std::make_shared<LibrarySnapshot>(), IngestionSummary{});
    } else {
        startScan();
    }
}

void LibraryService::excludeFiles(const QStringList& paths) {
    int n = 0;
    for (const QString& p : paths) {
        QString c = QDir::toNativeSeparators(QDir::cleanPath(p));
        if (c.isEmpty() || excludedFiles_.contains(c, Qt::CaseInsensitive)) continue;
        excludedFiles_.append(c);
        ++n;
    }
    if (!n) return;
    saveFolders();
    rescan();
}

void LibraryService::includeFiles(const QStringList& paths) {
    int n = 0;
    for (const QString& p : paths) {
        QString c = QDir::toNativeSeparators(QDir::cleanPath(p));
        for (int i = excludedFiles_.size() - 1; i >= 0; --i)
            if (excludedFiles_[i].compare(c, Qt::CaseInsensitive) == 0) {
                excludedFiles_.removeAt(i);
                ++n;
            }
    }
    if (!n) return;
    saveFolders();
    rescan();
}

void LibraryService::clearExcludedFiles() {
    if (excludedFiles_.isEmpty()) return;
    excludedFiles_.clear();
    saveFolders();
    rescan();
}

void LibraryService::rescan() {
    if (folders_.isEmpty() && extraFiles_.isEmpty()) return;
    if (scanning_) {
        rescanPending_ = true;
        quietPending_ = false;
        // 后台重扫进行中又有人要扫:改为正常提示
        if (quiet_) { quiet_ = false; emit scanningChanged(); }
        return;
    }
    quiet_ = false;
    startScan();
}

// 文件夹监控触发:悄悄重扫,没有变化时界面完全不动
void LibraryService::backgroundRescan() {
    if (folders_.isEmpty() && extraFiles_.isEmpty()) return;
    if (scanning_) {
        rescanPending_ = true;
        return;
    }
    quiet_ = true;
    startScan();
}

void LibraryService::cancelScan() {
    if (scanning_ && job_) job_->pipeline.Cancel();
}

void LibraryService::startScan() {
    job_ = std::make_unique<ScanJob>();
    ScanJob* job = job_.get();
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    job->pipeline.SetLazyCoverExtract(true);
    // 设置页:扫描线程 / CUE 分轨 / 旧编码标签代码页 / ID3v1 回读
    QSettings st;
    // 影响解析结果的设置改过:丢弃扫描缓存,本次全量解析
    if (st.value("library/needRescan", false).toBool()) QFile::remove(dataDir + "/scan.cache");
    job->pipeline.SetThreadCount(static_cast<size_t>(std::max(0, st.value("library/threads", 0).toInt())));
    job->pipeline.SetSplitCueSheets(st.value("library/splitCue", true).toBool());
    job->pipeline.SetLegacyCodePage(static_cast<unsigned>(st.value("library/codePage", 936).toInt()));
    job->pipeline.SetReadId3v1(st.value("library/readId3v1", true).toBool());
    job->pipeline.SetScanCacheFile((dataDir + "/scan.cache").toStdWString());
    // 加密源:扫描只收集,是否转换由 DecryptService 按「询问 / 自动 / 不转换」决定 (唯一的开关);
    // 已转换过的按解密记录 (路径或内容指纹) 直接入库。旧版本扫描时自动解密到 数据目录\decoded,
    // 那里还有文件时继续按文件名认领,新的转换一律写到转换服务的输出目录
    const QString legacy = dataDir + "/decoded";
    job->pipeline.SetTargetLibraryDir(QDir(legacy).exists() ? legacy.toStdWString() : std::wstring());
    job->pipeline.SetDecryptPolicy(DecryptPolicy::Ask);
    job->pipeline.SetDecryptIndexFile((dataDir + "/decrypt.index").toStdWString());
    for (const QString& f : folders_) job->pipeline.AddInputPath(f.toStdWString());
    for (const QString& f : extraFiles_)
        if (QFileInfo(f).isFile()) job->pipeline.AddInputPath(f.toStdWString());
    job->hidden = hiddenFolders_;
    job->excluded = excludedFiles_;
    job->folders = folders_;
    job->collapseCopies = displayDedupe_;
    job->clock.start();

    scanning_ = true;
    rescanPending_ = false;
    progress_ = 0;
    scanDone_ = scanTotal_ = 0;
    scanFile_.clear();
    status_ = tr("正在查找音乐文件…");
    emit scanningChanged();
    emit progressChanged();

    job->pipeline.StartAsync(
        *job->catalog,
        [this, job](const IngestionProgress& p) {
            // 进度节流:最多每 60ms 投递一次到界面线程
            qint64 now = job->clock.elapsed();
            if (now - job->lastPost.load() < 60 && p.current != p.total) return;
            job->lastPost.store(now);
            double v = p.total ? double(p.current) / double(p.total) : 0.0;
            int done = static_cast<int>(p.current), total = static_cast<int>(p.total);
            QString file = Q(p.current_filename);
            QMetaObject::invokeMethod(this, [this, v, done, total, file]() {
                if (!scanning_) return;
                progress_ = v;
                scanDone_ = done;
                scanTotal_ = total;
                scanFile_ = file;
                status_ = tr("正在识别 %1 / %2").arg(done).arg(total);
                emit progressChanged();
            }, Qt::QueuedConnection);
        },
        [this, job](const IngestionSummary& sum) {
            // 专辑聚合同样在后台线程完成,界面线程只做指针交换
            auto snap = BuildAlbums(std::move(job->catalog), job->hidden, job->excluded, job->collapseCopies, job->folders);
            QMetaObject::invokeMethod(this, [this, snap, sum]() { finishScan(snap, sum); }, Qt::QueuedConnection);
        });
}

void LibraryService::finishScan(std::shared_ptr<LibrarySnapshot> snap, const IngestionSummary& sum) {
    // 被取消的扫描结果不完整:曲库已有内容时保留旧快照,首次导入时保留已识别的部分。
    // 内容与当前完全一致 (监控误报、手动重扫但没变化):同样沿用旧快照,曲目 ID 不变,
    // 列表、详情页、播放队列、歌词都不会被刷新打断
    bool unchanged = false;
    {
        std::lock_guard<std::mutex> lock(snap_mutex_);
        unchanged = !sum.cancelled && snap_ && snap->signature != 0 && snap->signature == snap_->signature;
    }
    const bool keepOld = (sum.cancelled && albumCount() > 0) || unchanged;
    const bool wasQuiet = quiet_;
    quiet_ = false;
    if (!keepOld) {
        {
            std::lock_guard<std::mutex> lock(snap_mutex_);
            snap_ = snap;
        }
        ++generation_;
    }
    scanning_ = false;
    progress_ = 1.0;
    scanFile_.clear();
    if (!sum.cancelled) {
        pending_ = std::make_shared<std::vector<EncryptedFileInfo>>(sum.encrypted_pending);
        emit pendingEncryptedChanged();
    }

    const int tracks = static_cast<int>(snap->catalog.GetAllTracks().size());
    const int albums = static_cast<int>(snap->albums.size());
    lastScan_ = QVariantMap{
        {"tracks", tracks},
        {"albums", albums},
        {"failed", static_cast<int>(sum.failed_count)},
        {"skipped", static_cast<int>(sum.skipped_count)},
        {"seconds", sum.elapsed_seconds},
        {"incremental", sum.cache_hits > 0},
        {"cancelled", sum.cancelled},
        {"unchanged", unchanged},
        // 后台重扫没发现变化:不弹扫描结果
        {"shown", !(wasQuiet && unchanged)},
    };
    if (sum.cancelled) status_ = tr("已取消扫描");
    else if (folders_.isEmpty() && extraFiles_.isEmpty()) status_ = tr("曲库已清空");
    else
        status_ = tr("%1 首 · %2 张专辑 · %3 秒%4")
                      .arg(tracks)
                      .arg(albums)
                      .arg(sum.elapsed_seconds, 0, 'f', 1)
                      .arg(sum.failed_count ? tr(" · %1 个文件无法识别").arg(sum.failed_count) : QString());
    if (!keepOld) emit libraryChanged();
    emit scanningChanged();
    emit progressChanged();
    // 后台线程已经结束,回收作业对象放到事件循环下一轮,避免在回调栈内析构流水线;
    // 扫描期间有新的文件夹变动则紧接着再扫一次
    QMetaObject::invokeMethod(this, [this]() {
        if (scanning_) return;
        job_.reset();
        ReleaseFreeHeap();
        if (rescanPending_) {
            rescanPending_ = false;
            quiet_ = quietPending_;
            quietPending_ = true;
            if (folders_.isEmpty() && extraFiles_.isEmpty()) finishScan(std::make_shared<LibrarySnapshot>(), IngestionSummary{});
            else startScan();
        }
    }, Qt::QueuedConnection);
}

// 扫描时几十个线程同时解析标签,堆里会留下大量已释放的零碎块;扫描结束 (旧快照与流水线都已释放) 后
// 把空闲块交还系统,载入大曲库后的常驻内存回到实际占用水平
void LibraryService::ReleaseFreeHeap() {
#ifdef Q_OS_WIN
    _heapmin();
    HeapCompact(GetProcessHeap(), 0);
#endif
}

std::shared_ptr<LibrarySnapshot> LibraryService::BuildAlbums(std::unique_ptr<MusicCatalog> catalog, const QStringList& hidden,
                                                            const QStringList& excluded, bool collapseCopies,
                                                            const QStringList& folderOrder) {
    auto snap = std::make_shared<LibrarySnapshot>();
    snap->catalog = std::move(*catalog);
    const auto& tracks = snap->catalog.GetAllTracks();
    // 隐藏文件夹:曲目仍留在 catalog 里 (收藏 / 歌单按曲目查找时还能找到),只是不进入下面的专辑、歌手、歌曲列表
    std::vector<std::wstring> hiddenPrefixes;
    for (const QString& h : hidden) {
        std::wstring p = QDir::cleanPath(h).toLower().toStdWString();
        for (wchar_t& c : p) if (c == L'\\') c = L'/';
        if (!p.empty() && p.back() != L'/') p += L'/';
        hiddenPrefixes.push_back(p);
    }
    std::unordered_set<std::wstring> excludedKeys;
    for (const QString& e : excluded) {
        std::wstring p = QDir::cleanPath(e).toLower().toStdWString();
        for (wchar_t& c : p) if (c == L'\\') c = L'/';
        excludedKeys.insert(p);
    }
    auto isHiddenBase = [&](const TrackItem& t) {
        if (!excludedKeys.empty()) {
            std::wstring p;
            p.reserve(t.file_path.size());
            for (wchar_t c : t.file_path) p += c == L'\\' ? L'/' : static_cast<wchar_t>(towlower(c));
            if (excludedKeys.count(p)) return true;
        }
        if (hiddenPrefixes.empty()) return false;
        // 解密产物按它自己的落盘位置归类/隐藏 (转换输出目录就是用户可见的曲库)
        const std::wstring& path = t.file_path;
        std::wstring p;
        p.reserve(path.size());
        for (wchar_t c : path) p += c == L'\\' ? L'/' : static_cast<wchar_t>(towlower(c));
        for (const auto& h : hiddenPrefixes)
            if (p.compare(0, h.size(), h) == 0) return true;
        return false;
    };

    // 显示去重:歌手 + 歌名规范化后相同、时长相差不到 3 秒的算同一首,只显示音质最好的一份
    // (Live、伴奏等规范化后仍不同,不会被收起;没有歌手或歌名标签的不参与)
    if (collapseCopies) {
        std::vector<std::wstring> folderKeys;
        for (const QString& f : folderOrder) {
            std::wstring k = QDir::cleanPath(f).toLower().toStdWString();
            for (wchar_t& c : k) if (c == L'\\') c = L'/';
            if (!k.empty() && k.back() != L'/') k += L'/';
            folderKeys.push_back(k);
        }
        auto folderRank = [&](const TrackItem& t) {
            std::wstring p;
            p.reserve(t.file_path.size());
            for (wchar_t c : t.file_path) p += c == L'\\' ? L'/' : static_cast<wchar_t>(towlower(c));
            for (size_t i = 0; i < folderKeys.size(); ++i)
                if (p.compare(0, folderKeys[i].size(), folderKeys[i]) == 0) return static_cast<int>(i);
            return static_cast<int>(folderKeys.size());
        };
        auto better = [&](const TrackItem* a, const TrackItem* b) {
            if (a->is_lossless != b->is_lossless) return a->is_lossless;
            if (a->bits_per_sample != b->bits_per_sample) return a->bits_per_sample > b->bits_per_sample;
            if (a->sample_rate != b->sample_rate) return a->sample_rate > b->sample_rate;
            if (a->bitrate_kbps != b->bitrate_kbps) return a->bitrate_kbps > b->bitrate_kbps;
            const int ra = folderRank(*a), rb = folderRank(*b);
            if (ra != rb) return ra < rb;
            return a->file_path < b->file_path;
        };
        std::unordered_map<std::wstring, std::vector<const TrackItem*>> byKey;
        for (const auto& t : tracks) {
            if (t.duration_ms == 0 || isHiddenBase(t)) continue;
            const std::wstring artist = dedupe::NormalizeArtist(t.artist), title = dedupe::NormalizeTitle(t.title);
            if (artist.empty() || title.empty()) continue;
            byKey[artist + L"\x1f" + title].push_back(&t);
        }
        for (auto& kv : byKey) {
            auto& v = kv.second;
            if (v.size() < 2) continue;
            std::sort(v.begin(), v.end(), [](const TrackItem* a, const TrackItem* b) { return a->duration_ms < b->duration_ms; });
            for (size_t i = 0; i < v.size();) {
                size_t j = i + 1;
                while (j < v.size() && v[j]->duration_ms - v[i]->duration_ms <= 3000) ++j;
                if (j - i >= 2) {
                    const TrackItem* best = v[i];
                    for (size_t k = i + 1; k < j; ++k)
                        if (better(v[k], best)) best = v[k];
                    for (size_t k = i; k < j; ++k)
                        if (v[k] != best) snap->shownCopy[v[k]->id] = best->id;
                }
                i = j;
            }
        }
    }
    auto isHidden = [&](const TrackItem& t) { return isHiddenBase(t) || (!snap->shownCopy.empty() && snap->shownCopy.count(t.id)); };

    std::map<std::wstring, int> index;
    for (const auto& t : tracks) {
        if (isHidden(t)) continue;
        std::wstring albumLower;
        for (wchar_t c : t.album) albumLower += static_cast<wchar_t>(towlower(c));
        std::wstring key = albumLower + L"\x1F" + AlbumDirKey(t.file_path);
        auto it = index.find(key);
        int ai;
        if (it == index.end()) {
            ai = static_cast<int>(snap->albums.size());
            index.emplace(key, ai);
            snap->albums.emplace_back();
            snap->albums.back().title = Q(t.album);
        } else {
            ai = it->second;
        }
        snap->albums[ai].tracks.push_back(&t);
    }

    for (auto& a : snap->albums) {
        std::sort(a.tracks.begin(), a.tracks.end(), [](const TrackItem* x, const TrackItem* y) {
            if (x->disc_no != y->disc_no) return x->disc_no < y->disc_no;
            if (x->file_path == y->file_path && x->is_cue_track) return x->cue_start_ms < y->cue_start_ms;
            if (x->track_no != y->track_no) return x->track_no < y->track_no;
            return x->file_name < y->file_name;
        });
        std::map<std::wstring, int> artists;
        for (const TrackItem* t : a.tracks) {
            a.totalMs += t->duration_ms;
            a.maxRate = std::max(a.maxRate, t->sample_rate);
            a.maxBits = std::max(a.maxBits, t->bits_per_sample);
            a.lossless = a.lossless && t->is_lossless;
            a.hires = a.hires || t->is_hires;
            a.cue = a.cue || t->is_cue_track;
            if (t->codec.compare(0, 3, L"DSD") == 0) {
                a.dsd = true;
                a.dsdCodec = Q(t->codec);
            }
            if (!a.coverTrack && (t->has_cover || !t->external_cover_path.empty())) a.coverTrack = t;
            if (a.year.isEmpty() && !t->year.empty()) a.year = Q(t->year).left(4);
            artists[!t->album_artist.empty() ? t->album_artist : t->artist]++;
            a.addedTime = std::max(a.addedTime, t->added_time);
        }
        if (artists.size() == 1) a.artist = Q(artists.begin()->first);
        else if (artists.size() > 1) {
            // 多位歌手:取出现最多者,占比不足一半则视为合辑
            auto best = std::max_element(artists.begin(), artists.end(),
                                         [](const auto& l, const auto& r) { return l.second < r.second; });
            a.artist = best->second * 2 >= static_cast<int>(a.tracks.size()) ? Q(best->first) : QObject::tr("群星");
        }
        if (!a.coverTrack && !a.tracks.empty()) a.coverTrack = a.tracks.front();
        a.sortKey = SortKeyOf(a.title.toStdWString());
        // 群星 / 没有歌手的合辑排在按歌手排序的最后
        a.artistSortKey = a.artist.isEmpty() || a.artist == QObject::tr("群星") ? QStringLiteral("\uffff") + a.sortKey
                                                                                : SortKeyOf(a.artist.toStdWString());
    }
    std::sort(snap->albums.begin(), snap->albums.end(),
              [](const AlbumInfo& x, const AlbumInfo& y) { return x.sortKey < y.sortKey; });
    for (int i = 0; i < static_cast<int>(snap->albums.size()); ++i)
        for (const TrackItem* t : snap->albums[i].tracks) snap->trackAlbum[t->id] = i;

    // ---- 歌手 ----
    std::unordered_map<std::wstring, int> artistIdx;
    for (int ai = 0; ai < static_cast<int>(snap->albums.size()); ++ai) {
        for (const TrackItem* t : snap->albums[ai].tracks) {
            const std::wstring& raw = !t->artist.empty() ? t->artist : t->album_artist;
            std::vector<std::wstring> names = SplitArtists(raw);
            if (names.empty()) names.push_back(L"\u672a\u77e5\u6b4c\u624b"); // 未知歌手
            for (const std::wstring& n : names) {
                std::wstring key;
                for (wchar_t c : n) key += static_cast<wchar_t>(towlower(c));
                auto it = artistIdx.find(key);
                int idx;
                if (it == artistIdx.end()) {
                    idx = static_cast<int>(snap->artists.size());
                    artistIdx.emplace(key, idx);
                    snap->artists.emplace_back();
                    snap->artists.back().name = Q(n);
                } else {
                    idx = it->second;
                }
                ArtistInfo& ar = snap->artists[idx];
                ar.tracks.push_back(t);
                if (ar.albums.empty() || ar.albums.back() != ai) ar.albums.push_back(ai);
            }
        }
    }
    for (ArtistInfo& ar : snap->artists) {
        ar.sortKey = SortKeyOf(ar.name.toStdWString());
        ar.letter = LetterOf(ar.sortKey);
        // 头像:该歌手曲目最多且有封面的专辑
        int best = -1;
        size_t bestN = 0;
        for (int ai : ar.albums) {
            const AlbumInfo& a = snap->albums[ai];
            size_t n = 0;
            for (const TrackItem* t : ar.tracks) n += snap->trackAlbum[t->id] == ai;
            bool cover = a.coverTrack && (a.coverTrack->has_cover || !a.coverTrack->external_cover_path.empty());
            n = n * 2 + (cover ? 1000 : 0);
            if (best < 0 || n > bestN) best = ai, bestN = n;
        }
        ar.coverAlbum = best;
        std::stable_sort(ar.albums.begin(), ar.albums.end(), [&](int x, int y) {
            return snap->albums[x].year > snap->albums[y].year;
        });
    }
    std::sort(snap->artists.begin(), snap->artists.end(),
              [](const ArtistInfo& x, const ArtistInfo& y) { return x.sortKey < y.sortKey; });

    // ---- 全部歌曲 (按标题拼音) ----
    std::vector<std::pair<QString, const TrackItem*>> keyed;
    keyed.reserve(tracks.size());
    for (const auto& t : tracks)
        if (!isHidden(t)) keyed.emplace_back(SortKeyOf(t.title), &t);
    std::sort(keyed.begin(), keyed.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
    snap->songs.reserve(keyed.size());
    snap->songLetters.reserve(keyed.size());
    for (const auto& k : keyed) {
        snap->songs.push_back(k.second);
        snap->songLetters.push_back(LetterOf(k.first));
    }
    // 内容指纹:每首歌的路径 / CUE 起点 / 大小 / 修改时间 / 标签 / 音质各自哈希后求和 (与扫描线程的完成顺序无关),
    // 再并上影响显示的设置 (隐藏 / 移除的文件、显示去重、文件夹顺序)
    {
        uint64_t sum = 0, mix = 0;
        auto addStr = [](kp::Xxh64& h, const std::wstring& w) {
            h.Add(reinterpret_cast<const uint8_t*>(w.data()), w.size() * sizeof(wchar_t));
            const uint8_t sep = 0x1f;
            h.Add(&sep, 1);
        };
        auto addNum = [](kp::Xxh64& h, uint64_t v) { h.Add(reinterpret_cast<const uint8_t*>(&v), sizeof v); };
        for (const TrackItem& t : tracks) {
            kp::Xxh64 h(0x6b70);
            for (const std::wstring* w : {&t.file_path, &t.source_path, &t.title, &t.artist, &t.album, &t.album_artist,
                                          &t.genre, &t.year, &t.format, &t.codec, &t.external_cover_path})
                addStr(h, *w);
            for (uint64_t v : {uint64_t(t.cue_start_ms), uint64_t(t.cue_end_ms), t.file_size, t.added_time,
                               uint64_t(t.duration_ms), uint64_t(t.bitrate_kbps), uint64_t(t.sample_rate),
                               uint64_t(t.bits_per_sample), uint64_t(t.track_no), uint64_t(t.disc_no),
                               uint64_t(t.has_cover), uint64_t(t.is_cue_track)})
                addNum(h, v);
            const uint64_t d = h.Digest();
            sum += d;
            mix ^= d * 0x9E3779B97F4A7C15ull;
        }
        kp::Xxh64 h(0x736e);
        addNum(h, sum);
        addNum(h, mix);
        addNum(h, tracks.size());
        addNum(h, collapseCopies ? 1 : 0);
        for (const QStringList* l : {&hidden, &excluded, &folderOrder}) {
            for (const QString& x : *l) addStr(h, x.toStdWString());
            addStr(h, L"|");
        }
        snap->signature = h.Digest() | 1; // 0 留给「没有指纹」
    }
    return snap;
}

QString LibraryService::formatDuration(uint64_t ms) {
    uint64_t s = ms / 1000;
    if (s >= 3600) return QString("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QChar('0')).arg(s % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}

QString LibraryService::formatLongDuration(uint64_t ms) {
    const uint64_t min = (ms + 30000) / 60000;
    if (min < 60) return tr("%1 分钟").arg(std::max<uint64_t>(1, min));
    return min % 60 ? tr("%1 小时 %2 分钟").arg(min / 60).arg(min % 60) : tr("%1 小时").arg(min / 60);
}

QVariantList LibraryService::albumTrackIds(int i) const {
    QVariantList out;
    auto s = snapshot();
    if (i < 0 || i >= static_cast<int>(s->albums.size())) return out;
    for (const TrackItem* t : s->albums[i].tracks) out.append(static_cast<double>(t->id));
    return out;
}

QVariantList LibraryService::artistTrackIds(int i) const {
    QVariantList out;
    auto s = snapshot();
    if (i < 0 || i >= static_cast<int>(s->artists.size())) return out;
    out.reserve(static_cast<int>(s->artists[i].tracks.size()));
    for (const TrackItem* t : s->artists[i].tracks) out.append(static_cast<double>(t->id));
    return out;
}

QString LibraryService::trackQuality(const TrackItem& t) {
    if (t.codec.compare(0, 3, L"DSD") == 0) return Q(t.codec);
    if (t.is_lossless && t.sample_rate)
        return QString("%1/%2").arg(t.bits_per_sample).arg(t.sample_rate % 1000 ? QString::number(t.sample_rate / 1000.0, 'f', 1)
                                                                                    : QString::number(t.sample_rate / 1000));
    if (t.bitrate_kbps) return QString("%1k").arg(t.bitrate_kbps);
    return Q(t.format);
}

QString LibraryService::qualityBadge(int i) const {
    auto s = snapshot();
    if (i < 0 || i >= static_cast<int>(s->albums.size())) return {};
    const AlbumInfo& a = s->albums[i];
    if (a.dsd) return a.dsdCodec;
    if (a.hires) return QString("%1/%2").arg(a.maxBits).arg(a.maxRate % 1000 ? QString::number(a.maxRate / 1000.0, 'f', 1)
                                                                           : QString::number(a.maxRate / 1000));
    if (a.lossless) return tr("无损");
    return {};
}

int LibraryService::featuredAlbum(int skip) const {
    auto s = snapshot();
    const int n = static_cast<int>(s->albums.size());
    if (n == 0) return -1;
    std::vector<int> pool;
    for (int i = 0; i < n; ++i) {
        const AlbumInfo& a = s->albums[i];
        if (a.coverTrack && (a.coverTrack->has_cover || !a.coverTrack->external_cover_path.empty()) && (a.hires || a.dsd))
            pool.push_back(i);
    }
    if (pool.empty())
        for (int i = 0; i < n; ++i) pool.push_back(i);
    // 按「专辑名 + 歌手」和日期算出的散列排名:新增 / 删掉别的专辑不会让今天的推荐变掉
    const size_t seed = qHash(QDate::currentDate().toJulianDay());
    std::vector<std::pair<size_t, int>> ranked;
    ranked.reserve(pool.size());
    for (int i : pool) ranked.emplace_back(qHash(s->albums[i].title + QChar(0x1f) + s->albums[i].artist, seed), i);
    const size_t k = static_cast<size_t>(std::max(0, skip)) % ranked.size();
    std::nth_element(ranked.begin(), ranked.begin() + k, ranked.end());
    return ranked[k].second;
}

QVariantMap LibraryService::albumInfo(int i) const {
    QVariantMap m;
    auto s = snapshot();
    if (i < 0 || i >= static_cast<int>(s->albums.size())) return m;
    const AlbumInfo& a = s->albums[i];
    m["index"] = i;
    m["title"] = a.title;
    m["artist"] = a.artist;
    m["year"] = a.year;
    m["cover"] = QString("image://cover/a/%1/%2").arg(generation_).arg(i);
    m["badge"] = qualityBadge(i);
    m["trackCount"] = static_cast<int>(a.tracks.size());
    QStringList parts;
    if (!a.year.isEmpty()) parts << a.year;
    parts << tr("%1 首").arg(a.tracks.size()) << formatLongDuration(a.totalMs);
    m["summary"] = parts.join(QStringLiteral("  ·  "));
    return m;
}

QVariantMap LibraryService::artistInfo(int i) const {
    QVariantMap m;
    auto s = snapshot();
    if (i < 0 || i >= static_cast<int>(s->artists.size())) return m;
    const ArtistInfo& a = s->artists[i];
    uint64_t ms = 0;
    for (const TrackItem* t : a.tracks) ms += t->duration_ms;
    m["index"] = i;
    m["name"] = a.name;
    m["cover"] = a.coverAlbum >= 0 ? QString("image://cover/a/%1/%2").arg(generation_).arg(a.coverAlbum) : QString();
    m["albumCount"] = static_cast<int>(a.albums.size());
    m["trackCount"] = static_cast<int>(a.tracks.size());
    m["summary"] = tr("%1 张专辑  ·  %2 首  ·  %3").arg(a.albums.size()).arg(a.tracks.size()).arg(formatLongDuration(ms));
    return m;
}

QVariantList LibraryService::artistAlbums(int i) const {
    QVariantList out;
    auto s = snapshot();
    if (i < 0 || i >= static_cast<int>(s->artists.size())) return out;
    for (int ai : s->artists[i].albums) out.append(albumInfo(ai));
    return out;
}

int LibraryService::artistIndexOf(const QString& name) const {
    auto s = snapshot();
    for (int i = 0; i < static_cast<int>(s->artists.size()); ++i)
        if (s->artists[i].name.compare(name, Qt::CaseInsensitive) == 0) return i;
    // 合作曲目:取第一位歌手
    std::vector<std::wstring> parts = SplitArtists(name.toStdWString());
    if (parts.size() > 1) return artistIndexOf(QString::fromStdWString(parts.front()));
    // 异体字 / 简繁写法不同 (久石让 / 久石譲):全拼相同、字数相同且只有一位时才认,同音的几位歌手不乱认
    const std::wstring wn = name.toStdWString();
    if (!PinyinHelper::HasPhonetic(wn)) return -1;
    const std::string py = PinyinHelper::GetFullPinyin(wn);
    int found = -1;
    for (int i = 0; i < static_cast<int>(s->artists.size()); ++i) {
        const QString& a = s->artists[i].name;
        if (a.size() != name.size() || PinyinHelper::GetFullPinyin(a.toStdWString()) != py) continue;
        if (found >= 0) return -1;
        found = i;
    }
    return found;
}

int LibraryService::albumOfTrack(double trackId) const {
    auto s = snapshot();
    auto it = s->trackAlbum.find(static_cast<uint64_t>(trackId));
    return it == s->trackAlbum.end() ? -1 : it->second;
}

QVariantMap LibraryService::trackInfo(double trackId) const {
    QVariantMap m;
    auto s = snapshot();
    const TrackItem* t = s->catalog.FindById(static_cast<uint64_t>(trackId));
    if (!t) return m;
    const QString path = Q(t->file_path);
    m["title"] = Q(t->title.empty() ? t->file_name : t->title);
    m["artist"] = Q(t->artist.empty() ? t->album_artist : t->artist);
    m["album"] = Q(t->album);
    m["albumArtist"] = Q(t->album_artist);
    m["year"] = Q(t->year);
    m["genre"] = Q(t->genre);
    m["trackNo"] = t->track_no;
    m["discNo"] = t->disc_no;
    m["path"] = QDir::toNativeSeparators(path);
    m["folder"] = QDir::toNativeSeparators(QFileInfo(path).absolutePath());
    m["format"] = Q(t->format);
    m["codec"] = Q(t->codec);
    m["duration"] = formatDuration(t->duration_ms);
    m["bitrate"] = static_cast<int>(t->bitrate_kbps);
    m["sampleRate"] = static_cast<int>(t->sample_rate);
    m["bits"] = static_cast<int>(t->bits_per_sample);
    m["channels"] = static_cast<int>(t->channels);
    m["size"] = static_cast<double>(t->file_size);
    m["quality"] = trackQuality(*t);
    m["isCue"] = t->is_cue_track;
    m["exists"] = QFileInfo::exists(path);
    m["source"] = t->source_path.empty() ? QString() : QDir::toNativeSeparators(Q(t->source_path)); // 解密产物的加密源
    const QFileInfo fi(path);
    m["fileName"] = fi.fileName();
    m["modified"] = fi.exists() ? fi.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QString();
    // 回放增益 (dB) 与峰值 (线性,1.0 = 满刻度)
    m["hasRgTrack"] = t->has_rg_track;
    m["rgTrackGain"] = t->rg_track_gain;
    m["rgTrackPeak"] = t->rg_track_peak;
    m["hasRgAlbum"] = t->has_rg_album;
    m["rgAlbumGain"] = t->rg_album_gain;
    m["rgAlbumPeak"] = t->rg_album_peak;
    // 封面来源:embedded = 内嵌;否则为同目录封面的文件名;都没有为空
    m["cover"] = QString("image://cover/t/%1/%2").arg(generation_).arg(t->id);
    m["coverSource"] = t->has_cover ? QStringLiteral("embedded")
                       : t->external_cover_path.empty() ? QString() : QFileInfo(Q(t->external_cover_path)).fileName();
    return m;
}

bool LibraryService::revealTrack(double trackId) const {
    auto s = snapshot();
    const TrackItem* t = s->catalog.FindById(static_cast<uint64_t>(trackId));
    if (!t) return false;
    const QFileInfo fi(Q(t->file_path));
    if (fi.exists()) {
#ifdef Q_OS_WIN
        // explorer 只认 /select,"路径" 这种写法;QProcess 默认会把整个参数加引号,路径带空格时会失效
        QProcess p;
        p.setProgram(QStringLiteral("explorer.exe"));
        p.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(fi.absoluteFilePath())));
        return p.startDetached();
#endif
    }
    const QDir dir = fi.absoluteDir();
    if (!dir.exists()) return false;
    return QDesktopServices::openUrl(QUrl::fromLocalFile(dir.absolutePath()));
}

void LibraryService::copyToClipboard(const QString& text) const {
    if (auto* cb = QGuiApplication::clipboard()) cb->setText(text);
}

int LibraryService::artistOfTrack(double trackId) const {
    auto s = snapshot();
    const TrackItem* t = s->catalog.FindById(static_cast<uint64_t>(trackId));
    return t ? artistIndexOf(Q(t->artist.empty() ? t->album_artist : t->artist)) : -1;
}
