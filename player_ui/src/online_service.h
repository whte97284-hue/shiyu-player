#pragma once
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "online_music.hpp"

class LibraryService;
class PlayerController;

// ============================================================================
//  OnlineService —— 在线曲库的 QML 门面
//
//  只做编排，算法都在 player_core/online_music。三件事：
//    1. 榜单/搜索：把结果转成 QVariantList 给 QML
//    2. 扫码登录：二维码 PNG 落盘，QML 直接 Image 显示；定时轮询
//    3. 播放/下载：解析直链 → 下到曲库目录 → library.addFiles() 入库 →
//       等快照刷新 → 用 trackIdByPath 拿 id → 走现有 player.playTrack()
//  这样队列、歌词、封面、正在播放全部复用，不另开播放通道。
//
//  线程模型：所有网络/下载在 worker_ 里跑，结果用 QueuedConnection 回主线程。
//  worker_ 重新赋值前必须先 join（否则 std::terminate，之前踩过）。
// ============================================================================

class OnlineService : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList lists READ lists NOTIFY listsChanged)
    Q_PROPERTY(QVariantList tracks READ tracks NOTIFY tracksChanged)
    // 搜索结果按实体拆开：歌手 / 专辑 / 歌曲三段，跟本地搜索页一致
    Q_PROPERTY(QVariantList artists READ artists NOTIFY tracksChanged)
    Q_PROPERTY(QVariantList albums READ albums NOTIFY tracksChanged)
    Q_PROPERTY(bool isSearch READ isSearch NOTIFY viewChanged)
    // 0 榜单 / 1 搜索 / 2 歌手 / 3 专辑。歌手和专辑页共用一套详情头部
    Q_PROPERTY(int viewMode READ viewMode NOTIFY viewChanged)
    Q_PROPERTY(bool isDetail READ isDetail NOTIFY viewChanged)
    Q_PROPERTY(QString detailTitle READ detailTitle NOTIFY viewChanged)
    Q_PROPERTY(QString detailSubtitle READ detailSubtitle NOTIFY viewChanged)
    Q_PROPERTY(QString detailCover READ detailCover NOTIFY viewChanged)
    Q_PROPERTY(QString detailMeta READ detailMeta NOTIFY viewChanged)
    Q_PROPERTY(bool detailRound READ detailRound NOTIFY viewChanged)   // 歌手头像是圆的
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY tracksChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(int source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(QString listName READ listName NOTIFY tracksChanged)
    Q_PROPERTY(QString keyword READ keyword NOTIFY keywordChanged)
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY accountChanged)
    Q_PROPERTY(QString nickname READ nickname NOTIFY accountChanged)
    Q_PROPERTY(bool loginVisible READ loginVisible NOTIFY loginChanged)
    Q_PROPERTY(QString qrImage READ qrImage NOTIFY loginChanged)
    Q_PROPERTY(QString loginHint READ loginHint NOTIFY loginChanged)
    Q_PROPERTY(QString downloadDir READ downloadDir WRITE setDownloadDir NOTIFY downloadDirChanged)
    Q_PROPERTY(int quality READ quality WRITE setQuality NOTIFY qualityChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    // 边听边存：开 = 下载入库；关 = 只落到临时目录，听完自动删除
    Q_PROPERTY(bool autoSave READ autoSave WRITE setAutoSave NOTIFY autoSaveChanged)
    // 当前正在播放的在线行（-1 = 没有）。列表据此高亮那一行
    Q_PROPERTY(int playing READ playing NOTIFY playingChanged)
    // 正在解析 / 下载的那一行（-1 = 没有）。点下去立刻有转圈反馈，不等网络
    Q_PROPERTY(int pending READ pending NOTIFY playingChanged)

public:
    OnlineService(LibraryService* lib, PlayerController* player, QObject* parent = nullptr);
    ~OnlineService() override;

    QVariantList lists() const { return lists_; }
    QVariantList tracks() const { return tracks_; }
    QVariantList artists() const { return artists_; }
    QVariantList albums() const { return albums_; }
    bool isSearch() const { return viewMode_ == 1; }
    int viewMode() const { return viewMode_; }
    bool isDetail() const { return viewMode_ >= 2; }
    QString detailTitle() const { return detailTitle_; }
    QString detailSubtitle() const { return detailSubtitle_; }
    QString detailCover() const { return detailCover_; }
    QString detailMeta() const { return detailMeta_; }
    bool detailRound() const { return viewMode_ == 2; }
    bool canLoadMore() const { return canMore_; }
    bool busy() const { return busy_; }
    QString status() const { return status_; }
    int source() const { return source_; }
    void setSource(int s);
    QString listName() const { return listName_; }
    QString keyword() const { return keyword_; }
    bool loggedIn() const { return account_.Valid(); }
    QString nickname() const { return QString::fromStdString(account_.nickname); }
    bool loginVisible() const { return loginVisible_; }
    QString qrImage() const { return qrImage_; }
    QString loginHint() const { return loginHint_; }
    QString downloadDir() const;
    void setDownloadDir(const QString& dir);
    int quality() const { return static_cast<int>(quality_); }
    void setQuality(int q);
    int progress() const { return progress_; }
    bool autoSave() const { return autoSave_; }
    void setAutoSave(bool on);
    int playing() const { return playing_; }
    int pending() const { return pending_; }

    Q_INVOKABLE void reload();                 // 拉当前源的榜单
    Q_INVOKABLE void openList(int row);        // 打开榜单
    Q_INVOKABLE void search(const QString& kw);
    Q_INVOKABLE void backToLists();            // 搜索态 → 浏览态（重新拉当前榜单）
    // 进歌手 / 专辑页：id 为空时按名字反查（酷狗曲目搜索不带 singerid）
    Q_INVOKABLE void openArtist(const QString& id, const QString& name);
    Q_INVOKABLE void openAlbum(const QString& id, const QString& name);
    Q_INVOKABLE void back();                   // 从歌手/专辑页返回上一屏
    Q_INVOKABLE void loadMore();               // 搜索结果 / 歌手页加载更多
    Q_INVOKABLE void play(int row);            // 下载并播放
    Q_INVOKABLE void download(int row);        // 只下载
    Q_INVOKABLE void beginLogin();             // 生成二维码
    Q_INVOKABLE void cancelLogin();
    Q_INVOKABLE void logout();
    Q_INVOKABLE void clearTempCache();   // 清掉「听完即删」留下的临时文件
    // 上次退出时正在播的在线曲目：按 id 重新解析，恢复到原位置（暂停）
    void restoreStream(const QVariantMap& identity, double position);
    Q_INVOKABLE void openDownloadDir();

signals:
    void listsChanged();
    void tracksChanged();
    // 歌曲列表真的换了一批（不是只改了标题）：界面据此做一次整体淡入
    void tracksReset();
    void viewChanged();
    void busyChanged();
    void statusChanged();
    void sourceChanged();
    void keywordChanged();
    void accountChanged();
    void loginChanged();
    void downloadDirChanged();
    void qualityChanged();
    void progressChanged();
    void autoSaveChanged();
    void playingChanged();
    void toast(const QString& text);

private:
    void LoadSettings();
    void SaveAccount();
    online_core::Provider* ProviderFor(int src);
    void RunAsync(std::function<void()> fn, bool cancelPrevious = true);
    void SetBusy(bool b);
    void SetStatus(const QString& s);
    void SetProgress(int p);
    QString EnsureDownloadDir() const;
    void PushSnapshot();
    // 换了一批歌曲后，把"正在播放"的行号按 歌名+歌手 重新对上（对不上就清掉）
    void SyncPlayingRow();
    // 全局断网时拦下所有联网操作（返回 true 表示已拦下）
    bool Blocked();
    // 流式源只能跳到已下载的范围：恢复进度时反复试，直到数据追上
    void ApplyResumeSeek(double target);
    // 听完即删模式用的临时目录：不放进曲库目录，免得用户以为收藏了
    QString CacheDir() const;
    void PurgeCacheDir();
    // 下载完成后入库并播放；path 为空表示只下载
    void ImportAndPlay(const QString& path, bool playAfter);
    void PollLogin();

    LibraryService* lib_ = nullptr;
    PlayerController* player_ = nullptr;
    std::unique_ptr<online_core::Provider> kg_;
    std::unique_ptr<online_core::Provider> ne_;
    online_core::Account account_;
    online_core::Quality quality_ = online_core::Quality::Flac;
    bool autoSave_ = true;
    int playing_ = -1;
    int pending_ = -1;
    double resumeSeek_ = -1.0;    // 恢复会话时要跳到的位置
    bool restorePaused_ = false;  // 恢复的那一次不要自动出声
    // 正在播放的那首歌的身份：列表换了以后靠它重新定位，而不是沿用旧下标
    QString playingKey_;

    QVariantList lists_;
    QVariantList tracks_;
    QVariantList artists_;
    QVariantList albums_;
    int viewMode_ = 0;   // 0 榜单 / 1 搜索 / 2 歌手 / 3 专辑
    // 详情页头部
    QString detailTitle_, detailSubtitle_, detailCover_, detailMeta_;
    QString detailId_;
    bool canMore_ = false;
    // 进详情页之前的界面快照：返回时直接还原，不再发一次请求
    struct Snapshot {
        int mode = 0;
        QVariantList tracks, artists, albums;
        std::vector<online_core::Track> raw;
        QString listName, keyword;
        int listRow = -1;
    };
    std::vector<Snapshot> stack_;
    int listRow_ = -1;   // 当前打开的榜单下标：搜索完返回浏览态要原样拉回来
    std::vector<online_core::Track> raw_;

    bool busy_ = false;
    int source_ = 0;
    int progress_ = 0;
    QString status_;
    QString listName_;
    QString keyword_;
    QString downloadDir_;
    QString qrImage_;
    QString loginHint_;
    bool loginVisible_ = false;
    QString pendingPlayPath_;
    int pendingPlayTries_ = 0;

    QTimer loginTimer_;
    QTimer importTimer_;
    std::string qrKey_;

    // 单条工作线程 + 一个「已交出执行权」的回收表。
    // 关键：绝不在 UI 线程 join —— 上一轮请求还在飞时 join 会把界面卡住，
    // 那就是「点榜单要顿一下」的根源。新任务在 worker 里等上一轮收工。
    std::thread worker_;
    struct RetiredJob {
        std::thread th;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<RetiredJob> retired_;
    std::shared_ptr<std::atomic<bool>> lastDone_;
    std::shared_ptr<std::atomic<bool>> lastAbort_;
    void ReapFinished();
    std::atomic<bool> cancel_{false};
    // 轮询用：上一拍还没回来就跳过这一拍。
    // 注意不能用 worker_.joinable() 判断——线程跑完之后 joinable() 仍然是 true，
    // 那样会永远跳过，表现就是"扫了码界面一点反应都没有"。
    std::atomic<bool> pollInFlight_{false};
};
