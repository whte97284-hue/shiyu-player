#pragma once
#include <QObject>
#include <QStringList>
#include <memory>

// 监控若干根目录 (含全部子目录) 的文件增删改名。只负责报告「哪个根目录有变化」,防抖与重扫由调用方处理。
// Windows 用 ReadDirectoryChangesW 递归监控每个根目录 (一个根目录一条线程,不随子目录数量增长);
// 其他平台退回 QFileSystemWatcher 逐个目录监控。
class FolderWatcher : public QObject {
    Q_OBJECT
public:
    explicit FolderWatcher(QObject* parent = nullptr);
    ~FolderWatcher() override;

    // 替换监控列表 (空列表 = 停止监控)
    void setRoots(const QStringList& roots);
    QStringList roots() const { return roots_; }

signals:
    void changed(const QString& root);

private:
    void stopAll();
    QStringList roots_;
    struct Impl;
    std::unique_ptr<Impl> d_;
};
