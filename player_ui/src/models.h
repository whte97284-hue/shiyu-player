#pragma once
#include <QAbstractListModel>
#include <unordered_map>
#include <QString>
#include <memory>

#include "library_service.h"

// 专辑墙
class PlaylistService;

// 专辑墙:可排序 / 按音质筛选;行号经 order_ 映射到快照里的专辑下标
class AlbumModel : public QAbstractListModel {
    Q_OBJECT
    // 0 标题 / 1 歌手 / 2 年份 (新→旧) / 3 最近添加 / 4 最近播放
    Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode NOTIFY viewChanged)
    // 0 全部 / 1 无损 / 2 Hi-Res / 3 有损
    Q_PROPERTY(int filter READ filter WRITE setFilter NOTIFY viewChanged)
    Q_PROPERTY(int count READ count NOTIFY viewChanged)
    Q_PROPERTY(int total READ total NOTIFY viewChanged)
    // 「418 张专辑 · 4,906 首 · 13 天 2 小时」,筛选时「126 / 418 张专辑 · …」
    Q_PROPERTY(QString summary READ summary NOTIFY viewChanged)
    // 字母索引 (按标题 / 歌手排序时),按年份时为年代,其余排序为空
    Q_PROPERTY(QStringList letters READ letters NOTIFY viewChanged)
public:
    enum Roles { TitleRole = Qt::UserRole + 1, ArtistRole, YearRole, CountRole, DurationRole, BadgeRole, CoverRole, IndexRole, KeyRole, LetterRole };
    AlbumModel(LibraryService* lib, PlaylistService* playlists, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int sortMode() const { return sort_; }
    void setSortMode(int m);
    int filter() const { return filter_; }
    void setFilter(int f);
    int count() const { return static_cast<int>(order_.size()); }
    int total() const { return static_cast<int>(snap_->albums.size()); }
    QString summary() const;
    QStringList letters() const { return letters_; }
    // 第 row 行对应的专辑下标 (随机来一张等)
    Q_INVOKABLE int albumAt(int row) const;
    // 第 row 行的封面路径。封面墙是虚拟化的二维画布:格子按需绑定,
    // 不能靠 delegate 的 model 上下文取字段,所以这里单独给一个取值入口。
    Q_INVOKABLE QString coverAt(int row) const;
    // 专辑键 (专辑名 + 所在文件夹,重扫后不变) 在当前排序里的行号;找不到为 -1
    Q_INVOKABLE int rowOfKey(const QString& key) const;
    Q_INVOKABLE int indexOfLetter(const QString& letter) const;
    // 「最近播放」排序在专辑墙重新显示时才按新的播放历史重排 (避免卡片在鼠标下跳动)
    Q_INVOKABLE void refreshRecent();
    // 横幅右侧的专辑带:最近播放过的 / 最近添加的 n 张 (专辑摘要,同 library.albumInfo)
    Q_INVOKABLE QVariantList recentlyPlayed(int n, int exclude = -1) const;
    Q_INVOKABLE QVariantList recentlyAdded(int n, int exclude = -1) const;

signals:
    void viewChanged();

private:
    void reload();
    void rebuild();
    QString letterOf(const AlbumInfo& a) const;
    LibraryService* lib_;
    PlaylistService* playlists_;
    std::shared_ptr<const LibrarySnapshot> snap_;
    std::vector<int> order_;
    std::vector<QString> rowLetter_;
    QStringList letters_;
    int sort_ = 0;
    int filter_ = 0;
    bool recentDirty_ = false;
};

// 专辑详情的曲目列表
class TrackListModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int albumIndex READ albumIndex WRITE setAlbumIndex NOTIFY albumChanged)
    Q_PROPERTY(QString title READ title NOTIFY albumChanged)
    Q_PROPERTY(QString artist READ artist NOTIFY albumChanged)
    Q_PROPERTY(QString year READ year NOTIFY albumChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY albumChanged)
    Q_PROPERTY(QString cover READ cover NOTIFY albumChanged)
    Q_PROPERTY(QStringList badges READ badges NOTIFY albumChanged)
    Q_PROPERTY(bool multiDisc READ multiDisc NOTIFY albumChanged)
    // 整张专辑共同的音质 (每首都一样时);不一样时为空,曲目行只给与众不同的那几首标音质
    Q_PROPERTY(QString commonQuality READ commonQuality NOTIFY albumChanged)
    // 格式说明:「FLAC · 24 bit / 96 kHz」;混合时给出范围
    Q_PROPERTY(QString formatText READ formatText NOTIFY albumChanged)
    Q_PROPERTY(QString codecText READ codecText NOTIFY albumChanged) // 「FLAC」「FLAC / MP3」
    Q_PROPERTY(QString specText READ specText NOTIFY albumChanged)   // 「24 bit / 96 kHz」「16–24 bit / 44.1–96 kHz」
    Q_PROPERTY(QString genre READ genre NOTIFY albumChanged)
    // 页尾的专辑资料:{ size, folder, folderCount, firstTrackId, albumGain, discCount }
    Q_PROPERTY(QVariantMap details READ details NOTIFY albumChanged)
    // 每张碟:[{ disc, count, duration, firstRow }] (按曲目顺序)
    Q_PROPERTY(QVariantList discs READ discs NOTIFY albumChanged)
    // 全部曲目 ID (整张加入队列 / 歌单 / 收藏)
    Q_PROPERTY(QVariantList trackIds READ trackIds NOTIFY albumChanged)
public:
    enum Roles { TrackIdRole = Qt::UserRole + 1, NumberRole, TitleRole, ArtistRole, DurationRole, QualityRole, DiscRole };
    explicit TrackListModel(LibraryService* lib, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int albumIndex() const { return album_; }
    void setAlbumIndex(int i);
    QString title() const;
    QString artist() const;
    QString year() const;
    QString summary() const;
    QString cover() const;
    QStringList badges() const;
    bool multiDisc() const;
    QString commonQuality() const;
    QString formatText() const;
    QString codecText() const;
    QString specText() const;
    QString genre() const;
    QVariantMap details() const;
    QVariantList discs() const;
    QVariantList trackIds() const;

signals:
    void albumChanged();

private:
    const AlbumInfo* current() const;
    // 重新扫描后在新快照里找回同一张专辑 (曲目 ID 与专辑下标每次扫描都会变)
    void reresolve();
    LibraryService* lib_;
    std::shared_ptr<const LibrarySnapshot> snap_;
    int album_ = -1;
};

// 歌手墙
class ArtistModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QStringList letters READ letters NOTIFY lettersChanged)
public:
    enum Roles { NameRole = Qt::UserRole + 1, SummaryRole, CoverRole, IndexRole, LetterRole };
    explicit ArtistModel(LibraryService* lib, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    QStringList letters() const { return letters_; }
    Q_INVOKABLE int indexOfLetter(const QString& letter) const;

signals:
    void lettersChanged();

private:
    void reload();
    LibraryService* lib_;
    std::shared_ptr<const LibrarySnapshot> snap_;
    QStringList letters_;
};

// 通用曲目列表:source = "all" (全部歌曲,按标题拼音) / "artist" (某位歌手) / "playlist" (某个歌单) / "history" (播放历史)
class TrackCollectionModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(int artistIndex READ artistIndex WRITE setArtistIndex NOTIFY sourceChanged)
    Q_PROPERTY(QString playlistId READ playlistId WRITE setPlaylistId NOTIFY sourceChanged)
    Q_PROPERTY(int count READ count NOTIFY contentChanged)
    Q_PROPERTY(QStringList letters READ letters NOTIFY contentChanged)
    // 「全部」的排序:0 标题 · 1 歌手 · 2 专辑 · 3 最近添加 · 4 时长 · 5 音质 (记在 QSettings songs/sort)
    Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    // 是否有分节 (标题 / 歌手 / 专辑按首字母,音质按品质);最近添加与时长不分节
    Q_PROPERTY(bool sectioned READ sectioned NOTIFY contentChanged)
public:
    enum Roles {
        TrackIdRole = Qt::UserRole + 1, TitleRole, ArtistRole, AlbumRole, AlbumIndexRole,
        DurationRole, QualityRole, CoverRole, LetterRole, NumberRole
    };
    TrackCollectionModel(LibraryService* lib, PlaylistService* playlists, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString source() const { return source_; }
    void setSource(const QString& s);
    int artistIndex() const { return artist_; }
    void setArtistIndex(int i);
    QString playlistId() const { return playlist_; }
    void setPlaylistId(const QString& id);
    int count() const { return static_cast<int>(rows_.size()); }
    QStringList letters() const { return letters_; }
    int sortMode() const { return sortMode_; }
    void setSortMode(int m);
    bool sectioned() const { return !rowLetters_.empty(); }

    Q_INVOKABLE QVariantList trackIds() const;
    Q_INVOKABLE int indexOfLetter(const QString& letter) const;
    Q_INVOKABLE int rowOfTrack(double trackId) const;

signals:
    void sourceChanged();
    void contentChanged();
    void sortModeChanged();

private:
    void reload();
    void sortAll();
    LibraryService* lib_;
    PlaylistService* playlists_;
    std::shared_ptr<const LibrarySnapshot> snap_;
    QString source_ = QStringLiteral("all");
    int artist_ = -1;
    QString playlist_;
    std::vector<const TrackItem*> rows_;
    std::vector<QString> rowLetters_;
    QStringList letters_;
    int sortMode_ = 0;
    // 歌手 / 专辑的排序键 (拼音),按快照缓存:换排序时不重算
    const LibrarySnapshot* keySnap_ = nullptr;
    std::unordered_map<const TrackItem*, std::pair<QString, QString>> keys_;
};

// 搜索:曲目为列表行;专辑、歌手、最佳匹配作为属性提供
class SearchModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int count READ count NOTIFY resultsChanged)
    // 匹配的歌曲总数 (列表最多只列 500 首)
    Q_PROPERTY(int total READ total NOTIFY resultsChanged)
    Q_PROPERTY(QVariantList albums READ albums NOTIFY resultsChanged)
    Q_PROPERTY(QVariantList artists READ artists NOTIFY resultsChanged)
    // 最佳匹配:kind (track / album / artist) + 对应字段
    Q_PROPERTY(QVariantMap top READ top NOTIFY resultsChanged)
public:
    enum Roles { TrackIdRole = Qt::UserRole + 1, TitleRole, ArtistRole, AlbumRole, AlbumIndexRole, DurationRole, QualityRole, CoverRole };
    explicit SearchModel(LibraryService* lib, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString query() const { return query_; }
    void setQuery(const QString& q);
    int count() const { return static_cast<int>(rows_.size()); }
    int total() const { return total_; }
    QVariantList albums() const { return albums_; }
    QVariantList artists() const { return artists_; }
    QVariantMap top() const { return top_; }
    Q_INVOKABLE QVariantList trackIds() const;

signals:
    void queryChanged();
    void resultsChanged();

private:
    void run();
    LibraryService* lib_;
    std::shared_ptr<const LibrarySnapshot> snap_;
    QString query_;
    std::vector<const TrackItem*> rows_;
    int total_ = 0;
    QVariantList albums_;
    QVariantList artists_;
    QVariantMap top_;
};
