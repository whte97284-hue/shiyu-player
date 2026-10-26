#pragma once
#include <QAbstractListModel>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "library_dedupe.hpp"
#include "library_organizer.hpp"

class LibraryService;
class PlaylistService;
class PlayerController;

// 通用行模型:每行一个 QVariantMap,角色名即键名;filter 为逗号分隔的 cat 取值 (空 = 全部)
class RowModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
public:
    RowModel(const QStringList& roles, QObject* parent = nullptr);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return static_cast<int>(visible_.size()); }
    QString filter() const { return filter_; }
    void setFilter(const QString& f);
    void setRows(QVector<QVariantMap> rows);
    // 按原始下标更新一行 (不在过滤结果里时只改数据)
    void updateRow(int source, const QVariantMap& row);
    const QVector<QVariantMap>& rows() const { return rows_; }
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void countChanged();
    void filterChanged();

private:
    void refilter();
    QStringList roles_;
    QVector<QVariantMap> rows_;
    std::vector<int> visible_;
    QString filter_;
};

// 整理曲库与查找重复歌曲:界面与内核 (organizer / dedupe) 之间的桥。
// 计划、查找、执行都在后台线程进行;完成后把「旧路径 → 新路径」交给歌单 / 播放队列,并刷新曲库。
class LibraryTools : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString task READ task NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString progressText READ progressText NOTIFY progressChanged)

    // ---- 整理曲库
    Q_PROPERTY(QString pattern READ pattern WRITE setPattern NOTIFY optionsChanged)
    Q_PROPERTY(QString target READ target NOTIFY optionsChanged)
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY optionsChanged)  // 0 移动 1 复制 2 原地改名
    Q_PROPERTY(bool fullWidth READ fullWidth WRITE setFullWidth NOTIFY optionsChanged)
    Q_PROPERTY(bool companions READ companions WRITE setCompanions NOTIFY optionsChanged)
    Q_PROPERTY(bool folderArt READ folderArt WRITE setFolderArt NOTIFY optionsChanged)
    Q_PROPERTY(bool removeEmpty READ removeEmpty WRITE setRemoveEmpty NOTIFY optionsChanged)
    Q_PROPERTY(QStringList sources READ sources NOTIFY optionsChanged)  // 参与整理的曲库文件夹
    Q_PROPERTY(QVariantList presets READ presets CONSTANT)
    Q_PROPERTY(QString patternPreview READ patternPreview NOTIFY optionsChanged)
    Q_PROPERTY(QString patternError READ patternError NOTIFY optionsChanged)
    Q_PROPERTY(QVariantMap plan READ plan NOTIFY planChanged)  // 空表 = 还没算
    Q_PROPERTY(QObject* planItems READ planItems CONSTANT)
    Q_PROPERTY(QVariantMap organizeResult READ organizeResult NOTIFY organizeDone)
    Q_PROPERTY(bool canUndoOrganize READ canUndoOrganize NOTIFY organizeDone)

    // ---- 查找重复
    Q_PROPERTY(bool dupSameSong READ dupSameSong WRITE setDupSameSong NOTIFY optionsChanged)
    Q_PROPERTY(bool dupSameName READ dupSameName WRITE setDupSameName NOTIFY optionsChanged)
    Q_PROPERTY(int dupTolerance READ dupTolerance WRITE setDupTolerance NOTIFY optionsChanged)  // 秒
    Q_PROPERTY(QString quarantine READ quarantine NOTIFY optionsChanged)
    Q_PROPERTY(QObject* dupGroups READ dupGroups CONSTANT)
    Q_PROPERTY(QVariantMap dupSummary READ dupSummary NOTIFY dupChanged)  // 空表 = 还没查
    Q_PROPERTY(QVariantMap dedupeResult READ dedupeResult NOTIFY dedupeDone)
    Q_PROPERTY(bool canUndoDedupe READ canUndoDedupe NOTIFY dedupeDone)

public:
    LibraryTools(LibraryService* lib, PlaylistService* playlists, PlayerController* player, QObject* parent = nullptr);
    ~LibraryTools() override;

    bool busy() const { return !task_.isEmpty(); }
    QString task() const { return task_; }
    double progress() const { return progress_; }
    QString progressText() const { return progressText_; }

    QString pattern() const { return pattern_; }
    void setPattern(const QString& p);
    QString target() const { return target_; }
    int mode() const { return mode_; }
    void setMode(int m);
    bool fullWidth() const { return fullWidth_; }
    void setFullWidth(bool on);
    bool companions() const { return companions_; }
    void setCompanions(bool on);
    bool folderArt() const { return folderArt_; }
    void setFolderArt(bool on);
    bool removeEmpty() const { return removeEmpty_; }
    void setRemoveEmpty(bool on);
    QStringList sources() const;
    QVariantList presets() const;
    QString patternPreview() const { return preview_; }
    QString patternError() const { return patternError_; }
    QVariantMap plan() const { return plan_; }
    QObject* planItems() { return &planItems_; }
    QVariantMap organizeResult() const { return organizeResult_; }
    bool canUndoOrganize() const;

    bool dupSameSong() const { return dupSameSong_; }
    void setDupSameSong(bool on);
    bool dupSameName() const { return dupSameName_; }
    void setDupSameName(bool on);
    int dupTolerance() const { return dupTolerance_; }
    void setDupTolerance(int s);
    QString quarantine() const { return quarantine_; }
    QObject* dupGroups() { return &dupItems_; }
    QVariantMap dupSummary() const { return dupSummary_; }
    QVariantMap dedupeResult() const { return dedupeResult_; }
    bool canUndoDedupe() const;

    Q_INVOKABLE void setTargetUrl(const QVariant& url);
    Q_INVOKABLE void toggleSource(const QString& folder);
    Q_INVOKABLE bool isSource(const QString& folder) const;
    Q_INVOKABLE void buildPlan();
    Q_INVOKABLE void clearPlan();
    Q_INVOKABLE void execute();
    Q_INVOKABLE void undoOrganize();

    Q_INVOKABLE void findDuplicates();
    Q_INVOKABLE void clearDuplicates();
    // 0 自动 1 音质最好 2 首选目录 3 最早 4 最新 5 最大 6 最小
    Q_INVOKABLE void autoSelect(int rule);
    Q_INVOKABLE void setKeep(int groupSource, int member, bool keep);
    // 0 回收站 1 隔离文件夹 2 永久删除 3 只从曲库移除 4 硬链接
    Q_INVOKABLE void applyDedupe(int action);
    Q_INVOKABLE void undoDedupe();
    Q_INVOKABLE void setQuarantineUrl(const QVariant& url);

    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reveal(const QString& path) const;
    Q_INVOKABLE void openFolder(const QString& path) const;

signals:
    void stateChanged();
    void progressChanged();
    void optionsChanged();
    void planChanged();
    void organizeDone();
    void dupChanged();
    void dedupeDone();
    // 一句话结果,界面用 Toast 显示
    void message(const QString& icon, const QString& text);

private:
    template <class Work, class Done>
    void run(const QString& task, Work work, Done done);
    void setProgress(double v, const QString& text);
    void finishTask();
    void saveOptions();
    void updatePreview();
    organizer::OrganizeOptions organizeOptions() const;
    std::vector<organizer::OrganizeTrack> collectTracks(std::vector<std::wstring>* excluded) const;
    void showPlan(const organizer::OrganizePlan& plan);
    void showGroups();
    void applyPathChanges(const QHash<QString, QString>& map);
    QString journalPath(const char* name) const;

    LibraryService* lib_;
    PlaylistService* playlists_;
    PlayerController* player_;
    QString task_;
    double progress_ = 0;
    QString progressText_;
    std::atomic<bool> cancel_{false};
    std::thread worker_;

    QString pattern_, target_, quarantine_;
    int mode_ = 0;
    bool fullWidth_ = false, companions_ = true, folderArt_ = true, removeEmpty_ = true;
    QStringList skipped_;  // 不参与整理的曲库文件夹
    QString preview_, patternError_;
    std::shared_ptr<organizer::OrganizePlan> planData_;
    QVariantMap plan_;
    RowModel planItems_;
    QVariantMap organizeResult_;

    bool dupSameSong_ = true, dupSameName_ = true;
    int dupTolerance_ = 3;
    std::vector<dedupe::DupTrack> dupTracks_;
    std::vector<dedupe::DupGroup> groups_;
    RowModel dupItems_;
    QVariantMap dupSummary_;
    QVariantMap dedupeResult_;
};
