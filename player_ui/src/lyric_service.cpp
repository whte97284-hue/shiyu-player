#include "lyric_service.h"
#include "settings_service.h"

#include <QFileInfo>
#include <QMetaObject>
#include <QSet>
#include <algorithm>
#include <filesystem>

#include "library_service.h"
#include "lyric_batch.hpp"

namespace {

QString W(const std::wstring& s) { return QString::fromStdWString(s); }

// 同名 .lrc 是否存在。
// 这里只做精确同名匹配 —— 面板要的是"大致有没有",真正播放时走的是
// player_controller 那套完整规则(还会认「歌名.zh.lrc」、内嵌歌词、歌词库)。
bool HasLrc(const std::wstring& audioPath) {
    if (audioPath.empty()) return false;
    std::filesystem::path p(audioPath);
    p.replace_extension(L".lrc");
    std::error_code ec;
    return std::filesystem::exists(p, ec);
}

} // namespace

LyricService::LyricService(LibraryService* lib, QObject* parent) : QObject(parent), lib_(lib) {
    connect(lib_, &LibraryService::libraryChanged, this, [this]() {
        missingCache_ = -1;
        statesCache_.clear();
        emit missingCountChanged();
    });
}

LyricService::~LyricService() {
    if (cancel_) cancel_->store(true);
    if (worker_.joinable()) worker_.join();
}

QVariantList LyricService::trackStates() const {
    if (!statesCache_.isEmpty()) return statesCache_;
    QVariantList out;
    auto snap = lib_->snapshot();
    if (!snap) return out;
    out.reserve(static_cast<int>(snap->songs.size()));
    // 缺的排前面:面板默认就按这个顺序显示,用户一眼看到要补的
    for (const TrackItem* t : snap->songs) {
        if (HasLrc(t->file_path)) continue;
        QVariantMap m;
        m["id"] = static_cast<double>(t->id);
        m["title"] = W(t->title);
        m["artist"] = W(t->artist);
        m["album"] = W(t->album);
        m["duration_ms"] = static_cast<int>(t->duration_ms);
        out.append(m);
    }
    statesCache_ = out;
    return out;
}

int LyricService::missingCount() const {
    if (missingCache_ >= 0) return missingCache_;
    auto snap = lib_->snapshot();
    if (!snap) return 0;
    int n = 0;
    for (const TrackItem* t : snap->songs)
        if (!HasLrc(t->file_path)) ++n;
    missingCache_ = n;
    return n;
}

bool LyricService::hasLrc(double trackId) const {
    auto snap = lib_->snapshot();
    if (!snap) return false;
    const TrackItem* t = snap->catalog.FindById(static_cast<uint64_t>(trackId));
    return t && HasLrc(t->file_path);
}

void LyricService::fetchTracks(const QVariantList& ids) {
    if (busy_) return;
    if (SettingsService::Offline()) return;   // 全局断网：不联网找歌词
    auto snap = lib_->snapshot();
    if (!snap) return;

    QSet<double> want;
    for (const QVariant& v : ids) want.insert(v.toDouble());

    std::vector<Job> jobs;
    skip_ = 0;
    for (const TrackItem* t : snap->songs) {
        if (HasLrc(t->file_path)) {
            ++skip_;
            continue;
        }
        if (!want.isEmpty() && !want.contains(static_cast<double>(t->id))) continue;
        Job j;
        j.id = t->id;
        j.path = t->file_path;
        j.title = t->title;
        j.artist = t->artist;
        j.album = t->album;
        j.duration_ms = t->duration_ms;
        jobs.push_back(std::move(j));
    }
    if (jobs.empty()) {
        emit jobChanged();
        emit finished(0, 0);
        return;
    }
    start(std::move(jobs));
}

void LyricService::cancel() {
    if (cancel_) cancel_->store(true);
}

void LyricService::start(std::vector<Job> jobs) {
    // busy_ 是主线程在 invokeMethod 里提前置 false 的,那一刻上一个线程可能还没退出。
    // 给仍 joinable 的 std::thread 赋值会直接 std::terminate(没有任何异常信息),
    // 表现就是"点一下整个软件消失"。这里先收尸。
    if (worker_.joinable()) worker_.join();
    busy_ = true;
    done_ = 0;
    ok_ = 0;
    fail_ = 0;
    progress_ = 0;
    total_ = static_cast<int>(jobs.size());
    threadCount_ = 8;
    failures_.clear();
    current_.clear();
    elapsed_.start();
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    emit jobChanged();
    emit progressChanged();

    worker_ = std::thread([this, jobs = std::move(jobs)]() {
        std::vector<lyric_core::BatchTrack> tracks;
        tracks.reserve(jobs.size());
        for (const Job& j : jobs) {
            lyric_core::BatchTrack t;
            t.file_path = j.path;
            t.title = j.title;
            t.artist = j.artist;
            t.album = j.album;
            t.duration_ms = j.duration_ms;
            tracks.push_back(std::move(t));
        }

        lyric_core::BatchOptions opt;
        opt.threads = 8;
        opt.write_lrc = true;
        opt.overwrite = false; // 绝不覆盖用户已有的歌词
        opt.allow_fallback = true;
        opt.cancel = cancel_.get();
        opt.on_progress = [this, &tracks](size_t d, size_t n) {
            if (cancel_ && cancel_->load()) return;
            const QString name = d > 0 && d <= tracks.size() ? W(tracks[d - 1].title) : QString();
            QMetaObject::invokeMethod(
                this,
                [this, d, n, name]() {
                    done_ = static_cast<int>(d);
                    progress_ = n ? static_cast<double>(d) / static_cast<double>(n) : 0.0;
                    current_ = name;
                    emit progressChanged();
                },
                Qt::QueuedConnection);
        };

        auto res = lyric_core::LyricBatch::Run(tracks, opt);

        int ok = 0, fail = 0;
        QVariantList fails;
        for (size_t i = 0; i < res.size() && i < jobs.size(); ++i) {
            // 找到了歌词但没写进磁盘,同样算失败 —— 否则用户看到成功,曲库里却还是没有
            if (res[i].source != 0 && (!opt.write_lrc || res[i].saved)) {
                ++ok;
            } else {
                ++fail;
                if (fails.size() < 200) {
                    QVariantMap m;
                    m["title"] = W(jobs[i].title);
                    m["reason"] = QString::fromStdString(res[i].note);
                    fails.append(m);
                }
            }
        }

        QMetaObject::invokeMethod(
            this,
            [this, ok, fail, fails]() {
                busy_ = false;
                ok_ = ok;
                fail_ = fail;
                progress_ = 1.0;
                current_.clear();
                failures_ = fails;
                missingCache_ = -1;
                statesCache_.clear();
                emit jobChanged();
                emit progressChanged();
                emit missingCountChanged();
                emit finished(ok, fail);
            },
            Qt::QueuedConnection);
    });
}
