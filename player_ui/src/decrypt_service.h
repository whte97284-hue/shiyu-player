#pragma once
#include <QAbstractListModel>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class LibraryService;

// 待解密队列:曲库扫描中暂不解密的文件 + 手动导入的文件,都在这里待命
// 支持按格式 / 状态筛选与搜索;每行状态 (待命 / 解密中 / 完成 / 失败) 单独刷新,上千行也不卡
class DecryptQueueModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString filterKind READ filterKind WRITE setFilterKind NOTIFY filterChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged)
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
public:
    enum Roles { PathRole = Qt::UserRole + 1, NameRole, TitleRole, ArtistRole, KindRole, SizeRole, SourceRole,
                 StatusRole, MessageRole, OutputRole };
    enum Status { Waiting = 0, Working = 1, Done = 2, Failed = 3 };
    struct Entry {
        QString path, title, artist, kind, message, output;
        qint64 size = 0;
        quint64 mtime = 0;
        bool fromLibrary = false;
        int status = Waiting;
    };
    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString filterKind() const { return kind_; }
    void setFilterKind(const QString& k);
    QString filterText() const { return text_; }
    void setFilterText(const QString& t);

    // 全部条目 (不受筛选影响)
    const std::vector<Entry>& entries() const { return all_; }
    // 合并曲库待解密列表:新增的加入,已不在曲库待解密列表里的曲库条目移除 (完成 / 失败的保留到清除为止)
    void syncLibrary(const std::vector<Entry>& lib);
    bool add(Entry e);
    void remove(const QString& path);
    void setStatus(const QString& path, int status, const QString& message = {}, const QString& output = {});
    void clearFinished();
    // 统计:all / kgm / ncm / kgg / waiting / done / failed
    QVariantMap counts() const;
    // 当前筛选下可见的待命条目路径
    QStringList visibleWaiting() const;

signals:
    void filterChanged();
    void countChanged();
    void statsChanged();

private:
    void refilter();
    bool accept(const Entry& e) const;
    std::vector<Entry> all_;
    std::vector<int> rows_;
    QHash<QString, int> byPath_;
    QString kind_, text_;
};

// 解密服务:把三个解密内核 (KGM / NCM / KGG) 以「工具」的形式提供给界面
//  · 扫描结束发现未解密文件时,按策略询问 (默认) / 自动解密 / 不打扰
//  · 工具页:拖入或选择文件 / 文件夹,解密到输出目录;默认不进曲库,转换完可一键「加入曲库」
//  · 其他页面拖入的加密文件:转换后自动加入曲库
//  · 后台单线程依次处理 (机械盘上顺序读写最快),可随时取消;产物写入索引,下次扫描直接入库
class DecryptService : public QObject {
    Q_OBJECT
    // 0 = 每次询问 / 1 = 自动解密 / 2 = 不解密
    Q_PROPERTY(int policy READ policy WRITE setPolicy NOTIFY policyChanged)
    Q_PROPERTY(QString outputDir READ outputDir WRITE setOutputDir NOTIFY outputDirChanged)
    // 工具列表:id / name / exts / native / available / reason
    Q_PROPERTY(QVariantList decoders READ decoders CONSTANT)
    // 曲库中待解密的文件:total / bytes / kinds:[{id, name, count, available}]
    Q_PROPERTY(QVariantMap pending READ pending NOTIFY pendingChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(int done READ done NOTIFY progressChanged)
    Q_PROPERTY(int total READ total NOTIFY jobChanged)
    Q_PROPERTY(QString current READ current NOTIFY progressChanged)
    // 最近一次任务的结果:name / kind / ok / message / output
    Q_PROPERTY(QVariantList results READ results NOTIFY jobChanged)
    Q_PROPERTY(int okCount READ okCount NOTIFY jobChanged)
    Q_PROPERTY(int failCount READ failCount NOTIFY jobChanged)
    Q_PROPERTY(QObject* queue READ queue CONSTANT)
    // 队列统计:all / kgm / ncm / kgg / waiting / done / failed
    Q_PROPERTY(QVariantMap stats READ stats NOTIFY statsChanged)
    // 已转换、但还没进曲库的结果数 (工具页「加入曲库」按钮)
    Q_PROPERTY(int addableCount READ addableCount NOTIFY statsChanged)

public:
    explicit DecryptService(LibraryService* lib, QObject* parent = nullptr);
    ~DecryptService() override;

    int policy() const { return policy_; }
    void setPolicy(int p);
    QString outputDir() const { return outputDir_; }
    void setOutputDir(const QString& d);
    QVariantList decoders() const;
    QVariantMap pending() const;
    bool busy() const { return busy_; }
    double progress() const { return progress_; }
    int done() const { return done_; }
    int total() const { return total_; }
    QString current() const { return current_; }
    QVariantList results() const { return results_; }
    int okCount() const { return ok_; }
    int failCount() const { return fail_; }
    QObject* queue() { return &queue_; }
    QVariantMap stats() const { return queue_.counts(); }
    int addableCount() const { return static_cast<int>(addableOutputs().size()); }

    // 导入文件 / 文件夹到待解密队列 (不立即解密),返回新增数量
    Q_INVOKABLE int addUrls(const QVariantList& urls);
    // 解密队列中所有待命 (或失败) 的条目;onlyVisible = 只处理当前筛选下可见的
    Q_INVOKABLE void decryptQueue(bool onlyVisible = false);
    Q_INVOKABLE void decryptPaths(const QStringList& paths);
    Q_INVOKABLE void removeFromQueue(const QString& path);
    Q_INVOKABLE void clearFinished();
    Q_INVOKABLE void openOutput(const QString& path);

    // 解密曲库扫描发现的全部待解密文件,完成后自动重扫曲库
    Q_INVOKABLE void decryptPending();
    // 本次不解密:同一批文件不再提示
    Q_INVOKABLE void dismissPending();
    // 工具页:文件或文件夹 (递归查找加密文件)
    Q_INVOKABLE int decryptUrls(const QVariantList& urls);
    Q_INVOKABLE void cancel();
    // 把已转换、还不在曲库里的结果加入曲库,返回加入的数量
    Q_INVOKABLE int addResultsToLibrary();
    Q_INVOKABLE void openOutputFolder();
    Q_INVOKABLE void clearResults();
    Q_INVOKABLE void setOutputUrl(const QVariant& url);

signals:
    void policyChanged();
    void outputDirChanged();
    void pendingChanged();
    void jobChanged();
    void progressChanged();
    // 扫描发现了未解密文件且策略为「询问」
    void promptRequested();
    void finished(int ok, int failed);
    void statsChanged();

private:
    struct Item {
        std::wstring path;
        std::string kind;
        uint64_t size = 0;
        uint64_t mtime = 0;
    };
    void onPendingChanged();
    QStringList addableOutputs() const;
    void start(std::vector<Item> items, bool rescanAfter);
    // 已经答复过 (拒绝或已尝试解密) 的源文件:只有出现新文件时才再次询问
    void markSeen();
    bool hasUnseen() const;
    void loadSeen();
    void saveSeen() const;

    LibraryService* lib_;
    int policy_ = 0;
    QString outputDir_;
    QSet<uint> seen_;
    QSet<QString> toLibrary_; // 转换完自动加入曲库的源文件 (其他页面拖入的)
    QStringList autoAdd_;     // 本次任务里要自动加入曲库的产物
    bool recheck_ = false;    // 自动模式下等文件写完的复查已排队
    bool busy_ = false;
    double progress_ = 0;
    int done_ = 0;
    int total_ = 0;
    QString current_;
    QVariantList results_;
    int ok_ = 0;
    int fail_ = 0;
    std::thread worker_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    DecryptQueueModel queue_;
    std::vector<Item> collect(const QVariantList& urls) const;
};
