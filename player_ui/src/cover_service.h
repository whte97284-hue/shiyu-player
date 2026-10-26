#pragma once
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class LibraryService;

// 专辑封面服务:把「补封面」做成可批量、有进度的后台任务
//
// 为什么按「专辑」而不是按「曲目」:
//   曲库里**没有专辑封面这个独立实体**。AlbumInfo 只有一个 coverTrack 指针,
//   取的是该专辑里第一首「有封面」的曲目 (library_service.cpp:735),
//   一首都没有时退回第一首 (747) —— 也就是显示占位图。
//   所以一张专辑只要有一首带封面,专辑墙就有图,但其余曲目仍然是空的
//   (拷到别的播放器就没封面)。要真正修好,必须写进这张专辑的**每一首**。
//   而且一张专辑只需联网查一次,图片复用给全部曲目。
class CoverService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int done READ done NOTIFY progressChanged)
    Q_PROPERTY(int total READ total NOTIFY jobChanged)
    Q_PROPERTY(int okCount READ okCount NOTIFY jobChanged)
    Q_PROPERTY(int failCount READ failCount NOTIFY jobChanged)
    // 跳过数:已有封面 / 非 FLAC / 无标题 —— 不算失败,但也确实没动文件
    Q_PROPERTY(int skipCount READ skipCount NOTIFY jobChanged)
    // 有缺封面的专辑数 (曲库变化时更新;横幅按钮上的小圆点绑它)
    Q_PROPERTY(int missingCount READ missingCount NOTIFY missingCountChanged)
    // 上一次任务的失败明细:[{title, reason}] —— 给「错误」按钮看,排查用
    Q_PROPERTY(QVariantList failures READ failures NOTIFY jobChanged)
public:
    explicit CoverService(LibraryService* lib, QObject* parent = nullptr);
    ~CoverService() override;

    bool busy() const { return busy_; }
    double progress() const { return progress_; }
    int done() const { return done_; }
    int total() const { return total_; }
    int okCount() const { return ok_; }
    int failCount() const { return fail_; }
    int elapsedSec() const { return busy_ && elapsed_.isValid() ? static_cast<int>(elapsed_.elapsed() / 1000) : 0; }
    int etaSec() const {
        if (!busy_ || done_ <= 0 || total_ <= 0 || !elapsed_.isValid()) return -1;
        const double perItem = (elapsed_.elapsed() / 1000.0) / done_;
        return static_cast<int>(perItem * (total_ - done_));
    }
    int threadCount() const { return threadCount_; }
    int skipCount() const { return skip_; }

    // 全部专辑的封面状态:[{index, title, cover, total, missing}]
    //   只返回**缺封面**的专辑 (missing > 0) —— 已齐的不进面板,否则专辑一多就卡
    Q_INVOKABLE QVariantList albumStates() const;
    // 有缺封面的专辑数
    int missingCount() const;

    // 上一次任务的失败明细 (给「错误」按钮看,排查用)
    QVariantList failures() const { return failures_; }
    // 跳过明细:[{title, reason}] —— 不算失败,但用户必须能看到"为什么这张没动"
    Q_PROPERTY(QVariantList skips READ skips NOTIFY jobChanged)
    QVariantList skips() const { return skips_; }
    // 需要重建索引:有文件磁盘上已经带封面,但本机曲库快照还没认到。
    // 只有删掉 scan.cache 做全量重建才会刷新这些条目 —— 面板据此给出重建入口。
    Q_PROPERTY(bool needRebuild READ needRebuild NOTIFY jobChanged)
    bool needRebuild() const { return !alreadied_.isEmpty(); }
    // 这类专辑的具体名单(提示里告诉用户是哪几张)
    Q_PROPERTY(QVariantList alreadyCovered READ alreadyCovered NOTIFY jobChanged)
    QVariantList alreadyCovered() const { return alreadied_; }
    // 正在处理哪一张(面板上滚动显示,让用户知道没卡住)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY progressChanged)
    QString currentTitle() const { return current_; }
    // 已用时间(秒) / 估算剩余(秒,-1 = 算不出)
    Q_PROPERTY(int elapsedSec READ elapsedSec NOTIFY progressChanged)
    Q_PROPERTY(int etaSec READ etaSec NOTIFY progressChanged)
    // 并发线程数(面板上显示,让用户知道在并行跑)
    Q_PROPERTY(int threadCount READ threadCount NOTIFY jobChanged)
    // 面板里还有多少张缺封面 (判断"是不是没跑完")
    Q_PROPERTY(int pendingAlbums READ pendingAlbums NOTIFY missingCountChanged)
    int pendingAlbums() const { return pendingAlbums_; }

    // 补封面:indexes 为空 = 所有 missing > 0 的专辑。完成后自动重扫曲库
    Q_INVOKABLE void fetchAlbums(const QVariantList& indexes);
    Q_INVOKABLE void cancel();
    // 打开面板时调用:后台核对"快照说没封面 / 磁盘其实有"的条目,有就置 needRebuild
    Q_INVOKABLE void checkStale();
    // 把失败明细写到文件并打开 (排查错误用)
    Q_INVOKABLE void openFailureLog();

signals:
    void jobChanged();
    void progressChanged();
    void finished(int ok, int failed);
    void missingCountChanged();

private:
    // 开跑前在主线程把需要的数据拷出来,后台线程不碰曲库锁
    struct Job {
        int index = 0;
        std::wstring title;       // 专辑名(搜索用)
        std::wstring probe;       // 任意一首曲目路径(用于 size/md5 校验同一版文件)
        std::wstring searchTitle; // 代表曲目的歌名
        std::wstring searchArtist;
        std::wstring altTitle;    // 文件名后段(标签是罗马字时,原文往往只在这里)
        std::vector<std::wstring> targets; // 需要写入封面的文件
        std::wstring borrowFrom;           // 同作品的另一版本(文件里已有封面),优先向它借
        std::string skipReason;            // 非空 = 不用联网,直接算失败
    };

    void start(std::vector<Job> jobs);

    LibraryService* lib_;
    bool busy_ = false;
    double progress_ = 0;
    int done_ = 0;
    int total_ = 0;
    int ok_ = 0;
    int fail_ = 0;
    int skip_ = 0;
    QVariantList failures_;
    QVariantList skips_;
    QVariantList alreadied_;   // 磁盘上本来就有封面的
    QString current_;          // 正在处理的专辑名
    QElapsedTimer elapsed_;
    int threadCount_ = 0;
    mutable int pendingAlbums_ = 0;
    int lastJobCount_ = 0;
    std::thread worker_;
    std::thread staleWorker_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::shared_ptr<std::atomic<bool>> staleCancel_;
};
