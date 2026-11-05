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

// 歌词补全服务:把 player_core 的 LyricBatch 包成有进度的后台任务
//
// 和补封面的区别:
//   · 按**曲目**而不是专辑 —— 歌词本来就是一首一份,没有"专辑歌词"这种东西
//   · 不需要重扫曲库 —— 歌词不进快照,播放时按文件现查,写完下次放就有了
//   · 补完不用重建索引,也不会有"快照说没有、磁盘其实有"的偏差
class LyricService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int done READ done NOTIFY progressChanged)
    Q_PROPERTY(int total READ total NOTIFY jobChanged)
    Q_PROPERTY(int okCount READ okCount NOTIFY jobChanged)
    Q_PROPERTY(int failCount READ failCount NOTIFY jobChanged)
    // 已有歌词、这次没动的曲目数
    Q_PROPERTY(int skipCount READ skipCount NOTIFY jobChanged)
    // 还缺歌词的曲目数(曲库变化时更新;横幅按钮上的小圆点绑它)
    Q_PROPERTY(int missingCount READ missingCount NOTIFY missingCountChanged)
    // 正在处理哪一首(面板上滚动显示,让用户知道没卡住)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY progressChanged)
    Q_PROPERTY(int elapsedSec READ elapsedSec NOTIFY progressChanged)
    Q_PROPERTY(int etaSec READ etaSec NOTIFY progressChanged)
    Q_PROPERTY(int threadCount READ threadCount NOTIFY jobChanged)
    // 上一次任务的失败明细:[{title, reason}]
    Q_PROPERTY(QVariantList failures READ failures NOTIFY jobChanged)

public:
    explicit LyricService(LibraryService* lib, QObject* parent = nullptr);
    ~LyricService() override;

    bool busy() const { return busy_; }
    double progress() const { return progress_; }
    int done() const { return done_; }
    int total() const { return total_; }
    int okCount() const { return ok_; }
    int failCount() const { return fail_; }
    int skipCount() const { return skip_; }
    QString currentTitle() const { return current_; }
    QVariantList failures() const { return failures_; }
    int threadCount() const { return threadCount_; }
    int elapsedSec() const { return busy_ && elapsed_.isValid() ? static_cast<int>(elapsed_.elapsed() / 1000) : 0; }
    int etaSec() const {
        if (!busy_ || done_ <= 0 || total_ <= 0 || !elapsed_.isValid()) return -1;
        const double perItem = (elapsed_.elapsed() / 1000.0) / done_;
        return static_cast<int>(perItem * (total_ - done_));
    }

    // 全部曲目的歌词状态:[{id, title, artist, album, hasLrc}]
    // 按「缺歌词的在前」排,面板列表默认只看缺的
    Q_INVOKABLE QVariantList trackStates() const;
    int missingCount() const;

    // 补歌词:ids 为空 = 所有还缺歌词的曲目
    Q_INVOKABLE void fetchTracks(const QVariantList& ids);
    Q_INVOKABLE void cancel();
    // 单个曲目当前有没有同名 .lrc(面板刷新用)
    Q_INVOKABLE bool hasLrc(double trackId) const;

signals:
    void jobChanged();
    void progressChanged();
    void finished(int ok, int failed);
    void missingCountChanged();

private:
    struct Job {
        uint64_t id = 0;
        std::wstring path;
        std::wstring title;
        std::wstring artist;
        std::wstring album;
        uint32_t duration_ms = 0;
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
    int threadCount_ = 0;
    QVariantList failures_;
    QString current_;
    QElapsedTimer elapsed_;
    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    std::shared_ptr<std::atomic<int>> liveDone_;
    mutable int missingCache_ = -1;
    // trackStates() 会为每首歌 stat 一次同名 .lrc,几百首在主线程做会卡;
    // 曲库没变就复用上次的结果
    mutable QVariantList statesCache_;
};
