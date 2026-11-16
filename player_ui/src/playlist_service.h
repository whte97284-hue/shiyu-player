#pragma once
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>
#include <memory>
#include <unordered_map>
#include <vector>

#include "library_service.h"

// 歌单服务:
//  · 曲目以「路径 + CUE 起点」持久化 (曲目 ID 每次扫描都会重新分配),重新扫描、换盘符前后均可恢复
//  · 内置「我喜欢」歌单 (id = "fav"),不可删除
//  · 存储为 AppData/playlists.json,修改后 300ms 合并写盘,原子替换
class PlaylistService : public QObject {
    Q_OBJECT
    // 歌单列表:id / name / count / duration / covers (最多 4 张专辑封面) / builtin
    Q_PROPERTY(QVariantList playlists READ playlists NOTIFY playlistsChanged)
    // 任意歌单内容变化时递增,QML 绑定以此刷新 (如收藏状态)
    Q_PROPERTY(int revision READ revision NOTIFY playlistsChanged)

public:
    explicit PlaylistService(LibraryService* lib, QObject* parent = nullptr);
    ~PlaylistService() override;

    QVariantList playlists() const;
    int revision() const { return revision_; }

    Q_INVOKABLE QString create(const QString& name);
    Q_INVOKABLE void rename(const QString& id, const QString& name);
    Q_INVOKABLE void remove(const QString& id);
    // 返回实际新增的数量 (已存在的曲目跳过)
    Q_INVOKABLE int addTracks(const QString& id, const QVariantList& trackIds);
    Q_INVOKABLE int addOnlineTrack(const QString& id, const QVariantMap& meta);
    Q_INVOKABLE void removeEntry(const QString& id, double trackId, const QString& onlineKey);
    Q_INVOKABLE void removeTrack(const QString& id, double trackId);
    Q_INVOKABLE void move(const QString& id, int from, int to);
    Q_INVOKABLE bool isFavorite(double trackId) const;
    Q_INVOKABLE bool isFavoriteOnline(const QString& onlineKey) const;
    Q_INVOKABLE void toggleFavorite(double trackId);
    Q_INVOKABLE void toggleFavoriteOnline(const QVariantMap& meta);
    Q_INVOKABLE QVariantMap onlineMeta(const QString& onlineKey) const;
    Q_INVOKABLE QVariantMap info(const QString& id) const;
    // 一个不与现有歌单重名的默认名称
    Q_INVOKABLE QString suggestName() const;
    Q_INVOKABLE QVariantList trackIdsOf(const QString& id) const;
    // 播放历史:最近播放的排最前,同一首只保留最近一次,最多 kHistoryMax 首
    Q_INVOKABLE void recordPlay(double trackId);
    Q_INVOKABLE void recordOnlinePlay(const QVariantMap& meta);
    Q_INVOKABLE void clearHistory();
    std::vector<const TrackItem*> historyTracks() const;
    static constexpr int kHistoryMax = 300;

    // 整理 / 查重之后:把歌单、收藏、播放历史里的曲目按「旧路径 → 新路径」改过去 (键用 PathKeyOf)。
    // 同一歌单里新旧两份都在时只留一份。返回改动的条目数
    int remapPaths(const QHash<QString, QString>& pathMap);

    // 解析后的曲目 (仅包含当前曲库中存在的)
    std::vector<const TrackItem*> tracksOf(const QString& id) const;

    // 在线曲目元数据 (未下载到本地曲库前用于历史/歌单展示与重播)
    struct OnlineTrackMeta {
        int source = 0;
        QString id;
        QString albumId;
        QString title;
        QString artist;
        QString album;
        int durationMs = 0;
        QString cover;
        bool paywalled = false;
    };
    struct Entry {
        const TrackItem* local = nullptr;
        const OnlineTrackMeta* online = nullptr;
        QString key;
    };
    static QString OnlineKeyOf(int source, const QString& id);
    // 混合条目 (已入库优先解析为本地 TrackItem,未下载则保留 OnlineTrackMeta)
    std::vector<Entry> entriesOf(const QString& id) const;
    std::vector<Entry> historyEntries() const;

signals:
    void playlistsChanged();
    // 某个歌单的曲目变化 (详情页据此刷新)
    void contentChanged(const QString& id);
    void historyChanged();

private:
    struct List {
        QString id;
        QString name;
        qint64 created = 0;
        std::vector<QString> keys;
    };
    List* find(const QString& id);
    const List* find(const QString& id) const;
    const TrackItem* resolve(const QString& key) const;
    const OnlineTrackMeta* resolveOnline(const QString& key) const;
    QString upsertOnlineMeta(const QVariantMap& meta);
    // 列表里显示的存储下标:能在曲库里找到的;显示去重开着时,同一首歌的其他副本只留第一次出现的
    std::vector<size_t> visibleSlots(const List& l) const;
    QString keyOf(double trackId) const;
    void rebuildIndex();
    void touch(const QString& id);
    void load();
    void save() const;

    LibraryService* lib_;
    std::shared_ptr<const LibrarySnapshot> snap_;
    std::unordered_map<QString, const TrackItem*> index_; // key -> 曲目
    std::unordered_map<QString, const TrackItem*> titleArtistIndex_; // title\nartist -> 本地最优副本
    std::unordered_map<QString, OnlineTrackMeta> onlineMeta_;        // online:src:id -> 元数据
    std::vector<List> lists_;
    std::vector<QString> history_;
    int revision_ = 0;
    QTimer saveTimer_;
    QString file_;
};
