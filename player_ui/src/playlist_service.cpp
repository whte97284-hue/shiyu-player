#include "playlist_service.h"
#include <unordered_set>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <algorithm>

namespace {
const QString kFav = QStringLiteral("fav");
QString AlbumCover(int gen, int idx) { return QString("image://cover/a/%1/%2").arg(gen).arg(idx); }
} // namespace

PlaylistService::PlaylistService(LibraryService* lib, QObject* parent) : QObject(parent), lib_(lib) {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    file_ = dir + "/playlists.json";
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(300);
    connect(&saveTimer_, &QTimer::timeout, this, [this]() { save(); });
    load();
    rebuildIndex();
    connect(lib_, &LibraryService::libraryChanged, this, [this]() {
        rebuildIndex();
        ++revision_;
        emit playlistsChanged();
        for (const List& l : lists_) emit contentChanged(l.id);
    });
}

PlaylistService::~PlaylistService() {
    if (saveTimer_.isActive()) save();
}

void PlaylistService::rebuildIndex() {
    snap_ = lib_->snapshot();
    index_.clear();
    index_.reserve(snap_->catalog.GetAllTracks().size());
    for (const TrackItem& t : snap_->catalog.GetAllTracks()) index_.emplace(TrackKeyOf(t), &t);
}

const TrackItem* PlaylistService::resolve(const QString& key) const {
    auto it = index_.find(key);
    return it == index_.end() ? nullptr : it->second;
}

QString PlaylistService::keyOf(double trackId) const {
    const TrackItem* t = snap_ ? snap_->catalog.FindById(static_cast<uint64_t>(trackId)) : nullptr;
    if (!t) {
        // 曲库可能刚刚刷新:用最新快照再找一次
        auto s = lib_->snapshot();
        t = s->catalog.FindById(static_cast<uint64_t>(trackId));
    }
    return t ? TrackKeyOf(*t) : QString();
}

PlaylistService::List* PlaylistService::find(const QString& id) {
    for (List& l : lists_)
        if (l.id == id) return &l;
    return nullptr;
}
const PlaylistService::List* PlaylistService::find(const QString& id) const {
    for (const List& l : lists_)
        if (l.id == id) return &l;
    return nullptr;
}

std::vector<size_t> PlaylistService::visibleSlots(const List& l) const {
    std::vector<size_t> out;
    out.reserve(l.keys.size());
    const bool collapse = snap_ && !snap_->shownCopy.empty();
    std::unordered_set<uint64_t> seen;
    for (size_t i = 0; i < l.keys.size(); ++i) {
        const TrackItem* t = resolve(l.keys[i]);
        if (!t) continue;
        if (collapse) {
            auto it = snap_->shownCopy.find(t->id);
            if (!seen.insert(it == snap_->shownCopy.end() ? t->id : it->second).second) continue;
        }
        out.push_back(i);
    }
    return out;
}

std::vector<const TrackItem*> PlaylistService::tracksOf(const QString& id) const {
    std::vector<const TrackItem*> out;
    if (const List* l = find(id))
        for (size_t i : visibleSlots(*l)) out.push_back(resolve(l->keys[i]));
    return out;
}

QVariantMap PlaylistService::info(const QString& id) const {
    QVariantMap m;
    const List* l = find(id);
    if (!l) return m;
    auto tracks = tracksOf(id);
    uint64_t ms = 0;
    QStringList covers;
    std::vector<int> seen;
    for (const TrackItem* t : tracks) {
        ms += t->duration_ms;
        auto it = snap_->trackAlbum.find(t->id);
        if (covers.size() < 4 && it != snap_->trackAlbum.end() &&
            std::find(seen.begin(), seen.end(), it->second) == seen.end()) {
            seen.push_back(it->second);
            covers << AlbumCover(lib_->generation(), it->second);
        }
    }
    m["id"] = l->id;
    m["name"] = l->name;
    m["builtin"] = l->id == kFav;
    m["count"] = static_cast<int>(tracks.size());
    m["duration"] = LibraryService::formatDuration(ms);
    m["covers"] = covers;
    m["summary"] = tracks.empty() ? tr("还没有歌曲")
                                  : tr("%1 首  ·  %2").arg(tracks.size()).arg(LibraryService::formatLongDuration(ms));
    return m;
}

QVariantList PlaylistService::playlists() const {
    QVariantList out;
    for (const List& l : lists_) out.append(info(l.id));
    return out;
}

QString PlaylistService::suggestName() const {
    for (int n = 1;; ++n) {
        QString name = tr("歌单 %1").arg(n);
        bool used = false;
        for (const List& l : lists_) used = used || l.name == name;
        if (!used) return name;
    }
}

QString PlaylistService::create(const QString& name) {
    List l;
    l.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    l.name = name.trimmed().isEmpty() ? suggestName() : name.trimmed();
    l.created = QDateTime::currentMSecsSinceEpoch();
    lists_.push_back(std::move(l));
    touch(lists_.back().id);
    return lists_.back().id;
}

void PlaylistService::rename(const QString& id, const QString& name) {
    List* l = find(id);
    if (!l || id == kFav || name.trimmed().isEmpty() || l->name == name.trimmed()) return;
    l->name = name.trimmed();
    touch(id);
}

void PlaylistService::remove(const QString& id) {
    if (id == kFav) return;
    auto it = std::find_if(lists_.begin(), lists_.end(), [&](const List& l) { return l.id == id; });
    if (it == lists_.end()) return;
    lists_.erase(it);
    touch(id);
}

int PlaylistService::addTracks(const QString& id, const QVariantList& trackIds) {
    List* l = find(id);
    if (!l) return 0;
    int added = 0;
    for (const QVariant& v : trackIds) {
        QString k = keyOf(v.toDouble());
        if (k.isEmpty() || std::find(l->keys.begin(), l->keys.end(), k) != l->keys.end()) continue;
        l->keys.push_back(k);
        ++added;
    }
    if (added) touch(id);
    return added;
}

void PlaylistService::removeTrack(const QString& id, double trackId) {
    List* l = find(id);
    QString k = keyOf(trackId);
    if (!l || k.isEmpty()) return;
    auto it = std::find(l->keys.begin(), l->keys.end(), k);
    if (it == l->keys.end()) return;
    l->keys.erase(it);
    touch(id);
}

void PlaylistService::move(const QString& id, int from, int to) {
    List* l = find(id);
    if (!l) return;
    // from / to 是可见行号 (只含能解析、没被显示去重收起的曲目),换算为存储下标
    const std::vector<size_t> visible = visibleSlots(*l);
    if (from < 0 || to < 0 || from >= static_cast<int>(visible.size()) || to >= static_cast<int>(visible.size()) || from == to)
        return;
    // 只在可见槽位之间重排,曲库中暂时缺失的曲目保持原位
    std::vector<QString> keys;
    for (size_t i : visible) keys.push_back(l->keys[i]);
    QString k = keys[from];
    keys.erase(keys.begin() + from);
    keys.insert(keys.begin() + to, k);
    for (size_t i = 0; i < visible.size(); ++i) l->keys[visible[i]] = keys[i];
    touch(id);
}

bool PlaylistService::isFavorite(double trackId) const {
    const List* l = find(kFav);
    if (!l || !snap_) return false;
    const TrackItem* t = snap_->catalog.FindById(static_cast<uint64_t>(trackId));
    if (!t) return false;
    QString k = TrackKeyOf(*t);
    return std::find(l->keys.begin(), l->keys.end(), k) != l->keys.end();
}

void PlaylistService::toggleFavorite(double trackId) {
    List* l = find(kFav);
    QString k = keyOf(trackId);
    if (!l || k.isEmpty()) return;
    auto it = std::find(l->keys.begin(), l->keys.end(), k);
    if (it == l->keys.end()) l->keys.insert(l->keys.begin(), k); // 最新收藏排在最前
    else l->keys.erase(it);
    touch(kFav);
}

void PlaylistService::touch(const QString& id) {
    ++revision_;
    saveTimer_.start();
    emit playlistsChanged();
    emit contentChanged(id);
}

void PlaylistService::load() {
    lists_.clear();
    QFile f(file_);
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonArray arr = QJsonDocument::fromJson(f.readAll()).object().value("playlists").toArray();
        for (const QJsonValue& v : arr) {
            QJsonObject o = v.toObject();
            List l;
            l.id = o.value("id").toString();
            l.name = o.value("name").toString();
            l.created = static_cast<qint64>(o.value("created").toDouble());
            for (const QJsonValue& k : o.value("tracks").toArray()) l.keys.push_back(k.toString());
            if (!l.id.isEmpty() && !find(l.id)) lists_.push_back(std::move(l));
        }
        f.seek(0);
        for (const QJsonValue& k : QJsonDocument::fromJson(f.readAll()).object().value("history").toArray())
            history_.push_back(k.toString());
    }
    if (!find(kFav)) {
        List fav;
        fav.id = kFav;
        fav.name = tr("我喜欢");
        lists_.insert(lists_.begin(), std::move(fav));
    } else if (lists_.front().id != kFav) {
        auto it = std::find_if(lists_.begin(), lists_.end(), [](const List& l) { return l.id == kFav; });
        std::rotate(lists_.begin(), it, it + 1);
    }
}

void PlaylistService::save() const {
    QJsonArray arr;
    for (const List& l : lists_) {
        QJsonObject o;
        o["id"] = l.id;
        o["name"] = l.name;
        o["created"] = static_cast<double>(l.created);
        QJsonArray tracks;
        for (const QString& k : l.keys) tracks.append(k);
        o["tracks"] = tracks;
        arr.append(o);
    }
    QJsonObject root;
    root["version"] = 1;
    root["playlists"] = arr;
    QJsonArray hist;
    for (const QString& k : history_) hist.append(k);
    root["history"] = hist;
    QSaveFile f(file_);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        f.commit();
    }
}

int PlaylistService::remapPaths(const QHash<QString, QString>& pathMap) {
    if (pathMap.isEmpty()) return 0;
    int changed = 0;
    auto remapList = [&](std::vector<QString>& keys) {
        std::vector<QString> out;
        out.reserve(keys.size());
        for (const QString& k : keys) {
            QString n = RemapTrackKey(k, pathMap);
            if (!n.isEmpty()) ++changed;
            const QString& use = n.isEmpty() ? k : n;
            if (std::find(out.begin(), out.end(), use) == out.end()) out.push_back(use);
        }
        keys.swap(out);
    };
    for (List& l : lists_) remapList(l.keys);
    remapList(history_);
    if (changed) {
        saveTimer_.start();
        ++revision_;
        emit playlistsChanged();
        for (const List& l : lists_) emit contentChanged(l.id);
        emit historyChanged();
    }
    return changed;
}

void PlaylistService::recordPlay(double trackId) {
    QString k = keyOf(trackId);
    if (k.isEmpty() || (!history_.empty() && history_.front() == k)) return;
    auto it = std::find(history_.begin(), history_.end(), k);
    if (it != history_.end()) history_.erase(it);
    history_.insert(history_.begin(), k);
    if (static_cast<int>(history_.size()) > kHistoryMax) history_.resize(kHistoryMax);
    saveTimer_.start();
    emit historyChanged();
}

void PlaylistService::clearHistory() {
    if (history_.empty()) return;
    history_.clear();
    saveTimer_.start();
    emit historyChanged();
}

std::vector<const TrackItem*> PlaylistService::historyTracks() const {
    std::vector<const TrackItem*> out;
    out.reserve(history_.size());
    for (const QString& k : history_)
        if (const TrackItem* t = resolve(k)) out.push_back(t);
    return out;
}

QVariantList PlaylistService::trackIdsOf(const QString& id) const {
    QVariantList out;
    for (const TrackItem* t : tracksOf(id)) out.append(static_cast<double>(t->id));
    return out;
}
