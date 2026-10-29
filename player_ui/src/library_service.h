#pragma once
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "music_catalog.hpp"

struct IngestionSummary;
struct EncryptedFileInfo;

// 一张专辑 (同一目录 + 同一专辑名 聚合),曲目指针指向所属快照中的 MusicCatalog,快照存活期间稳定
struct AlbumInfo {
    QString title;
    QString artist;
    QString year;
    std::vector<const TrackItem*> tracks; // 按 碟号 / 曲号 / CUE 起点 / 文件名 排序
    const TrackItem* coverTrack = nullptr;
    uint64_t totalMs = 0;
    uint32_t maxRate = 0;
    uint16_t maxBits = 0;
    bool lossless = true;
    bool hires = false;
    bool dsd = false;
    bool cue = false;
    QString dsdCodec;
    QString sortKey;       // 拼音排序键
    QString artistSortKey; // 按歌手排序用 (群星排最后)
    uint64_t addedTime = 0; // 最新一首的入库时间 (文件修改时间)
};

// 歌手:由曲目歌手字段拆分 (A / B、A、B、feat.) 聚合
struct ArtistInfo {
    QString name;
    std::vector<int> albums;              // 参与的专辑下标 (年份新→旧)
    std::vector<const TrackItem*> tracks; // 按专辑、曲序排列
    int coverAlbum = -1;                  // 头像所用专辑
    QString sortKey;
    QString letter;                       // 索引字母 A-Z / #
};

// 不可变的曲库快照:扫描完成后整体替换,界面线程与后台线程互不干扰
struct LibrarySnapshot {
    MusicCatalog catalog;
    std::vector<AlbumInfo> albums;
    std::unordered_map<uint64_t, int> trackAlbum; // 曲目 ID -> 专辑下标
    std::vector<ArtistInfo> artists;              // 按拼音排序
    std::vector<const TrackItem*> songs;          // 全部歌曲,按标题拼音排序
    std::vector<QString> songLetters;             // 与 songs 一一对应
    // 显示去重 (只影响显示,不动文件):被收起的同一首歌的其他副本 → 显示的那份 (音质最好的)。
    // 被收起的仍在 catalog 里,收藏 / 歌单里已有的照常能找到
    std::unordered_map<uint64_t, uint64_t> shownCopy;
    // 内容指纹 (与曲目顺序无关):重扫结果与当前一致时沿用旧快照,不通知界面刷新
    uint64_t signature = 0;
};

// 排序键 (拼音 + 小写原文) 与索引字母
QString SortKeyOf(const std::wstring& text);
QString LetterOf(const QString& sortKey);
// 曲目的持久化标识:路径 + CUE 起点 (曲目 ID 每次扫描重新分配,不能用于持久化)
QString TrackKeyOf(const TrackItem& t);
// 路径比较键 (分隔符统一为 /,Windows 不分大小写),用于整理 / 查重后的「旧路径 → 新路径」映射
QString PathKeyOf(const QString& path);
// 把曲目标识 (路径#CUE 起点) 里的路径按映射替换;不在映射里返回空
QString RemapTrackKey(const QString& key, const QHash<QString, QString>& pathMap);

class LibraryService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    // 需要在界面上提示的扫描:文件夹监控触发的后台重扫不提示 (没有变化时界面完全不动)
    Q_PROPERTY(bool scanShown READ scanShown NOTIFY scanningChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY progressChanged)
    Q_PROPERTY(int trackCount READ trackCount NOTIFY libraryChanged)
    Q_PROPERTY(int albumCount READ albumCount NOTIFY libraryChanged)
    Q_PROPERTY(int generation READ generation NOTIFY libraryChanged)
    Q_PROPERTY(QStringList folders READ folders NOTIFY foldersChanged)
    // 隐藏的文件夹:其中的歌曲不出现在专辑 / 歌手 / 歌曲 / 搜索里,但收藏与歌单里已有的照常可见可播
    Q_PROPERTY(QStringList hiddenFolders READ hiddenFolders NOTIFY foldersChanged)
    // 文件夹备注 (路径 → 备注),只用于在音乐文件夹列表里区分各个文件夹
    Q_PROPERTY(QVariantMap folderNotes READ folderNotes NOTIFY foldersChanged)
    // 导入进度 (扫描期间实时刷新):已处理 / 总数 / 当前文件
    Q_PROPERTY(int scanDone READ scanDone NOTIFY progressChanged)
    Q_PROPERTY(int scanTotal READ scanTotal NOTIFY progressChanged)
    Q_PROPERTY(QString scanFile READ scanFile NOTIFY progressChanged)
    // 上次扫描结果:tracks / albums / failed / skipped / seconds / incremental / cancelled
    Q_PROPERTY(QVariantMap lastScan READ lastScan NOTIFY scanningChanged)
    // 单独加入曲库的歌曲 (转换页「加入曲库」、拖入的加密文件转换结果),不属于任何音乐文件夹
    Q_PROPERTY(int extraFileCount READ extraFileCount NOTIFY foldersChanged)
    // 实时监控音乐文件夹:有新歌、删除、改名时自动重扫 (增量,几乎不读盘)
    Q_PROPERTY(bool watchFolders READ watchFolders WRITE setWatchFolders NOTIFY watchFoldersChanged)
    // 「只从曲库移除」的文件 (查找重复时选择不动磁盘):扫描照常,但不出现在专辑 / 歌手 / 歌曲 / 搜索里
    Q_PROPERTY(int excludedFileCount READ excludedFileCount NOTIFY foldersChanged)
    // 显示去重:同一首歌 (歌手 + 歌名相同、时长相差不到 3 秒) 在专辑 / 歌手 / 歌曲 / 搜索 / 歌单里只显示音质最好的一份
    Q_PROPERTY(bool displayDedupe READ displayDedupe WRITE setDisplayDedupe NOTIFY displayDedupeChanged)

public:
    explicit LibraryService(QObject* parent = nullptr);
    ~LibraryService() override;

    static LibraryService* instance();

    bool scanning() const { return scanning_; }
    bool scanShown() const { return scanning_ && !quiet_; }
    double progress() const { return progress_; }
    QString statusText() const { return status_; }
    int trackCount() const;
    int albumCount() const;
    int generation() const { return generation_; }
    QStringList folders() const { return folders_; }
    QStringList hiddenFolders() const { return hiddenFolders_; }
    int scanDone() const { return scanDone_; }
    int scanTotal() const { return scanTotal_; }
    QString scanFile() const { return scanFile_; }
    QVariantMap lastScan() const { return lastScan_; }
    // 上次扫描发现、尚未解密的加密源 (扫描结束时更新)
    const std::vector<EncryptedFileInfo>& pendingEncrypted() const { return *pending_; }
    int extraFileCount() const { return static_cast<int>(extraFiles_.size()); }
    const QStringList& extraFiles() const { return extraFiles_; }
    bool watchFolders() const { return watch_; }
    int excludedFileCount() const { return static_cast<int>(excludedFiles_.size()); }
    const QStringList& excludedFiles() const { return excludedFiles_; }
    void excludeFiles(const QStringList& paths);
    void includeFiles(const QStringList& paths);
    Q_INVOKABLE void clearExcludedFiles();
    // 进后台(最小化/托盘)时释放可重建的缓存,并把工作集换出到页面文件。
    // 只放"能重新算出来"的东西:缩略图缓存 + 工作集,曲库/索引不动。
    Q_INVOKABLE void releaseMemory();
    void setWatchFolders(bool on);
    bool displayDedupe() const { return displayDedupe_; }
    void setDisplayDedupe(bool on);
    static QString dataDir();
    // 全部转换产物的唯一输出目录 (设置项 decrypt/outputDir,默认 我的音乐\Decrypted)
    static QString decryptOutputDir();

    // 线程安全:获取当前快照 (持有 shared_ptr 期间快照不会被释放)
    std::shared_ptr<const LibrarySnapshot> snapshot() const;

    // 添加文件夹并立即扫描;扫描进行中时排队,当前扫描结束后自动再扫一次
    Q_INVOKABLE void addFolder(const QString& path);
    // 接受文件对话框 / 拖放给出的 file:// URL (或本地路径),返回实际添加的文件夹数
    Q_INVOKABLE int addFolderUrls(const QVariantList& urls);
    Q_INVOKABLE void removeFolder(const QString& path);
    // 单独加入曲库的文件 (已在曲库里的不重复加),返回新加入的数量
    Q_INVOKABLE int addFiles(const QStringList& paths);
    Q_INVOKABLE void clearExtraFiles();
    // 酷狗音乐的下载目录 (读酷狗配置 + 各磁盘下的 KuGou 目录),已加入或用户忽略过的不再列出
    Q_INVOKABLE QStringList kugouCandidates() const;
    Q_INVOKABLE int addKugouFolders(const QStringList& paths);
    Q_INVOKABLE void dismissKugouCandidates(const QStringList& paths);
    Q_INVOKABLE bool isKugouFolder(const QString& path) const;
    Q_INVOKABLE void setFolderHidden(const QString& path, bool hidden);
    // 备注为空 = 删除备注
    Q_INVOKABLE void setFolderNote(const QString& path, const QString& note);
    QVariantMap folderNotes() const { return folderNotes_; }
    Q_INVOKABLE void rescan();
    // 整理 / 撤销移动了文件:把扫描缓存里的解析结果挪到新路径,随后的重扫不必再逐个解析这些文件。
    // 正在扫描时不做 (扫描结束会覆盖缓存文件)
    void carryScanCache(const std::vector<std::pair<std::wstring, std::wstring>>& moves, bool keepSource);
    Q_INVOKABLE void cancelScan();
    Q_INVOKABLE QString qualityBadge(int albumIndex) const;
    // 今日推荐:有封面的 Hi-Res 专辑优先;同一天内只要那张专辑还在就不变,skip 依次换下一张
    Q_INVOKABLE int featuredAlbum(int skip = 0) const;
    // 专辑摘要:title / artist / year / cover / badge / summary / trackCount
    Q_INVOKABLE QVariantMap albumInfo(int albumIndex) const;
    // 歌手摘要:name / cover / summary / albumCount / trackCount
    Q_INVOKABLE QVariantMap artistInfo(int artistIndex) const;
    // 歌手参与的专辑 (albumInfo 列表)
    Q_INVOKABLE QVariantList artistAlbums(int artistIndex) const;
    // 按名字找歌手 (找不到返回 -1);曲目所属专辑 / 首位歌手
    Q_INVOKABLE int artistIndexOf(const QString& name) const;
    Q_INVOKABLE int albumOfTrack(double trackId) const;
    // 专辑的全部曲目 ID (按曲序);专辑墙上的右键菜单等没有绑定曲目列表的地方用
    Q_INVOKABLE QVariantList albumTrackIds(int albumIndex) const;
    // 歌手的全部曲目 ID (歌手墙 / 搜索的播放键、右键菜单用,不动歌手详情的列表模型)
    Q_INVOKABLE QVariantList artistTrackIds(int artistIndex) const;
    Q_INVOKABLE int artistOfTrack(double trackId) const;
    // 曲目详情 (右键「歌曲信息」):标签、文件路径、格式、采样率、码率、大小等;找不到返回空表
    Q_INVOKABLE QVariantMap trackInfo(double trackId) const;
    // 在资源管理器中打开曲目所在文件夹并选中该文件 (非 Windows 只打开文件夹)
    Q_INVOKABLE bool revealTrack(double trackId) const;
    Q_INVOKABLE void copyToClipboard(const QString& text) const;

    static QString formatDuration(uint64_t ms);
    // 专辑 / 歌手总时长:「52 分钟」「1 小时 8 分钟」(不写成 1:08:00,免得像时间点)
    static QString formatLongDuration(uint64_t ms);
    static QString trackQuality(const TrackItem& t);

    // 把堆里的空闲块交还系统 (载入曲库、查重整理这类重活之后调用)
    static void ReleaseFreeHeap();

signals:
    void scanningChanged();
    void progressChanged();
    void libraryChanged();
    void foldersChanged();
    void pendingEncryptedChanged();
    void watchFoldersChanged();
    void displayDedupeChanged();

private:
    void startScan();
    void finishScan(std::shared_ptr<LibrarySnapshot> snap, const IngestionSummary& sum);
    void backgroundRescan();
    void saveFolders();
    static std::shared_ptr<LibrarySnapshot> BuildAlbums(std::unique_ptr<MusicCatalog> catalog, const QStringList& hidden = {},
                                                        const QStringList& excluded = {}, bool collapseCopies = false,
                                                        const QStringList& folderOrder = {});

    mutable std::mutex snap_mutex_;
    std::shared_ptr<const LibrarySnapshot> snap_;
    QStringList folders_;
    QStringList hiddenFolders_;
    bool scanning_ = false;
    bool quiet_ = false;        // 当前扫描是文件夹监控触发的后台重扫
    bool quietPending_ = true;  // 排队中的重扫是否全部来自文件夹监控
    double progress_ = 0.0;
    QString status_;
    int generation_ = 0;
    int scanDone_ = 0;
    int scanTotal_ = 0;
    QString scanFile_;
    QVariantMap lastScan_;
    bool rescanPending_ = false;
    QStringList extraFiles_;
    QStringList excludedFiles_;
    QVariantMap folderNotes_;
    QStringList kugouFolders_;
    bool watch_ = true;
    bool displayDedupe_ = false;
    class FolderWatcher* watcher_ = nullptr;
    class QTimer* watchDebounce_ = nullptr;
    void updateWatcher();
    std::shared_ptr<std::vector<EncryptedFileInfo>> pending_;
    std::unique_ptr<class ScanJob> job_;
};
