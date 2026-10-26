#include "library_tools.h"

#include <QDesktopServices>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>

#include "library_service.h"
#include "player_controller.h"
#include "playlist_service.h"

namespace {
QString Q(const std::wstring& w) { return QString::fromStdWString(w); }
std::wstring W(const QString& s) { return QDir::toNativeSeparators(s).toStdWString(); }

const char* const kDefaultPattern =
    "$if2(%album artist%,%artist%)\\[%date% - ]%album%\\[%discnumber%-]$num(%tracknumber%,2) %title%";

bool UnderFolder(const QString& path, const QString& folder) {
    const QString p = PathKeyOf(path), f = PathKeyOf(folder);
    return p == f || p.startsWith(f.endsWith('/') ? f : f + '/');
}

// 相对某个根目录的显示路径 (不在其下时返回完整路径)
QString Relative(const QString& path, const QString& root) {
    if (root.isEmpty() || !UnderFolder(path, root)) return QDir::toNativeSeparators(path);
    QString r = QDir::fromNativeSeparators(path).mid(QDir::cleanPath(QDir::fromNativeSeparators(root)).size());
    while (r.startsWith('/')) r.remove(0, 1);
    return QDir::toNativeSeparators(r);
}

QString FormatMs(uint32_t ms) {
    const uint32_t s = ms / 1000;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString LocalFromUrl(const QVariant& v) {
    const QUrl u = v.toUrl();
    QString p = u.isLocalFile() ? u.toLocalFile() : v.toString();
    return p.isEmpty() ? QString() : QDir::toNativeSeparators(QDir::cleanPath(p));
}

QVariantList Failures(const std::vector<std::pair<std::wstring, std::wstring>>& f) {
    QVariantList out;
    for (size_t i = 0; i < f.size() && i < 200; ++i)
        out.append(QVariantMap{{"path", Q(f[i].first)}, {"reason", Q(f[i].second)}});
    return out;
}
} // namespace

// ============================================================================
RowModel::RowModel(const QStringList& roles, QObject* parent) : QAbstractListModel(parent), roles_(roles) {}

int RowModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QVariant RowModel::data(const QModelIndex& index, int role) const {
    const int r = index.row();
    if (r < 0 || r >= count()) return {};
    const int src = visible_[r];
    if (role == Qt::UserRole) return src;
    const int k = role - Qt::UserRole - 1;
    if (k < 0 || k >= roles_.size()) return {};
    return rows_[src].value(roles_[k]);
}

QHash<int, QByteArray> RowModel::roleNames() const {
    QHash<int, QByteArray> h;
    h.insert(Qt::UserRole, "src");
    for (int i = 0; i < roles_.size(); ++i) h.insert(Qt::UserRole + 1 + i, roles_[i].toUtf8());
    return h;
}

void RowModel::setFilter(const QString& f) {
    if (f == filter_) return;
    filter_ = f;
    beginResetModel();
    refilter();
    endResetModel();
    emit filterChanged();
    emit countChanged();
}

void RowModel::refilter() {
    visible_.clear();
    const QStringList cats = filter_.split(',', Qt::SkipEmptyParts);
    for (int i = 0; i < rows_.size(); ++i)
        if (cats.isEmpty() || cats.contains(rows_[i].value("cat").toString())) visible_.push_back(i);
}

void RowModel::setRows(QVector<QVariantMap> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    refilter();
    endResetModel();
    emit countChanged();
}

void RowModel::updateRow(int source, const QVariantMap& row) {
    if (source < 0 || source >= rows_.size()) return;
    rows_[source] = row;
    auto it = std::find(visible_.begin(), visible_.end(), source);
    if (it == visible_.end()) return;
    const QModelIndex i = index(static_cast<int>(it - visible_.begin()));
    emit dataChanged(i, i);
}

QVariantMap RowModel::get(int row) const { return row >= 0 && row < count() ? rows_[visible_[row]] : QVariantMap(); }

// ============================================================================
LibraryTools::LibraryTools(LibraryService* lib, PlaylistService* playlists, PlayerController* player, QObject* parent)
    : QObject(parent),
      lib_(lib),
      playlists_(playlists),
      player_(player),
      planItems_({"from", "to", "fromRel", "toRel", "name", "toName", "kind", "cat", "note", "size", "cross", "renamed"}),
      dupItems_({"cat", "kindLabel", "review", "reclaim", "count", "keepCount", "title", "members"}) {
    QSettings s;
    pattern_ = s.value("organize/pattern", QString::fromLatin1(kDefaultPattern)).toString();
    target_ = s.value("organize/target").toString();
    mode_ = s.value("organize/mode", 0).toInt();
    fullWidth_ = s.value("organize/fullWidth", false).toBool();
    companions_ = s.value("organize/companions", true).toBool();
    folderArt_ = s.value("organize/folderArt", true).toBool();
    removeEmpty_ = s.value("organize/removeEmpty", true).toBool();
    skipped_ = s.value("organize/skipped").toStringList();
    dupSameSong_ = s.value("dedupe/sameSong", true).toBool();
    dupSameName_ = s.value("dedupe/sameName", true).toBool();
    dupTolerance_ = s.value("dedupe/tolerance", 3).toInt();
    quarantine_ = s.value("dedupe/quarantine").toString();
    if (quarantine_.isEmpty())
        quarantine_ = QDir::toNativeSeparators(QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/重复歌曲");
    // 默认目标:第一个不是酷狗下载目录的曲库文件夹
    if (target_.isEmpty())
        for (const QString& f : lib_->folders())
            if (!lib_->isKugouFolder(f)) {
                target_ = f;
                break;
            }
    connect(lib_, &LibraryService::libraryChanged, this, &LibraryTools::updatePreview);
    connect(lib_, &LibraryService::foldersChanged, this, &LibraryTools::optionsChanged);
    updatePreview();
}

LibraryTools::~LibraryTools() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

template <class Work, class Done>
void LibraryTools::run(const QString& task, Work work, Done done) {
    if (busy()) return;
    if (worker_.joinable()) worker_.join();
    task_ = task;
    cancel_ = false;
    progress_ = 0;
    progressText_.clear();
    emit stateChanged();
    emit progressChanged();
    worker_ = std::thread([this, work, done]() mutable {
        auto result = work();
        QMetaObject::invokeMethod(this, [this, result, done]() mutable {
            done(result);
            finishTask();
        }, Qt::QueuedConnection);
    });
}

void LibraryTools::setProgress(double v, const QString& text) {
    progress_ = v;
    progressText_ = text;
    emit progressChanged();
}

void LibraryTools::finishTask() {
    task_.clear();
    emit stateChanged();
    // 查重 / 整理是一次性的重活,跑完把堆里空出来的大块还给系统,别一直挂在进程名下
    LibraryService::ReleaseFreeHeap();
}

void LibraryTools::cancel() { cancel_ = true; }

QString LibraryTools::journalPath(const char* name) const { return LibraryService::dataDir() + "/" + name; }

void LibraryTools::saveOptions() {
    QSettings s;
    s.setValue("organize/pattern", pattern_);
    s.setValue("organize/target", target_);
    s.setValue("organize/mode", mode_);
    s.setValue("organize/fullWidth", fullWidth_);
    s.setValue("organize/companions", companions_);
    s.setValue("organize/folderArt", folderArt_);
    s.setValue("organize/removeEmpty", removeEmpty_);
    s.setValue("organize/skipped", skipped_);
    s.setValue("dedupe/sameSong", dupSameSong_);
    s.setValue("dedupe/sameName", dupSameName_);
    s.setValue("dedupe/tolerance", dupTolerance_);
    s.setValue("dedupe/quarantine", quarantine_);
}

// 选项一变,旧计划就作废
#define TOOLS_SETTER(name, member, type)        \
    void LibraryTools::name(type v) {           \
        if (member == v) return;                \
        member = v;                             \
        saveOptions();                          \
        clearPlan();                            \
        updatePreview();                        \
    }
TOOLS_SETTER(setPattern, pattern_, const QString&)
TOOLS_SETTER(setMode, mode_, int)
TOOLS_SETTER(setFullWidth, fullWidth_, bool)
TOOLS_SETTER(setCompanions, companions_, bool)
TOOLS_SETTER(setFolderArt, folderArt_, bool)
TOOLS_SETTER(setRemoveEmpty, removeEmpty_, bool)
#undef TOOLS_SETTER

void LibraryTools::setDupSameSong(bool on) {
    if (on == dupSameSong_) return;
    dupSameSong_ = on;
    saveOptions();
    emit optionsChanged();
}
void LibraryTools::setDupSameName(bool on) {
    if (on == dupSameName_) return;
    dupSameName_ = on;
    saveOptions();
    emit optionsChanged();
}
void LibraryTools::setDupTolerance(int s) {
    s = std::clamp(s, 1, 30);
    if (s == dupTolerance_) return;
    dupTolerance_ = s;
    saveOptions();
    emit optionsChanged();
}

void LibraryTools::setTargetUrl(const QVariant& url) {
    const QString p = LocalFromUrl(url);
    if (p.isEmpty() || p == target_) return;
    target_ = p;
    saveOptions();
    clearPlan();
    updatePreview();
}

void LibraryTools::setQuarantineUrl(const QVariant& url) {
    const QString p = LocalFromUrl(url);
    if (p.isEmpty() || p == quarantine_) return;
    quarantine_ = p;
    saveOptions();
    emit optionsChanged();
}

QStringList LibraryTools::sources() const {
    QStringList out;
    for (const QString& f : lib_->folders())
        if (isSource(f)) out << f;
    return out;
}

bool LibraryTools::isSource(const QString& folder) const {
    // 默认不动酷狗的下载目录 (酷狗自己还要用);用户可以手动勾上
    if (skipped_.contains(folder, Qt::CaseInsensitive)) return false;
    if (lib_->isKugouFolder(folder)) return skipped_.contains("+" + folder, Qt::CaseInsensitive);
    return true;
}

void LibraryTools::toggleSource(const QString& folder) {
    const bool kugou = lib_->isKugouFolder(folder);
    if (isSource(folder)) {
        skipped_.removeAll("+" + folder);
        if (!kugou) skipped_ << folder;
    } else {
        skipped_.removeAll(folder);
        if (kugou) skipped_ << "+" + folder;
    }
    saveOptions();
    clearPlan();
    emit optionsChanged();
}

QVariantList LibraryTools::presets() const {
    return {
        QVariantMap{{"name", tr("歌手 / 年份 - 专辑 / 曲号 歌名")}, {"pattern", QString::fromLatin1(kDefaultPattern)}},
        QVariantMap{{"name", tr("歌手 / 专辑 / 曲号 歌名")}, {"pattern", "%artist%\\%album%\\[$num(%tracknumber%,2) ]%title%"}},
        QVariantMap{{"name", tr("歌手 / 歌手 - 歌名")}, {"pattern", "%artist%\\%artist% - %title%"}},
        QVariantMap{{"name", tr("格式 / 歌手 / 专辑 / 歌名")}, {"pattern", "%codec%\\%artist%\\%album%\\[$num(%tracknumber%,2) ]%title%"}},
        QVariantMap{{"name", tr("只改文件名:歌手 - 歌名")}, {"pattern", "%artist% - %title%"}},
    };
}

organizer::OrganizeOptions LibraryTools::organizeOptions() const {
    organizer::OrganizeOptions o;
    o.mode = mode_ == 1 ? organizer::Mode::Copy : mode_ == 2 ? organizer::Mode::RenameInPlace : organizer::Mode::Move;
    o.target_root = W(target_);
    o.pattern = pattern_.toStdWString();
    o.illegal = fullWidth_ ? organizer::IllegalCharPolicy::FullWidth : organizer::IllegalCharPolicy::Underscore;
    o.unknown_value = tr("未知").toStdWString();
    o.move_companions = companions_;
    o.move_folder_art = folderArt_;
    o.remove_empty_dirs = removeEmpty_;
    for (const QString& f : lib_->folders()) o.protect_roots.push_back(W(f));
    // 正在播放的歌 (文件被占用) 与转换产物 (解密记录按路径认领) 不动
    const QString cur = player_->currentPath();
    if (!cur.isEmpty()) o.exclude_paths.push_back(W(cur));
    o.exclude_paths.push_back(W(LibraryService::decryptOutputDir()));
    return o;
}

// 曲库里参与整理的物理文件 (CUE 整轨只算一个,用专辑级标签)
std::vector<organizer::OrganizeTrack> LibraryTools::collectTracks(std::vector<std::wstring>*) const {
    auto snap = lib_->snapshot();
    const QStringList src = sources();
    std::unordered_map<const TrackItem*, int> discTotal;
    for (const AlbumInfo& a : snap->albums) {
        int maxDisc = 0;
        for (const TrackItem* t : a.tracks) maxDisc = std::max(maxDisc, static_cast<int>(t->disc_no));
        for (const TrackItem* t : a.tracks) discTotal[t] = maxDisc;
    }
    std::vector<organizer::OrganizeTrack> out;
    std::unordered_set<std::wstring> seen;
    for (const TrackItem& t : snap->catalog.GetAllTracks()) {
        if (!t.source_path.empty()) continue; // 转换产物
        const QString path = Q(t.file_path);
        bool inSource = false;
        for (const QString& f : src) inSource = inSource || UnderFolder(path, f);
        if (!inSource || !seen.insert(PathKeyOf(path).toStdWString()).second) continue;
        organizer::OrganizeTrack o;
        o.path = t.file_path;
        o.artist = t.artist == L"未知艺术家" ? std::wstring() : t.artist;
        o.album_artist = t.album_artist;
        o.album = t.album == L"未知专辑" ? std::wstring() : t.album;
        o.genre = t.genre;
        o.date = t.year;
        o.codec = t.codec;
        o.format = t.format;
        o.disc_no = t.disc_no;
        auto dt = discTotal.find(&t);
        o.disc_total = dt == discTotal.end() ? 0 : dt->second;
        if (t.is_cue_track) {
            o.cue_image = true; // 整轨:文件名不变,只按模板决定目录
        } else {
            o.title = t.title;
            o.track_no = t.track_no;
        }
        out.push_back(std::move(o));
    }
    return out;
}

void LibraryTools::updatePreview() {
    std::wstring err;
    patternError_.clear();
    preview_.clear();
    if (!organizer::ValidatePattern(pattern_.toStdWString(), &err)) {
        patternError_ = Q(err);
        emit optionsChanged();
        return;
    }
    // 示例:正在播放的那首,没有就用曲库里的第一首
    auto snap = lib_->snapshot();
    const TrackItem* sample = nullptr;
    const QString cur = player_->currentPath();
    for (const TrackItem& t : snap->catalog.GetAllTracks()) {
        if (!sample && !t.is_cue_track) sample = &t;
        if (!cur.isEmpty() && Q(t.file_path) == cur) {
            sample = &t;
            break;
        }
    }
    organizer::OrganizeTrack o;
    if (sample) {
        o.path = sample->file_path;
        o.title = sample->title;
        o.artist = sample->artist;
        o.album_artist = sample->album_artist;
        o.album = sample->album;
        o.date = sample->year;
        o.genre = sample->genre;
        o.codec = sample->codec;
        o.format = sample->format;
        o.track_no = sample->track_no;
        o.disc_no = sample->disc_no;
    } else {
        o.path = L"C:\\Music\\05 东风破.flac";
        o.title = L"东风破";
        o.artist = L"周杰伦";
        o.album = L"叶惠美";
        o.date = L"2003";
        o.codec = L"FLAC";
        o.track_no = 5;
    }
    organizer::OrganizeOptions opt = organizeOptions();
    std::wstring rel = organizer::FormatPattern(opt.pattern, o, opt, &err);
    const QString ext = QFileInfo(Q(o.path)).suffix().toLower();
    if (mode_ == 2) {
        preview_ = Q(rel).section(QDir::separator(), -1) + "." + ext;
    } else {
        preview_ = QDir::toNativeSeparators(target_.isEmpty() ? Q(rel) : target_ + "/" + Q(rel)) + "." + ext;
    }
    emit optionsChanged();
}

// ---------------------------------------------------------------- 整理
void LibraryTools::clearPlan() {
    planData_.reset();
    if (!plan_.isEmpty()) {
        plan_.clear();
        emit planChanged();
    }
    planItems_.setRows({});
    emit optionsChanged();
}

void LibraryTools::buildPlan() {
    if (mode_ != 2 && target_.isEmpty()) {
        emit message("", tr("先选一个目标文件夹"));
        return;
    }
    auto tracks = std::make_shared<std::vector<organizer::OrganizeTrack>>(collectTracks(nullptr));
    const organizer::OrganizeOptions opt = organizeOptions();
    setProgress(0, tr("正在计算 %1 个文件的新位置…").arg(tracks->size()));
    run("plan",
        [this, tracks, opt]() {
            QElapsedTimer clock;
            clock.start();
            qint64 last = -1000;
            auto plan = organizer::BuildPlan(*tracks, opt, [&](const organizer::OrganizeProgress& p) {
                if (clock.elapsed() - last < 80) return;
                last = clock.elapsed();
                // 算新位置很快 (占进度条前 20%),比对可能重复的文件要读盘
                const double f = p.total ? std::min(1.0, double(p.done) / double(p.total)) : 1.0;
                const double v = p.stage == 0 ? 0.2 * f : 0.2 + 0.8 * f;
                const QString text = p.stage == 0 ? tr("正在计算新位置 %1 / %2").arg(p.done).arg(p.total)
                                                  : tr("正在比对可能重复的文件 %1 / %2").arg(p.done).arg(p.total);
                QMetaObject::invokeMethod(this, [this, v, text]() { setProgress(v, text); }, Qt::QueuedConnection);
            }, &cancel_);
            return std::make_shared<organizer::OrganizePlan>(std::move(plan));
        },
        [this](std::shared_ptr<organizer::OrganizePlan> plan) {
            if (plan->cancelled) {
                setProgress(0, QString());
                return;
            }
            planData_ = plan;
            showPlan(*plan);
        });
}

void LibraryTools::showPlan(const organizer::OrganizePlan& p) {
    QVector<QVariantMap> rows;
    rows.reserve(static_cast<int>(p.ops.size()));
    const QStringList folders = lib_->folders();
    auto folderOf = [&](const QString& path) {
        for (const QString& f : folders)
            if (UnderFolder(path, f)) return f;
        return QString();
    };
    uint64_t bytes = 0;
    int companions = 0, art = 0, actionable = 0;
    for (const auto& op : p.ops) {
        const QString from = Q(op.from), to = Q(op.to);
        QString cat;
        switch (op.state) {
            case organizer::OpState::Unchanged: cat = "same"; break;
            case organizer::OpState::Duplicate: cat = "dup"; break;
            case organizer::OpState::Error: cat = "error"; break;
            case organizer::OpState::Excluded: cat = "excluded"; break;
            default: cat = op.action == organizer::OpAction::Skip ? "same" : "change"; break;
        }
        if (op.action != organizer::OpAction::Skip) {
            ++actionable;
            bytes += op.size;
            if (op.kind == organizer::OpKind::Companion) ++companions;
            if (op.kind == organizer::OpKind::FolderArt) ++art;
        }
        rows.append(QVariantMap{
            {"from", from},
            {"to", to},
            {"fromRel", Relative(from, folderOf(from))},
            {"toRel", to.isEmpty() ? QString() : Relative(to, mode_ == 2 ? QFileInfo(from).absolutePath() : target_)},
            {"name", QFileInfo(from).fileName()},
            {"toName", QFileInfo(to).fileName()},
            {"kind", op.kind == organizer::OpKind::Audio ? "audio" : op.kind == organizer::OpKind::Companion ? "companion" : "art"},
            {"cat", cat},
            {"note", Q(op.note)},
            {"size", static_cast<double>(op.size)},
            {"cross", op.cross_volume},
            {"renamed", op.state == organizer::OpState::Renamed},
        });
    }
    planItems_.setRows(std::move(rows));
    QVariantList volumes;
    for (const auto& v : p.volumes)
        volumes.append(QVariantMap{{"root", Q(v.root)}, {"need", static_cast<double>(v.bytes)},
                                   {"available", static_cast<double>(v.available)}, {"enough", v.enough}});
    QStringList warnings;
    for (const auto& w : p.warnings) warnings << Q(w);
    plan_ = QVariantMap{
        {"toMove", static_cast<int>(p.to_move)}, {"toCopy", static_cast<int>(p.to_copy)},
        {"unchanged", static_cast<int>(p.unchanged)}, {"duplicates", static_cast<int>(p.duplicates)},
        {"renamed", static_cast<int>(p.renamed)}, {"errors", static_cast<int>(p.errors)},
        {"excluded", static_cast<int>(p.excluded)}, {"companions", companions}, {"art", art},
        {"actionable", actionable}, {"bytes", static_cast<double>(bytes)}, {"volumes", volumes},
        {"warnings", warnings}, {"ok", p.ok()},
    };
    emit planChanged();
}

void LibraryTools::applyPathChanges(const QHash<QString, QString>& map) {
    if (map.isEmpty()) return;
    playlists_->remapPaths(map);
    player_->remapPathsOnNextScan(map);
}

void LibraryTools::execute() {
    if (!planData_ || busy()) return;
    if (!planData_->ok()) {
        emit message("", tr("目标盘空间不够"));
        return;
    }
    auto plan = planData_;
    const organizer::OrganizeOptions opt = organizeOptions();
    const std::wstring journal = journalPath("organize-last.log").toStdWString();
    setProgress(0, tr("准备中…"));
    run("organize",
        [this, plan, opt, journal]() {
            QElapsedTimer clock;
            clock.start();
            qint64 last = -1000;
            auto res = organizer::Execute(*plan, opt, journal, [&](const organizer::OrganizeProgress& p) {
                if (clock.elapsed() - last < 80 && p.done != p.total) return;
                last = clock.elapsed();
                const double v = p.bytes_total ? double(p.bytes_done) / double(p.bytes_total) : p.total ? double(p.done) / p.total : 0;
                const QString text = tr("%1 / %2  %3").arg(p.done).arg(p.total).arg(QFileInfo(Q(p.current)).fileName());
                QMetaObject::invokeMethod(this, [this, v, text]() { setProgress(v, text); }, Qt::QueuedConnection);
            }, &cancel_);
            return std::make_shared<organizer::OrganizeResult>(std::move(res));
        },
        [this](std::shared_ptr<organizer::OrganizeResult> r) {
            QHash<QString, QString> map;
            std::vector<std::pair<std::wstring, std::wstring>> moved, copied;
            for (const auto& c : r->changes) {
                if (!c.copied) map.insert(PathKeyOf(Q(c.from)), Q(c.to));
                (c.copied ? copied : moved).emplace_back(c.from, c.to);
            }
            applyPathChanges(map);
            // 文件内容没变,只是换了位置:沿用扫描缓存,重扫时不再逐个重新解析
            lib_->carryScanCache(moved, false);
            lib_->carryScanCache(copied, true);
            organizeResult_ = QVariantMap{
                {"moved", static_cast<int>(r->moved)}, {"copied", static_cast<int>(r->copied)},
                {"failed", static_cast<int>(r->failed)}, {"skipped", static_cast<int>(r->skipped)},
                {"cancelled", r->cancelled}, {"removedDirs", static_cast<int>(r->removed_dirs.size())},
                {"failures", Failures(r->failures)}, {"mode", mode_}, {"target", target_},
            };
            // 移动到曲库之外的目录:把目标加进曲库,否则这些歌就从曲库里消失了
            bool inside = mode_ == 2;
            for (const QString& f : lib_->folders()) inside = inside || UnderFolder(target_, f);
            if (mode_ == 0 && !inside && r->moved > 0) lib_->addFolder(target_);
            else if (r->moved > 0) lib_->rescan();
            clearPlan();
            emit organizeDone();
            const int n = static_cast<int>(r->moved + r->copied);
            emit message(r->failed ? "" : "check", r->cancelled ? tr("已取消,完成了 %1 个文件").arg(n)
                                                   : r->failed ? tr("完成 %1 个,%2 个失败").arg(n).arg(r->failed)
                                                               : tr("整理完成,%1 个文件").arg(n));
        });
}

bool LibraryTools::canUndoOrganize() const {
    auto info = organizer::InspectJournal(journalPath("organize-last.log").toStdWString());
    return info.exists && !info.undone && info.completed > 0;
}

void LibraryTools::undoOrganize() {
    const std::wstring journal = journalPath("organize-last.log").toStdWString();
    setProgress(0, tr("正在撤销…"));
    run("undoOrganize", [journal]() { return std::make_shared<organizer::OrganizeResult>(organizer::Undo(journal)); },
        [this](std::shared_ptr<organizer::OrganizeResult> r) {
            QHash<QString, QString> map;
            std::vector<std::pair<std::wstring, std::wstring>> moved;
            for (const auto& c : r->changes)
                if (!c.to.empty()) {
                    map.insert(PathKeyOf(Q(c.from)), Q(c.to));
                    moved.emplace_back(c.from, c.to);
                }
            applyPathChanges(map);
            lib_->carryScanCache(moved, false);
            lib_->rescan();
            organizeResult_.clear();
            emit organizeDone();
            emit message(r->failed ? "" : "check", r->failed ? tr("已撤销 %1 个,%2 个无法还原").arg(r->moved).arg(r->failed)
                                                             : tr("已撤销,%1 个文件回到原处").arg(r->moved));
        });
}

// ---------------------------------------------------------------- 查重
void LibraryTools::findDuplicates() {
    auto snap = lib_->snapshot();
    auto tracks = std::make_shared<std::vector<dedupe::DupTrack>>();
    std::unordered_set<std::wstring> seen;
    QSet<QString> excluded;
    for (const QString& e : lib_->excludedFiles()) excluded.insert(PathKeyOf(e));
    for (const TrackItem& t : snap->catalog.GetAllTracks()) {
        // 转换产物删掉后扫描会再次提示转换;已「只从曲库移除」的不再列出
        if (t.is_cue_track || !t.source_path.empty()) continue;
        const QString path = Q(t.file_path);
        if (excluded.contains(PathKeyOf(path)) || !seen.insert(PathKeyOf(path).toStdWString()).second) continue;
        dedupe::DupTrack d;
        d.path = t.file_path;
        d.title = t.title;
        d.artist = t.artist;
        d.album = t.album;
        d.duration_ms = t.duration_ms;
        d.lossless = t.is_lossless;
        d.sample_rate = static_cast<int>(t.sample_rate);
        d.bits_per_sample = t.bits_per_sample;
        d.bitrate_kbps = static_cast<int>(t.bitrate_kbps);
        d.track_no = t.track_no;
        d.has_cover = t.has_cover;
        d.size = t.file_size;
        tracks->push_back(std::move(d));
    }
    dedupe::FindOptions opt;
    opt.same_song = dupSameSong_;
    opt.same_name = dupSameName_;
    opt.versions = dupSameSong_;
    opt.duration_tolerance_ms = static_cast<uint32_t>(dupTolerance_) * 1000;
    // 首选保留:不是酷狗下载目录、也没隐藏的曲库文件夹
    for (const QString& f : lib_->folders())
        if (!lib_->isKugouFolder(f) && !lib_->hiddenFolders().contains(f)) opt.preferred_roots.push_back(W(f));
    setProgress(0, tr("正在比对 %1 首歌…").arg(tracks->size()));
    run("find",
        [this, tracks, opt]() {
            QElapsedTimer clock;
            clock.start();
            qint64 last = -1000;
            auto res = dedupe::FindDuplicates(*tracks, opt, [&](const dedupe::FindProgress& p) {
                if (clock.elapsed() - last < 80) return;
                last = clock.elapsed();
                const double v = p.total ? std::min(1.0, double(p.done) / double(p.total)) : 0;
                const QString text = tr("正在比对内容 %1%").arg(qRound(v * 100));
                QMetaObject::invokeMethod(this, [this, v, text]() { setProgress(v, text); }, Qt::QueuedConnection);
            }, &cancel_);
            return std::make_shared<std::pair<std::vector<dedupe::DupTrack>, dedupe::FindResult>>(*tracks, std::move(res));
        },
        [this](std::shared_ptr<std::pair<std::vector<dedupe::DupTrack>, dedupe::FindResult>> r) {
            dupTracks_ = std::move(r->first);
            groups_ = std::move(r->second.groups);
            dupSummary_ = QVariantMap{{"scanned", static_cast<int>(dupTracks_.size())},
                                      {"cancelled", r->second.cancelled}};
            showGroups();
            if (groups_.empty()) emit message("check", tr("没有找到重复的歌"));
        });
}

void LibraryTools::clearDuplicates() {
    groups_.clear();
    dupTracks_.clear();
    dupItems_.setRows({});
    dupSummary_.clear();
    emit dupChanged();
}

void LibraryTools::showGroups() {
    static const char* const kCats[] = {"identical", "audio", "song", "name", "versions"};
    const QStringList labels = {tr("完全相同"), tr("只是标签不同"), tr("同一首 · 不同音质"), tr("文件名相同"), tr("不同版本")};
    QVector<QVariantMap> rows;
    rows.reserve(static_cast<int>(groups_.size()));
    QVariantMap byCat;
    int selected = 0;
    double reclaim = 0;
    dedupe::UpdateReclaim(groups_);
    for (const auto& g : groups_) {
        const int k = static_cast<int>(g.kind);
        QVariantList members;
        int keep = 0;
        for (size_t i = 0; i < g.members.size(); ++i) {
            const auto& m = g.members[i];
            const auto& t = dupTracks_[m.track];
            const QString path = Q(t.path);
            QFileInfo fi(path);
            members.append(QVariantMap{
                {"path", path}, {"name", fi.fileName()}, {"dir", QDir::toNativeSeparators(fi.absolutePath())},
                {"title", Q(t.title)}, {"artist", Q(t.artist)}, {"note", Q(m.note)},
                {"size", static_cast<double>(m.size)}, {"keep", m.keep}, {"duration", FormatMs(t.duration_ms)},
                {"suggested", i == g.suggested},
            });
            keep += m.keep;
            if (!m.keep) ++selected;
        }
        reclaim += static_cast<double>(g.reclaim_bytes);
        byCat[kCats[k]] = byCat.value(kCats[k]).toInt() + 1;
        const auto& first = dupTracks_[g.members[g.suggested].track];
        QString title = first.title.empty() ? QFileInfo(Q(first.path)).completeBaseName() : Q(first.title);
        if (!first.artist.empty() && first.artist != L"未知艺术家") title += " — " + Q(first.artist);
        rows.append(QVariantMap{
            {"cat", kCats[k]}, {"kindLabel", labels[k]}, {"review", g.needs_review},
            {"reclaim", static_cast<double>(g.reclaim_bytes)}, {"count", static_cast<int>(g.members.size())},
            {"keepCount", keep}, {"title", title}, {"members", members},
        });
    }
    dupItems_.setRows(std::move(rows));
    dupSummary_["groups"] = static_cast<int>(groups_.size());
    dupSummary_["byCat"] = byCat;
    dupSummary_["selected"] = selected;
    dupSummary_["reclaim"] = reclaim;
    emit dupChanged();
}

void LibraryTools::autoSelect(int rule) {
    if (groups_.empty()) return;
    std::vector<std::wstring> preferred;
    for (const QString& f : lib_->folders())
        if (!lib_->isKugouFolder(f) && !lib_->hiddenFolders().contains(f)) preferred.push_back(W(f));
    dedupe::AutoSelect(groups_, dupTracks_, static_cast<dedupe::KeepRule>(std::clamp(rule, 0, 6)), preferred);
    showGroups();
}

void LibraryTools::setKeep(int g, int m, bool keep) {
    if (g < 0 || g >= static_cast<int>(groups_.size())) return;
    auto& grp = groups_[g];
    if (m < 0 || m >= static_cast<int>(grp.members.size()) || grp.members[m].keep == keep) return;
    grp.members[m].keep = keep;
    // 只刷新这一行和汇总,不重置整个列表 (保持滚动位置与展开状态)
    dedupe::UpdateReclaim(groups_);
    QVariantMap row = dupItems_.rows()[g];
    QVariantList members = row.value("members").toList();
    QVariantMap mm = members[m].toMap();
    mm["keep"] = keep;
    members[m] = mm;
    int keepCount = 0, selected = 0;
    double reclaim = 0;
    for (const auto& x : grp.members) keepCount += x.keep;
    for (const auto& gg : groups_) {
        reclaim += static_cast<double>(gg.reclaim_bytes);
        for (const auto& x : gg.members) selected += !x.keep;
    }
    row["members"] = members;
    row["keepCount"] = keepCount;
    row["reclaim"] = static_cast<double>(grp.reclaim_bytes);
    dupItems_.updateRow(g, row);
    dupSummary_["selected"] = selected;
    dupSummary_["reclaim"] = reclaim;
    emit dupChanged();
}

void LibraryTools::applyDedupe(int action) {
    if (groups_.empty() || busy()) return;
    dedupe::ApplyOptions opt;
    opt.action = static_cast<dedupe::DupAction>(std::clamp(action, 0, 4));
    opt.quarantine_root = W(quarantine_);
    for (const QString& f : lib_->folders()) opt.protect_roots.push_back(W(f));
    auto groups = std::make_shared<std::vector<dedupe::DupGroup>>(groups_);
    auto tracks = std::make_shared<std::vector<dedupe::DupTrack>>(dupTracks_);
    // 正在播放的那首不处理
    const QString cur = player_->currentPath();
    if (!cur.isEmpty())
        for (auto& g : *groups)
            for (auto& m : g.members)
                if (PathKeyOf(Q((*tracks)[m.track].path)) == PathKeyOf(cur)) m.keep = true;
    const std::wstring journal = journalPath("dedupe-last.log").toStdWString();
    setProgress(0, tr("正在处理…"));
    run("apply",
        [this, groups, tracks, opt, journal]() {
            auto res = dedupe::ApplySelection(*groups, *tracks, opt, journal, [this](const dedupe::FindProgress& p) {
                const double v = p.total ? double(p.done) / p.total : 0;
                const QString text = tr("%1 / %2").arg(p.done).arg(p.total);
                QMetaObject::invokeMethod(this, [this, v, text]() { setProgress(v, text); }, Qt::QueuedConnection);
            }, &cancel_);
            return std::make_shared<dedupe::ApplyResult>(std::move(res));
        },
        [this, action](std::shared_ptr<dedupe::ApplyResult> r) {
            QHash<QString, QString> map;
            QStringList removed;
            QSet<QString> removedKeys;
            for (const auto& c : r->changes) {
                removed << Q(c.removed);
                removedKeys.insert(PathKeyOf(Q(c.removed)));
                if (!c.keeper.empty()) map.insert(PathKeyOf(Q(c.removed)), Q(c.keeper));
            }
            // 歌单、收藏、播放记录改指向保留的那份
            applyPathChanges(map);
            if (action == 3 || action == 4) lib_->excludeFiles(removed); // 文件路径还在,只是不再出现在曲库里
            else if (!removed.isEmpty()) lib_->rescan();
            // 列表里去掉已处理的文件,剩不到两份的组也去掉
            for (auto& g : groups_) {
                std::vector<dedupe::DupMember> keep;
                for (const auto& m : g.members)
                    if (!removedKeys.contains(PathKeyOf(Q(dupTracks_[m.track].path)))) keep.push_back(m);
                g.members.swap(keep);
                g.suggested = 0;
                for (size_t i = 0; i < g.members.size(); ++i)
                    if (g.members[i].keep) { g.suggested = i; break; }
            }
            groups_.erase(std::remove_if(groups_.begin(), groups_.end(), [](const dedupe::DupGroup& g) { return g.members.size() < 2; }),
                          groups_.end());
            showGroups();
            dedupeResult_ = QVariantMap{
                {"done", static_cast<int>(r->done)}, {"failed", static_cast<int>(r->failed)},
                {"skipped", static_cast<int>(r->skipped)}, {"freed", static_cast<double>(r->freed_bytes)},
                {"cancelled", r->cancelled}, {"failures", Failures(r->failures)}, {"action", action},
                {"lyrics", static_cast<int>(r->lyric_moves.size())},
            };
            emit dedupeDone();
            const QString freed = QLocale().formattedDataSize(static_cast<qint64>(r->freed_bytes));
            emit message(r->failed ? "" : "check",
                         r->failed ? tr("处理了 %1 个,%2 个失败").arg(r->done).arg(r->failed)
                         : (action == 3 ? tr("已从曲库移除 %1 首").arg(r->done) : tr("处理了 %1 个,腾出 %2").arg(r->done).arg(freed)));
        });
}

bool LibraryTools::canUndoDedupe() const {
    QFile f(journalPath("dedupe-last.log"));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QList<QByteArray> lines = f.readAll().split('\n');
    bool any = false;
    for (const QByteArray& l : lines) {
        if (l == "U") return false;
        if (l.startsWith("Q\t") || l.startsWith("L\t") || l.startsWith("R\t") || l.startsWith("Y\t") ||
            (l.startsWith("B\t") && l.count('\t') >= 2))
            any = true;
    }
    return any;
}

void LibraryTools::undoDedupe() {
    // 只从曲库移除 / 硬链接的文件:撤销后重新出现在曲库里
    QStringList restore;
    QFile f(journalPath("dedupe-last.log"));
    if (f.open(QIODevice::ReadOnly))
        for (const QByteArray& l : f.readAll().split('\n'))
            if (l.startsWith("R\t") || l.startsWith("L\t")) restore << QString::fromUtf8(l.mid(2).split('\t').value(0));
    const std::wstring journal = journalPath("dedupe-last.log").toStdWString();
    setProgress(0, tr("正在撤销…"));
    run("undoDedupe", [journal]() { return std::make_shared<dedupe::ApplyResult>(dedupe::UndoDedupe(journal)); },
        [this, restore](std::shared_ptr<dedupe::ApplyResult> r) {
            if (!restore.isEmpty()) lib_->includeFiles(restore);
            else lib_->rescan();
            dedupeResult_.clear();
            emit dedupeDone();
            emit message(r->failed ? "" : "check", r->failed ? tr("已还原 %1 个,%2 个无法还原").arg(r->done).arg(r->failed)
                                                             : tr("已撤销,还原了 %1 个文件").arg(r->done));
        });
}

void LibraryTools::reveal(const QString& path) const {
    const QFileInfo fi(path);
#ifdef Q_OS_WIN
    if (fi.exists()) {
        QProcess p;
        p.setProgram(QStringLiteral("explorer.exe"));
        p.setNativeArguments(QStringLiteral("/select,\"%1\"").arg(QDir::toNativeSeparators(fi.absoluteFilePath())));
        p.startDetached();
        return;
    }
#endif
    QDesktopServices::openUrl(QUrl::fromLocalFile(fi.absolutePath()));
}

void LibraryTools::openFolder(const QString& path) const {
    QDir().mkpath(path);
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}
