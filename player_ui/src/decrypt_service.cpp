#include "decrypt_service.h"

#include <QDesktopServices>
#include <QDir>
#include <QDateTime>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSaveFile>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <map>

#include "audio_ingestion_pipeline.hpp"
#include "audio_tag_parser.hpp"
#include "decryptor.hpp"
#include "library_service.h"
#include "platform_io.hpp"

namespace {
QString Q(const std::wstring& w) { return QString::fromStdWString(w); }
QString FileName(const std::wstring& p) { return QFileInfo(Q(p)).fileName(); }
} // namespace

// ============================================================================
// 待解密队列
// ============================================================================
namespace {
// 「歌手 - 歌名」式文件名拆成两部分;拆不出时整段作标题
void SplitName(const QString& path, QString& title, QString& artist) {
    QString stem = QFileInfo(path).completeBaseName();
    int i = stem.indexOf(QStringLiteral(" - "));
    if (i > 0) {
        artist = stem.left(i).trimmed();
        title = stem.mid(i + 3).trimmed();
    } else {
        title = stem;
        artist.clear();
    }
}
} // namespace

int DecryptQueueModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : int(rows_.size()); }

QVariant DecryptQueueModel::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= rowCount()) return {};
    const Entry& e = all_[rows_[index.row()]];
    switch (role) {
        case PathRole: return e.path;
        case NameRole: return QFileInfo(e.path).fileName();
        case TitleRole: return e.title;
        case ArtistRole: return e.artist;
        case KindRole: return e.kind;
        case SizeRole: return e.size;
        case SourceRole: return e.fromLibrary ? QStringLiteral("library") : QStringLiteral("manual");
        case StatusRole: return e.status;
        case MessageRole: return e.message;
        case OutputRole: return e.output;
    }
    return {};
}

QHash<int, QByteArray> DecryptQueueModel::roleNames() const {
    return {{PathRole, "path"},     {NameRole, "name"},     {TitleRole, "title"},   {ArtistRole, "artist"},
            {KindRole, "kind"},     {SizeRole, "size"},     {SourceRole, "source"}, {StatusRole, "status"},
            {MessageRole, "message"}, {OutputRole, "output"}};
}

void DecryptQueueModel::setFilterKind(const QString& k) {
    if (k == kind_) return;
    kind_ = k;
    refilter();
    emit filterChanged();
}

void DecryptQueueModel::setFilterText(const QString& t) {
    if (t == text_) return;
    text_ = t;
    refilter();
    emit filterChanged();
}

bool DecryptQueueModel::accept(const Entry& e) const {
    if (kind_ == QLatin1String("failed")) {
        if (e.status != Failed) return false;
    } else if (kind_ == QLatin1String("done")) {
        if (e.status != Done) return false;
    } else if (!kind_.isEmpty() && e.kind != kind_) {
        return false;
    }
    const QString q = text_.trimmed();
    if (q.isEmpty()) return true;
    return e.title.contains(q, Qt::CaseInsensitive) || e.artist.contains(q, Qt::CaseInsensitive) ||
           e.path.contains(q, Qt::CaseInsensitive);
}

void DecryptQueueModel::refilter() {
    beginResetModel();
    rows_.clear();
    for (int i = 0; i < int(all_.size()); ++i)
        if (accept(all_[i])) rows_.push_back(i);
    endResetModel();
    emit countChanged();
    emit statsChanged();
}

void DecryptQueueModel::syncLibrary(const std::vector<Entry>& lib) {
    QSet<QString> keep;
    for (const Entry& e : lib) keep.insert(e.path);
    std::vector<Entry> next;
    next.reserve(all_.size() + lib.size());
    // 保留:手动导入的、仍在曲库待解密列表里的、已完成 / 失败 (留给用户查看)
    for (const Entry& e : all_)
        if (!e.fromLibrary || keep.contains(e.path) || e.status == Done || e.status == Failed) next.push_back(e);
    QSet<QString> have;
    for (const Entry& e : next) have.insert(e.path);
    for (const Entry& e : lib)
        if (!have.contains(e.path)) next.push_back(e);
    all_ = std::move(next);
    byPath_.clear();
    for (int i = 0; i < int(all_.size()); ++i) byPath_.insert(all_[i].path, i);
    refilter();
}

bool DecryptQueueModel::add(Entry e) {
    auto it = byPath_.find(e.path);
    if (it != byPath_.end()) {
        Entry& old = all_[it.value()];
        if (old.status == Done || old.status == Failed) old.status = Waiting; // 重新导入:回到待命
        refilter();
        return false;
    }
    byPath_.insert(e.path, int(all_.size()));
    all_.push_back(std::move(e));
    refilter();
    return true;
}

void DecryptQueueModel::remove(const QString& path) {
    auto it = byPath_.find(path);
    if (it == byPath_.end() || all_[it.value()].status == Working) return;
    all_.erase(all_.begin() + it.value());
    byPath_.clear();
    for (int i = 0; i < int(all_.size()); ++i) byPath_.insert(all_[i].path, i);
    refilter();
}

void DecryptQueueModel::setStatus(const QString& path, int status, const QString& message, const QString& output) {
    auto it = byPath_.find(path);
    if (it == byPath_.end()) return;
    Entry& e = all_[it.value()];
    e.status = status;
    e.message = message;
    e.output = output;
    // 只刷新这一行;筛选条件与状态相关时重新筛选
    if (kind_ == QLatin1String("failed") || kind_ == QLatin1String("done")) {
        refilter();
        return;
    }
    auto r = std::find(rows_.begin(), rows_.end(), it.value());
    if (r != rows_.end()) {
        QModelIndex idx = index(int(r - rows_.begin()));
        emit dataChanged(idx, idx, {StatusRole, MessageRole, OutputRole});
    }
    emit statsChanged();
}

void DecryptQueueModel::clearFinished() {
    std::vector<Entry> next;
    for (const Entry& e : all_)
        if (e.status != Done) next.push_back(e);
    all_ = std::move(next);
    byPath_.clear();
    for (int i = 0; i < int(all_.size()); ++i) byPath_.insert(all_[i].path, i);
    refilter();
}

QVariantMap DecryptQueueModel::counts() const {
    int kgm = 0, ncm = 0, kgg = 0, waiting = 0, done = 0, failed = 0;
    for (const Entry& e : all_) {
        if (e.kind == QLatin1String("kgm")) ++kgm;
        else if (e.kind == QLatin1String("ncm")) ++ncm;
        else if (e.kind == QLatin1String("kgg")) ++kgg;
        if (e.status == Waiting) ++waiting;
        else if (e.status == Done) ++done;
        else if (e.status == Failed) ++failed;
    }
    return {{"all", int(all_.size())}, {"kgm", kgm}, {"ncm", ncm},     {"kgg", kgg},
            {"waiting", waiting},      {"done", done}, {"failed", failed}};
}

QStringList DecryptQueueModel::visibleWaiting() const {
    QStringList out;
    for (int r : rows_)
        if (all_[r].status == Waiting || all_[r].status == Failed) out << all_[r].path;
    return out;
}

// ============================================================================
DecryptService::DecryptService(LibraryService* lib, QObject* parent) : QObject(parent), lib_(lib) {
    connect(&queue_, &DecryptQueueModel::statsChanged, this, &DecryptService::statsChanged);
    QSettings s;
    policy_ = std::clamp(s.value("decrypt/policy", 0).toInt(), 0, 2);
    outputDir_ = LibraryService::decryptOutputDir();
    loadSeen();
    connect(lib_, &LibraryService::pendingEncryptedChanged, this, &DecryptService::onPendingChanged);
    connect(lib_, &LibraryService::foldersChanged, this, &DecryptService::statsChanged); // 「加入曲库」数量随之变化
}

DecryptService::~DecryptService() {
    if (cancel_) cancel_->store(true);
    if (worker_.joinable()) worker_.join();
}

void DecryptService::setPolicy(int p) {
    p = std::clamp(p, 0, 2);
    if (p == policy_) return;
    policy_ = p;
    QSettings().setValue("decrypt/policy", p);
    emit policyChanged();
}

void DecryptService::setOutputDir(const QString& d) {
    QString n = QDir::toNativeSeparators(QDir::cleanPath(d));
    if (n.isEmpty() || n == outputDir_) return;
    outputDir_ = n;
    QSettings().setValue("decrypt/outputDir", n);
    emit outputDirChanged();
}

void DecryptService::setOutputUrl(const QVariant& url) {
    QUrl u = url.toUrl();
    setOutputDir(u.isLocalFile() ? u.toLocalFile() : url.toString());
}

QVariantList DecryptService::decoders() const {
    QVariantList out;
    for (const auto& d : DecryptorRegistry::Instance().All()) {
        std::wstring reason;
        bool ok = d->IsAvailable(&reason);
        QStringList exts;
        for (const auto& e : d->Extensions()) exts << Q(e).mid(1).toUpper();
        QVariantMap m;
        m["id"] = QString::fromLatin1(d->Id());
        m["name"] = Q(d->DisplayName());
        m["exts"] = exts;
        m["native"] = d->Native();
        m["available"] = ok;
        m["reason"] = Q(reason);
        out.append(m);
    }
    return out;
}

QVariantMap DecryptService::pending() const {
    const auto& list = lib_->pendingEncrypted();
    std::map<std::string, int> counts;
    qint64 bytes = 0;
    for (const auto& e : list) {
        counts[e.kind]++;
        bytes += static_cast<qint64>(e.size);
    }
    QVariantList kinds;
    for (const auto& d : DecryptorRegistry::Instance().All()) {
        auto it = counts.find(d->Id());
        if (it == counts.end()) continue;
        QVariantMap k;
        k["id"] = QString::fromLatin1(d->Id());
        k["name"] = Q(d->DisplayName());
        k["count"] = it->second;
        k["available"] = d->IsAvailable();
        kinds.append(k);
    }
    QVariantMap m;
    m["total"] = static_cast<int>(list.size());
    m["bytes"] = bytes;
    m["kinds"] = kinds;
    return m;
}

namespace {
bool SameContent(const QString& a, const QString& b) {
    QFile fa(a), fb(b);
    if (!fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly) || fa.size() != fb.size()) return false;
    while (!fa.atEnd()) {
        if (fa.read(1 << 20) != fb.read(1 << 20)) return false;
    }
    return true;
}

// 输出目录里已经有同一首歌 (例如解密记录丢了又转了一次) 时会生成「歌名 (2).flac」:
// 内容与「歌名.flac」完全一样就删掉新的,复用原来那份
QString CollapseCopy(const QString& out) {
    QFileInfo fi(out);
    static const QRegularExpression re(QStringLiteral("^(.*) \\((\\d+)\\)$"));
    QRegularExpressionMatch m = re.match(fi.completeBaseName());
    if (!m.hasMatch()) return out;
    const QString base = fi.absolutePath() + "/" + m.captured(1) + "." + fi.suffix();
    if (!QFileInfo(base).isFile() || !SameContent(out, base)) return out;
    QFile::remove(out);
    return QDir::toNativeSeparators(base);
}

// 加密源旁边的同名歌词 (网易云会把 .lrc 和 .ncm 一起下载) 跟着复制到产物旁边,并按产物文件名改名;
// 与整理 / 查重同一规则:「歌名.lrc」「歌名.zh.lrc」都算 (主文件名后紧跟一个点)。产物旁已有同名歌词时不覆盖
void CarryLyrics(const QString& source, const QString& output) {
    const QFileInfo s(source), o(output);
    if (!o.isFile() || (s.absolutePath() == o.absolutePath() && s.completeBaseName() == o.completeBaseName())) return;
    const QString stem = s.completeBaseName().toLower() + ".";
    static const QStringList exts = {QStringLiteral(".lrc"), QStringLiteral(".krc"), QStringLiteral(".qrc"), QStringLiteral(".trc")};
    QDirIterator it(s.absolutePath(), QDir::Files);
    while (it.hasNext()) {
        const QString f = it.next();
        const QString name = it.fileName();
        const QString low = name.toLower();
        if (!low.startsWith(stem)) continue;
        bool lyric = false;
        for (const QString& e : exts) if (low.endsWith(e)) lyric = true;
        if (!lyric) continue;
        const QString target = o.absolutePath() + "/" + o.completeBaseName() + name.mid(stem.size() - 1);
        if (!QFileInfo::exists(target)) QFile::copy(f, target);
    }
}

QString SeenFile() { return LibraryService::dataDir() + "/decrypt.seen"; }
} // namespace

void DecryptService::loadSeen() {
    QFile f(SeenFile());
    if (!f.open(QIODevice::ReadOnly)) return;
    for (const QByteArray& line : f.readAll().split('\n'))
        if (!line.isEmpty()) seen_.insert(line.toUInt(nullptr, 16));
}

void DecryptService::saveSeen() const {
    QSaveFile f(SeenFile());
    if (!f.open(QIODevice::WriteOnly)) return;
    QByteArray out;
    for (uint h : seen_) out += QByteArray::number(h, 16) + '\n';
    f.write(out);
    f.commit();
}

void DecryptService::markSeen() {
    for (const auto& e : lib_->pendingEncrypted()) seen_.insert(qHash(Q(e.path)));
    saveSeen();
}

bool DecryptService::hasUnseen() const {
    for (const auto& e : lib_->pendingEncrypted())
        if (!seen_.contains(qHash(Q(e.path)))) return true;
    return false;
}

void DecryptService::onPendingChanged() {
    std::vector<DecryptQueueModel::Entry> lib;
    for (const auto& e : lib_->pendingEncrypted()) {
        DecryptQueueModel::Entry q;
        q.path = Q(e.path);
        q.kind = QString::fromStdString(e.kind);
        q.size = qint64(e.size);
        q.mtime = e.mtime;
        q.fromLibrary = true;
        SplitName(q.path, q.title, q.artist);
        lib.push_back(std::move(q));
    }
    queue_.syncLibrary(lib);
    emit pendingChanged();
    if (lib_->pendingEncrypted().empty() || busy_ || !hasUnseen()) return;
    // 自动模式也只处理新出现的文件:解密失败的不会反复重试
    if (policy_ == 0) emit promptRequested();
    else if (policy_ == 1) {
        // 刚出现、还在变化的文件 (酷狗正在下载) 先不转:5 秒后再看,大小和修改时间稳定了才转换
        const QDateTime now = QDateTime::currentDateTime();
        for (const auto& e : lib_->pendingEncrypted()) {
            QFileInfo fi(Q(e.path));
            if (fi.exists() && (fi.size() != qint64(e.size) || fi.lastModified().secsTo(now) < 5)) {
                if (!recheck_) {
                    recheck_ = true;
                    QTimer::singleShot(5000, this, [this]() {
                        recheck_ = false;
                        lib_->rescan(); // 重扫拿到文件的最终大小,再按策略处理
                    });
                }
                return;
            }
        }
        decryptPending();
    }
}

void DecryptService::dismissPending() { markSeen(); }

void DecryptService::decryptPending() {
    markSeen();
    decryptQueue(false);
}

void DecryptService::decryptQueue(bool onlyVisible) {
    QStringList paths;
    if (onlyVisible) paths = queue_.visibleWaiting();
    else
        for (const auto& e : queue_.entries())
            if (e.status == DecryptQueueModel::Waiting || e.status == DecryptQueueModel::Failed) paths << e.path;
    decryptPaths(paths);
}

void DecryptService::decryptPaths(const QStringList& paths) {
    if (busy_ || paths.isEmpty()) return;
    QSet<QString> want(paths.begin(), paths.end());
    std::vector<Item> items;
    bool library = false;
    for (const auto& e : queue_.entries()) {
        if (!want.contains(e.path) || e.status == DecryptQueueModel::Working || e.status == DecryptQueueModel::Done) continue;
        items.push_back({e.path.toStdWString(), e.kind.toStdString(), uint64_t(e.size), e.mtime});
        library = library || e.fromLibrary;
    }
    start(std::move(items), library);
}

void DecryptService::removeFromQueue(const QString& path) {
    queue_.remove(path);
    seen_.insert(qHash(path)); // 从队列移除 = 这一个不再询问
    saveSeen();
}

void DecryptService::clearFinished() { queue_.clearFinished(); }

void DecryptService::openOutput(const QString& path) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
}

std::vector<DecryptService::Item> DecryptService::collect(const QVariantList& urls) const {
    std::vector<Item> items;
    auto addFile = [&](const QString& path) {
        std::wstring w = QDir::toNativeSeparators(path).toStdWString();
        const Decryptor* d = DecryptorRegistry::Instance().Detect(w);
        if (!d) return;
        Item it;
        it.path = w;
        it.kind = d->Id();
        platform::GetFileInfo(w, it.size, it.mtime);
        items.push_back(std::move(it));
    };
    for (const QVariant& v : urls) {
        QUrl u = v.toUrl();
        QString local = u.isLocalFile() ? u.toLocalFile() : v.toString();
        QFileInfo fi(local);
        if (fi.isDir()) {
            QDirIterator it(fi.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                QString f = it.next();
                if (IsEncryptedAudioExtension(("." + QFileInfo(f).suffix().toLower()).toStdWString())) addFile(f);
            }
        } else if (fi.isFile()) {
            addFile(fi.absoluteFilePath());
        }
    }
    return items;
}

int DecryptService::addUrls(const QVariantList& urls) {
    int added = 0;
    for (const Item& it : collect(urls)) {
        DecryptQueueModel::Entry e;
        e.path = Q(it.path);
        e.kind = QString::fromStdString(it.kind);
        e.size = qint64(it.size);
        e.mtime = it.mtime;
        SplitName(e.path, e.title, e.artist);
        // NCM 头部带元数据,能拿到准确的歌名与歌手
        if (it.kind == "ncm") {
            AudioMetadata m;
            if (AudioTagParser::ParseFile(it.path, m) && !m.title.empty()) {
                e.title = Q(m.title);
                e.artist = Q(m.artist);
            }
        }
        added += queue_.add(std::move(e));
    }
    return added;
}

// 其他页面拖入的加密文件:加入队列并立即解密
int DecryptService::decryptUrls(const QVariantList& urls) {
    std::vector<Item> items = collect(urls);
    addUrls(urls);
    QStringList paths;
    for (const Item& it : items) {
        paths << Q(it.path);
        toLibrary_.insert(Q(it.path)); // 拖进播放器 = 想听:转换完直接进曲库
    }
    decryptPaths(paths);
    return int(items.size());
}

void DecryptService::cancel() {
    if (cancel_) cancel_->store(true);
}

void DecryptService::clearResults() {
    if (busy_) return;
    results_.clear();
    ok_ = fail_ = 0;
    total_ = 0;
    emit jobChanged();
}

QStringList DecryptService::addableOutputs() const {
    QStringList out;
    const QStringList& have = lib_->extraFiles();
    for (const auto& e : queue_.entries()) {
        if (e.status != DecryptQueueModel::Done || e.fromLibrary || e.output.isEmpty()) continue;
        const QString o = QDir::toNativeSeparators(e.output);
        if (!have.contains(o, Qt::CaseInsensitive) && !out.contains(o, Qt::CaseInsensitive) && QFileInfo::exists(o)) out << o;
    }
    return out;
}

int DecryptService::addResultsToLibrary() { return lib_->addFiles(addableOutputs()); }

void DecryptService::openOutputFolder() {
    QDir().mkpath(outputDir_);
    QDesktopServices::openUrl(QUrl::fromLocalFile(outputDir_));
}

void DecryptService::start(std::vector<Item> items, bool rescanAfter) {
    if (busy_ || items.empty()) return;
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    total_ = static_cast<int>(items.size());
    done_ = 0;
    progress_ = 0;
    current_.clear();
    results_.clear();
    ok_ = fail_ = 0;
    emit jobChanged();
    emit progressChanged();

    cancel_ = std::make_shared<std::atomic<bool>>(false);
    auto cancel = cancel_;
    const std::wstring outDir = outputDir_.toStdWString();
    const std::wstring indexPath = (LibraryService::dataDir() + "/decrypt.index").toStdWString();
    QStringList folders = lib_->folders();
    const bool writeTags = QSettings().value("decrypt/writeTags", true).toBool();
    QPointer<DecryptService> self(this);

    worker_ = std::thread([self, items = std::move(items), cancel, outDir, indexPath, rescanAfter, folders, writeTags]() {
        DecryptIndex index;
        index.Load(indexPath);
        QElapsedTimer clock;
        clock.start();
        qint64 lastPost = -100;
        bool touchesLibrary = rescanAfter;
        const double n = static_cast<double>(items.size());
        for (size_t i = 0; i < items.size() && !cancel->load(); ++i) {
            const Item& it = items[i];
            const QString name = FileName(it.path);
            const QString qpath = Q(it.path);
            QMetaObject::invokeMethod(self, [self, name, i, qpath]() {
                if (!self) return;
                self->queue_.setStatus(qpath, DecryptQueueModel::Working);
                self->current_ = name;
                self->done_ = static_cast<int>(i);
                emit self->progressChanged();
            }, Qt::QueuedConnection);

            DecryptOptions o;
            o.output_dir = outDir;
            o.write_tags = writeTags;
            o.cancel = cancel.get();
            o.progress = [&](uint64_t done, uint64_t total) {
                qint64 now = clock.elapsed();
                if (now - lastPost < 50) return;
                lastPost = now;
                double v = (static_cast<double>(i) + (total ? double(done) / double(total) : 0.0)) / n;
                QMetaObject::invokeMethod(self, [self, v]() {
                    if (!self) return;
                    self->progress_ = v;
                    emit self->progressChanged();
                }, Qt::QueuedConnection);
            };
            const Decryptor* d = DecryptorRegistry::Instance().ById(it.kind);
            DecryptResult r;
            // 转换过的 (按路径,或按内容指纹认出被移动 / 改名的源文件) 直接复用原来的结果
            const std::wstring existing = index.Lookup(it.path, it.size, it.mtime);
            if (!existing.empty()) {
                r.ok = true;
                r.output = existing;
                r.message = L"已转换过,直接复用";
            } else if (d) {
                r = d->Decrypt(it.path, o);
                if (r.ok) r.output = CollapseCopy(Q(r.output)).toStdWString();
            } else {
                r.message = L"未知格式";
            }
            if (r.ok) {
                CarryLyrics(Q(it.path), Q(r.output));
                index.Put(it.path, it.size, it.mtime, r.output, DecryptIndex::Fingerprint(it.path, it.size));
                // 源文件在曲库里:解密后需要重扫才能入库
                QString src = QDir::fromNativeSeparators(Q(it.path));
                for (const QString& f : folders)
                    if (src.startsWith(QDir::fromNativeSeparators(f) + "/", Qt::CaseInsensitive)) touchesLibrary = true;
            }
            QVariantMap m;
            m["name"] = name;
            m["kind"] = QString::fromStdString(it.kind);
            m["ok"] = r.ok;
            m["message"] = Q(r.message);
            m["output"] = Q(r.output);
            QMetaObject::invokeMethod(self, [self, m, i, n, qpath]() {
                if (!self) return;
                self->queue_.setStatus(qpath, m.value("ok").toBool() ? DecryptQueueModel::Done : DecryptQueueModel::Failed,
                                       m.value("message").toString(), m.value("output").toString());
                self->results_.append(m);
                if (m.value("ok").toBool() && self->toLibrary_.remove(qpath)) self->autoAdd_ << m.value("output").toString();
                (m.value("ok").toBool() ? self->ok_ : self->fail_)++;
                self->done_ = static_cast<int>(i) + 1;
                self->progress_ = (i + 1) / n;
                emit self->jobChanged();
                emit self->progressChanged();
            }, Qt::QueuedConnection);
        }
        index.Save(indexPath);
        QMetaObject::invokeMethod(self, [self, touchesLibrary]() {
            if (!self) return;
            self->busy_ = false;
            self->current_.clear();
            for (const auto& e : self->queue_.entries())
                if (e.status == DecryptQueueModel::Working) self->queue_.setStatus(e.path, DecryptQueueModel::Waiting);
            emit self->jobChanged();
            emit self->progressChanged();
            emit self->finished(self->ok_, self->fail_);
            if (!self->autoAdd_.isEmpty()) self->lib_->addFiles(self->autoAdd_); // 加入后会自己重扫
            self->autoAdd_.clear();
            if (touchesLibrary && self->ok_ > 0) self->lib_->rescan();
            emit self->statsChanged();
        }, Qt::QueuedConnection);
    });
}
