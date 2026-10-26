#pragma once
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <memory>
#include <vector>

#include "library_service.h"
#include "playlist_matcher.hpp"

class PlaylistService;

// 导入在线歌单:粘贴酷狗 / 网易云的分享链接或 ID → 后台解析成曲目清单 → 与本地曲库匹配
// (复用查重的规范化,见 playlist_matcher) → 界面预览哪些本地已有 → 用本地有的存成歌单。
// 曲库重新扫描后自动重新匹配 (曲目 ID 每次扫描都会变)。
// 设置环境变量 KP_TEST_PLAYLIST_FAKE=1 时不联网,用当前曲库拼一份假歌单 (截图 / 自测)
class PlaylistImporter : public QObject {
    Q_OBJECT
    // idle / loading / platform (纯数字 ID,等用户选平台) / ready / error
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    // name / author / source (平台名) / total (标称曲目数) / covers (本地已有曲目的专辑封面,最多 4 张)
    Q_PROPERTY(QVariantMap meta READ meta NOTIFY resultChanged)
    // 每首:title / artist / album / duration (秒) / level (0 缺 · 1 可能 · 2 本地有) / trackId (-1 = 无)
    //       / cover / local (本地那份的格式,如「FLAC」「MP3 320k」)
    Q_PROPERTY(QVariantList rows READ rows NOTIFY resultChanged)
    Q_PROPERTY(int exact READ exact NOTIFY resultChanged)
    Q_PROPERTY(int likely READ likely NOTIFY resultChanged)
    Q_PROPERTY(int missing READ missing NOTIFY resultChanged)

public:
    PlaylistImporter(LibraryService* lib, PlaylistService* playlists, QObject* parent = nullptr);
    ~PlaylistImporter() override;

    QString state() const { return state_; }
    QString error() const { return error_; }
    QVariantMap meta() const { return meta_; }
    QVariantList rows() const { return rows_; }
    int exact() const { return sum_.exact; }
    int likely() const { return sum_.likely; }
    int missing() const { return sum_.missing; }

    // 输入框实时提示:"kugou" / "netease" / "id" (纯数字,要选平台) / "" (认不出)
    Q_INVOKABLE QString detect(const QString& text) const;
    Q_INVOKABLE void parse(const QString& text);
    // platform: "kugou" / "netease";用上次 parse 认出的纯数字 ID
    Q_INVOKABLE void parseAs(const QString& platform);
    // 用本地有的 (includeLikely 时连同「可能」的) 按歌单顺序新建歌单,返回歌单 ID;一首都没有时返回空
    Q_INVOKABLE QString save(const QString& name, bool includeLikely);
    // 把缺少的曲目按「歌手 - 歌名」逐行复制到剪贴板,返回条数
    Q_INVOKABLE int copyMissing() const;
    Q_INVOKABLE void reset();

signals:
    void stateChanged();
    void resultChanged();

private:
    void start(playlist_core::Platform plat, const QString& id);
    void setState(const QString& s, const QString& err = {});
    void rematch(bool force);
    playlist_core::HttpGetFn httpFn() const;

    LibraryService* lib_;
    PlaylistService* playlists_;
    QString state_ = QStringLiteral("idle");
    QString error_;
    QString pendingId_;
    int gen_ = 0;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    playlist_core::PlaylistResult result_;
    std::vector<uint64_t> matchedIds_;   // 与 result_.tracks 一一对应;0 = 没有
    std::vector<int> levels_;
    playlist_core::MatchSummary sum_;
    QVariantMap meta_;
    QVariantList rows_;
};
