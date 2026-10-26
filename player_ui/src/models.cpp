#include "models.h"

#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QSettings>
#include <climits>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>

#include "pinyin_table.hpp"
#include "playlist_service.h"

namespace {
QString Q(const std::wstring& w) { return QString::fromStdWString(w); }
QString AlbumCover(int gen, int idx) { return QString("image://cover/a/%1/%2").arg(gen).arg(idx); }
QString TrackCover(int gen, uint64_t id) { return QString("image://cover/t/%1/%2").arg(gen).arg(id); }
} // namespace

// ============================================================================
AlbumModel::AlbumModel(LibraryService* lib, PlaylistService* playlists, QObject* parent)
    : QAbstractListModel(parent), lib_(lib), playlists_(playlists) {
    QSettings st;
    sort_ = std::clamp(st.value("albumWall/sort", 0).toInt(), 0, 4);
    filter_ = std::clamp(st.value("albumWall/filter", 0).toInt(), 0, 3);
    snap_ = lib_->snapshot();
    rebuild();
    connect(lib_, &LibraryService::libraryChanged, this, &AlbumModel::reload);
    connect(playlists_, &PlaylistService::historyChanged, this, [this]() { recentDirty_ = sort_ == 4; });
}

void AlbumModel::reload() {
    beginResetModel();
    snap_ = lib_->snapshot();
    rebuild();
    endResetModel();
    emit viewChanged();
}

QString AlbumModel::letterOf(const AlbumInfo& a) const {
    switch (sort_) {
        case 0: return LetterOf(a.sortKey);
        case 1: return LetterOf(a.artistSortKey);
        case 2: {
            bool ok = false;
            const int y = a.year.left(4).toInt(&ok);
            return ok && y > 1000 ? QString::number(y / 10 * 10) : QStringLiteral("#");
        }
    }
    return {};
}

void AlbumModel::rebuild() {
    recentDirty_ = false;
    const auto& albums = snap_->albums;
    order_.clear();
    order_.reserve(albums.size());
    for (int i = 0; i < static_cast<int>(albums.size()); ++i) {
        const AlbumInfo& a = albums[i];
        const bool keep = filter_ == 0 || (filter_ == 1 && a.lossless) || (filter_ == 2 && (a.hires || a.dsd)) ||
                          (filter_ == 3 && !a.lossless);
        if (keep) order_.push_back(i);
    }
    // 快照本身按标题拼音排好;其余排序用稳定排序,同组内保持标题顺序
    auto yearOf = [&](int i) {
        bool ok = false;
        const int y = albums[i].year.left(4).toInt(&ok);
        return ok ? y : 0;
    };
    switch (sort_) {
        case 1:
            std::stable_sort(order_.begin(), order_.end(), [&](int x, int y) {
                const AlbumInfo &a = albums[x], &b = albums[y];
                if (a.artistSortKey != b.artistSortKey) return a.artistSortKey < b.artistSortKey;
                return yearOf(x) < yearOf(y);
            });
            break;
        case 2:
            std::stable_sort(order_.begin(), order_.end(), [&](int x, int y) { return yearOf(x) > yearOf(y); });
            break;
        case 3:
            std::stable_sort(order_.begin(), order_.end(),
                             [&](int x, int y) { return albums[x].addedTime > albums[y].addedTime; });
            break;
        case 4: {
            // 播放历史里第一次出现的位置 (越小越近);没播过的排在后面,保持标题顺序
            std::unordered_map<int, int> rank;
            int n = 0;
            for (const TrackItem* t : playlists_->historyTracks()) {
                auto it = snap_->trackAlbum.find(t->id);
                if (it != snap_->trackAlbum.end()) rank.emplace(it->second, n++);
            }
            auto r = [&](int i) {
                auto it = rank.find(i);
                return it == rank.end() ? INT_MAX : it->second;
            };
            std::stable_sort(order_.begin(), order_.end(), [&](int x, int y) { return r(x) < r(y); });
            break;
        }
    }
    rowLetter_.clear();
    letters_.clear();
    if (sort_ <= 2) {
        rowLetter_.reserve(order_.size());
        for (int i : order_) {
            rowLetter_.push_back(letterOf(albums[i]));
            if (!letters_.contains(rowLetter_.back())) letters_ << rowLetter_.back();
        }
    }
}

void AlbumModel::setSortMode(int m) {
    m = std::clamp(m, 0, 4);
    if (m == sort_ && !(m == 4 && recentDirty_)) return;
    sort_ = m;
    QSettings().setValue("albumWall/sort", m);
    reload();
}

void AlbumModel::setFilter(int f) {
    f = std::clamp(f, 0, 3);
    if (f == filter_) return;
    filter_ = f;
    QSettings().setValue("albumWall/filter", f);
    reload();
}

void AlbumModel::refreshRecent() {
    if (sort_ == 4 && recentDirty_) reload();
}

QString AlbumModel::summary() const {
    uint64_t ms = 0;
    size_t tracks = 0;
    for (int i : order_) {
        ms += snap_->albums[i].totalMs;
        tracks += snap_->albums[i].tracks.size();
    }
    const QString albums = order_.size() == snap_->albums.size()
                               ? tr("%1 张专辑").arg(QLocale().toString(static_cast<qulonglong>(order_.size())))
                               : tr("%1 / %2 张专辑").arg(order_.size()).arg(snap_->albums.size());
    const uint64_t hours = (ms + 1800000) / 3600000;
    const QString length = hours >= 48 ? (hours % 24 ? tr("%1 天 %2 小时").arg(hours / 24).arg(hours % 24) : tr("%1 天").arg(hours / 24))
                                       : LibraryService::formatLongDuration(ms);
    if (order_.empty()) return albums;
    return albums + QStringLiteral("  ·  ") + tr("%1 首").arg(QLocale().toString(static_cast<qulonglong>(tracks))) +
           QStringLiteral("  ·  ") + length;
}

QVariantList AlbumModel::recentlyPlayed(int n, int exclude) const {
    QVariantList out;
    std::unordered_set<int> seen;
    for (const TrackItem* t : playlists_->historyTracks()) {
        if (static_cast<int>(out.size()) >= n) break;
        auto it = snap_->trackAlbum.find(t->id);
        if (it == snap_->trackAlbum.end() || it->second == exclude || !seen.insert(it->second).second) continue;
        out.append(lib_->albumInfo(it->second));
    }
    return out;
}

QVariantList AlbumModel::recentlyAdded(int n, int exclude) const {
    std::vector<int> idx;
    for (int i = 0; i < static_cast<int>(snap_->albums.size()); ++i)
        if (i != exclude && snap_->albums[i].addedTime > 0) idx.push_back(i);
    const size_t k = std::min(idx.size(), static_cast<size_t>(std::max(0, n)));
    std::partial_sort(idx.begin(), idx.begin() + k, idx.end(),
                      [&](int x, int y) { return snap_->albums[x].addedTime > snap_->albums[y].addedTime; });
    QVariantList out;
    for (size_t i = 0; i < k; ++i) out.append(lib_->albumInfo(idx[i]));
    return out;
}

int AlbumModel::albumAt(int row) const {
    return row >= 0 && row < count() ? order_[row] : -1;
}

QString AlbumModel::coverAt(int row) const {
    if (row < 0 || row >= count()) return {};
    return data(index(row, 0), CoverRole).toString();
}

int AlbumModel::rowOfKey(const QString& key) const {
    if (key.isEmpty()) return -1;
    for (int r = 0; r < count(); ++r)
        if (data(index(r), KeyRole).toString() == key) return r;
    return -1;
}

int AlbumModel::indexOfLetter(const QString& letter) const {
    for (int r = 0; r < static_cast<int>(rowLetter_.size()); ++r)
        if (rowLetter_[r] == letter) return r;
    return -1;
}

int AlbumModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant AlbumModel::data(const QModelIndex& index, int role) const {
    const int r = index.row();
    if (r < 0 || r >= count()) return {};
    const int ai = order_[r];
    const AlbumInfo& a = snap_->albums[ai];
    switch (role) {
        case TitleRole: return a.title;
        case ArtistRole: return a.artist;
        case YearRole: return a.year;
        case CountRole: return static_cast<int>(a.tracks.size());
        case DurationRole: return LibraryService::formatDuration(a.totalMs);
        case BadgeRole: return lib_->qualityBadge(ai);
        case CoverRole: return AlbumCover(lib_->generation(), ai);
        case IndexRole: return ai;
        case KeyRole:
            return a.tracks.empty() ? a.title
                                    : a.title + QChar(0x1f) + QFileInfo(Q(a.tracks.front()->file_path)).path();
        case LetterRole: return r < static_cast<int>(rowLetter_.size()) ? rowLetter_[r] : QString();
    }
    return {};
}

QHash<int, QByteArray> AlbumModel::roleNames() const {
    return {{TitleRole, "title"},   {ArtistRole, "artist"}, {YearRole, "year"},           {CountRole, "trackCount"},
            {DurationRole, "duration"}, {BadgeRole, "badge"}, {CoverRole, "cover"},      {IndexRole, "albumIndex"},
            {KeyRole, "albumKey"},  {LetterRole, "letter"}};
}

// ============================================================================
TrackListModel::TrackListModel(LibraryService* lib, QObject* parent) : QAbstractListModel(parent), lib_(lib) {
    snap_ = lib_->snapshot();
    connect(lib_, &LibraryService::libraryChanged, this, &TrackListModel::reresolve);
}

void TrackListModel::reresolve() {
    auto fresh = lib_->snapshot();
    if (fresh == snap_) return;
    int found = -1;
    if (const AlbumInfo* a = current()) {
        // 用旧快照 (仍然有效) 里这张专辑的曲目标识,到新快照里找第一首还在的
        std::unordered_set<QString> keys;
        for (const TrackItem* t : a->tracks) keys.insert(TrackKeyOf(*t));
        for (const TrackItem& t : fresh->catalog.GetAllTracks()) {
            if (!keys.count(TrackKeyOf(t))) continue;
            auto it = fresh->trackAlbum.find(t.id);
            if (it != fresh->trackAlbum.end()) {
                found = it->second;
                break;
            }
        }
    }
    beginResetModel();
    snap_ = fresh;
    album_ = found;
    endResetModel();
    emit albumChanged();
}

const AlbumInfo* TrackListModel::current() const {
    if (album_ < 0 || album_ >= static_cast<int>(snap_->albums.size())) return nullptr;
    return &snap_->albums[album_];
}

void TrackListModel::setAlbumIndex(int i) {
    // 同一张专辑、同一份快照:不重置 (否则列表会无故跳回顶部)
    if (i == album_ && snap_ == lib_->snapshot()) return;
    beginResetModel();
    snap_ = lib_->snapshot();
    album_ = i;
    endResetModel();
    emit albumChanged();
}

int TrackListModel::rowCount(const QModelIndex& parent) const {
    const AlbumInfo* a = current();
    return parent.isValid() || !a ? 0 : static_cast<int>(a->tracks.size());
}

QVariant TrackListModel::data(const QModelIndex& index, int role) const {
    const AlbumInfo* a = current();
    if (!a || index.row() < 0 || index.row() >= static_cast<int>(a->tracks.size())) return {};
    const TrackItem& t = *a->tracks[index.row()];
    switch (role) {
        case TrackIdRole: return static_cast<double>(t.id);
        case NumberRole: return t.track_no > 0 ? t.track_no : index.row() + 1;
        case TitleRole: return Q(t.title);
        case ArtistRole: return Q(t.artist);
        case DurationRole: return LibraryService::formatDuration(t.duration_ms);
        case QualityRole: return LibraryService::trackQuality(t);
        case DiscRole: return std::max(1, t.disc_no); // 没写碟号的算第 1 张,不单独分出「0」
    }
    return {};
}

QHash<int, QByteArray> TrackListModel::roleNames() const {
    return {{TrackIdRole, "trackId"},   {NumberRole, "number"},   {TitleRole, "title"}, {ArtistRole, "artist"},
            {DurationRole, "duration"}, {QualityRole, "quality"}, {DiscRole, "disc"}};
}

QString TrackListModel::title() const { return current() ? current()->title : QString(); }
QString TrackListModel::artist() const { return current() ? current()->artist : QString(); }
QString TrackListModel::year() const { return current() ? current()->year : QString(); }
QString TrackListModel::cover() const { return current() ? AlbumCover(lib_->generation(), album_) : QString(); }

QString TrackListModel::summary() const {
    const AlbumInfo* a = current();
    if (!a) return {};
    QStringList parts;
    if (!a->year.isEmpty()) parts << a->year;
    const QString g = genre();
    if (!g.isEmpty()) parts << g;
    parts << tr("%1 首").arg(a->tracks.size());
    parts << LibraryService::formatLongDuration(a->totalMs);
    return parts.join(QStringLiteral("  ·  "));
}

// 只留一个「档次」标签 (DSD / Hi-Res / 无损),具体的位深采样率由 formatText 用文字写出,不再叠三个相似的胶囊
QStringList TrackListModel::badges() const {
    const AlbumInfo* a = current();
    if (!a) return {};
    QStringList b;
    if (a->dsd) b << a->dsdCodec;
    else if (a->hires) b << QStringLiteral("Hi-Res");
    else if (a->lossless) b << tr("无损");
    if (a->cue) b << QStringLiteral("CUE");
    return b;
}

bool TrackListModel::multiDisc() const {
    const AlbumInfo* a = current();
    if (!a || a->tracks.empty()) return false;
    const int first = std::max(1, a->tracks.front()->disc_no);
    for (const TrackItem* t : a->tracks)
        if (std::max(1, t->disc_no) != first) return true;
    return false;
}

QString TrackListModel::commonQuality() const {
    const AlbumInfo* a = current();
    if (!a || a->tracks.empty()) return {};
    const QString q = LibraryService::trackQuality(*a->tracks.front());
    for (const TrackItem* t : a->tracks)
        if (LibraryService::trackQuality(*t) != q) return {};
    return q;
}

namespace {
QString KHz(uint32_t rate) {
    return rate % 1000 ? QString::number(rate / 1000.0, 'f', 1) : QString::number(rate / 1000);
}
QString Range(const QString& lo, const QString& hi) { return lo == hi ? lo : lo + QStringLiteral("–") + hi; }
} // namespace

QString TrackListModel::formatText() const {
    const QString head = codecText(), spec = specText();
    if (head.isEmpty()) return spec;
    return spec.isEmpty() ? head : head + QStringLiteral("  ·  ") + spec;
}

QString TrackListModel::codecText() const {
    const AlbumInfo* a = current();
    if (!a) return {};
    QStringList codecs;
    for (const TrackItem* t : a->tracks) {
        const QString c = Q(t->codec.empty() ? t->format : t->codec);
        if (!c.isEmpty() && !codecs.contains(c)) codecs << c;
    }
    return codecs.size() > 2 ? tr("%1 等 %2 种格式").arg(codecs.front()).arg(codecs.size()) : codecs.join(QStringLiteral(" / "));
}

// 位深 / 采样率 (无损);DSD 为 MHz;有损为最高码率。混合时给出范围
QString TrackListModel::specText() const {
    const AlbumInfo* a = current();
    if (!a || a->tracks.empty()) return {};
    uint32_t minRate = UINT32_MAX, maxRate = 0, maxKbps = 0;
    uint16_t minBits = UINT16_MAX, maxBits = 0;
    bool allLossless = true;
    for (const TrackItem* t : a->tracks) {
        allLossless = allLossless && t->is_lossless;
        if (t->sample_rate) minRate = std::min(minRate, t->sample_rate), maxRate = std::max(maxRate, t->sample_rate);
        if (t->bits_per_sample) minBits = std::min(minBits, t->bits_per_sample), maxBits = std::max(maxBits, t->bits_per_sample);
        maxKbps = std::max(maxKbps, t->bitrate_kbps);
    }
    QString spec;
    if (a->dsd && maxRate) {
        spec = Range(QString::number(minRate / 1e6, 'f', 1), QString::number(maxRate / 1e6, 'f', 1)) + QStringLiteral(" MHz");
    } else if (allLossless && maxRate) {
        if (maxBits) spec = Range(QString::number(minBits), QString::number(maxBits)) + QStringLiteral(" bit / ");
        spec += Range(KHz(minRate), KHz(maxRate)) + QStringLiteral(" kHz");
    } else if (maxKbps) {
        spec = tr("最高 %1 kbps").arg(maxKbps);
    }
    return spec;
}

QString TrackListModel::genre() const {
    const AlbumInfo* a = current();
    if (!a) return {};
    std::map<std::wstring, int> count;
    for (const TrackItem* t : a->tracks)
        if (!t->genre.empty()) ++count[t->genre];
    const std::wstring* best = nullptr;
    int n = 0;
    for (const auto& kv : count)
        if (kv.second > n) best = &kv.first, n = kv.second;
    return best ? Q(*best) : QString();
}

QVariantMap TrackListModel::details() const {
    QVariantMap m;
    const AlbumInfo* a = current();
    if (!a || a->tracks.empty()) return m;
    // CUE 分轨共用一个文件:大小按不同的文件累计
    std::set<std::wstring> files;
    std::set<QString> folders;
    uint64_t bytes = 0;
    std::set<int> discSet;
    for (const TrackItem* t : a->tracks) {
        if (files.insert(t->file_path).second) bytes += t->file_size;
        folders.insert(QFileInfo(Q(t->file_path)).absolutePath());
        discSet.insert(std::max(1, t->disc_no));
    }
    const TrackItem* front = a->tracks.front();
    m["size"] = static_cast<double>(bytes);
    m["folder"] = QDir::toNativeSeparators(QFileInfo(Q(front->file_path)).absolutePath());
    m["folderCount"] = static_cast<int>(folders.size());
    m["firstTrackId"] = static_cast<double>(front->id);
    m["discCount"] = static_cast<int>(discSet.size());
    m["albumGain"] = front->has_rg_album ? QString::asprintf("%+.2f dB", front->rg_album_gain) : QString();
    return m;
}

QVariantList TrackListModel::discs() const {
    QVariantList out;
    const AlbumInfo* a = current();
    if (!a) return out;
    int row = 0, disc = -1, count = 0, first = 0;
    uint64_t ms = 0;
    auto flush = [&]() {
        if (disc < 0) return;
        out.append(QVariantMap{{"disc", disc}, {"count", count}, {"duration", LibraryService::formatLongDuration(ms)}, {"firstRow", first}});
    };
    for (const TrackItem* t : a->tracks) {
        const int d = std::max(1, t->disc_no);
        if (d != disc) {
            flush();
            disc = d, count = 0, ms = 0, first = row;
        }
        ++count;
        ms += t->duration_ms;
        ++row;
    }
    flush();
    return out;
}

QVariantList TrackListModel::trackIds() const {
    QVariantList out;
    const AlbumInfo* a = current();
    if (!a) return out;
    out.reserve(static_cast<int>(a->tracks.size()));
    for (const TrackItem* t : a->tracks) out.append(static_cast<double>(t->id));
    return out;
}

// ============================================================================
namespace {
QVariant TrackRole(const LibrarySnapshot& snap, const TrackItem& t, int role, int gen, int titleRole, int artistRole,
                   int albumRole, int albumIndexRole, int durationRole, int qualityRole, int coverRole, int idRole) {
    if (role == idRole) return static_cast<double>(t.id);
    if (role == titleRole) return Q(t.title);
    if (role == artistRole) return Q(t.artist);
    if (role == albumRole) return Q(t.album);
    if (role == durationRole) return LibraryService::formatDuration(t.duration_ms);
    if (role == qualityRole) return LibraryService::trackQuality(t);
    if (role == albumIndexRole || role == coverRole) {
        auto it = snap.trackAlbum.find(t.id);
        int ai = it == snap.trackAlbum.end() ? -1 : it->second;
        if (role == albumIndexRole) return ai;
        return ai >= 0 ? AlbumCover(gen, ai) : TrackCover(gen, t.id);
    }
    return {};
}
} // namespace

// ============================================================================
ArtistModel::ArtistModel(LibraryService* lib, QObject* parent) : QAbstractListModel(parent), lib_(lib) {
    reload();
    connect(lib_, &LibraryService::libraryChanged, this, &ArtistModel::reload);
}

void ArtistModel::reload() {
    beginResetModel();
    snap_ = lib_->snapshot();
    letters_.clear();
    for (const ArtistInfo& a : snap_->artists)
        if (letters_.isEmpty() || letters_.back() != a.letter) letters_ << a.letter;
    endResetModel();
    emit lettersChanged();
}

int ArtistModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(snap_->artists.size());
}

QVariant ArtistModel::data(const QModelIndex& index, int role) const {
    int r = index.row();
    if (r < 0 || r >= rowCount()) return {};
    const ArtistInfo& a = snap_->artists[r];
    switch (role) {
        case NameRole: return a.name;
        case SummaryRole:
            return a.albums.size() > 1 ? tr("%1 张专辑 · %2 首").arg(a.albums.size()).arg(a.tracks.size())
                                       : tr("%1 首").arg(a.tracks.size());
        case CoverRole: return a.coverAlbum >= 0 ? AlbumCover(lib_->generation(), a.coverAlbum) : QString();
        case IndexRole: return r;
        case LetterRole: return a.letter;
    }
    return {};
}

QHash<int, QByteArray> ArtistModel::roleNames() const {
    return {{NameRole, "name"}, {SummaryRole, "summary"}, {CoverRole, "cover"}, {IndexRole, "artistIndex"}, {LetterRole, "letter"}};
}

int ArtistModel::indexOfLetter(const QString& letter) const {
    for (int i = 0; i < static_cast<int>(snap_->artists.size()); ++i)
        if (snap_->artists[i].letter == letter) return i;
    return -1;
}

// ============================================================================
TrackCollectionModel::TrackCollectionModel(LibraryService* lib, PlaylistService* playlists, QObject* parent)
    : QAbstractListModel(parent), lib_(lib), playlists_(playlists) {
    sortMode_ = std::clamp(QSettings().value(QStringLiteral("songs/sort"), 0).toInt(), 0, 5);
    reload();
    connect(lib_, &LibraryService::libraryChanged, this, &TrackCollectionModel::reload);
    connect(playlists_, &PlaylistService::contentChanged, this, [this](const QString& id) {
        if (source_ == QLatin1String("playlist") && id == playlist_) reload();
    });
    connect(playlists_, &PlaylistService::historyChanged, this, [this]() {
        if (source_ == QLatin1String("history")) reload();
    });
}

void TrackCollectionModel::setSource(const QString& s) {
    if (s == source_) return;
    source_ = s;
    reload();
    emit sourceChanged();
}
void TrackCollectionModel::setArtistIndex(int i) {
    if (i == artist_ && source_ == QLatin1String("artist")) return;
    artist_ = i;
    reload();
    emit sourceChanged();
}
void TrackCollectionModel::setPlaylistId(const QString& id) {
    if (id == playlist_ && source_ == QLatin1String("playlist")) return;
    playlist_ = id;
    reload();
    emit sourceChanged();
}

void TrackCollectionModel::setSortMode(int m) {
    m = std::clamp(m, 0, 5);
    if (m == sortMode_) return;
    sortMode_ = m;
    QSettings().setValue(QStringLiteral("songs/sort"), m);
    emit sortModeChanged();
    if (source_ == QLatin1String("all")) reload();
}

void TrackCollectionModel::sortAll() {
    rows_ = snap_->songs;
    if (sortMode_ == 0) {
        rowLetters_ = snap_->songLetters;
        return;
    }
    if (sortMode_ == 1 || sortMode_ == 2) {
        if (keySnap_ != snap_.get()) {
            keys_.clear();
            keySnap_ = snap_.get();
        }
        // 没有歌手的排到最后
        auto keyOf = [this](const TrackItem* t) -> const std::pair<QString, QString>& {
            auto it = keys_.find(t);
            if (it != keys_.end()) return it->second;
            const std::wstring& ar = t->artist.empty() ? t->album_artist : t->artist;
            QString a = ar.empty() ? QStringLiteral("\uffff") : SortKeyOf(ar);
            QString b = t->album.empty() ? QStringLiteral("\uffff") : SortKeyOf(t->album);
            return keys_.emplace(t, std::make_pair(std::move(a), std::move(b))).first->second;
        };
        for (const TrackItem* t : rows_) keyOf(t);
        const bool byArtist = sortMode_ == 1;
        std::stable_sort(rows_.begin(), rows_.end(), [&](const TrackItem* x, const TrackItem* y) {
            const auto& kx = keys_[x];
            const auto& ky = keys_[y];
            const QString& p1 = byArtist ? kx.first : kx.second;
            const QString& p2 = byArtist ? ky.first : ky.second;
            if (int c = QString::compare(p1, p2); c != 0) return c < 0;
            if (byArtist) { if (int c = QString::compare(kx.second, ky.second); c != 0) return c < 0; }
            if (x->disc_no != y->disc_no) return x->disc_no < y->disc_no;
            return x->track_no < y->track_no;
        });
        rowLetters_.reserve(rows_.size());
        for (const TrackItem* t : rows_) {
            const QString& k = byArtist ? keys_[t].first : keys_[t].second;
            rowLetters_.push_back(k.startsWith(QChar(0xffff)) ? QStringLiteral("#") : LetterOf(k));
        }
        return;
    }
    if (sortMode_ == 3) {
        std::stable_sort(rows_.begin(), rows_.end(), [](const TrackItem* x, const TrackItem* y) { return x->added_time > y->added_time; });
    } else if (sortMode_ == 4) {
        std::stable_sort(rows_.begin(), rows_.end(), [](const TrackItem* x, const TrackItem* y) { return x->duration_ms > y->duration_ms; });
    } else {
        // 音质:DSD → 高采样率 × 位深 → 无损 → 有损 (按码率);同档内保持标题顺序,按品质分节
        auto isDsd = [](const TrackItem* t) { return t->codec.rfind(L"DSD", 0) == 0 || t->format == L"DSF" || t->format == L"DFF"; };
        auto rank = [&](const TrackItem* t) -> uint64_t {
            if (isDsd(t)) return (3ull << 40) + t->sample_rate;
            if (t->is_lossless) return (2ull << 40) + uint64_t(t->sample_rate) * std::max<uint16_t>(t->bits_per_sample, 16);
            return (1ull << 40) + t->bitrate_kbps;
        };
        std::stable_sort(rows_.begin(), rows_.end(), [&](const TrackItem* x, const TrackItem* y) { return rank(x) > rank(y); });
        rowLetters_.reserve(rows_.size());
        for (const TrackItem* t : rows_) rowLetters_.push_back(LibraryService::trackQuality(*t));
    }
}

void TrackCollectionModel::reload() {
    beginResetModel();
    snap_ = lib_->snapshot();
    rows_.clear();
    rowLetters_.clear();
    letters_.clear();
    if (source_ == QLatin1String("all")) {
        sortAll();
        // 字母条只给按字母分节的排序 (音质分节不进字母条)
        if (sortMode_ <= 2)
            for (const QString& l : rowLetters_)
                if (letters_.isEmpty() || letters_.back() != l) letters_ << l;
    } else if (source_ == QLatin1String("artist")) {
        if (artist_ >= 0 && artist_ < static_cast<int>(snap_->artists.size())) rows_ = snap_->artists[artist_].tracks;
    } else if (source_ == QLatin1String("playlist")) {
        rows_ = playlists_->tracksOf(playlist_);
    } else if (source_ == QLatin1String("history")) {
        rows_ = playlists_->historyTracks();
    }
    endResetModel();
    emit contentChanged();
}

int TrackCollectionModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QVariant TrackCollectionModel::data(const QModelIndex& index, int role) const {
    int r = index.row();
    if (r < 0 || r >= count()) return {};
    if (role == LetterRole) return r < static_cast<int>(rowLetters_.size()) ? rowLetters_[r] : QString();
    if (role == NumberRole) return r + 1;
    return TrackRole(*snap_, *rows_[r], role, lib_->generation(), TitleRole, ArtistRole, AlbumRole, AlbumIndexRole,
                     DurationRole, QualityRole, CoverRole, TrackIdRole);
}

QHash<int, QByteArray> TrackCollectionModel::roleNames() const {
    return {{TrackIdRole, "trackId"},   {TitleRole, "title"},     {ArtistRole, "artist"}, {AlbumRole, "album"},
            {AlbumIndexRole, "albumIndex"}, {DurationRole, "duration"}, {QualityRole, "quality"}, {CoverRole, "cover"},
            {LetterRole, "letter"},     {NumberRole, "number"}};
}

QVariantList TrackCollectionModel::trackIds() const {
    QVariantList out;
    out.reserve(static_cast<int>(rows_.size()));
    for (const TrackItem* t : rows_) out.append(static_cast<double>(t->id));
    return out;
}

int TrackCollectionModel::indexOfLetter(const QString& letter) const {
    for (int i = 0; i < static_cast<int>(rowLetters_.size()); ++i)
        if (rowLetters_[i] == letter) return i;
    return -1;
}

int TrackCollectionModel::rowOfTrack(double trackId) const {
    for (int i = 0; i < count(); ++i)
        if (rows_[i]->id == static_cast<uint64_t>(trackId)) return i;
    return -1;
}

// ============================================================================
SearchModel::SearchModel(LibraryService* lib, QObject* parent) : QAbstractListModel(parent), lib_(lib) {
    snap_ = lib_->snapshot();
    connect(lib_, &LibraryService::libraryChanged, this, [this]() { run(); });
}

void SearchModel::setQuery(const QString& q) {
    if (q == query_) return;
    query_ = q;
    emit queryChanged();
    run();
}

void SearchModel::run() {
    beginResetModel();
    snap_ = lib_->snapshot();
    rows_.clear();
    albums_.clear();
    artists_.clear();
    top_.clear();
    total_ = 0;
    const QString trimmed = query_.trimmed();
    std::wstring wq = trimmed.toStdWString();
    if (!wq.empty()) {
        const int gen = lib_->generation();
        int exactArtist = -1, exactAlbum = -1;
        for (int i = 0; i < static_cast<int>(snap_->artists.size()); ++i) {
            const ArtistInfo& a = snap_->artists[i];
            if (!PinyinHelper::Matches(a.name.toStdWString(), wq)) continue;
            if (exactArtist < 0 && (a.name.compare(trimmed, Qt::CaseInsensitive) == 0 ||
                                    QString::fromStdString(PinyinHelper::GetFullPinyin(a.name.toStdWString())) ==
                                        trimmed.toLower().remove(' ')))
                exactArtist = i;
            if (artists_.size() < 12) artists_.append(lib_->artistInfo(i));
        }
        for (int i = 0; i < static_cast<int>(snap_->albums.size()); ++i) {
            const AlbumInfo& a = snap_->albums[i];
            if (!PinyinHelper::Matches(a.title.toStdWString(), wq) && !PinyinHelper::Matches(a.artist.toStdWString(), wq))
                continue;
            if (exactAlbum < 0 && a.title.compare(trimmed, Qt::CaseInsensitive) == 0) exactAlbum = i;
            if (albums_.size() < 12) albums_.append(lib_->albumInfo(i));
        }
        auto hits = snap_->catalog.Search(wq, true);
        for (const TrackItem* t : hits) {
            // 隐藏文件夹里的歌不进任何专辑,搜索里也不出现
            if (!snap_->trackAlbum.count(t->id)) continue;
            ++total_;
            if (rows_.size() < 500) rows_.push_back(t);
        }
        // 最佳匹配:精确歌手 > 精确专辑 > 第一首歌 > 第一张专辑 > 第一位歌手
        if (exactArtist >= 0) {
            top_ = lib_->artistInfo(exactArtist);
            top_["kind"] = QStringLiteral("artist");
        } else if (exactAlbum >= 0) {
            top_ = lib_->albumInfo(exactAlbum);
            top_["kind"] = QStringLiteral("album");
        } else if (!rows_.empty()) {
            const TrackItem& t = *rows_.front();
            auto it = snap_->trackAlbum.find(t.id);
            top_["kind"] = QStringLiteral("track");
            top_["trackId"] = static_cast<double>(t.id);
            top_["title"] = Q(t.title);
            top_["subtitle"] = Q(t.artist);
            top_["cover"] = it == snap_->trackAlbum.end() ? TrackCover(gen, t.id) : AlbumCover(gen, it->second);
            top_["badge"] = LibraryService::trackQuality(t);
        } else if (!albums_.isEmpty()) {
            top_ = albums_.front().toMap();
            top_["kind"] = QStringLiteral("album");
        } else if (!artists_.isEmpty()) {
            top_ = artists_.front().toMap();
            top_["kind"] = QStringLiteral("artist");
        }
    }
    endResetModel();
    emit resultsChanged();
}

int SearchModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : count(); }

QVariant SearchModel::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= count()) return {};
    return TrackRole(*snap_, *rows_[index.row()], role, lib_->generation(), TitleRole, ArtistRole, AlbumRole,
                     AlbumIndexRole, DurationRole, QualityRole, CoverRole, TrackIdRole);
}

QHash<int, QByteArray> SearchModel::roleNames() const {
    return {{TrackIdRole, "trackId"},     {TitleRole, "title"},       {ArtistRole, "artist"},
            {AlbumRole, "album"},         {AlbumIndexRole, "albumIndex"}, {DurationRole, "duration"},
            {QualityRole, "quality"},     {CoverRole, "cover"}};
}

QVariantList SearchModel::trackIds() const {
    QVariantList out;
    for (const TrackItem* t : rows_) out.append(static_cast<double>(t->id));
    return out;
}
