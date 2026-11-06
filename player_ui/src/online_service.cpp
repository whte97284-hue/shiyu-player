#include "online_service.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QSettings>
#include <QStandardPaths>
#include <QDesktopServices>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <chrono>
#include <memory>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#endif

#include "library_service.h"
#include "player_controller.h"
#include "settings_service.h"
#include "cover_writer.hpp"

using namespace online_core;

namespace {

QString ToQ(const std::string& s) { return QString::fromStdString(s); }

// 曲目 -> QML 行。搜索结果、歌手页、专辑页、加载更多都走这一条，字段不会各写一套
QVariantMap TrackToMap(const Track& t) {
    QVariantMap x;
    x["title"] = ToQ(t.title);
    x["artist"] = ToQ(t.artist);
    x["album"] = ToQ(t.album);
    x["duration"] = ToQ(t.DurationText());
    x["paywalled"] = t.Paywalled();
    x["privilege"] = t.privilege;
    return x;
}

// 酷狗给 "2003-07-31 00:00:00"，网易给毫秒时间戳 —— 都归成年份
QString YearText(const std::string& s) {
    if (s.size() >= 4 && s[0] >= '1' && s[0] <= '9') return QString::fromStdString(s.substr(0, 4));
    if (s.empty()) return QString();
    try {
        const long long ms = std::stoll(s);
        if (ms > 0) return QString::number(QDateTime::fromMSecsSinceEpoch(ms).date().year());
    } catch (...) {
    }
    return QString();
}

#ifdef _WIN32
std::wstring U8ToW(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

// 流式下载：边收边写盘，避免把 30MB 无损整个塞进内存。
// 返回 false 时 target 已被删除，不会留下半截文件。
bool DownloadToFile(const std::string& url, const std::wstring& target,
                    const std::atomic<bool>& cancel, const std::function<void(int)>& onProgress) {
    const std::wstring wurl = U8ToW(url);
    const size_t schemeEnd = wurl.find(L"://");
    if (schemeEnd == std::wstring::npos) return false;
    const std::wstring scheme = wurl.substr(0, schemeEnd);
    const std::wstring rest = wurl.substr(schemeEnd + 3);
    const size_t slash = rest.find(L'/');
    const std::wstring hostport = (slash == std::wstring::npos) ? rest : rest.substr(0, slash);
    const std::wstring object = (slash == std::wstring::npos) ? L"/" : rest.substr(slash);
    std::wstring host = hostport;
    INTERNET_PORT port = 0;
    const size_t colon = hostport.find(L':');
    if (colon != std::wstring::npos) {
        host = hostport.substr(0, colon);
        port = static_cast<INTERNET_PORT>(_wtoi(hostport.c_str() + colon + 1));
    }
    const bool secure = (scheme == L"https");
    if (port == 0) port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;

    HINTERNET session = WinHttpOpen(L"Mozilla/5.0 KugouPlayer/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, 8000, 8000, 15000, 60000);

    bool ok = false;
    HINTERNET conn = WinHttpConnect(session, host.c_str(), port, 0);
    if (conn) {
        HINTERNET r = WinHttpOpenRequest(conn, L"GET", object.c_str(), NULL, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0);
        if (r) {
            if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                                   0, 0) &&
                WinHttpReceiveResponse(r, NULL)) {
                DWORD status = 0, len = sizeof(status);
                WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                                    WINHTTP_NO_HEADER_INDEX);
                if (status == 200) {
                    long long total = 0;
                    wchar_t clen[64] = {0};
                    DWORD clenLen = sizeof(clen);
                    if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH,
                                            WINHTTP_HEADER_NAME_BY_INDEX, clen, &clenLen,
                                            WINHTTP_NO_HEADER_INDEX))
                        total = _wtoi64(clen);

                    QFile out(QString::fromStdWString(target));
                    if (out.open(QIODevice::WriteOnly)) {
                        ok = true;
                        long long got = 0;
                        for (;;) {
                            if (cancel.load()) { ok = false; break; }
                            DWORD avail = 0;
                            if (!WinHttpQueryDataAvailable(r, &avail)) break;
                            if (avail == 0) break;
                            std::vector<char> buf(avail);
                            DWORD read = 0;
                            if (!WinHttpReadData(r, buf.data(), avail, &read)) { ok = false; break; }
                            if (read && out.write(buf.data(), read) != static_cast<qint64>(read)) {
                                ok = false;
                                break;
                            }
                            got += read;
                            if (total > 0 && onProgress)
                                onProgress(static_cast<int>(got * 100 / total));
                        }
                        out.close();
                        if (ok && onProgress) onProgress(100);
                    }
                }
            }
            WinHttpCloseHandle(r);
        }
        WinHttpCloseHandle(conn);
    }
    WinHttpCloseHandle(session);
    if (!ok) QFile::remove(QString::fromStdWString(target));
    return ok;
}
#endif

// 下载完补封面：酷狗搜索不给封面地址，用 album_id 查专辑图；
// 网易云搜索直接带 picUrl。取不到就静默跳过，不影响播放。
void ApplyCover(const QString& path, const Track& t) {
    std::string url = t.coverUrl;
    if (url.empty() && t.source == Source::Kugou) url = KugouAlbumCover(t.albumId);
    if (url.empty()) return;
    std::string bytes;
    if (!FetchBytes(url, bytes) || bytes.size() < 1024) return;
    std::string err;
    cover_core::EmbedCover(path.toStdWString(), bytes, "image/jpeg", &err);
}

// 封面落成本地文件：QML 的 Image 直接加载 http 地址时走 Qt 自己的 TLS 栈，
// 本机代理换了证书就是白图；落盘最稳，顺便给引擎取主色用。
QString WriteTempCover(const std::string& bytes) {
    if (bytes.size() < 1024) return QString();
    const bool png = static_cast<unsigned char>(bytes[0]) == 0x89;
    const QByteArray sum =
        QCryptographicHash::hash(QByteArray::fromRawData(bytes.data(), static_cast<int>(bytes.size())),
                                 QCryptographicHash::Md5)
            .toHex()
            .left(16);
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/shiyu-online-cache";
    QDir().mkpath(dir);
    const QString path = dir + "/cover_" + QString::fromLatin1(sum) + (png ? ".png" : ".jpg");
    if (QFileInfo::exists(path) && QFileInfo(path).size() == static_cast<qint64>(bytes.size())) return path;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write(bytes.data(), static_cast<qint64>(bytes.size()));
    f.close();
    return path;
}

QString SanitizeFileName(QString s) {
    static const QString bad = "\\/:*?\"<>|";
    for (QChar c : bad) s.replace(c, '_');
    s = s.trimmed();
    if (s.isEmpty()) s = "unknown";
    if (s.size() > 120) s = s.left(120);
    return s;
}

} // namespace

OnlineService::OnlineService(LibraryService* lib, PlayerController* player, QObject* parent)
    : QObject(parent), lib_(lib), player_(player) {
    kg_ = MakeKugou();
    ne_ = MakeNetease();
    LoadSettings();

    loginTimer_.setInterval(1500);
    connect(&loginTimer_, &QTimer::timeout, this, &OnlineService::PollLogin);

    // 入库后快照刷新是异步的，用短轮询等它出现（最多 ~10 秒）
    importTimer_.setInterval(500);
    connect(&importTimer_, &QTimer::timeout, this, [this]() {
        if (pendingPlayPath_.isEmpty()) { importTimer_.stop(); return; }
        const double id = lib_ ? lib_->trackIdByPath(pendingPlayPath_) : 0;
        if (id > 0) {
            importTimer_.stop();
            const QString p = pendingPlayPath_;
            pendingPlayPath_.clear();
            SetStatus(QString());
            SetProgress(0);
            if (player_) player_->playTrack(id);
            emit toast(tr("已加入曲库：%1").arg(QFileInfo(p).fileName()));
        } else if (++pendingPlayTries_ > 20) {
            importTimer_.stop();
            pendingPlayPath_.clear();
            SetStatus(QString());
            emit toast(tr("下载完成，但曲库还没索引到（稍后刷新试试）"));
        }
    });

    if (player_) {
        // 切到本地曲目：在线行的高亮要撤掉
        connect(player_, &PlayerController::trackChanged, this, [this]() {
            if (!player_ || player_->isStreaming()) return;
            if (playing_ >= 0) {
                playing_ = -1;
                emit playingChanged();
            }
        });
        // 输出设备重建后旧直链已经失效（多半过期）：重新解析当前这首一次。
        // 只重试一轮，再失败就让用户自己点，避免无限循环。
        connect(player_, &PlayerController::streamRestoreRequested, this,
                [this](const QVariantMap& m, double pos) { restoreStream(m, pos); });
        // 播本地曲目时，在线列表不该再高亮任何一行（playing_ 只是 raw_ 的下标）
        connect(player_, &PlayerController::trackChanged, this, [this]() {
            if (player_ && !player_->isStreaming() && (playing_ >= 0 || !playingKey_.isEmpty())) {
                playing_ = -1;
                pending_ = -1;
                playingKey_.clear();
                emit playingChanged();
            }
        });
        connect(player_, &PlayerController::streamNeedsReload, this, [this]() {
            const int row = playing_;
            if (row < 0 || row >= static_cast<int>(raw_.size())) return;
            playing_ = -1;
            emit playingChanged();
            play(row);
        });
        connect(player_, &PlayerController::streamEnded, this, [this]() {
            if (playing_ >= 0) {
                playing_ = -1;
                emit playingChanged();
            }
            if (autoSave_) return;
            // 晚一点再删：引擎刚松开文件句柄，立刻删 Windows 会拒绝
            QTimer::singleShot(1200, this, [this]() {
                PurgeCacheDir();
                SetStatus(QString());
            });
        });
    }
}

OnlineService::~OnlineService() {
    cancel_.store(true);
    if (lastAbort_) lastAbort_->store(true);   // 排队中的任务直接放弃
    loginTimer_.stop();
    importTimer_.stop();
    // 只有退出时才允许等：此时必须保证没有线程还在用 this
    if (worker_.joinable()) worker_.join();
    for (auto& j : retired_)
        if (j.th.joinable()) j.th.join();
    // 听完即删模式不留下任何东西：退出时把临时缓存清干净
    if (!autoSave_) PurgeCacheDir();
}

// ── 设置 ──────────────────────────────────────────────────────────────────

void OnlineService::LoadSettings() {
    QSettings st;
    // mid 必须先恢复再登录态：token 是绑在登录时那台设备号上的
    const QString mid = st.value("online/kugou_mid").toString();
    if (!mid.isEmpty()) KugouAuth::SetDeviceMid(mid.toStdString());

    account_.token = st.value("online/kugou_token").toString().toStdString();
    account_.userid = st.value("online/kugou_userid").toString().toStdString();
    account_.nickname = st.value("online/kugou_nick").toString().toStdString();
    account_.avatar = st.value("online/kugou_avatar").toString().toStdString();

    quality_ = static_cast<Quality>(st.value("online/quality", 0).toInt());
    downloadDir_ = st.value("online/download_dir").toString();
    autoSave_ = st.value("online/auto_save", true).toBool();

    if (kg_) {
        kg_->SetAccount(account_);
        kg_->SetPreferredQuality(quality_);
    }
}

void OnlineService::SaveAccount() {
    QSettings st;
    st.setValue("online/kugou_mid", QString::fromStdString(KugouAuth::DeviceMid()));
    st.setValue("online/kugou_token", QString::fromStdString(account_.token));
    st.setValue("online/kugou_userid", QString::fromStdString(account_.userid));
    st.setValue("online/kugou_nick", QString::fromStdString(account_.nickname));
    st.setValue("online/kugou_avatar", QString::fromStdString(account_.avatar));
}

QString OnlineService::downloadDir() const {
    if (!downloadDir_.isEmpty()) return downloadDir_;
    return QStandardPaths::writableLocation(QStandardPaths::MusicLocation) + "/拾羽在线";
}

void OnlineService::setDownloadDir(const QString& dir) {
    if (downloadDir_ == dir) return;
    downloadDir_ = dir;
    QSettings().setValue("online/download_dir", dir);
    emit downloadDirChanged();
}

void OnlineService::setQuality(int q) {
    q = q < 0 ? 0 : (q > 2 ? 2 : q);
    if (static_cast<int>(quality_) == q) return;
    quality_ = static_cast<Quality>(q);
    if (kg_) kg_->SetPreferredQuality(quality_);
    QSettings().setValue("online/quality", q);
    emit qualityChanged();
}

QString OnlineService::EnsureDownloadDir() const {
    const QString dir = downloadDir();
    QDir().mkpath(dir);
    return dir;
}

// 「听完即删」模式的落盘位置：系统临时目录，绝不混进曲库目录 ——
// 混进去会让用户以为这些歌已经收藏了，而实际上下一首就没了
QString OnlineService::CacheDir() const {
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::TempLocation) + "/shiyu-online-cache";
    QDir().mkpath(dir);
    return dir;
}

void OnlineService::PurgeCacheDir() {
    QDir d(CacheDir());
    const QFileInfoList files = d.entryInfoList(QDir::Files);
    for (const QFileInfo& fi : files) QFile::remove(fi.absoluteFilePath());
}

void OnlineService::setAutoSave(bool on) {
    if (autoSave_ == on) return;
    autoSave_ = on;
    QSettings().setValue("online/auto_save", on);
    // 从「入库」切到「听完即删」时，把之前遗留的缓存清掉；反向切换不动曲库里的文件
    if (!on) PurgeCacheDir();
    emit autoSaveChanged();
}

// ── 基础设施 ──────────────────────────────────────────────────────────────

Provider* OnlineService::ProviderFor(int src) { return src == 0 ? kg_.get() : ne_.get(); }

void OnlineService::RunAsync(std::function<void()> fn, bool cancelPrevious) {
    // 让正在跑的下载尽快停；网络请求本身有超时，不在这里等
    if (cancelPrevious) cancel_.store(true);
    // 还在排队的那一轮直接作废：用户已经改主意了，没必要再跑
    if (lastAbort_) lastAbort_->store(true);

    if (worker_.joinable()) retired_.push_back({std::move(worker_), lastDone_});
    ReapFinished();

    auto done = std::make_shared<std::atomic<bool>>(false);
    auto abort = std::make_shared<std::atomic<bool>>(false);
    auto prev = lastDone_;
    lastDone_ = done;
    lastAbort_ = abort;

    worker_ = std::thread([this, fn, prev, done, abort]() {
        // 等上一轮收工再开跑 —— 但这段等待发生在工作线程里，UI 不受影响。
        // 原来是在调用方（UI 线程）join，切歌 / 换榜单时就会顿一下。
        while (prev && !prev->load()) std::this_thread::sleep_for(std::chrono::milliseconds(4));
        if (!abort->load()) {
            cancel_.store(false);   // 上一轮已经结束，这时清标志是安全的
            fn();
        }
        done->store(true);
    });
}

void OnlineService::ReapFinished() {
    for (auto it = retired_.begin(); it != retired_.end();) {
        if (it->done && it->done->load()) {
            if (it->th.joinable()) it->th.join();
            it = retired_.erase(it);
        } else {
            ++it;
        }
    }
}

void OnlineService::SyncPlayingRow() {
    int found = -1;
    if (!playingKey_.isEmpty()) {
        for (int i = 0; i < static_cast<int>(raw_.size()); ++i) {
            if (ToQ(raw_[i].title) + QLatin1Char('\n') + ToQ(raw_[i].artist) == playingKey_) {
                found = i;
                break;
            }
        }
    }
    if (found != playing_) {
        playing_ = found;
        emit playingChanged();
    }
}

// 全局断网：所有会发出请求的入口都在最前面调它
bool OnlineService::Blocked() {
    if (!SettingsService::Offline()) return false;
    SetBusy(false);
    SetStatus(tr("已关闭联网 · 可在设置里重新打开"));
    return true;
}

void OnlineService::restoreStream(const QVariantMap& m, double position) {
    if (Blocked()) return;
    Track t;
    t.source = m.value("source").toInt() == 0 ? Source::Kugou : Source::Netease;
    t.id = m.value("id").toString().toStdString();
    if (t.id.empty()) return;
    t.title = m.value("title").toString().toStdString();
    t.artist = m.value("artist").toString().toStdString();
    t.album = m.value("album").toString().toStdString();
    t.albumId = m.value("albumId").toString().toStdString();
    t.durationMs = m.value("durationMs").toInt();
    t.coverUrl = m.value("cover").toString().toStdString();

    // 上次在播的那首歌单独铺一屏，界面能看见它、也能点播放
    raw_.clear();
    raw_.push_back(t);
    tracks_.clear();
    tracks_.append(TrackToMap(t));
    artists_.clear();
    albums_.clear();
    viewMode_ = 1;
    listName_ = tr("上次播放");
    canMore_ = false;
    resumeSeek_ = position;
    restorePaused_ = true;   // 和本地曲目一样：停在上次的位置，等用户按播放
    emit viewChanged();
    emit tracksChanged();
    emit tracksReset();
    play(0);
}

// 流式源只能跳到"已经下载到的范围"。刚开播时目标位置还没数据，SeekTo 会失败并保持原位，
// 所以每隔 400ms 重试一次，数据追上就停（最多约 20 秒）。
void OnlineService::ApplyResumeSeek(double target) {
    auto tries = std::make_shared<int>(0);
    auto* timer = new QTimer(this);
    timer->setInterval(400);
    connect(timer, &QTimer::timeout, this, [this, target, tries, timer]() {
        if (!player_ || ++(*tries) > 50) {
            timer->stop();
            timer->deleteLater();
            return;
        }
        player_->seek(target);
        if (player_->position() > target - 3.0) {
            timer->stop();
            timer->deleteLater();
        }
    });
    timer->start();
}

void OnlineService::clearTempCache() {
    PurgeCacheDir();
    emit toast(tr("已清理在线缓存"));
}

void OnlineService::openDownloadDir() {
    const QString dir = EnsureDownloadDir();
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
}

void OnlineService::SetBusy(bool b) {
    if (busy_ == b) return;
    busy_ = b;
    emit busyChanged();
}

void OnlineService::SetStatus(const QString& s) {
    if (status_ == s) return;
    status_ = s;
    emit statusChanged();
}

void OnlineService::SetProgress(int p) {
    if (progress_ == p) return;
    progress_ = p;
    emit progressChanged();
}

void OnlineService::setSource(int s) {
    if (source_ == s) return;
    source_ = s;
    emit sourceChanged();
    lists_.clear();
    emit listsChanged();
    tracks_.clear();
    artists_.clear();
    albums_.clear();
    viewMode_ = 0;
    stack_.clear();
    listRow_ = -1;
    emit viewChanged();
    emit tracksChanged();
    emit tracksReset();
    reload();
}

// ── 榜单 / 搜索 ───────────────────────────────────────────────────────────

void OnlineService::reload() {
    if (Blocked()) return;
    const int src = source_;
    Provider* p = ProviderFor(src);
    if (!p) return;
    SetBusy(true);
    SetStatus(tr("加载榜单…"));
    RunAsync([this, p, src]() {
        auto lists = p->Lists();
        QVariantList out;
        for (const auto& l : lists) {
            QVariantMap m;
            m["id"] = ToQ(l.id);
            m["name"] = ToQ(l.name);
            m["cover"] = ToQ(l.coverUrl);
            m["note"] = ToQ(l.note);
            out.append(m);
        }
        QMetaObject::invokeMethod(
            this,
            [this, out, src]() {
                if (src != source_) return;
                lists_ = out;
                emit listsChanged();
                SetBusy(false);
                SetStatus(out.isEmpty() ? tr("榜单加载失败") : QString());
                // 首次进入 / 换源后自动打开第一个榜单：不让用户先对着空页面点一下
                if (!out.isEmpty() && tracks_.empty() && viewMode_ == 0) openList(0);
            },
            Qt::QueuedConnection);
    });
}

void OnlineService::openList(int row) {
    if (Blocked()) return;
    if (row < 0 || row >= lists_.size()) return;
    listRow_ = row;
    if (playing_ >= 0) {   // 换了榜单，旧的播放行下标就失效了
        playing_ = -1;
        emit playingChanged();
    }
    const QVariantMap m = lists_[row].toMap();
    const std::string id = m.value("id").toString().toStdString();
    const int src = source_;
    Provider* p = ProviderFor(src);
    if (!p || id.empty()) return;

    SetBusy(true);
    SetStatus(tr("加载歌曲…"));
    listName_ = m.value("name").toString();
    // 立刻清空旧榜单：先让"正在加载"出现，比让上一张榜单挂在那里再突然替换更像即时响应
    tracks_.clear();
    raw_.clear();
    artists_.clear();
    albums_.clear();
    canMore_ = false;
    if (viewMode_ != 0) { viewMode_ = 0; emit viewChanged(); }
    emit tracksChanged();
    RunAsync([this, p, id, src, name = listName_]() {
        auto songs = p->ListSongs(id, 120);
        QVariantList out;
        std::vector<Track> raw;
        for (const auto& t : songs) {
            QVariantMap x;
            x["title"] = ToQ(t.title);
            x["artist"] = ToQ(t.artist);
            x["album"] = ToQ(t.album);
            x["duration"] = ToQ(t.DurationText());
            x["paywalled"] = t.Paywalled();
            x["privilege"] = t.privilege;
            out.append(x);
            raw.push_back(t);
        }
        QMetaObject::invokeMethod(
            this,
            [this, out, raw, src, name]() {
                if (src != source_) return;
                tracks_ = out;
                raw_ = raw;
                SyncPlayingRow();
                artists_.clear();
                albums_.clear();
                viewMode_ = 0;
                listName_ = name;
                emit tracksChanged();
                emit tracksReset();
                SetBusy(false);
                SetStatus(QString());
            },
            Qt::QueuedConnection);
    });
}

void OnlineService::backToLists() {
    if (viewMode_ != 1) return;
    viewMode_ = 0;
    keyword_.clear();
    emit viewChanged();
    emit keywordChanged();
    // 搜索结果已经把 tracks_ 覆盖掉了，回到浏览态必须把原来那个榜单重新拉一遍
    if (listRow_ >= 0 && listRow_ < lists_.size()) {
        openList(listRow_);
    } else {
        tracks_.clear();
        artists_.clear();
        albums_.clear();
        emit tracksChanged();
        emit tracksReset();
    }
}

// ── 歌手 / 专辑页 ─────────────────────────────────────────────────────────

void OnlineService::PushSnapshot() {
    Snapshot s;
    s.mode = viewMode_;
    s.tracks = tracks_;
    s.artists = artists_;
    s.albums = albums_;
    s.raw = raw_;
    s.listName = listName_;
    s.keyword = keyword_;
    s.listRow = listRow_;
    stack_.push_back(std::move(s));
    if (stack_.size() > 6) stack_.erase(stack_.begin());
}

void OnlineService::openArtist(const QString& id, const QString& name) {
    if (id.isEmpty() && name.trimmed().isEmpty()) return;
    PushSnapshot();

    viewMode_ = 2;
    detailId_ = id;
    detailTitle_ = name;
    detailSubtitle_.clear();
    detailCover_.clear();
    detailMeta_ = tr("加载中…");
    canMore_ = false;
    tracks_.clear();
    raw_.clear();
    artists_.clear();
    albums_.clear();
    emit viewChanged();
    emit tracksChanged();
    emit tracksReset();

    const int src = source_;
    Provider* p = ProviderFor(src);
    if (!p) return;
    SetBusy(true);

    RunAsync([this, p, src, id = id.toStdString(), name = name.toStdString()]() {
        // 酷狗曲目搜索没有 singerid：用名字反查，优先取完全同名的那个
        std::string aid = id;
        if (aid.empty()) {
            auto cand = p->SearchArtists(name, 10);
            for (const auto& a : cand)
                if (a.name == name) { aid = a.id; break; }
            if (aid.empty() && !cand.empty()) aid = cand.front().id;
        }

        online_core::ArtistInfo info =
            aid.empty() ? online_core::ArtistInfo() : p->Artist(aid);
        auto songs = aid.empty() ? std::vector<Track>() : p->ArtistSongs(aid, 100, 0);

        QVariantList out;
        std::vector<Track> raw;
        for (const auto& t : songs) {
            out.append(TrackToMap(t));
            raw.push_back(t);
        }

        const bool more = songs.size() >= 100 && (info.songCount == 0 || info.songCount > 100);
        const QString title = info.name.empty() ? ToQ(name) : ToQ(info.name);
        QString meta;
        if (info.songCount > 0) meta = tr("%1 首").arg(info.songCount);
        if (info.albumCount > 0)
            meta = meta.isEmpty() ? tr("%1 张专辑").arg(info.albumCount)
                                  : tr("%1 · %2 张专辑").arg(meta).arg(info.albumCount);
        if (meta.isEmpty()) meta = tr("%1 首").arg(raw.size());
        const QString cover = ToQ(info.avatar);

        QMetaObject::invokeMethod(
            this,
            [this, out, raw, src, more, title, meta, cover, aid = ToQ(aid)]() {
                if (src != source_ || viewMode_ != 2) return;
                tracks_ = out;
                raw_ = raw;
                SyncPlayingRow();
                canMore_ = more;
                if (!aid.isEmpty()) detailId_ = aid;
                detailTitle_ = title;
                detailMeta_ = meta;
                detailCover_ = cover;
                emit viewChanged();
                emit tracksChanged();
                emit tracksReset();
                SetBusy(false);
                SetStatus(out.isEmpty() ? tr("没拿到这个歌手的歌") : QString());
            },
            Qt::QueuedConnection);
    });
}

void OnlineService::openAlbum(const QString& id, const QString& name) {
    if (id.isEmpty()) return;
    PushSnapshot();

    viewMode_ = 3;
    detailId_ = id;
    detailTitle_ = name;
    detailSubtitle_.clear();
    detailCover_.clear();
    detailMeta_ = tr("加载中…");
    canMore_ = false;
    tracks_.clear();
    raw_.clear();
    artists_.clear();
    albums_.clear();
    emit viewChanged();
    emit tracksChanged();
    emit tracksReset();

    const int src = source_;
    Provider* p = ProviderFor(src);
    if (!p) return;
    SetBusy(true);

    RunAsync([this, p, src, id = id.toStdString(), name = name.toStdString()]() {
        online_core::AlbumInfo info = p->Album(id);
        auto songs = p->AlbumSongs(id, 200);

        QVariantList out;
        std::vector<Track> raw;
        for (const auto& t : songs) {
            out.append(TrackToMap(t));
            raw.push_back(t);
        }

        const QString title = info.name.empty() ? ToQ(name) : ToQ(info.name);
        const QString sub = ToQ(info.artist);
        QString meta = YearText(info.publishTime);
        if (info.songCount > 0) {
            const QString n = tr("%1 首").arg(info.songCount);
            meta = meta.isEmpty() ? n : meta + " · " + n;
        }
        if (meta.isEmpty()) meta = tr("%1 首").arg(raw.size());
        const QString cover = ToQ(info.cover);

        QMetaObject::invokeMethod(
            this,
            [this, out, raw, src, title, sub, meta, cover]() {
                if (src != source_ || viewMode_ != 3) return;
                tracks_ = out;
                raw_ = raw;
                SyncPlayingRow();
                canMore_ = false;
                detailTitle_ = title;
                detailSubtitle_ = sub;
                detailMeta_ = meta;
                detailCover_ = cover;
                emit viewChanged();
                emit tracksChanged();
                emit tracksReset();
                SetBusy(false);
                SetStatus(out.isEmpty() ? tr("没拿到这张专辑的歌") : QString());
            },
            Qt::QueuedConnection);
    });
}

void OnlineService::back() {
    if (stack_.empty()) {
        backToLists();
        return;
    }
    Snapshot s = std::move(stack_.back());
    stack_.pop_back();

    viewMode_ = s.mode;
    tracks_ = s.tracks;
    artists_ = s.artists;
    albums_ = s.albums;
    raw_ = std::move(s.raw);
    SyncPlayingRow();
    listName_ = s.listName;
    keyword_ = s.keyword;
    listRow_ = s.listRow;
    canMore_ = false;
    detailId_.clear();
    detailTitle_.clear();
    detailSubtitle_.clear();
    detailCover_.clear();
    detailMeta_.clear();
    SetBusy(false);
    SetStatus(QString());
    emit viewChanged();
    emit keywordChanged();
    emit tracksChanged();
}

void OnlineService::loadMore() {
    if (Blocked()) return;
    if (!canMore_ || (viewMode_ != 1 && viewMode_ != 2)) return;
    const int src = source_;
    Provider* p = ProviderFor(src);
    if (!p) return;

    const int mode = viewMode_;
    const int offset = static_cast<int>(raw_.size());
    const int limit = 100;
    const std::string id = detailId_.toStdString();
    const std::string key = keyword_.toStdString();

    canMore_ = false;   // 这一拍先关掉，避免连点重复请求
    emit tracksChanged();
    SetBusy(true);

    RunAsync([this, p, src, mode, offset, limit, id, key]() {
        auto songs = mode == 2 ? p->ArtistSongs(id, limit, offset) : p->Search(key, limit, offset);
        QVariantList add;
        std::vector<Track> rawAdd;
        for (const auto& t : songs) {
            add.append(TrackToMap(t));
            rawAdd.push_back(t);
        }
        const bool more = static_cast<int>(songs.size()) >= limit;
        QMetaObject::invokeMethod(
            this,
            [this, add, rawAdd, src, mode, more]() {
                if (src != source_ || viewMode_ != mode) return;
                tracks_ += add;
                raw_.insert(raw_.end(), rawAdd.begin(), rawAdd.end());
                canMore_ = more;
                emit tracksChanged();
                SetBusy(false);
                SetStatus(QString());
            },
            Qt::QueuedConnection);
    });
}

void OnlineService::search(const QString& kw) {
    if (Blocked()) return;
    keyword_ = kw;
    emit keywordChanged();
    if (kw.trimmed().isEmpty()) return;
    const int src = source_;
    Provider* p = ProviderFor(src);
    if (!p) return;

    // 立刻切到搜索态并清掉旧结果：先让界面有反应，再去等网络。
    // 原来要等结果回来才切页面，中间那几百毫秒界面一动不动 —— 这就是"滞后感"。
    SetBusy(true);
    SetStatus(tr("搜索中…"));
    viewMode_ = 1;
    emit viewChanged();
    listName_ = tr("搜索：%1").arg(kw);
    tracks_.clear();
    raw_.clear();
    artists_.clear();
    albums_.clear();
    emit tracksChanged();

    RunAsync([this, p, src, key = kw.toStdString()]() {
        auto songs = p->Search(key, 100, 0);

        QVariantList out;
        std::vector<Track> raw;
        for (const auto& t : songs) {
            QVariantMap x;
            x["title"] = ToQ(t.title);
            x["artist"] = ToQ(t.artist);
            x["album"] = ToQ(t.album);
            x["duration"] = ToQ(t.DurationText());
            x["paywalled"] = t.Paywalled();
            x["privilege"] = t.privilege;
            out.append(x);
            raw.push_back(t);
        }

        // 把结果按实体拆开，跟本地搜索页一样分「歌手 / 专辑 / 歌曲」三段。
        // 酷狗搜索只给曲目，歌手和专辑得自己归并。
        QVariantList artists;
        std::vector<std::pair<std::string, int>> aOrder;   // 保序
        std::vector<std::pair<std::string, int>> bOrder;
        std::vector<std::string> bAlbumId;
        std::vector<std::string> bArtist;
        std::vector<std::string> bCover;
        {
            auto bump = [](std::vector<std::pair<std::string, int>>& v, const std::string& k) {
                for (auto& e : v)
                    if (e.first == k) { ++e.second; return; }
                v.emplace_back(k, 1);
            };
            for (const auto& t : songs) {
                if (!t.artist.empty()) bump(aOrder, t.artist);
                if (!t.album.empty()) {
                    bool found = false;
                    for (size_t i = 0; i < bOrder.size(); ++i)
                        if (bOrder[i].first == t.album) { ++bOrder[i].second; found = true; break; }
                    if (!found) {
                        bOrder.emplace_back(t.album, 1);
                        bAlbumId.push_back(t.albumId);
                        bArtist.push_back(t.artist);
                        bCover.push_back(t.coverUrl);
                    }
                }
            }
            for (const auto& e : aOrder) {
                QVariantMap m;
                m["name"] = ToQ(e.first);
                m["count"] = e.second;
                artists.append(m);
            }
        }

        const int n = static_cast<int>(std::min<size_t>(bOrder.size(), 8));
        auto buildAlbums = [&](const std::vector<std::string>& covers) {
            QVariantList v;
            for (int i = 0; i < n; ++i) {
                QVariantMap m;
                m["id"] = ToQ(bAlbumId[i]);
                m["name"] = ToQ(bOrder[i].first);
                m["artist"] = ToQ(bArtist[i]);
                m["count"] = bOrder[i].second;
                m["cover"] = ToQ(i < static_cast<int>(covers.size()) ? covers[i] : std::string());
                v.append(m);
            }
            return v;
        };

        // 先不带封面的结果立刻发出去：搜索的感知延迟只该等于一次搜索请求。
        // 之前 8 张封面是串行查的，每张一次 HTTPS，合计 1~3 秒 —— 那才是"搜索总是很慢"的主因。
        QVariantList albums = buildAlbums({});
        const bool more = songs.size() >= 100;
        QMetaObject::invokeMethod(
            this,
            [this, out, raw, artists, albums, src, more, name = tr("搜索：%1").arg(ToQ(key))]() {
                if (src != source_ || viewMode_ != 1) return;
                tracks_ = out;
                raw_ = raw;
                SyncPlayingRow();
                artists_ = artists;
                albums_ = albums;
                viewMode_ = 1;
                listName_ = name;
                canMore_ = more;
                emit tracksChanged();
                emit tracksReset();
                SetBusy(false);
                SetStatus(out.isEmpty() ? tr("没有结果") : QString());
            },
            Qt::QueuedConnection);

        // 歌手实体：曲目搜索不带 singerid，单独查一次。查到就换成带 id 的版本 ——
        // 点歌手才能进「他的全部歌曲」；查不到就保留按名字归并的那份，点的时候再反查。
        {
            auto real = p->SearchArtists(key, 8);
            if (!real.empty()) {
                QVariantList av;
                for (const auto& a : real) {
                    QVariantMap m;
                    m["id"] = ToQ(a.id);
                    m["name"] = ToQ(a.name);
                    m["count"] = a.songCount;
                    av.append(m);
                }
                QMetaObject::invokeMethod(
                    this,
                    [this, av, src]() {
                        if (src != source_ || viewMode_ != 1) return;
                        artists_ = av;
                        emit tracksChanged();
                    },
                    Qt::QueuedConnection);
            }
        }

        if (n <= 0) return;

        // 封面随后补：6 路并行，8 张两轮就回来；补上时只发 tracksChanged，
        // 不发 tracksReset —— 否则列表会再淡入一次，反而更"跳"
        std::vector<std::string> covers(static_cast<size_t>(n));
        std::atomic<int> next{0};
        auto fetchOne = [&]() {
            for (;;) {
                const int i = next.fetch_add(1);
                if (i >= n) break;
                covers[static_cast<size_t>(i)] =
                    src == 0 ? KugouAlbumCover(bAlbumId[static_cast<size_t>(i)])
                             : bCover[static_cast<size_t>(i)];
            }
        };
        std::vector<std::thread> pool;
        const int lanes = std::min(6, n);
        for (int i = 0; i < lanes; ++i) pool.emplace_back(fetchOne);
        for (auto& t : pool) t.join();

        QVariantList withCovers = buildAlbums(covers);
        QMetaObject::invokeMethod(
            this,
            [this, withCovers, src]() {
                if (src != source_ || viewMode_ != 1) return;
                albums_ = withCovers;
                emit tracksChanged();
            },
            Qt::QueuedConnection);
    });
}

void OnlineService::play(int row) {
    if (Blocked()) return;
    if (row < 0 || row >= static_cast<int>(raw_.size())) return;
    const Track t = raw_[row];
    const int src = source_;
    Provider* p = ProviderFor(src);
    if (!p) return;

    // 这一次点击的意图先定下来：中途改开关不影响本轮
    const bool save = autoSave_;
    // 先把这一行标成"处理中"：解析要 200~400ms，这期间界面必须有反应，
    // 否则就是"点了没动静"的手感
    pending_ = row;
    playing_ = -1;
    emit playingChanged();
    SetBusy(true);
    SetProgress(0);
    SetStatus(tr("解析中…"));
    const QString dir = EnsureDownloadDir();
    const QString cacheDir = save ? QString() : CacheDir();
    const QString base = SanitizeFileName(
        QString("%1 - %2").arg(ToQ(t.artist), ToQ(t.title)));

    RunAsync([this, p, t, dir, cacheDir, base, save, row]() {
        // 上一首的临时文件先清掉：此时上一轮 worker 已经 join，文件句柄是松的
        if (!save) PurgeCacheDir();

        Resolved r = p->Resolve(t);
        if (!r.ok) {
            QMetaObject::invokeMethod(
                this,
                [this, r]() {
                    pending_ = -1;
                    restorePaused_ = false;
                    resumeSeek_ = -1.0;
                    emit playingChanged();
                    SetBusy(false);
                    SetProgress(0);
                    SetStatus(r.message.empty() ? tr("解析失败") : ToQ(r.message));
                    if (r.need_login) emit toast(tr("需要扫码登录酷狗账号"));
                },
                Qt::QueuedConnection);
            return;
        }

        // 封面：先取地址再取字节，落成本地文件。
        // 一次几十 KB 的小请求，换来「正在播放」有图 + 整页强调色跟着这首歌走；
        // 直接让 QML 加载 http 地址会走 Qt 自己的 TLS 栈，本机代理换证书就是白图。
        std::string coverUrl = t.coverUrl;
        if (coverUrl.empty() && t.source == Source::Kugou) coverUrl = KugouAlbumCover(t.albumId);
        std::string coverBytes;
        if (!coverUrl.empty()) FetchBytes(coverUrl, coverBytes);
        const QString coverPath = coverBytes.empty() ? QString() : WriteTempCover(coverBytes);

        // 先把直链交给引擎流式解码 —— 这一步返回就出声，不等下载。
        QMetaObject::invokeMethod(
            this,
            [this, url = r.url, t, coverPath, save, row]() {
                SetBusy(false);
                if (!player_) {
                    SetStatus(tr("播放器不可用"));
                    return;
                }
                const QString coverArg =
                    coverPath.isEmpty() ? QString() : QUrl::fromLocalFile(coverPath).toString();
                QVariantMap ident;
                ident["source"] = static_cast<int>(t.source);
                ident["id"] = ToQ(t.id);
                ident["albumId"] = ToQ(t.albumId);
                const bool autoPlay = !restorePaused_;
                restorePaused_ = false;
                const bool ok = player_->playStream(ToQ(url), ToQ(t.title), ToQ(t.artist),
                                                    ToQ(t.album), coverArg, autoPlay,
                                                    t.durationMs, ident);
                pending_ = -1;
                if (ok) {
                    playing_ = row;
                    playingKey_ = ToQ(t.title) + QLatin1Char('\n') + ToQ(t.artist);
                    emit playingChanged();
                    if (resumeSeek_ > 1.0) {
                        const double target = resumeSeek_;
                        resumeSeek_ = -1.0;
                        ApplyResumeSeek(target);
                    }
                } else if (playing_ == row) {
                    // 起播失败还留着"正在播放"，再点这一行只会走暂停/继续 ——
                    // 清掉它，用户再点一次才会重新解析
                    playing_ = -1;
                    restorePaused_ = false;
                    resumeSeek_ = -1.0;
                    emit playingChanged();
                }
                SetStatus(ok ? (save ? tr("播放中 · 后台保存")
                                     : tr("播放中 · 听完即删"))
                             : tr("播放失败，链接可能已过期"));
            },
            Qt::QueuedConnection);

        // 同一个 worker 继续落盘。入库模式存进曲库目录；听完即删模式只进临时目录
        QString ext = ToQ(r.ext);
        if (ext.isEmpty()) ext = "mp3";
        const QString path = QDir(save ? dir : cacheDir).filePath(base + "." + ext);
        if (QFileInfo::exists(path) && QFileInfo(path).size() > 1024) {
            if (save)
                QMetaObject::invokeMethod(this, [this, path]() { ImportAndPlay(path, false); },
                                          Qt::QueuedConnection);
            return;
        }
#ifdef _WIN32
        const bool ok = DownloadToFile(
            r.url, path.toStdWString(), cancel_, [this](int pct) {
                QMetaObject::invokeMethod(this, [this, pct]() { SetProgress(pct); },
                                          Qt::QueuedConnection);
            });
#else
        const bool ok = false;
#endif
        if (ok && save) ApplyCover(path, t);
        QMetaObject::invokeMethod(
            this,
            [this, ok, path, save]() {
                SetProgress(0);
                if (!ok) {
                    SetStatus(tr("保存失败"));
                    return;
                }
                if (save) {
                    ImportAndPlay(path, false);
                    SetStatus(QString());
                } else {
                    SetStatus(tr("已缓存，播完清理"));
                }
            },
            Qt::QueuedConnection);
    });
}

void OnlineService::download(int row) {
    if (Blocked()) return;
    if (row < 0 || row >= static_cast<int>(raw_.size())) return;
    const Track t = raw_[row];
    const int src = source_;
    Provider* p = ProviderFor(src);
    if (!p) return;

    SetBusy(true);
    SetProgress(0);
    SetStatus(tr("解析中…"));
    const QString dir = EnsureDownloadDir();
    const QString base = SanitizeFileName(
        QString("%1 - %2").arg(ToQ(t.artist), ToQ(t.title)));

    RunAsync([this, p, t, dir, base]() {
        Resolved r = p->Resolve(t);
        if (!r.ok) {
            QMetaObject::invokeMethod(
                this,
                [this, r]() {
                    SetBusy(false);
                    SetProgress(0);
                    SetStatus(r.message.empty() ? tr("解析失败") : ToQ(r.message));
                    if (r.need_login) emit toast(tr("需要扫码登录酷狗账号"));
                },
                Qt::QueuedConnection);
            return;
        }
        QString ext = ToQ(r.ext);
        if (ext.isEmpty()) ext = "mp3";
        const QString path = QDir(dir).filePath(base + "." + ext);
        if (QFileInfo::exists(path) && QFileInfo(path).size() > 1024) {
            QMetaObject::invokeMethod(this, [this, path]() { ImportAndPlay(path, false); },
                                      Qt::QueuedConnection);
            return;
        }
        QMetaObject::invokeMethod(this, [this]() { SetStatus(tr("正在下载…")); },
                                  Qt::QueuedConnection);
#ifdef _WIN32
        const bool ok = DownloadToFile(
            r.url, path.toStdWString(), cancel_, [this](int pct) {
                QMetaObject::invokeMethod(this, [this, pct]() { SetProgress(pct); },
                                          Qt::QueuedConnection);
            });
#else
        const bool ok = false;
#endif
        if (ok) ApplyCover(path, t);
        QMetaObject::invokeMethod(
            this,
            [this, ok, path]() {
                if (!ok) {
                    SetBusy(false);
                    SetProgress(0);
                    SetStatus(tr("下载失败，可能是链接已过期"));
                    return;
                }
                ImportAndPlay(path, false);
            },
            Qt::QueuedConnection);
    });
}

void OnlineService::ImportAndPlay(const QString& path, bool playAfter) {
    SetBusy(false);
    SetProgress(0);
    if (!lib_) {
        SetStatus(QString());
        return;
    }
    lib_->addFiles({path});
    if (!playAfter) {
        SetStatus(QString());
        emit toast(tr("已保存：%1").arg(QFileInfo(path).fileName()));
        return;
    }
    pendingPlayPath_ = path;
    pendingPlayTries_ = 0;
    SetStatus(tr("正在入库…"));
    importTimer_.start();
}

// ── 扫码登录 ──────────────────────────────────────────────────────────────

void OnlineService::beginLogin() {
    if (Blocked()) return;
    if (loginVisible_) return;
    loginVisible_ = true;
    loginHint_ = tr("正在生成二维码…");
    qrImage_.clear();
    emit loginChanged();
    SetBusy(true);

    RunAsync([this]() {
        KugouAuth auth;
        KugouAuth::QrCode qr;
        std::string err;
        const bool ok = auth.CreateQr(qr, &err);
        QString path;
        if (ok) {
            path = QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
                   "/shiyu_online_qr.png";
            QFile f(path);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(reinterpret_cast<const char*>(qr.png.data()),
                        static_cast<qint64>(qr.png.size()));
                f.close();
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, ok, path, key = QString::fromStdString(qr.key), err]() {
                SetBusy(false);
                if (!ok || path.isEmpty()) {
                    loginHint_ = err.empty() ? tr("二维码生成失败") : ToQ(err);
                    emit loginChanged();
                    return;
                }
                qrKey_ = key.toStdString();
                qrImage_ = QString("file:///%1?t=%2")
                               .arg(path)
                               .arg(QDateTime::currentMSecsSinceEpoch());
                loginHint_ = tr("用酷狗 App 扫一扫，二维码 5 分钟内有效");
                emit loginChanged();
                loginTimer_.start();
            },
            Qt::QueuedConnection);
    });
}

void OnlineService::PollLogin() {
    if (qrKey_.empty() || SettingsService::Offline()) { loginTimer_.stop(); return; }
    // 上一拍还没回来就跳过（不能用 worker_.joinable 判断，见头文件注释）
    if (pollInFlight_.exchange(true)) return;
    const std::string key = qrKey_;
    RunAsync([this, key]() {
        KugouAuth auth;
        Account acc;
        std::string err;
        const auto st = auth.CheckQr(key, acc, &err);
        pollInFlight_.store(false);
        QMetaObject::invokeMethod(
            this,
            [this, st, acc, err]() {
                switch (st) {
                    case KugouAuth::State::Confirmed: {
                        loginTimer_.stop();
                        account_ = acc;
                        SaveAccount();
                        if (kg_) kg_->SetAccount(account_);
                        loginVisible_ = false;
                        qrImage_.clear();
                        loginHint_.clear();
                        emit loginChanged();
                        emit accountChanged();
                        emit toast(tr("已登录：%1").arg(ToQ(account_.nickname)));
                        reload();
                        break;
                    }
                    case KugouAuth::State::Scanned:
                        loginHint_ = tr("已扫码，请在手机上点确认");
                        emit loginChanged();
                        break;
                    case KugouAuth::State::Expired:
                        loginTimer_.stop();
                        loginHint_ = tr("二维码已过期，点「重新生成」");
                        emit loginChanged();
                        break;
                    case KugouAuth::State::Error:
                        // 网络抖动不终止，下一拍继续
                        break;
                    default: break;
                }
            },
            Qt::QueuedConnection);
    }, /*cancelPrevious=*/false);
}

void OnlineService::cancelLogin() {
    loginTimer_.stop();
    qrKey_.clear();
    loginVisible_ = false;
    qrImage_.clear();
    loginHint_.clear();
    emit loginChanged();
}

void OnlineService::logout() {
    account_.Clear();
    SaveAccount();
    if (kg_) kg_->SetAccount(account_);
    emit accountChanged();
    emit toast(tr("已退出酷狗账号"));
}
