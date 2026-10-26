#include "player_controller.h"
#include "settings_service.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QPointer>
#include <QSettings>
#include <QThreadPool>
#include <QVariantMap>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>
#include <QUrlQuery>
#include <QStandardPaths>
#include <QRegularExpression>
#include <QTimer>
#include <algorithm>
#include <cmath>
#include <limits>
#include <atomic>
#include <mutex>
#include <random>
#include <unordered_set>
#include "audio_tag_parser.hpp"
#include "platform_io.hpp"
#include "lyric_manager.hpp"

#include "cover_provider.h"

namespace {
QString Q(const std::wstring& w) { return QString::fromStdWString(w); }

QString KHz(uint32_t rate) {
    if (rate == 0) return QStringLiteral("?");
    return rate % 1000 ? QString::number(rate / 1000.0, 'f', 1) + "k" : QString::number(rate / 1000) + "k";
}

QVariantMap Stage(const QString& label, const QString& detail, bool ok) {
    QVariantMap m;
    m["label"] = label;
    m["detail"] = detail;
    m["ok"] = ok;
    return m;
}
} // namespace

PlayerController::PlayerController(LibraryService* lib, QObject* parent) : QObject(parent), lib_(lib) {
    QSettings s;
    volume_ = s.value("player/volume", 0.85).toDouble();
    for (int i = 0; i < 32; ++i) spectrum_.append(0.0);
    shuffle_ = s.value("player/shuffle", false).toBool();
    exclusive_ = s.value("audio/exclusive", false).toBool();
    repeat_ = std::clamp(s.value("player/repeat", 0).toInt(), 0, 2);
    gapless_ = s.value("player/gapless", true).toBool();
    rgMode_ = std::clamp(s.value("player/replayGain", 0).toInt(), 0, 2);
    device_ = s.value("audio/device").toString();
    eqEnabled_ = s.value("audio/eqEnabled", false).toBool();
    const QVariantList eqSaved = s.value("audio/eqGains").toList();
    for (int i = 0; i < Equalizer::kBands && i < eqSaved.size(); ++i)
        eq_[i] = std::clamp(eqSaved[i].toFloat(), -Equalizer::kMaxGainDb, Equalizer::kMaxGainDb);
    // 播放期间的节拍器:窗口可见时只负责请求下一帧,真正的更新在窗口每帧的 afterAnimating 里做;
    // 窗口最小化 / 不可见时退回到由它直接驱动
    timer_.setInterval(16);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &PlayerController::onFrameTimer);
    frameClock_.start();

    sleepTimer_.setInterval(1000);
    connect(&sleepTimer_, &QTimer::timeout, this, [this]() {
        if (sleepMinutes_ > 0 && QDateTime::currentMSecsSinceEpoch() >= sleepDeadline_) {
            setSleepTimer(0);
            if (playing_) toggle();
            emit sleepFired();
            return;
        }
        emit sleepChanged();
    });

    fade_.setEasingCurve(QEasingCurve::OutCubic);
    connect(&fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
        gain_ = v.toDouble();
        applyVolume();
    });
    // 曲库首次就绪后恢复上次的播放会话
    connect(lib_, &LibraryService::libraryChanged, this, [this]() {
        if (!restored_) {
            restored_ = true;
            restoreSession();
        }
        // 每次重新扫描曲目 ID 都会重新分配:无论有没有整理 / 查重带来的路径变化,都把队列换算到新快照,
        // 否则界面 (新快照的 ID) 和播放器 (旧快照的 ID) 对不上,当前行高亮、收藏心形会指向别的歌
        applyPendingRemap();
    });
}

void PlayerController::remapPathsOnNextScan(const QHash<QString, QString>& pathMap) {
    for (auto it = pathMap.constBegin(); it != pathMap.constEnd(); ++it) pendingRemap_.insert(it.key(), it.value());
}

QString PlayerController::currentPath() const { return current_ ? QString::fromStdWString(current_->file_path) : QString(); }

void PlayerController::applyPendingRemap() {
    const QHash<QString, QString> map = pendingRemap_;
    pendingRemap_.clear();
    if (!snap_ || queue_.empty()) return;
    auto snap = lib_->snapshot();
    if (snap == snap_) return;
    std::unordered_map<QString, uint64_t> byKey;
    byKey.reserve(snap->catalog.GetAllTracks().size());
    for (const TrackItem& t : snap->catalog.GetAllTracks()) byKey.emplace(TrackKeyOf(t), t.id);
    // 旧快照里的曲目 → 新快照里的曲目 ID (路径按映射替换);找不到返回 false
    auto mapId = [&](uint64_t id, uint64_t& out) {
        const TrackItem* t = snap_->catalog.FindById(id);
        if (!t) return false;
        QString key = TrackKeyOf(*t);
        QString moved = RemapTrackKey(key, map);
        auto it = byKey.find(moved.isEmpty() ? key : moved);
        if (it == byKey.end()) return false;
        out = it->second;
        return true;
    };
    uint64_t curNew = 0;
    if (current_ && !mapId(current_->id, curNew)) {
        // 正在播放的那首在新曲库里找不到:保持原样;专辑下标已对不上新曲库,标签作废
        if (queueTag_.startsWith(QLatin1String("album:"))) {
            queueTag_.clear();
            emit queueChanged();
        }
        return;
    }
    auto remapVec = [&](std::vector<uint64_t>& v) {
        std::vector<uint64_t> out;
        out.reserve(v.size());
        for (uint64_t id : v) {
            uint64_t n = 0;
            if (mapId(id, n)) out.push_back(n);
        }
        v.swap(out);
    };
    remapVec(queue_);
    remapVec(unshuffled_);
    remapVec(context_);
    snap_ = snap;
    if (current_) {
        current_ = snap_->catalog.FindById(curNew);
        currentSnap_ = snap_;
        auto it = std::find(queue_.begin(), queue_.end(), curNew);
        queueIndex_ = it == queue_.end() ? -1 : static_cast<int>(it - queue_.begin());
    }
    nextId_ = 0;
    nextIndex_ = -1;
    if (engine_) engine_->ClearNext();
    reresolveAlbumTag();
    queueContentChanged();
    emit trackChanged();
    saveSession();
}

void PlayerController::reresolveAlbumTag() {
    if (!queueTag_.startsWith(QLatin1String("album:"))) return;
    const std::vector<uint64_t>& src = context_.empty() ? queue_ : context_;
    queueTag_.clear();
    if (!snap_ || src.empty()) return;
    auto it = snap_->trackAlbum.find(src.front());
    if (it != snap_->trackAlbum.end()) queueTag_ = QStringLiteral("album:%1").arg(it->second);
}

void PlayerController::applyVolume() {
    if (engine_) engine_->SetVolume(static_cast<float>(volume_ * gain_));
}

namespace {
QString SessionFile() { return LibraryService::dataDir() + "/session.json"; }
} // namespace

void PlayerController::saveSession() {
    if (!snap_ || queue_.empty()) return;
    QJsonArray keys;
    for (uint64_t id : queue_)
        if (const TrackItem* t = snap_->catalog.FindById(id)) keys.append(TrackKeyOf(*t));
    QJsonObject o;
    o["keys"] = keys;
    o["index"] = queueIndex_;
    o["tag"] = queueTag_;
    o["position"] = position_;
    QSaveFile f(SessionFile());
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
        f.commit();
    }
    lastSessionSave_ = QDateTime::currentMSecsSinceEpoch();
}

void PlayerController::restoreSession() {
    if (!QSettings().value("player/resume", true).toBool() || !queue_.empty()) return;
    QFile f(SessionFile());
    if (!f.open(QIODevice::ReadOnly)) return;
    QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    auto snap = lib_->snapshot();
    std::unordered_map<QString, uint64_t> byKey;
    byKey.reserve(snap->catalog.GetAllTracks().size());
    for (const TrackItem& t : snap->catalog.GetAllTracks()) byKey.emplace(TrackKeyOf(t), t.id);
    std::vector<uint64_t> ids;
    int index = o.value("index").toInt(), mapped = -1;
    const QJsonArray keys = o.value("keys").toArray();
    for (int i = 0; i < keys.size(); ++i) {
        auto it = byKey.find(keys[i].toString());
        if (it == byKey.end()) continue;
        if (i == index) mapped = static_cast<int>(ids.size());
        ids.push_back(it->second);
    }
    if (ids.empty()) return;
    snap_ = snap;
    queue_ = ids;
    unshuffled_ = ids;
    queueTag_ = o.value("tag").toString();
    reresolveAlbumTag();
    queueContentChanged();
    // 暂停在上次的位置,等用户按播放
    if (openIndex(std::max(0, mapped), false) && mapped >= 0) {
        double pos = o.value("position").toDouble();
        if (pos > 1.0 && pos < duration() - 1.0) seek(pos);
    }
}

PlayerController::~PlayerController() {
    saveSession();
    QSettings st;
    st.setValue("player/volume", volume_);
    st.setValue("player/shuffle", shuffle_);
    st.setValue("player/repeat", repeat_);
    timer_.stop();
    if (engine_) {
        engine_->on_playback_ended = nullptr;
        engine_->on_track_advanced = nullptr;
        engine_->Shutdown();
    }
}

int PlayerController::albumIndex() const {
    if (!current_ || !snap_) return -1;
    auto it = snap_->trackAlbum.find(current_->id);
    return it == snap_->trackAlbum.end() ? -1 : it->second;
}

QString PlayerController::title() const { return current_ ? Q(current_->title) : QString(); }
QString PlayerController::artist() const { return current_ ? Q(current_->artist) : QString(); }
QString PlayerController::album() const { return current_ ? Q(current_->album) : QString(); }
QString PlayerController::quality() const { return current_ ? LibraryService::trackQuality(*current_) : QString(); }
double PlayerController::duration() const { return engine_ ? engine_->GetDurationSeconds() : 0.0; }

QString PlayerController::cover() const {
    if (!current_) return {};
    return QString("image://cover/t/%1/%2").arg(lib_->generation()).arg(current_->id);
}

QString PlayerController::backdrop() const {
    if (!current_) return {};
    return QString("image://cover/b/%1/%2").arg(lib_->generation()).arg(current_->id);
}

void PlayerController::setVolume(double v) {
    v = std::clamp(v, 0.0, 1.0);
    if (qFuzzyCompare(v + 1, volume_ + 1)) return;
    volume_ = v;
    applyVolume();
    emit volumeChanged();
    rebuildSignalChain();
}

void PlayerController::setPlaying(bool p) {
    if (p == playing_) return;
    playing_ = p;
    emit playingChanged();
}

void PlayerController::playAlbum(int albumIndex, int startRow) {
    auto snap = lib_->snapshot();
    if (albumIndex < 0 || albumIndex >= static_cast<int>(snap->albums.size())) return;
    snap_ = snap;
    std::vector<uint64_t> ids;
    for (const TrackItem* t : snap_->albums[albumIndex].tracks) ids.push_back(t->id);
    setQueue(std::move(ids), startRow, QStringLiteral("album:%1").arg(albumIndex));
}

void PlayerController::setQueue(std::vector<uint64_t> ids, int start, const QString& tag) {
    if (ids.empty()) return;
    start = std::clamp(start, 0, static_cast<int>(ids.size()) - 1);
    // 点的是正在播放的同一份列表 (同一张专辑 / 歌单 / 歌手) 里的另一首:
    // 只在现有队列里跳过去,不重建队列 (随机顺序、手动加的「下一首播放」、播放列表面板的位置都保留)
    if (ids == context_ && !queue_.empty()) {
        auto it = std::find(queue_.begin(), queue_.end(), ids[start]);
        if (it != queue_.end()) {
            // 内容相同的另一份列表 (歌单恰好等于某张专辑):队列不变,来源记成这一次点的;没带来源的不覆盖
            if (!tag.isEmpty() && tag != queueTag_) {
                queueTag_ = tag;
                emit queueChanged();
            }
            openIndex(static_cast<int>(it - queue_.begin()), true);
            return;
        }
    }
    context_ = ids;
    queueTag_ = tag;
    unshuffled_ = ids;
    if (shuffle_) {
        // 点选的曲目先播,其余随机
        uint64_t first = ids[start];
        ids.erase(ids.begin() + start);
        std::shuffle(ids.begin(), ids.end(), std::mt19937(std::random_device{}()));
        ids.insert(ids.begin(), first);
        start = 0;
    }
    queue_ = std::move(ids);
    queueContentChanged();
    openIndex(start, true);
}

void PlayerController::queueContentChanged() {
    queueModel_.reset(snap_, queue_, lib_->generation());
    emit queueChanged();
    updateQueueStats(true);
    scheduleGapless(); // 队列变了,预加载的「下一首」可能也变了
}

void PlayerController::queueEdited() {
    emit queueChanged();
    updateQueueStats(true);
    scheduleGapless();
}

void PlayerController::updateQueueStats(bool recount) {
    if (recount) {
        queueAfterMs_ = 0;
        if (snap_)
            for (size_t i = static_cast<size_t>(std::max(0, queueIndex_ + 1)); i < queue_.size(); ++i)
                if (const TrackItem* t = snap_->catalog.FindById(queue_[i])) queueAfterMs_ += t->duration_ms;
    }
    const qint64 curLeft = current_ ? std::max<qint64>(0, qint64(current_->duration_ms) - qint64(position_ * 1000)) : 0;
    const qint64 minutes = (qint64(queueAfterMs_) + curLeft + 30000) / 60000;
    if (minutes == remainingShown_) return;
    remainingShown_ = minutes;
    emit queueStatsChanged();
}

QString PlayerController::queueRemaining() const {
    const qint64 curLeft = current_ ? std::max<qint64>(0, qint64(current_->duration_ms) - qint64(position_ * 1000)) : 0;
    return LibraryService::formatLongDuration(queueAfterMs_ + static_cast<uint64_t>(curLeft));
}

void PlayerController::playTracks(const QVariantList& trackIds, int start, const QString& tag) {
    snap_ = lib_->snapshot();
    std::vector<uint64_t> ids;
    ids.reserve(trackIds.size());
    for (const QVariant& v : trackIds) ids.push_back(static_cast<uint64_t>(v.toDouble()));
    setQueue(std::move(ids), start, tag);
}

QString PlayerController::pickTracks(const QVariantList& trackIds, int start, const QString& tag, bool whole, bool now) {
    std::vector<uint64_t> ids;
    ids.reserve(trackIds.size());
    for (const QVariant& v : trackIds) ids.push_back(static_cast<uint64_t>(v.toDouble()));
    return pick(std::move(ids), start, tag, whole, now);
}

QString PlayerController::pickAlbum(int albumIndex, int startRow, bool whole, bool now) {
    auto snap = lib_->snapshot();
    if (albumIndex < 0 || albumIndex >= static_cast<int>(snap->albums.size())) return {};
    std::vector<uint64_t> ids;
    for (const TrackItem* t : snap->albums[albumIndex].tracks) ids.push_back(t->id);
    return pick(std::move(ids), startRow, QStringLiteral("album:%1").arg(albumIndex), whole, now);
}

QString PlayerController::pick(std::vector<uint64_t> ids, int start, const QString& tag, bool whole, bool now) {
    if (ids.empty()) return {};
    start = std::clamp(start, 0, static_cast<int>(ids.size()) - 1);
    const QSettings s;
    const bool later = s.value("player/clickAction", 0).toInt() == 1; // 下一首播放
    const bool append = s.value("player/queueMode", 0).toInt() == 1;  // 加到队列末尾
    const bool idle = queue_.empty() || !current_ || queueIndex_ < 0 || queueIndex_ >= static_cast<int>(queue_.size());
    // 什么都没在放,或者就是默认的「立即播放 + 换成这份列表」:与原来一样
    if (now || idle || (!later && !append)) {
        snap_ = lib_->snapshot();
        setQueue(std::move(ids), start, tag);
        return QStringLiteral("play");
    }
    const uint64_t cur = queue_[queueIndex_];
    if (!later) {
        // 立即播放 + 加到队列:队列里还没有的歌按原顺序接到末尾,然后跳到点的那首;原有队列不动
        std::unordered_set<uint64_t> have(queue_.begin(), queue_.end());
        std::vector<uint64_t> add;
        for (uint64_t id : ids) if (have.insert(id).second) add.push_back(id);
        if (!add.empty()) {
            const int first = static_cast<int>(queue_.size());
            queue_.insert(queue_.end(), add.begin(), add.end());
            unshuffled_.insert(unshuffled_.end(), add.begin(), add.end());
            queueModel_.insertAt(first, add);
            queueEdited();
        }
        auto it = std::find(queue_.begin(), queue_.end(), ids[start]);
        if (it != queue_.end() && *it != cur) openIndex(static_cast<int>(it - queue_.begin()), true);
        return QStringLiteral("queued");
    }
    // 下一首播放:点的那首 (整份列表的播放键时是从它开始的整份列表) 排在正在播放的歌后面,不打断当前播放
    std::vector<uint64_t> pickIds;
    if (whole) {
        pickIds.insert(pickIds.end(), ids.begin() + start, ids.end());
        pickIds.insert(pickIds.end(), ids.begin(), ids.begin() + start);
    } else {
        pickIds.push_back(ids[start]);
    }
    pickIds.erase(std::remove(pickIds.begin(), pickIds.end(), cur), pickIds.end());
    if (pickIds.empty()) return {};
    if (append) {
        // 加到队列:插到当前歌之后,队列里原有的同一首挪过来 (不重复)
        std::unordered_set<uint64_t> moving(pickIds.begin(), pickIds.end());
        std::vector<uint64_t> rest;
        for (int i = 0; i < static_cast<int>(queue_.size()); ++i)
            if (i == queueIndex_ || !moving.count(queue_[i])) rest.push_back(queue_[i]);
        const int at = static_cast<int>(std::find(rest.begin(), rest.end(), cur) - rest.begin());
        rest.insert(rest.begin() + at + 1, pickIds.begin(), pickIds.end());
        for (uint64_t id : pickIds)
            if (std::find(unshuffled_.begin(), unshuffled_.end(), id) == unshuffled_.end()) unshuffled_.push_back(id);
        queue_ = std::move(rest);
        queueIndex_ = at;
    } else {
        // 换成这份列表:当前这首播完后接着放点的那首,再按列表顺序往下 (随机模式下其余打乱);当前这首留在队首
        std::vector<uint64_t> order;
        order.insert(order.end(), ids.begin() + start, ids.end());
        order.insert(order.end(), ids.begin(), ids.begin() + start);
        order.erase(std::remove(order.begin(), order.end(), cur), order.end());
        if (shuffle_ && order.size() > 2) std::shuffle(order.begin() + 1, order.end(), std::mt19937(std::random_device{}()));
        snap_ = lib_->snapshot();
        context_ = ids;
        queueTag_ = tag;
        unshuffled_ = ids;
        queue_.assign(1, cur);
        queue_.insert(queue_.end(), order.begin(), order.end());
        queueIndex_ = 0;
    }
    queueContentChanged();
    emit queueIndexChanged();
    return QStringLiteral("next");
}

void PlayerController::playNext(double trackId) {
    if (queue_.empty() || !current_) {
        playTracks({trackId}, 0);
        return;
    }
    uint64_t id = static_cast<uint64_t>(trackId);
    queue_.insert(queue_.begin() + queueIndex_ + 1, id);
    unshuffled_.push_back(id);
    queueModel_.insertAt(queueIndex_ + 1, {id});
    queueEdited();
}

void PlayerController::playNextTracks(const QVariantList& trackIds) {
    if (trackIds.isEmpty()) return;
    if (queue_.empty() || !current_) {
        playTracks(trackIds, 0);
        return;
    }
    std::vector<uint64_t> ids;
    ids.reserve(trackIds.size());
    for (const QVariant& v : trackIds) ids.push_back(static_cast<uint64_t>(v.toDouble()));
    queue_.insert(queue_.begin() + queueIndex_ + 1, ids.begin(), ids.end());
    unshuffled_.insert(unshuffled_.end(), ids.begin(), ids.end());
    queueModel_.insertAt(queueIndex_ + 1, ids);
    queueEdited();
}

void PlayerController::enqueue(const QVariantList& trackIds) {
    if (queue_.empty() || !current_) {
        playTracks(trackIds, 0);
        return;
    }
    const size_t first = queue_.size();
    for (const QVariant& v : trackIds) {
        queue_.push_back(static_cast<uint64_t>(v.toDouble()));
        unshuffled_.push_back(queue_.back());
    }
    queueModel_.insertAt(static_cast<int>(first), std::vector<uint64_t>(queue_.begin() + first, queue_.end()));
    queueEdited();
}

void PlayerController::removeFromQueue(int index) {
    if (index < 0 || index >= static_cast<int>(queue_.size()) || index == queueIndex_) return;
    uint64_t id = queue_[index];
    queue_.erase(queue_.begin() + index);
    auto it = std::find(unshuffled_.begin(), unshuffled_.end(), id);
    if (it != unshuffled_.end()) unshuffled_.erase(it);
    if (index < queueIndex_) --queueIndex_;
    queueModel_.removeAt(index);
    queueEdited();
    emit queueIndexChanged();
}

void PlayerController::moveInQueue(int from, int to) {
    const int n = static_cast<int>(queue_.size());
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    const uint64_t id = queue_[from];
    queue_.erase(queue_.begin() + from);
    queue_.insert(queue_.begin() + to, id);
    // 正在播放的那首:被拖动时跟着走;别的歌跨过它时前后挪一位
    const int old = queueIndex_;
    if (queueIndex_ == from) queueIndex_ = to;
    else if (from < queueIndex_ && to >= queueIndex_) --queueIndex_;
    else if (from > queueIndex_ && to <= queueIndex_) ++queueIndex_;
    queueModel_.moveRow(from, to);
    queueEdited();
    if (queueIndex_ != old) emit queueIndexChanged();
}

void PlayerController::clearQueue() {
    if (queue_.size() <= 1) return;
    lastCleared_ = {snap_, queue_, unshuffled_, context_, queueTag_};
    queueTag_.clear();
    if (queueIndex_ < 0 || queueIndex_ >= static_cast<int>(queue_.size())) {
        queue_.clear();
        unshuffled_.clear();
        queueIndex_ = -1;
    } else {
        const uint64_t cur = queue_[queueIndex_];
        context_.clear();
        queue_.assign(1, cur);
        unshuffled_.clear();
        queueIndex_ = 0;
    }
    queueContentChanged();
    emit queueIndexChanged();
}

bool PlayerController::undoClearQueue() {
    ClearedQueue c = std::move(lastCleared_);
    lastCleared_ = {};
    if (c.queue.empty() || c.snap != snap_ || !current_) return false;
    // 清空后又换了歌:接着从那首在原队列里的位置播放
    auto it = std::find(c.queue.begin(), c.queue.end(), current_->id);
    if (it == c.queue.end()) return false;
    queueIndex_ = static_cast<int>(it - c.queue.begin());
    queue_ = std::move(c.queue);
    unshuffled_ = std::move(c.unshuffled);
    context_ = std::move(c.context);
    queueTag_ = c.tag;
    queueContentChanged();
    emit queueIndexChanged();
    return true;
}

void PlayerController::setShuffle(bool on) {
    if (on == shuffle_) return;
    shuffle_ = on;
    if (!queue_.empty() && current_) {
        uint64_t cur = queue_[queueIndex_];
        if (on) {
            // 当前曲目之后的部分打乱,已播放的保持不动
            unshuffled_ = queue_;
            std::shuffle(queue_.begin() + queueIndex_ + 1, queue_.end(), std::mt19937(std::random_device{}()));
        } else if (!unshuffled_.empty()) {
            queue_ = unshuffled_;
            auto it = std::find(queue_.begin(), queue_.end(), cur);
            queueIndex_ = it == queue_.end() ? 0 : static_cast<int>(it - queue_.begin());
        }
        queueContentChanged();
        emit queueIndexChanged();
    }
    emit modeChanged();
}

void PlayerController::setExclusive(bool on) {
    if (on == exclusive_) return;
    exclusive_ = on;
    QSettings().setValue("audio/exclusive", on);
    emit modeChanged();
    rebuildOutput();
}

void PlayerController::setOutputDevice(const QString& name) {
    if (name == device_) return;
    device_ = name;
    QSettings().setValue("audio/device", name);
    emit modeChanged();
    rebuildOutput();
}

void PlayerController::refreshDevices() {
    QStringList list;
    for (const auto& d : AudioPlaybackEngine::EnumerateOutputDevices()) list << QString::fromStdString(d.name);
    if (list == devices_) return;
    devices_ = list;
    emit devicesChanged();
}

void PlayerController::setGapless(bool on) {
    if (on == gapless_) return;
    gapless_ = on;
    QSettings().setValue("player/gapless", on);
    emit modeChanged();
    if (!on && engine_) {
        engine_->ClearNext();
        nextId_ = 0;
        nextIndex_ = -1;
    }
    scheduleGapless();
}

// ---- 均衡器 ----
namespace {
struct EqPreset { const char* key; const char* name; std::array<float, Equalizer::kBands> g; };
// 31 62 125 250 500 1k 2k 4k 8k 16k
const EqPreset kEqPresets[] = {
    {"flat",       QT_TR_NOOP("原声"), {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    {"bass",       QT_TR_NOOP("低音"), {6, 5, 4, 2, 0.5, 0, 0, 0, 0, 0}},
    {"vocal",      QT_TR_NOOP("人声"), {-2, -2, -1, 0, 1.5, 3, 3, 2, 0.5, -1}},
    {"treble",     QT_TR_NOOP("高音"), {0, 0, 0, 0, 0, 0, 1.5, 3, 4.5, 5.5}},
    {"rock",       QT_TR_NOOP("摇滚"), {4, 3, 1, -1, -2, -1, 1, 3, 4, 4}},
    {"electronic", QT_TR_NOOP("电子"), {5, 4, 1.5, 0, -1.5, 0, 1, 2, 4, 5}},
    {"classical",  QT_TR_NOOP("古典"), {3, 2, 1, 0, 0, 0, 0, 1, 2, 3}},
    {"soft",       QT_TR_NOOP("柔和"), {2, 2.5, 2, 1, 0, -0.5, -1, -2, -2.5, -3}},
};
}

void PlayerController::applyEq() {
    if (!engine_) return;
    Equalizer& eq = engine_->GetEqualizer();
    for (int i = 0; i < Equalizer::kBands; ++i) eq.SetGain(i, eq_[i]);
    eq.SetEnabled(eqEnabled_);
}

void PlayerController::saveEq() {
    QSettings s;
    s.setValue("audio/eqEnabled", eqEnabled_);
    QVariantList g;
    for (float v : eq_) g << static_cast<double>(v);
    s.setValue("audio/eqGains", g);
}

void PlayerController::setEqEnabled(bool on) {
    if (on == eqEnabled_) return;
    eqEnabled_ = on;
    applyEq();
    saveEq();
    emit eqChanged();
    rebuildSignalChain();
}

void PlayerController::setEqGain(int band, double db) {
    if (band < 0 || band >= Equalizer::kBands || !std::isfinite(db)) return;
    // 0.5 dB 一档:拖动时数值干净,预设也能被认出来
    const float v = std::clamp(static_cast<float>(std::round(db * 2) / 2), -Equalizer::kMaxGainDb, Equalizer::kMaxGainDb);
    if (v == eq_[band] && eqEnabled_) return;
    eq_[band] = v;
    eqEnabled_ = true; // 动了曲线就是要用
    applyEq();
    saveEq();
    emit eqChanged();
    rebuildSignalChain();
}

void PlayerController::applyEqPreset(const QString& key) {
    for (const EqPreset& p : kEqPresets) {
        if (key != QLatin1String(p.key)) continue;
        eq_ = p.g;
        eqEnabled_ = true;
        applyEq();
        saveEq();
        emit eqChanged();
        rebuildSignalChain();
        return;
    }
}

QVariantList PlayerController::eqGains() const {
    QVariantList out;
    for (float v : eq_) out << static_cast<double>(v);
    return out;
}

QString PlayerController::eqPreset() const {
    for (const EqPreset& p : kEqPresets)
        if (p.g == eq_) return QLatin1String(p.key);
    return {};
}

QVariantList PlayerController::eqPresets() const {
    QVariantList out;
    for (const EqPreset& p : kEqPresets) {
        QVariantList g;
        for (float v : p.g) g << static_cast<double>(v);
        out << QVariantMap{{"key", QLatin1String(p.key)}, {"name", tr(p.name)}, {"gains", g}};
    }
    return out;
}

QVariantList PlayerController::eqFrequencies() const {
    QVariantList out;
    for (float f : Equalizer::kFrequencies) out << static_cast<double>(f);
    return out;
}

void PlayerController::setReplayGain(int mode) {
    mode = std::clamp(mode, 0, 2);
    if (mode == rgMode_) return;
    rgMode_ = mode;
    QSettings().setValue("player/replayGain", mode);
    emit modeChanged();
    if (engine_ && current_) engine_->SetReplayGain(gainFor(current_));
    nextId_ = 0; // 增益变了,下一首重新预加载
    scheduleGapless();
    rebuildSignalChain();
}

// ReplayGain:dB 换算为线性增益,并以峰值限制不削波;缺少所选模式的数据时退回另一种
float PlayerController::gainFor(const TrackItem* t) const {
    if (!t || rgMode_ == 0) return 1.0f;
    bool album = rgMode_ == 2 ? t->has_rg_album : !t->has_rg_track && t->has_rg_album;
    if (!album && !t->has_rg_track) return 1.0f;
    float db = album ? t->rg_album_gain : t->rg_track_gain;
    float peak = album ? t->rg_album_peak : t->rg_track_peak;
    float g = std::pow(10.0f, db / 20.0f);
    if (peak > 0.0f) g = std::min(g, 1.0f / peak);
    return std::clamp(g, 0.05f, 4.0f);
}

// 重建输出:记下位置与播放状态,关闭旧设备后在同一位置继续
void PlayerController::rebuildOutput() {
    if (!engine_) return;
    const double pos = position_;
    const bool wasPlaying = playing_;
    const int idx = queueIndex_;
    engine_->on_playback_ended = nullptr;
    engine_->on_track_advanced = nullptr;
    engine_->Shutdown();
    engine_.reset();
    if (idx >= 0 && openIndex(idx, false)) {
        if (pos > 0.5) seek(pos);
        if (wasPlaying) toggle();
    }
}

void PlayerController::ensureEngine() {
    if (engine_) return;
    engine_ = std::make_unique<AudioPlaybackEngine>();
    engine_->SetPreferredDevice(device_.toStdString());
    engine_->Init(exclusive_);
    applyEq();
    // 回调来自引擎通知线程,排队回到界面线程处理
    QPointer<PlayerController> self(this);
    engine_->on_playback_ended = [self]() {
        if (self) QMetaObject::invokeMethod(self, [self]() { if (self) self->onEnded(); }, Qt::QueuedConnection);
    };
    engine_->on_track_advanced = [self]() {
        if (self) QMetaObject::invokeMethod(self, [self]() { if (self) self->onAdvanced(); }, Qt::QueuedConnection);
    };
}

void PlayerController::scheduleGapless() {
    if (gaplessPending_) return;
    gaplessPending_ = true;
    // 合并同一轮事件里的多次队列变动,也让切歌后的界面先更新,再去打开下一首
    QTimer::singleShot(0, this, [this]() {
        gaplessPending_ = false;
        queueGapless();
    });
}

// 把「下一首」交给引擎预加载;当前曲目结束时在同一音频回调内无缝衔接
void PlayerController::queueGapless() {
    if (!engine_ || !current_ || !snap_ || queue_.empty()) return;
    int idx = -1;
    if (gapless_ && sleepMinutes_ != -1) {
        if (repeat_ == 2) idx = queueIndex_;
        else if (queueIndex_ + 1 < static_cast<int>(queue_.size())) idx = queueIndex_ + 1;
        else if (repeat_ == 1) idx = 0;
    }
    const TrackItem* t = idx >= 0 ? snap_->catalog.FindById(queue_[idx]) : nullptr;
    if (!t) {
        if (nextId_) engine_->ClearNext();
        nextId_ = 0;
        nextIndex_ = -1;
        return;
    }
    if (t->id == nextId_ && idx == nextIndex_) return;
    bool ok = t->is_cue_track
                  ? engine_->QueueNext(t->file_path, t->cue_start_ms / 1000.0, t->cue_end_ms / 1000.0, gainFor(t))
                  : engine_->QueueNext(t->file_path, 0.0, 0.0, gainFor(t));
    nextId_ = ok ? t->id : 0;
    nextIndex_ = ok ? idx : -1;
}

// 引擎已无缝切到预加载的曲目:同步界面状态,再预加载再下一首
void PlayerController::onAdvanced() {
    uint64_t id = nextId_;
    int idx = nextIndex_;
    nextId_ = 0;
    nextIndex_ = -1;
    if (!snap_ || !id) return;
    if (idx < 0 || idx >= static_cast<int>(queue_.size()) || queue_[idx] != id) {
        auto it = std::find(queue_.begin(), queue_.end(), id);
        idx = it == queue_.end() ? queueIndex_ : static_cast<int>(it - queue_.begin());
    }
    const TrackItem* t = snap_->catalog.FindById(id);
    if (!t) return;
    onTrackOpened(t, idx);
}

void PlayerController::setRepeatMode(int m) {
    m = std::clamp(m, 0, 2);
    if (m == repeat_) return;
    repeat_ = m;
    emit modeChanged();
    scheduleGapless();
}

void PlayerController::setSleepTimer(int minutes) {
    minutes = std::max(-1, minutes);
    sleepMinutes_ = minutes;
    if (minutes > 0) {
        sleepDeadline_ = QDateTime::currentMSecsSinceEpoch() + qint64(minutes) * 60000;
        sleepTimer_.start();
    } else {
        sleepDeadline_ = 0;
        sleepTimer_.stop();
    }
    emit sleepChanged();
    // 「播完本首」时不预加载下一首,让当前曲目自然结束并触发 onEnded
    scheduleGapless();
}

int PlayerController::sleepRemaining() const {
    if (sleepMinutes_ <= 0) return 0;
    return static_cast<int>(std::max<qint64>(0, (sleepDeadline_ - QDateTime::currentMSecsSinceEpoch() + 999) / 1000));
}

void PlayerController::onEnded() {
    if (sleepMinutes_ == -1) {
        // 播完本首后停止:停在下一首的开头,按播放即可继续
        setSleepTimer(0);
        if (queueIndex_ + 1 < static_cast<int>(queue_.size())) openIndex(queueIndex_ + 1, false);
        else if (engine_) engine_->Stop();
        setPlaying(false);
        emit sleepFired();
        return;
    }
    if (repeat_ == 2 && current_) {
        openIndex(queueIndex_, true);
        return;
    }
    next();
}

void PlayerController::playTrack(double trackId) {
    auto snap = lib_->snapshot();
    auto it = snap->trackAlbum.find(static_cast<uint64_t>(trackId));
    if (it == snap->trackAlbum.end()) return;
    const AlbumInfo& a = snap->albums[it->second];
    int row = 0;
    for (size_t i = 0; i < a.tracks.size(); ++i)
        if (a.tracks[i]->id == static_cast<uint64_t>(trackId)) row = static_cast<int>(i);
    playAlbum(it->second, row);
}

bool PlayerController::openIndex(int index, bool autoPlay) {
    if (!snap_ || index < 0 || index >= static_cast<int>(queue_.size())) return false;
    ensureEngine();
    // 跳过无法解码的曲目 (例如 DSD:当前解码器不支持),最多尝试整个队列一遍
    for (size_t attempt = 0; attempt < queue_.size(); ++attempt) {
        int i = static_cast<int>((index + attempt) % queue_.size());
        const TrackItem* t = snap_->catalog.FindById(queue_[i]);
        if (!t) continue;
        bool ok = t->is_cue_track
                      ? engine_->OpenFile(t->file_path, t->cue_start_ms / 1000.0, t->cue_end_ms / 1000.0)
                      : engine_->OpenFile(t->file_path);
        if (!ok) {
            error_ = tr("无法播放:%1 (%2)").arg(Q(t->title), Q(t->codec));
            emit errorChanged();
            continue;
        }
        if (!error_.isEmpty() && attempt == 0) {
            error_.clear();
            emit errorChanged();
        }
        nextId_ = 0; // OpenFile 已清掉引擎里预加载的下一首
        nextIndex_ = -1;
        engine_->SetReplayGain(gainFor(t));
        fade_.stop();
        gain_ = 1.0;
        applyVolume();
        if (autoPlay) engine_->Play();
        // 先换 current_ 再发 playingChanged:监听者 (播放记录等) 读到的是新歌,而不是上一首
        onTrackOpened(t, i);
        setPlaying(autoPlay);
        return true;
    }
    return false;
}

void PlayerController::onTrackOpened(const TrackItem* t, int index) {
    current_ = t;
    currentSnap_ = snap_;
    queueIndex_ = index;
    position_ = 0;
    timer_.start();
    emit trackChanged();
    emit queueIndexChanged();
    emit positionChanged();
    rebuildSignalChain();
    extractAccent();
    loadLyrics();
    updateQueueStats(true);
    scheduleGapless();
}

void PlayerController::toggle() {
    if (!engine_ || !current_) {
        if (!queue_.empty()) openIndex(std::max(0, queueIndex_), true);
        return;
    }
    const int fadeOut = SettingsService::FadeOutMs(), fadeIn = SettingsService::FadeInMs();
    const bool useFade = fadeOut > 0;
    if (playing_) {
        setPlaying(false);
        if (!useFade) {
            engine_->Pause();
            return;
        }
        // 淡出后再真正暂停,避免「咔」的一声截断 (时长见设置里的淡入淡出)
        fade_.stop();
        fade_.setDuration(fadeOut);
        fade_.setStartValue(gain_);
        fade_.setEndValue(0.0);
        disconnect(&fade_, &QVariantAnimation::finished, nullptr, nullptr);
        connect(&fade_, &QVariantAnimation::finished, this, [this]() {
            if (!playing_ && engine_) engine_->Pause();
        });
        fade_.start();
        saveSession();
    } else {
        fade_.stop();
        disconnect(&fade_, &QVariantAnimation::finished, nullptr, nullptr);
        if (engine_->GetState() != PlaybackState::Playing) engine_->TogglePlay();
        setPlaying(true);
        timer_.start();
        if (!useFade) {
            gain_ = 1.0;
            applyVolume();
            return;
        }
        fade_.setDuration(fadeIn);
        fade_.setStartValue(engine_->GetState() == PlaybackState::Playing && gain_ < 1.0 ? gain_ : 0.0);
        fade_.setEndValue(1.0);
        fade_.start();
    }
}

void PlayerController::next() {
    if (queue_.empty()) return;
    if (queueIndex_ + 1 < static_cast<int>(queue_.size())) {
        openIndex(queueIndex_ + 1, true);
    } else if (repeat_ == 1) {
        openIndex(0, true);
    } else {
        // 队列结束:停在最后一首的起点
        if (engine_) engine_->Stop();
        setPlaying(false);
        position_ = 0;
        emit positionChanged();
    }
}

void PlayerController::previous() {
    if (queue_.empty()) return;
    // 播放超过 3 秒时「上一首」回到本曲开头,符合主流播放器习惯
    if (position_ > 3.0 || (queueIndex_ == 0 && repeat_ != 1)) {
        seek(0);
        return;
    }
    if (queueIndex_ == 0) {
        openIndex(static_cast<int>(queue_.size()) - 1, playing_);
        return;
    }
    openIndex(queueIndex_ - 1, playing_);
}

void PlayerController::seek(double seconds) {
    if (!engine_) return;
    engine_->SeekTo(seconds);
    position_ = engine_->GetPositionSeconds();
    emit positionChanged();
    updateLyricIndex();
}

void PlayerController::attachWindow(QQuickWindow* window) {
    window_ = window;
    if (!window) return;
    connect(window, &QQuickWindow::afterAnimating, this, [this]() {
        if (!timer_.isActive()) return;
        tick();
        // 前台播放时连续出帧,跟随显示器刷新率 (vsync 节流);后台时只按 16ms 节拍器出帧
        if (window_ && QGuiApplication::applicationState() == Qt::ApplicationActive) window_->update();
    });
}

void PlayerController::onFrameTimer() {
    const bool frameDriven = window_ && window_->isExposed() && window_->visibility() != QWindow::Minimized;
    // 帧驱动时只请求下一帧;若 100ms 内一帧都没出 (渲染被阻塞),直接更新,保证进度与歌词不停
    if (frameDriven && lastTickMs_ >= 0 && frameClock_.elapsed() - lastTickMs_ < 100) {
        window_->update();
        return;
    }
    tick();
    if (frameDriven) window_->update();
}

void PlayerController::tick() {
    if (!engine_) return;
    // 距上一帧的真实间隔:所有平滑都按它换算,60 / 120 / 144Hz 下速度一致
    const qint64 nowMs = frameClock_.elapsed();
    const double dt = lastTickMs_ < 0 ? 1.0 / 60.0 : std::clamp((nowMs - lastTickMs_) / 1000.0, 0.001, 0.1);
    lastTickMs_ = nowMs;
    const double frames = dt * 60.0;
    auto follow = [frames](double k) { return 1.0 - std::pow(1.0 - k, frames); };
    double p = engine_->GetPositionSeconds();
    if (std::abs(p - position_) > 1e-4) {
        position_ = p;
        emit positionChanged();
        updateQueueStats(false);
    }
    bool isPlaying = engine_->GetState() == PlaybackState::Playing;
    if (playing_ != isPlaying && !isPlaying && engine_->GetState() == PlaybackState::Stopped) setPlaying(false);

    std::vector<float> bars = engine_->GetVisualizer().GetSpectrumBars(static_cast<float>(dt));
    double energy = 0, low = 0;
    for (size_t i = 0; i < bars.size() && i < 32; ++i) {
        spectrum_[static_cast<int>(i)] = bars[i];
        energy += bars[i];
        if (i < 8) low += bars[i];
    }
    // 低频能量 (约 30~250 Hz) 快升慢降,驱动界面的「呼吸光」
    low /= 8.0;
    // 三段律动值:低频取峰、中高频取平均,略微放大让小图标更有生气
    auto band = [&](size_t a, size_t b) {
        double m = 0;
        for (size_t i = a; i < b && i < bars.size(); ++i) m = std::max(m, double(bars[i]));
        return std::min(1.0, m * 1.15);
    };
    pulse_[0] = band(2, 8);
    pulse_[1] = band(10, 18);
    pulse_[2] = band(18, 28);
    bass_ += (low - bass_) * follow(low > bass_ ? 0.6 : 0.1);
    // 呼吸:低音包络起得稳 (约 120ms)、落得更慢 (约 400ms),避免逐帧抖动;
    // 底波让安静段落也有生命感;播放 / 暂停时整体在约 0.6s 内浮现或沉下,而不是跳变
    const double target = playing_ ? std::min(1.0, bass_ * 1.25) : 0.0;
    breathEnv_ += (target - breathEnv_) * follow(target > breathEnv_ ? 0.12 : 0.04);
    breathPresence_ += ((playing_ ? 1.0 : 0.0) - breathPresence_) * follow(0.045);
    if (playing_) breathPhase_ = std::fmod(breathPhase_ + dt / 5.2, 1.0);
    breath_ = breathPresence_ * (0.30 + 0.12 * std::sin(breathPhase_ * 6.283185307)) + breathEnv_ * 0.58;
    emit spectrumChanged();
    updateLyricIndex();
    if (playing_ && QDateTime::currentMSecsSinceEpoch() - lastSessionSave_ > 5000) saveSession();
    // 暂停且频谱已完全衰减后停止定时器,空闲时零 CPU
    if (!isPlaying && energy < 1e-3 && breathPresence_ < 1e-3 && breathEnv_ < 1e-3) timer_.stop();
}

void PlayerController::rebuildSignalChain() {
    chain_.clear();
    if (!current_ || !engine_) {
        bitPerfect_ = false;
        emit signalChainChanged();
        return;
    }
    const TrackItem& t = *current_;
    uint32_t src = engine_->GetSourceSampleRate();
    uint32_t out = engine_->GetOutputSampleRate();
    uint32_t outBits = engine_->GetOutputBitDepth();
    bool exclusive = engine_->IsExclusiveMode();
    bool lossy = !t.is_lossless;
    bool perfect = !lossy;

    QString source = Q(t.codec.empty() ? t.format : t.codec);
    if (t.is_lossless) source += QString(" %1/%2").arg(t.bits_per_sample).arg(KHz(t.sample_rate));
    else source += QString(" %1k").arg(t.bitrate_kbps);
    chain_ << Stage(source, lossy ? tr("有损压缩音源") : tr("无损音源"), !lossy);

    if (src != out && out) {
        perfect = false;
        chain_ << Stage(tr("重采样 %1→%2").arg(KHz(src), KHz(out)),
                        exclusive ? tr("输出设备独占模式不支持 %1 Hz,已用最高质量滤波转换").arg(src)
                                  : tr("共享模式下系统混音器以 %1 Hz 运行").arg(out),
                        false);
    } else {
        chain_ << Stage(tr("原生 %1").arg(KHz(out)), tr("采样率与音源一致,无重采样"), true);
    }

    if (outBits && t.bits_per_sample > outBits && t.is_lossless) {
        perfect = false;
        chain_ << Stage(tr("%1→%2 bit 抖动").arg(t.bits_per_sample).arg(outBits),
                        tr("设备只接受 %1 bit,已加 TPDF 抖动避免截断失真").arg(outBits), false);
    }
    if (volume_ < 0.999) {
        perfect = false;
        chain_ << Stage(tr("数字音量 %1%").arg(qRound(volume_ * 100)), tr("软件音量会改变样本值,100% 时可实现无损直通"), false);
    }
    bool eqOn = false;
    for (float v : eq_) eqOn = eqOn || (eqEnabled_ && v != 0.0f);
    if (eqOn) {
        perfect = false;
        chain_ << Stage(tr("均衡器"), tr("按曲线调整了各频段,关闭或设为「原声」即恢复直通"), false);
    }
    chain_ << Stage(exclusive ? tr("WASAPI 独占") : tr("共享模式"),
                    exclusive ? tr("绕过系统混音器,直接送往设备") : tr("经过 Windows 系统混音器"), exclusive);
    if (!exclusive) perfect = false;

    bitPerfect_ = perfect;
    emit signalChainChanged();
}

void PlayerController::extractAccent() {
    if (!current_) return;
    int req = ++accentRequest_;
    TrackItem copy = *current_; // 后台线程只使用副本
    copy.cover_data.clear();
    QPointer<PlayerController> self(this);
    QThreadPool::globalInstance()->start([self, req, copy]() {
        QImage img = CoverProvider::LoadTrackCover(copy, 256);
        if (img.isNull()) img = CoverProvider::Placeholder(QString::fromStdWString(copy.album), 64);
        QColor c = ExtractAccentColor(img);
        QList<QColor> pal = ExtractPalette(img);
        QMetaObject::invokeMethod(self, [self, req, c, pal, img]() {
            if (!self || req != self->accentRequest_) return; // 期间已切歌,丢弃过期结果
            self->coverImage_ = img;
            emit self->coverImageChanged();
            self->accent_ = c;
            self->palette_.clear();
            for (const QColor& p : pal) self->palette_.append(p);
            emit self->accentChanged();
        }, Qt::QueuedConnection);
    });
}

// ============================================================================
int QueueModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : static_cast<int>(rows_.size()); }

void QueueModel::reset(std::shared_ptr<const LibrarySnapshot> snap, const std::vector<uint64_t>& ids, int gen) {
    beginResetModel();
    snap_ = std::move(snap);
    gen_ = gen;
    rows_.clear();
    rows_.reserve(ids.size());
    for (uint64_t id : ids) rows_.push_back(snap_ ? snap_->catalog.FindById(id) : nullptr);
    endResetModel();
}

void QueueModel::insertAt(int row, const std::vector<uint64_t>& ids) {
    row = std::clamp(row, 0, rowCount());
    if (ids.empty()) return;
    beginInsertRows({}, row, row + static_cast<int>(ids.size()) - 1);
    std::vector<const TrackItem*> add;
    add.reserve(ids.size());
    for (uint64_t id : ids) add.push_back(snap_ ? snap_->catalog.FindById(id) : nullptr);
    rows_.insert(rows_.begin() + row, add.begin(), add.end());
    endInsertRows();
    shiftedFrom(row + static_cast<int>(ids.size()), rowCount() - 1);
}

// 行号就是队列下标 (queueIndex 角色):插入 / 删除 / 挪动之后,后面已建好的代理要拿到新下标
void QueueModel::shiftedFrom(int first, int last) {
    if (first <= last) emit dataChanged(index(first), index(last), {QueueIndexRole});
}

void QueueModel::removeAt(int row) {
    if (row < 0 || row >= rowCount()) return;
    beginRemoveRows({}, row, row);
    rows_.erase(rows_.begin() + row);
    endRemoveRows();
    shiftedFrom(row, rowCount() - 1);
}

void QueueModel::moveRow(int from, int to) {
    const int n = rowCount();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
    // beginMoveRows 的目标是「插到哪一行之前」(按移动前的行号):往后挪要再加一
    beginMoveRows({}, from, from, {}, to > from ? to + 1 : to);
    const TrackItem* t = rows_[from];
    rows_.erase(rows_.begin() + from);
    rows_.insert(rows_.begin() + to, t);
    endMoveRows();
    shiftedFrom(std::min(from, to), std::max(from, to));
}

QVariant QueueModel::data(const QModelIndex& index, int role) const {
    if (index.row() < 0 || index.row() >= rowCount()) return {};
    if (!rows_[index.row()]) return role == TrackIdRole ? QVariant(-1.0) : role == QueueIndexRole ? QVariant(index.row()) : QVariant(QString());
    const TrackItem& t = *rows_[index.row()];
    switch (role) {
        case TrackIdRole: return static_cast<double>(t.id);
        case TitleRole: return Q(t.title);
        case ArtistRole: return Q(t.artist);
        case DurationRole: return LibraryService::formatDuration(t.duration_ms);
        case QueueIndexRole: return index.row();
        case CoverRole: {
            auto it = snap_->trackAlbum.find(t.id);
            return it == snap_->trackAlbum.end() ? QString() : QString("image://cover/a/%1/%2").arg(gen_).arg(it->second);
        }
    }
    return {};
}

QHash<int, QByteArray> QueueModel::roleNames() const {
    return {{TrackIdRole, "trackId"}, {TitleRole, "title"}, {ArtistRole, "artist"},
            {DurationRole, "duration"}, {CoverRole, "cover"}, {QueueIndexRole, "queueIndex"}};
}

void PlayerController::playQueueIndex(int index) { openIndex(index, true); }

QString PlayerController::chainSummary() const {
    QStringList parts;
    for (const QVariant& v : chain_) parts << v.toMap().value("label").toString();
    return parts.join(QStringLiteral("  ·  "));
}

// 同名歌词文件:与整理 / 查重同一规则,「歌名.lrc」之外也认「歌名.zh.lrc」这类 (主文件名后紧跟一个点)。
// 找的位置依次是:音频所在目录;解密产物的加密源所在目录 (按源文件名);网易云把加密歌曲与歌词一起放在
// 下载目录的 VipSongsDownload 子目录里,用别的工具解密到上一级时歌词留在那里,也去那里找
static QString FindLyricFile(const QString& audioPath, const QString& sourcePath) {
    auto lookIn = [](const QString& dir, const QString& stem) -> QString {
        if (dir.isEmpty() || stem.isEmpty()) return {};
        const QString exact = dir + "/" + stem + ".lrc";
        if (QFileInfo::exists(exact)) return exact;
        const QString upper = dir + "/" + stem + ".LRC";
        if (QFileInfo::exists(upper)) return upper;
        const QString low = stem.toLower() + ".";
        QString best;
        QDirIterator it(dir, {QStringLiteral("*.lrc"), QStringLiteral("*.LRC")}, QDir::Files);
        while (it.hasNext()) {
            const QString f = it.next();
            const QString name = it.fileName().toLower();
            if (name.size() > low.size() + 3 && name.startsWith(low) && name.endsWith(QStringLiteral(".lrc"))) {
                if (best.isEmpty() || f.size() < best.size()) best = f;
            }
        }
        return best;
    };
    const QFileInfo a(audioPath);
    QString hit = lookIn(a.path(), a.completeBaseName());
    if (hit.isEmpty() && !sourcePath.isEmpty()) {
        const QFileInfo s(sourcePath);
        hit = lookIn(s.path(), s.completeBaseName());
    }
    if (hit.isEmpty()) hit = lookIn(a.path() + QStringLiteral("/VipSongsDownload"), a.completeBaseName());
    return hit;
}

namespace {
QString LyricSourceName(const std::string& source) {
    if (source == "kugou") return QStringLiteral("酷狗");
    if (source == "netease") return QStringLiteral("网易云");
    if (source == "lrclib") return QStringLiteral("LRCLIB");
    return QString::fromStdString(source);
}

// 截图 / 演示用的假歌词接口 (KP_TEST_LYRIC_FAKE=1):按查询的歌名造几条 LRCLIB / 网易云候选,
// 走的是真实的搜索、排名、拉取、解析流程,只有 HTTP 是假的
bool FakeLyricHttp(const std::string& url, std::string& body) {
    const QUrl u(QString::fromStdString(url));
    const QUrlQuery q(u);
    auto json = [](const QString& s) { return QString::fromUtf8(QJsonDocument(QJsonArray{s}).toJson(QJsonDocument::Compact)).mid(1).chopped(1); };
    QString title = q.queryItemValue(QStringLiteral("track_name"), QUrl::FullyDecoded);
    QString artist = q.queryItemValue(QStringLiteral("artist_name"), QUrl::FullyDecoded);
    if (title.isEmpty()) title = q.queryItemValue(QStringLiteral("q"), QUrl::FullyDecoded);
    if (title.isEmpty()) title = q.queryItemValue(QStringLiteral("s"), QUrl::FullyDecoded);
    if (artist.isEmpty()) artist = QStringLiteral("群星");
    const QString lrc = QStringLiteral("[00:00.50]%1\n[00:04.00]风吹过窗前的那一页\n[00:08.00]我把旧时光折成纸飞机\n"
                                       "[00:12.00]落在你经过的街角\n[00:16.00]等一场不会迟到的雨\n[00:20.00]%1").arg(title);
    if (u.host() == QLatin1String("lrclib.net") && u.path() == QLatin1String("/api/search")) {
        body = QStringLiteral("[{\"id\":101,\"trackName\":%1,\"artistName\":%2,\"albumName\":\"Single\",\"duration\":241,\"syncedLyrics\":%3},"
                              "{\"id\":102,\"trackName\":%4,\"artistName\":%2,\"albumName\":\"Live 2019\",\"duration\":262,\"syncedLyrics\":%3},"
                              "{\"id\":103,\"trackName\":%1,\"artistName\":\"Cover\",\"albumName\":\"\",\"duration\":238,\"plainLyrics\":\"x\"}]")
                   .arg(json(title), json(artist), json(lrc), json(title + QStringLiteral(" (Live)"))).toStdString();
        return true;
    }
    if (u.host() == QLatin1String("lrclib.net") && u.path().startsWith(QLatin1String("/api/get/"))) {
        body = QStringLiteral("{\"syncedLyrics\":%1}").arg(json(lrc)).toStdString();
        return true;
    }
    if (u.host() == QLatin1String("music.163.com") && u.path().contains(QLatin1String("search"))) {
        const QString name = title.section(QLatin1Char(' '), 0, 0);
        body = QStringLiteral("{\"result\":{\"songs\":[{\"id\":7001,\"name\":%1,\"artists\":[{\"name\":%2}],\"album\":{\"name\":\"精选集\"},\"duration\":240500}]}}")
                   .arg(json(name), json(artist)).toStdString();
        return true;
    }
    if (u.host() == QLatin1String("music.163.com") && u.path().contains(QLatin1String("lyric"))) {
        body = QStringLiteral("{\"lrc\":{\"lyric\":%1},\"tlyric\":{\"lyric\":%2}}")
                   .arg(json(lrc), json(QStringLiteral("[00:04.00]The wind blew past the page by the window"))).toStdString();
        return true;
    }
    return false;
}

lyric_core::HttpGetFn LyricHttp() {
    static const bool fake = qEnvironmentVariableIsSet("KP_TEST_LYRIC_FAKE");
    return fake ? lyric_core::HttpGetFn(&FakeLyricHttp) : lyric_core::HttpGetFn();
}

// 结构化歌词 → applyLyricsText 的参数:显示用 LRC (不含翻译)、保存用 LRC (含翻译)、每句时长与翻译 (键为 10ms 精度的时间戳)。
// 没有时间轴的纯文本按行拼接,显示为静态歌词
using LyricPayload = PreparedLyrics;
LyricPayload ToPayload(const lyric_core::ParsedLyrics& parsed) {
    LyricPayload p;
    bool timed = false;
    for (const auto& l : parsed.lines) timed = timed || l.start_ms > 0;
    if (!timed) {
        QStringList rows;
        for (const auto& l : parsed.lines) rows << QString::fromStdWString(l.full_text);
        p.lrc = p.saved = rows.join(QLatin1Char('\n'));
        return p;
    }
    p.lrc = QString::fromStdString(parsed.ToStandardLrc());
    p.saved = QString::fromStdString(parsed.ToStandardLrc(true));
    for (const auto& l : parsed.lines) {
        if (l.duration_ms) p.durations.insert(l.start_ms / 10 * 10, l.duration_ms);
        if (!l.translation.empty()) p.translations.insert(l.start_ms / 10 * 10, QString::fromStdWString(l.translation));
        // 逐字时间轴:酷狗 KRC 才有,普通 LRC 的 syllables 是空的
        if (!l.syllables.empty()) {
            QVariantList wl;
            wl.reserve(static_cast<int>(l.syllables.size()));
            for (const auto& s : l.syllables) {
                QVariantMap w;
                w[QStringLiteral("t")] = QString::fromStdWString(s.text);
                w[QStringLiteral("s")] = static_cast<int>(s.start_ms);
                w[QStringLiteral("d")] = static_cast<int>(s.duration_ms);
                wl.append(w);
            }
            p.words.insert(l.start_ms / 10 * 10, wl);
        }
    }
    return p;
}
// 歌词内核单例:首次使用时探测本机酷狗歌词库 (要扫磁盘,只在后台线程里建)、设好在线缓存目录,之后常驻;
// 内部自带锁,可跨线程复用。界面线程只用 ReadyLyrics():还没建好就返回空,不在界面线程里等
std::atomic<lyric_core::LyricManager*> g_lyrics{nullptr};
lyric_core::LyricManager& SharedLyrics() {
    static lyric_core::LyricManager mgr;
    static std::once_flag once;
    std::call_once(once, [] {
        mgr.SetCacheDirectory(
            (QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/lyrics")).toStdWString());
        if (auto http = LyricHttp()) mgr.SetHttpGetter(http);
        g_lyrics.store(&mgr);
    });
    return mgr;
}
lyric_core::LyricManager* ReadyLyrics() { return g_lyrics.load(); }

// 本地歌词文本 (同名 .lrc / 内嵌) -> 结构化歌词,用来判断要不要补翻译、和在线版比对时间轴。没有时间轴的纯文本按行切
lyric_core::ParsedLyrics ParseLocalText(const QString& text) {
    lyric_core::ParsedLyrics l;
    if (lyric_core::KrcDecoder::ParseLrcText(text.toStdString(), l) && !l.lines.empty()) return l;
    l = lyric_core::ParsedLyrics();
    for (const QString& row : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")))) {
        const QString t = row.trimmed();
        if (t.isEmpty() || t.startsWith(QLatin1Char('{')) || t.startsWith(QLatin1Char('['))) continue;
        l.lines.emplace_back();
        l.lines.back().full_text = t.toStdWString();
    }
    return l;
}

// 候选标签:外语歌词带中文翻译为 1,外语没翻译为 0,中文歌等不适用为 -1
int TranslationTag(const lyric_core::ParsedLyrics& l) {
    if (!lyric_core::LyricManager::IsForeignLyrics(l)) return -1;
    return lyric_core::LyricManager::NeedsTranslation(l) ? 0 : 1;
}

// 「用本地歌词」点过的曲目 (按文件路径),不再自动换成在线版;最多记 500 首
const char* const kKeepLocalKey = "lyrics/keepLocal";
bool KeepLocal(const QString& path) { return QSettings().value(kKeepLocalKey).toStringList().contains(path); }
void SetKeepLocal(const QString& path) {
    QSettings st;
    QStringList list = st.value(kKeepLocalKey).toStringList();
    list.removeAll(path);
    list.append(path);
    while (list.size() > 500) list.removeFirst();
    st.setValue(kKeepLocalKey, list);
}

// 搜索代数的跨线程镜像:后台预取看到搜索已作废就停
std::atomic<quint64> g_searchGen{0};
} // namespace

void PlayerController::setLyricSource(const QString& kind, const QString& label, const QString& detail, const QString& path) {
    QVariantMap m;
    m[QStringLiteral("kind")] = kind;
    m[QStringLiteral("label")] = label;
    m[QStringLiteral("detail")] = detail;
    m[QStringLiteral("path")] = path;
    m[QStringLiteral("savable")] = kind != QLatin1String("file") && kind != QLatin1String("none") && !lyricText_.trimmed().isEmpty() &&
                                   current_ && !current_->is_cue_track;
    // 自动换成了带翻译的在线版:来源面板里给出「用本地歌词」
    m[QStringLiteral("local")] = kind == QLatin1String("online") && !lyricLocalText_.isEmpty()
        ? lyricLocalSource_.value(QStringLiteral("label")).toString() : QString();
    if (m == lyricSource_) return;
    lyricSource_ = m;
    emit lyricSourceChanged();
}

// 歌词来源:同名 .lrc (自动识别 UTF-8 / UTF-16 / GBK) 与文件内嵌歌词 (USLT / LYRICS / ©lyr),优先级可在设置中调整;
// 两者都没有时,在后台到本机酷狗歌词库 (.krc) 里按歌名 / 歌手严格匹配
void PlayerController::loadLyrics() {
    ++lyricsGen_;
    // 换歌 / 重新加载:上一首的搜索结果作废
    ++lyricSearchGen_;
    lyricCandRaw_.clear();
    lyricCandidates_.clear();
    lyricSearching_ = false;
    lyricPicked_ = -1;
    lyricSearchNote_.clear();
    lyricPrefetch_.clear();
    lyricCandidateTags_.clear();
    lyricLocalText_.clear();
    lyricLocalSource_.clear();
    g_searchGen.store(lyricSearchGen_);
    emit lyricSearchChanged();
    emit lyricCandidateTagsChanged();
    QString text;
    QString kind = QStringLiteral("none"), lrcPath;
    if (current_) {
        auto readLrc = [this, &lrcPath]() -> QString {
            const QString path = FindLyricFile(Q(current_->file_path), Q(current_->source_path));
            lrcPath = path;
            platform::ReadOnlyFile f;
            if (!path.isEmpty() && f.Open(path.toStdWString(), false) && f.Size() > 0 && f.Size() < (1 << 20)) {
                QByteArray raw(static_cast<int>(f.Size()), Qt::Uninitialized);
                f.ReadAt(0, raw.data(), static_cast<size_t>(raw.size()));
                return QString::fromStdWString(AudioTagParser::DecodeTextAuto(raw.toStdString()));
            }
            return {};
        };
        auto readEmbedded = [this]() -> QString {
            AudioMetadata m;
            ParseOptions o;
            o.load_lyrics = true;
            o.load_cover = false;
            AudioTagParser::ParseFile(current_->file_path, m, o);
            return QString::fromStdWString(m.lyrics);
        };
        const bool embeddedFirst = QSettings().value("player/lyricsSource", 0).toInt() == 1;
        // 只有 JSON 署名行 (网易云纯 JSON 歌词:只有作词 / 作曲,没有一句正文) 不算有歌词,继续找别的来源
        auto hasBody = [](const QString& t) {
            for (const QString& l : t.split(QRegularExpression(QStringLiteral("[\\r\\n]+")))) {
                const QString b = l.trimmed();
                if (!b.isEmpty() && !b.startsWith(QLatin1Char('{'))) return true;
            }
            return false;
        };
        const QString first = embeddedFirst ? readEmbedded() : readLrc();
        text = first;
        bool firstWon = hasBody(text);
        if (!firstWon) text = embeddedFirst ? readLrc() : readEmbedded();
        if (hasBody(text)) kind = firstWon == embeddedFirst ? QStringLiteral("embedded") : QStringLiteral("file");
        else text = hasBody(first) ? first : QString();
        // 两处都只有署名或为空:去酷狗歌词库 / 在线找;找不到时先显示署名行
        if (!hasBody(text)) { if (text.isEmpty()) text = first; loadKugouLyrics(); }
    }
    lyricText_ = kind == QLatin1String("none") ? QString() : text;
    if (kind == QLatin1String("file")) setLyricSource(kind, QStringLiteral("同名歌词文件"), QFileInfo(lrcPath).fileName(), lrcPath);
    else if (kind == QLatin1String("embedded")) setLyricSource(kind, QStringLiteral("内嵌歌词"), QStringLiteral("写在音频文件的标签里"));
    else setLyricSource(kind, QString(), QString());
    applyLyricsText(text, true);
    if (kind == QLatin1String("file") || kind == QLatin1String("embedded")) loadTranslatedLyrics(text);
}

// 外语歌的本地歌词 (网易云 / 别的工具下的同名 .lrc、内嵌歌词) 没有中文翻译时,换成带翻译的在线版本:
//  · 本地版先照常显示,不等网络;翻译缓存里已有的在这里直接换上 (只读几个小文件,不联网,不会先闪一下本地版)
//  · 缓存没有就到后台多源查找,只采用时间轴与本地对得上的版本,找不到就一直用本地 (确认没有的 30 天内不再查)
//  · 「在线查找」或「补翻译」关掉、CUE 分轨、点过「用本地歌词」的曲目都不换
void PlayerController::loadTranslatedLyrics(const QString& localText) {
    if (!current_ || current_->is_cue_track) return;
    QSettings settings;
    if (!settings.value("player/onlineLyrics", true).toBool() || !settings.value("player/lyricsTranslate", true).toBool()) return;
    const QString filePath = Q(current_->file_path);
    if (KeepLocal(filePath)) return;
    const lyric_core::ParsedLyrics local = ParseLocalText(localText);
    if (!lyric_core::LyricManager::NeedsTranslation(local)) return;

    lyricLocalText_ = lyricText_;
    lyricLocalSource_ = lyricSource_;
    const std::wstring path = current_->file_path, title = current_->title, artist = current_->artist;
    const uint32_t durationMs = current_->duration_ms;
    if (lyric_core::LyricManager* mgr = ReadyLyrics()) {
        lyric_core::ParsedLyrics parsed;
        lyric_core::LyricCandidate picked;
        if (mgr->FindTranslatedVersion(local, path, title, artist, durationMs, parsed, &picked, false)) {
            applyTranslated(ToPayload(parsed), LyricSourceName(picked.source));
            return;
        }
    }
    const quint64 gen = lyricsGen_;
    QPointer<PlayerController> self(this);
    QThreadPool::globalInstance()->start([self, gen, local, path, title, artist, durationMs]() {
        lyric_core::ParsedLyrics parsed;
        lyric_core::LyricCandidate picked;
        if (!SharedLyrics().FindTranslatedVersion(local, path, title, artist, durationMs, parsed, &picked)) return;
        const LyricPayload p = ToPayload(parsed);
        const QString name = LyricSourceName(picked.source);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, gen, p, name]() {
            // 期间切了歌、重新加载过或手动换过歌词:作废
            if (self && self->lyricsGen_ == gen) self->applyTranslated(p, name);
        }, Qt::QueuedConnection);
    });
}

void PlayerController::applyTranslated(const PreparedLyrics& p, const QString& sourceName) {
    lyricText_ = p.saved;
    setLyricSource(QStringLiteral("online"), QStringLiteral("在线 · ") + sourceName,
                   QStringLiteral("已补上中文翻译"));
    applyLyricsText(p.lrc, false, p.durations, p.translations, p.words);
    updateLyricIndex();
}

void PlayerController::useLocalLyrics() {
    if (!current_ || lyricLocalText_.isEmpty()) return;
    ++lyricsGen_; // 还在路上的后台结果作废
    SetKeepLocal(Q(current_->file_path));
    const QString text = lyricLocalText_;
    const QVariantMap src = lyricLocalSource_;
    lyricLocalText_.clear();
    lyricLocalSource_.clear();
    lyricText_ = text;
    setLyricSource(src.value(QStringLiteral("kind")).toString(), src.value(QStringLiteral("label")).toString(),
                   src.value(QStringLiteral("detail")).toString(), src.value(QStringLiteral("path")).toString());
    lyricPicked_ = -1;
    emit lyricSearchChanged();
    applyLyricsText(text, true);
    updateLyricIndex();
}

// 酷狗歌词:先查本地酷狗歌词库 (.krc),没有再请求酷狗在线接口 (设置 player/onlineLyrics,默认开),
// 在线拉到的 KRC 缓存到应用数据目录 lyrics/,下次直接从本地命中
void PlayerController::loadKugouLyrics() {
    const quint64 gen = lyricsGen_;
    const std::wstring path = current_->file_path, title = current_->title, artist = current_->artist;
    const uint32_t durationMs = current_->duration_ms;
    const bool online = QSettings().value("player/onlineLyrics", true).toBool();
    const std::wstring cacheDir =
        (QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + QStringLiteral("/lyrics")).toStdWString();
    QPointer<PlayerController> self(this);
    QThreadPool::globalInstance()->start([self, gen, path, title, artist, durationMs, online, cacheDir]() {
        lyric_core::LyricManager& kugou = SharedLyrics();
        lyric_core::ParsedLyrics parsed;
        QString kind, label, detail, file;
        if (kugou.LoadLyricsForTrack(path, title, artist, parsed)) {
            // 在线拉过的歌词缓存在应用数据目录,也在歌词库索引里
            file = QString::fromStdWString(kugou.FindMatchingKrcFile(path, title, artist));
            const bool cached = !file.isEmpty() && QDir::cleanPath(file).startsWith(QDir::cleanPath(QString::fromStdWString(cacheDir)), Qt::CaseInsensitive);
            kind = cached ? QStringLiteral("online") : QStringLiteral("library");
            label = cached ? QStringLiteral("在线歌词") : QStringLiteral("酷狗歌词库");
            detail = cached ? QStringLiteral("之前在线找到,已缓存") : QFileInfo(file).fileName();
            if (cached) file.clear();
        } else if (online) {
            lyric_core::LyricCandidate picked;
            bool ok = kugou.FetchOnlineMulti(path, title, artist, durationMs, parsed, &picked);
            if (!ok && kugou.FetchOnline(path, title, artist, durationMs, parsed)) {
                ok = true;
                picked.source = "kugou";
                picked.title = parsed.title;
                picked.artist = parsed.artist;
            }
            if (!ok) return;
            kind = QStringLiteral("online");
            label = QStringLiteral("在线 · ") + LyricSourceName(picked.source);
            detail = QString::fromStdWString(picked.artist.empty() ? picked.title : picked.title + L" - " + picked.artist);
        } else {
            return;
        }
        // 酷狗歌词带每句的演唱时长,用来让当前句的填充与演唱同步;外文歌的中文翻译同样按时间戳挂到对应行
        const LyricPayload p = ToPayload(parsed);
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, gen, p, kind, label, detail, file]() {
            // 期间切了歌或重新加载过,结果作废
            if (self && self->lyricsGen_ == gen) {
                self->lyricText_ = p.saved;
                self->setLyricSource(kind, label, detail, file);
                self->applyLyricsText(p.lrc, false, p.durations, p.translations, p.words);
                self->updateLyricIndex();
            }
        }, Qt::QueuedConnection);
    });
}

// 解析 LRC 文本:支持一行多时间戳与 [offset:] 标签;没有时间戳的纯文本歌词按静态歌词显示。
// wholeFileTimeline:歌词时间轴按整个音频文件计 (CUE 分轨需换算到分轨起点);酷狗歌词按单曲计,不换算
void PlayerController::applyLyricsText(const QString& text, bool wholeFileTimeline, const QHash<qint64, qint64>& durations,
                                       const QHash<qint64, QString>& translations,
                                       const QHash<qint64, QVariantList>& words) {
    lyrics_.clear();
    lyricTimes_.clear();
    lyricIndex_ = -1;
    lyricsSynced_ = false;
    if (current_ && !text.trimmed().isEmpty()) {
        static const QRegularExpression tagRe(QStringLiteral("\\[(\\d{1,3}):(\\d{1,2})(?:[.:](\\d{1,3}))?\\]"));
        static const QRegularExpression offRe(QStringLiteral("\\[offset:\\s*([+-]?\\d+)\\]"), QRegularExpression::CaseInsensitiveOption);
        static const QRegularExpression metaRe(QStringLiteral("^\\[[a-zA-Z]+:.*\\]$"));
        qint64 offset = 0;
        auto om = offRe.match(text);
        if (om.hasMatch()) offset = om.captured(1).toLongLong();
        struct Line { qint64 time; QString text; qint64 duration; QString translation; QVariantList words; };
        std::vector<Line> lines;
        std::vector<Line> jsonLines;
        QStringList plain;
        for (const QString& line : text.split(QRegularExpression(QStringLiteral("[\\r\\n]+")))) {
            std::vector<qint64> times;
            int pos = 0;
            QRegularExpressionMatch m;
            while ((m = tagRe.match(line, pos)).hasMatch() && m.capturedStart() == pos) {
                QString frac = m.captured(3);
                qint64 ms = m.captured(1).toLongLong() * 60000 + m.captured(2).toLongLong() * 1000;
                if (!frac.isEmpty()) ms += frac.length() == 1 ? frac.toLongLong() * 100 : frac.length() == 2 ? frac.toLongLong() * 10 : frac.toLongLong();
                times.push_back(ms);
                pos = m.capturedEnd();
            }
            QString body = line.mid(pos).trimmed();
            if (body.isEmpty()) continue;
            if (times.empty()) {
                // 网易云歌词里的 JSON 行 ({"t":毫秒,"c":[{"tx":"作词: "},{"tx":"某某"}]}):取出文字按时间显示,
                // 不能当成纯文本歌词把整串 JSON 露出来
                if (body.startsWith(QLatin1Char('{'))) {
                    const QJsonObject o = QJsonDocument::fromJson(body.toUtf8()).object();
                    QString tx;
                    for (const QJsonValue& c : o.value(QStringLiteral("c")).toArray()) tx += c.toObject().value(QStringLiteral("tx")).toString();
                    tx = tx.trimmed();
                    if (!tx.isEmpty() && o.contains(QStringLiteral("t"))) jsonLines.push_back({qint64(o.value(QStringLiteral("t")).toDouble()), tx, -1, QString()});
                    continue;
                }
                if (!metaRe.match(body).hasMatch()) plain << body;
                continue;
            }
            for (qint64 t : times) lines.push_back({t - offset, body, durations.value(t, -1), translations.value(t), words.value(t)});
        }
        // JSON 行 (作词 / 作曲等) 只补在第一句正式歌词之前,时间戳与正式歌词撞上时不会被当成翻译并进去
        if (!jsonLines.empty()) {
            qint64 first = std::numeric_limits<qint64>::max();
            for (const Line& l : lines) first = std::min(first, l.time);
            for (const Line& l : jsonLines)
                if (lines.empty() || l.time < first) lines.push_back(l);
        }
        if (!lines.empty()) {
            lyricsSynced_ = true;
            std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.time < b.time; });
            // 双语 LRC:与上一句时间戳相同的第二行是翻译,并到上一句里而不是单独占一行
            std::vector<Line> merged;
            for (Line& l : lines) {
                if (!merged.empty() && merged.back().time == l.time && merged.back().translation.isEmpty() && merged.back().text != l.text) {
                    merged.back().translation = l.text;
                    continue;
                }
                merged.push_back(std::move(l));
            }
            lines.swap(merged);
            // CUE 分轨:歌词时间轴通常按整轨计,换算到分轨起点
            qint64 shift = wholeFileTimeline && current_->is_cue_track ? static_cast<qint64>(current_->cue_start_ms) : 0;
            for (size_t i = 0; i < lines.size(); ++i) {
                const Line& l = lines[i];
                if (l.time - shift < 0 && shift > 0) continue;
                lyricTimes_.push_back(l.time - shift);
                QVariantMap m;
                m["text"] = l.text;
                m["time"] = static_cast<double>(l.time - shift) / 1000.0;
                if (!l.translation.isEmpty()) m["translation"] = l.translation;
                // 本句演唱时长 (秒):酷狗歌词自带;普通 LRC 按字数估算,且不超过到下一句的间隔
                const qint64 gap = i + 1 < lines.size() ? lines[i + 1].time - l.time : 6000;
                const qint64 guess = std::max<qint64>(1200, static_cast<qint64>(l.text.size()) * 260);
                const qint64 dur = l.duration > 0 ? l.duration : guess;
                m["duration"] = static_cast<double>(std::max<qint64>(200, std::min(dur, gap))) / 1000.0;
                // 逐字时间轴(酷狗 KRC 才有):UI 用它做逐字染色,没有时退回整行按比例推进
                if (!l.words.isEmpty()) m["words"] = l.words;
                lyrics_.append(m);
            }
        } else {
            for (const QString& l : plain) {
                QVariantMap m;
                m["text"] = l;
                m["time"] = -1.0;
                lyrics_.append(m);
            }
        }
    }
    emit lyricsChanged();
    emit lyricIndexChanged();
}

QString PlayerController::lyricAt(double seconds) const {
    if (lyricTimes_.empty() || !(seconds >= 0)) return {};
    const qint64 ms = static_cast<qint64>(seconds * 1000.0);
    auto it = std::upper_bound(lyricTimes_.begin(), lyricTimes_.end(), ms);
    const int idx = static_cast<int>(it - lyricTimes_.begin()) - 1;
    if (idx < 0 || idx >= lyrics_.size()) return {};
    return lyrics_.at(idx).toMap().value(QStringLiteral("text")).toString();
}

QString PlayerController::lyricQuery() const {
    if (!current_) return {};
    const QString title = Q(current_->title).trimmed();
    const QString artist = Q(current_->artist).trimmed();
    if (title.isEmpty()) return QFileInfo(Q(current_->file_path)).completeBaseName();
    return artist.isEmpty() || artist == QStringLiteral("未知艺术家") ? title : title + QLatin1Char(' ') + artist;
}

// 歌词搜索:后台依次查各源,全部候选按匹配度排好交给歌词来源面板。
// 搜索词没改过时按歌名 / 歌手 / 时长结构化查询;改过就把整串当作关键词。两种都不丢候选,由用户挑
void PlayerController::searchLyrics(const QString& keyword) {
    if (!current_) return;
    const QString kw = keyword.simplified();
    if (kw.isEmpty()) return;
    const quint64 gen = ++lyricSearchGen_;
    lyricSearching_ = true;
    lyricSearchNote_.clear();
    emit lyricSearchChanged();

    lyric_core::LyricQuery q;
    q.loose = true;
    q.duration_ms = current_->duration_ms;
    if (kw == lyricQuery().simplified()) {
        q.title = current_->title.empty() ? kw.toStdWString() : current_->title;
        if (Q(current_->artist) != QStringLiteral("未知艺术家")) q.artist = current_->artist;
    } else {
        q.title = kw.toStdWString();
    }
    g_searchGen.store(gen);
    lyricPrefetch_.clear();
    lyricCandidateTags_.clear();
    emit lyricCandidateTagsChanged();
    QPointer<PlayerController> self(this);
    QThreadPool::globalInstance()->start([self, gen, q]() {
        lyric_core::LyricProvider provider(LyricHttp());
        lyric_core::LyricSearchResult r = provider.SearchAll(q);
        if (r.candidates.size() > 30) r.candidates.resize(30);
        // 预取前几名 (一首一个请求,依次进行):每取好一份就把「带翻译」标签和歌词交给界面
        auto prefetch = [self, gen, r, &provider]() {
            const size_t n = std::min<size_t>(r.candidates.size(), 6);
            for (size_t i = 0; i < n; ++i) {
                if (g_searchGen.load() != gen) return; // 又搜了一次 / 换了歌
                lyric_core::RawLyricDoc doc;
                lyric_core::ParsedLyrics parsed;
                if (!provider.Fetch(r.candidates[i], doc, nullptr) || !lyric_core::LyricManager::ParseDoc(doc, parsed)) continue;
                const LyricPayload p = ToPayload(parsed);
                const int tag = TranslationTag(parsed);
                const int index = static_cast<int>(i);
                QMetaObject::invokeMethod(QCoreApplication::instance(), [self, gen, index, p, tag]() {
                    if (!self || self->lyricSearchGen_ != gen) return;
                    self->lyricPrefetch_.insert(index, p);
                    while (self->lyricCandidateTags_.size() <= index) self->lyricCandidateTags_.append(-1);
                    self->lyricCandidateTags_[index] = tag;
                    emit self->lyricCandidateTagsChanged();
                }, Qt::QueuedConnection);
            }
        };
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, gen, r]() {
            if (!self || self->lyricSearchGen_ != gen) return;
            self->lyricCandRaw_ = r.candidates;
            self->lyricCandidates_.clear();
            for (const auto& c : r.candidates) {
                QVariantMap m;
                m[QStringLiteral("title")] = QString::fromStdWString(c.title);
                m[QStringLiteral("artist")] = QString::fromStdWString(c.artist);
                m[QStringLiteral("album")] = QString::fromStdWString(c.album);
                m[QStringLiteral("duration")] = c.duration_ms / 1000.0;
                m[QStringLiteral("source")] = LyricSourceName(c.source);
                m[QStringLiteral("synced")] = c.has_synced;
                m[QStringLiteral("wordTimed")] = c.source == "kugou"; // 酷狗 KRC 带逐字时间
                self->lyricCandidates_.append(m);
            }
            self->lyricSearching_ = false;
            self->lyricPicked_ = -1;
            self->lyricSearchNote_ = r.candidates.empty()
                ? (r.answered == 0 ? QStringLiteral("连不上歌词服务,检查一下网络") : QStringLiteral("没有找到,换个关键词试试"))
                : QString();
            emit self->lyricSearchChanged();
        }, Qt::QueuedConnection);
        prefetch();
    });
}

// 换用第 index 个候选:后台拉取后立即显示 (不写盘,要留住请点「保存到本地」)
void PlayerController::useLyricCandidate(int index) {
    if (!current_ || index < 0 || index >= static_cast<int>(lyricCandRaw_.size())) return;
    const lyric_core::LyricCandidate c = lyricCandRaw_[static_cast<size_t>(index)];
    const quint64 gen = ++lyricsGen_; // 自动查找的结果晚到也不会盖掉手选的
    // 从本地歌词换到在线候选:记下本地版,面板里可以「用本地歌词」换回
    const QString curKind = lyricSource_.value(QStringLiteral("kind")).toString();
    if (lyricLocalText_.isEmpty() && (curKind == QLatin1String("file") || curKind == QLatin1String("embedded"))) {
        lyricLocalText_ = lyricText_;
        lyricLocalSource_ = lyricSource_;
    }
    // 后台已经预取好了:立即换上,不用等网络
    if (lyricPrefetch_.contains(index)) {
        const LyricPayload p = lyricPrefetch_.value(index);
        lyricText_ = p.saved;
        setLyricSource(QStringLiteral("online"), QStringLiteral("在线 · ") + LyricSourceName(c.source),
                       QString::fromStdWString(c.artist.empty() ? c.title : c.title + L" - " + c.artist));
        applyLyricsText(p.lrc, false, p.durations, p.translations, p.words);
        updateLyricIndex();
        lyricPicked_ = index;
        lyricSearchNote_.clear();
        emit lyricSearchChanged();
        return;
    }
    const quint64 searchGen = lyricSearchGen_;
    QPointer<PlayerController> self(this);
    QThreadPool::globalInstance()->start([self, gen, searchGen, index, c]() {
        lyric_core::LyricProvider provider(LyricHttp());
        lyric_core::RawLyricDoc doc;
        lyric_core::ParsedLyrics parsed;
        const bool ok = provider.Fetch(c, doc, nullptr) && lyric_core::LyricManager::ParseDoc(doc, parsed);
        const LyricPayload p = ok ? ToPayload(parsed) : LyricPayload();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [self, gen, searchGen, index, c, ok, p]() {
            if (!self || self->lyricsGen_ != gen || self->lyricSearchGen_ != searchGen) return;
            if (!ok) {
                self->lyricSearchNote_ = QStringLiteral("这份歌词拉取失败,换一个试试");
                emit self->lyricSearchChanged();
                return;
            }
            self->lyricText_ = p.saved;
            self->setLyricSource(QStringLiteral("online"), QStringLiteral("在线 · ") + LyricSourceName(c.source),
                                 QString::fromStdWString(c.artist.empty() ? c.title : c.title + L" - " + c.artist));
            self->applyLyricsText(p.lrc, false, p.durations, p.translations, p.words);
            self->updateLyricIndex();
            self->lyricPicked_ = index;
            self->lyricSearchNote_.clear();
            emit self->lyricSearchChanged();
        }, Qt::QueuedConnection);
    });
}

// 把当前显示的歌词存成歌曲旁边的同名 .lrc (UTF-8 带 BOM),之后这首歌总是先用它。
// 已有内容不同的同名歌词时先改名为 .lrc.bak,不直接覆盖
bool PlayerController::saveLyrics() {
    if (!current_ || current_->is_cue_track || lyricText_.trimmed().isEmpty()) return false;
    const QFileInfo audio(Q(current_->file_path));
    const QString target = audio.path() + QLatin1Char('/') + audio.completeBaseName() + QStringLiteral(".lrc");
    const QByteArray data = QByteArrayLiteral("\xEF\xBB\xBF") + lyricText_.toUtf8();
    QFile old(target);
    if (old.exists()) {
        if (old.open(QIODevice::ReadOnly) && old.readAll() == data) {
            old.close();
        } else {
            old.close();
            QFile::remove(target + QStringLiteral(".bak"));
            if (!QFile::rename(target, target + QStringLiteral(".bak"))) return false;
        }
    }
    QSaveFile f(target);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit()) {
        lyricSearchNote_ = QStringLiteral("保存失败:歌曲所在文件夹不可写");
        emit lyricSearchChanged();
        return false;
    }
    setLyricSource(QStringLiteral("file"), QStringLiteral("同名歌词文件"), QFileInfo(target).fileName(), target);
    return true;
}

void PlayerController::updateLyricIndex() {
    if (lyricTimes_.empty()) return;
    qint64 ms = static_cast<qint64>(position_ * 1000.0) + 120; // 略微提前,视觉上与演唱同步
    auto it = std::upper_bound(lyricTimes_.begin(), lyricTimes_.end(), ms);
    int idx = static_cast<int>(it - lyricTimes_.begin()) - 1;
    if (idx != lyricIndex_) {
        lyricIndex_ = idx;
        emit lyricIndexChanged();
    }
}
