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
QString NormalizeTA(const QString& title, const QString& artist) {
    return title.trimmed().toLower() + QLatin1Char('\n') + artist.trimmed().toLower();
}
QVariantMap MetaToMap(const QString& key, const PlaylistService::OnlineTrackMeta& m) {
    QVariantMap v;
    v["onlineKey"] = key;
    v["source"] = m.source;
    v["id"] = m.id;
    v["albumId"] = m.albumId;
    v["title"] = m.title;
    v["artist"] = m.artist;
    v["album"] = m.album;
    v["durationMs"] = m.durationMs;
    v["cover"] = m.cover;
    v["paywalled"] = m.paywalled;
    return v;
}
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
    titleArtistIndex_.clear();
    titleArtistIndex_.reserve(snap_->catalog.GetAllTracks().size());
    for (const TrackItem& t : snap_->catalog.GetAllTracks()) {
        const QString ta = NormalizeTA(QString::fromStdWString(t.title), QString::fromStdWString(t.artist));
        if (ta.size() > 1) {
            auto it = titleArtistIndex_.find(ta);
            if (it == titleArtistIndex_.end() ||
                (t.is_lossless && !it->second->is_lossless) ||
                (t.is_lossless == it->second->is_lossless && t.bitrate_kbps > it->second->bitrate_kbps)) {
                titleArtistIndex_[ta] = &t;
            }
        }
    }
}

QString PlaylistService::OnlineKeyOf(int source, const QString& id) {
    if (id.trimmed().isEmpty()) return {};
    return QStringLiteral("online:%1:%2").arg(source).arg(id.trimmed());
}

const TrackItem* PlaylistService::resolve(const QString& key) const {
    auto it = index_.find(key);
    if (it != index_.end()) return it->second;
    if (key.startsWith(QLatin1String("online:"))) {
        auto oit = onlineMeta_.find(key);
        if (oit != onlineMeta_.end()) {
            const QString ta = NormalizeTA(oit->second.title, oit->second.artist);
            auto tit = titleArtistIndex_.find(ta);
            if (tit != titleArtistIndex_.end()) return tit->second;
        }
    }
    return nullptr;
}

const PlaylistService::OnlineTrackMeta* PlaylistService::resolveOnline(const QString& key) const {
    auto it = onlineMeta_.find(key);
    return it == onlineMeta_.end() ? nullptr : &it->second;
}

QString PlaylistService::upsertOnlineMeta(const QVariantMap& meta) {
    const int src = meta.value("source").toInt();
    const QString id = meta.value("id").toString().trimmed();
    const QString key = OnlineKeyOf(src, id);
    if (key.isEmpty()) return {};
    OnlineTrackMeta& m = onlineMeta_[key];
    m.source = src;
    m.id = id;
    if (const QString a = meta.value("albumId").toString(); !a.isEmpty()) m.albumId = a;
    if (const QString t = meta.value("title").toString(); !t.isEmpty()) m.title = t;
    if (const QString ar = meta.value("artist").toString(); !ar.isEmpty()) m.artist = ar;
    if (const QString al = meta.value("album").toString(); !al.isEmpty()) m.album = al;
    if (const int d = meta.value("durationMs").toInt(); d > 0) m.durationMs = d;
    if (const QString c = meta.value("cover").toString(); !c.isEmpty()) m.cover = c;
    if (meta.contains("paywalled")) m.paywalled = meta.value("paywalled").toBool();
    return key;
}

QVariantMap PlaylistService::onlineMeta(const QString& onlineKey) const {
    if (const OnlineTrackMeta* m = resolveOnline(onlineKey)) return MetaToMap(onlineKey, *m);
    return {};
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
        if (!t) {
            if (resolveOnline(l.keys[i])) out.push_back(i);
            continue;
        }
        if (collapse) {
            auto it = snap_->shownCopy.find(t->id);
            if (!seen.insert(it == snap_->shownCopy.end() ? t->id : it->second).second) continue;
        }
        out.push_back(i);
    }
    return out;
}

std::vector<PlaylistService::Entry> PlaylistService::entriesOf(const QString& id) const {
    std::vector<Entry> out;
    if (const List* l = find(id)) {
        for (size_t i : visibleSlots(*l)) {
            const QString& k = l->keys[i];
            if (const TrackItem* t = resolve(k)) out.push_back(Entry{t, nullptr, k});
            else if (const OnlineTrackMeta* o = resolveOnline(k)) out.push_back(Entry{nullptr, o, k});
        }
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
    auto entries = entriesOf(id);
    uint64_t ms = 0;
    QStringList covers;
    std::vector<int> seen;
    for (const Entry& e : entries) {
        if (e.local) {
            const TrackItem* t = e.local;
            ms += t->duration_ms;
            auto it = snap_->trackAlbum.find(t->id);
            if (covers.size() < 4 && it != snap_->trackAlbum.end() &&
                std::find(seen.begin(), seen.end(), it->second) == seen.end()) {
                seen.push_back(it->second);
                covers << AlbumCover(lib_->generation(), it->second);
            }
        } else if (e.online && e.online->durationMs > 0) {
            ms += static_cast<uint64_t>(e.online->durationMs);
        }
    }
    m["id"] = l->id;
    m["name"] = l->name;
    m["builtin"] = l->id == kFav;
    m["count"] = static_cast<int>(entries.size());
    m["duration"] = LibraryService::formatDuration(ms);
    m["covers"] = covers;
    m["summary"] = entries.empty() ? tr("还没有歌曲")
                                   : tr("%1 首  ·  %2").arg(entries.size()).arg(LibraryService::formatLongDuration(ms));
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

int PlaylistService::addOnlineTrack(const QString& id, const QVariantMap& meta) {
    List* l = find(id);
    if (!l) return 0;
    QString k = upsertOnlineMeta(meta);
    if (k.isEmpty()) return 0;
    if (const TrackItem* local = resolve(k)) {
        const QString lk = TrackKeyOf(*local);
        if (!lk.isEmpty()) k = lk;
    }
    if (std::find(l->keys.begin(), l->keys.end(), k) != l->keys.end()) return 0;
    l->keys.push_back(k);
    touch(id);
    return 1;
}

void PlaylistService::removeTrack(const QString& id, double trackId) {
    removeEntry(id, trackId, QString());
}

void PlaylistService::removeEntry(const QString& id, double trackId, const QString& onlineKey) {
    List* l = find(id);
    if (!l) return;
    QString k = trackId >= 0 ? keyOf(trackId) : onlineKey;
    if (k.isEmpty()) return;
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

bool PlaylistService::isFavoriteOnline(const QString& onlineKey) const {
    const List* l = find(kFav);
    if (!l || onlineKey.isEmpty()) return false;
    if (std::find(l->keys.begin(), l->keys.end(), onlineKey) != l->keys.end()) return true;
    if (const TrackItem* local = resolve(onlineKey)) {
        const QString lk = TrackKeyOf(*local);
        if (!lk.isEmpty() && std::find(l->keys.begin(), l->keys.end(), lk) != l->keys.end()) return true;
    }
    return false;
}

void PlaylistService::toggleFavoriteOnline(const QVariantMap& meta) {
    List* l = find(kFav);
    QString k = upsertOnlineMeta(meta);
    if (!l || k.isEmpty()) return;
    if (const TrackItem* local = resolve(k)) {
        const QString lk = TrackKeyOf(*local);
        if (!lk.isEmpty()) {
            auto oit = std::find(l->keys.begin(), l->keys.end(), k);
            auto lit = std::find(l->keys.begin(), l->keys.end(), lk);
            if (oit != l->keys.end()) l->keys.erase(oit);
            else if (lit != l->keys.end()) l->keys.erase(lit);
            else l->keys.insert(l->keys.begin(), lk);
            touch(kFav);
            return;
        }
    }
    auto it = std::find(l->keys.begin(), l->keys.end(), k);
    if (it == l->keys.end()) l->keys.insert(l->keys.begin(), k);
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
        f.seek(0);
        const QJsonObject metaObj = QJsonDocument::fromJson(f.readAll()).object().value("onlineMeta").toObject();
        for (auto it = metaObj.begin(); it != metaObj.end(); ++it) {
            const QJsonObject mo = it.value().toObject();
            OnlineTrackMeta om;
            om.source = mo.value("source").toInt();
            om.id = mo.value("id").toString();
            om.albumId = mo.value("albumId").toString();
            om.title = mo.value("title").toString();
            om.artist = mo.value("artist").toString();
            om.album = mo.value("album").toString();
            om.durationMs = mo.value("durationMs").toInt();
            om.cover = mo.value("cover").toString();
            om.paywalled = mo.value("paywalled").toBool();
            if (!it.key().isEmpty() && !om.id.isEmpty()) onlineMeta_.emplace(it.key(), std::move(om));
        }
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
    std::unordered_set<QString> usedOnline;
    for (const List& l : lists_)
        for (const QString& k : l.keys) if (k.startsWith(QLatin1String("online:"))) usedOnline.insert(k);
    for (const QString& k : history_) if (k.startsWith(QLatin1String("online:"))) usedOnline.insert(k);
    QJsonObject metaObj;
    for (const QString& k : usedOnline) {
        auto it = onlineMeta_.find(k);
        if (it == onlineMeta_.end()) continue;
        QJsonObject mo;
        mo["source"] = it->second.source;
        mo["id"] = it->second.id;
        if (!it->second.albumId.isEmpty()) mo["albumId"] = it->second.albumId;
        mo["title"] = it->second.title;
        mo["artist"] = it->second.artist;
        if (!it->second.album.isEmpty()) mo["album"] = it->second.album;
        if (it->second.durationMs > 0) mo["durationMs"] = it->second.durationMs;
        if (!it->second.cover.isEmpty()) mo["cover"] = it->second.cover;
        if (it->second.paywalled) mo["paywalled"] = true;
        metaObj[k] = mo;
    }
    root["onlineMeta"] = metaObj;
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
    const TrackItem* target = resolve(k);
    history_.erase(std::remove_if(history_.begin(), history_.end(), [&](const QString& item) {
        return item == k || (target && resolve(item) == target);
    }), history_.end());
    history_.insert(history_.begin(), k);
    if (static_cast<int>(history_.size()) > kHistoryMax) history_.resize(kHistoryMax);
    saveTimer_.start();
    emit historyChanged();
}

void PlaylistService::recordOnlinePlay(const QVariantMap& meta) {
    QString k = upsertOnlineMeta(meta);
    if (k.isEmpty()) return;
    if (const TrackItem* local = resolve(k)) {
        recordPlay(static_cast<double>(local->id));
        return;
    }
    if (!history_.empty() && history_.front() == k) return;
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

std::vector<PlaylistService::Entry> PlaylistService::historyEntries() const {
    std::vector<Entry> out;
    out.reserve(history_.size());
    std::unordered_set<uint64_t> seenLocal;
    for (const QString& k : history_) {
        if (const TrackItem* t = resolve(k)) {
            if (seenLocal.insert(t->id).second) out.push_back(Entry{t, nullptr, k});
        } else if (const OnlineTrackMeta* o = resolveOnline(k)) {
            out.push_back(Entry{nullptr, o, k});
        }
    }
    return out;
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
