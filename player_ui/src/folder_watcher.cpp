#include "folder_watcher.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QPointer>
#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <QFileSystemWatcher>
#endif

namespace {
// 解密的临时文件、下载中的临时文件不算变化;它们完成时会改名成正式文件名,那一下才算
bool Ignored(const QString& name) {
    const QString l = name.toLower();
    return l.endsWith(QLatin1String(".part")) || l.endsWith(QLatin1String(".tmp")) || l.endsWith(QLatin1String(".kgtemp")) ||
           l.endsWith(QLatin1String(".crdownload")) || l.endsWith(QLatin1String("desktop.ini")) ||
           l.endsWith(QLatin1String("thumbs.db"));
}
#ifdef Q_OS_WIN
QString ExtOf(const QString& name) {
    const int slash = std::max(name.lastIndexOf('\\'), name.lastIndexOf('/'));
    const int dot = name.lastIndexOf('.');
    return dot > slash ? name.mid(dot + 1).toLower() : QString();
}

bool AudioOrCue(const QString& ext) {
    static const QStringList kExts = {"flac", "mp3", "mp2", "ogg", "oga", "opus", "wav", "wave", "aif", "aiff", "aifc",
                                      "m4a", "m4b", "mp4", "alac", "dsf", "dff", "ape", "ncm", "kgm", "kgma", "kgg", "vpr",
                                      "cue"};
    return kExts.contains(ext);
}

// 歌词、封面、文本、数据库这类伴生 / 杂项文件:听歌时酷狗客户端、播放器、杀毒、索引常常改它们,与曲库无关
bool SideFile(const QString& ext) {
    static const QStringList kExts = {"lrc", "krc", "qrc", "trc", "txt", "jpg", "jpeg", "png", "webp", "bmp", "gif",
                                      "ini", "db", "log", "json", "xml", "dat", "cache", "bak", "tmp", "part", "kgtemp",
                                      "lnk", "url", "nfo", "m3u", "m3u8", "pls", "sfv", "md5", "accurip", "pdf"};
    return kExts.contains(ext);
}

// 只有影响曲库的变化才算:音频 / CUE 的增删改名改写,以及文件夹的增删改名。
// 文件夹自身的「修改」通知 (里面任何文件变了都会报) 和伴生文件一律不算
bool Relevant(const QString& root, const QString& name, DWORD action) {
    if (Ignored(name)) return false;
    const QString ext = ExtOf(name);
    if (AudioOrCue(ext)) return true;
    if (action == FILE_ACTION_MODIFIED) return false;
    if (SideFile(ext)) return false;
    if (action == FILE_ACTION_ADDED || action == FILE_ACTION_RENAMED_NEW_NAME) {
        const DWORD attr = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(root + "/" + name).utf16()));
        return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    }
    // 删除 / 改名前的旧名:已经查不到是不是文件夹,扩展名认不出的按文件夹处理
    return true;
}
#endif
} // namespace

#ifdef Q_OS_WIN
struct FolderWatcher::Impl {
    struct Root {
        QString path;
        HANDLE stop = nullptr;
        std::thread th;
    };
    std::vector<std::unique_ptr<Root>> roots;
};

FolderWatcher::FolderWatcher(QObject* parent) : QObject(parent), d_(std::make_unique<Impl>()) {}

FolderWatcher::~FolderWatcher() { stopAll(); }

void FolderWatcher::stopAll() {
    for (auto& r : d_->roots) SetEvent(r->stop);
    for (auto& r : d_->roots) {
        if (r->th.joinable()) r->th.join();
        CloseHandle(r->stop);
    }
    d_->roots.clear();
}

void FolderWatcher::setRoots(const QStringList& roots) {
    if (roots == roots_) return;
    stopAll();
    roots_ = roots;
    QPointer<FolderWatcher> self(this);
    for (const QString& path : roots) {
        auto r = std::make_unique<Impl::Root>();
        r->path = path;
        r->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        HANDLE stop = r->stop;
        r->th = std::thread([self, path, stop]() {
            HANDLE dir = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()), FILE_LIST_DIRECTORY,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                     FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (dir == INVALID_HANDLE_VALUE) return;
            OVERLAPPED ov{};
            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            std::vector<DWORD> store(16 * 1024); // 64KB,DWORD 对齐
            BYTE* buf = reinterpret_cast<BYTE*>(store.data());
            const DWORD bufBytes = DWORD(store.size() * sizeof(DWORD));
            const DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                 FILE_NOTIFY_CHANGE_LAST_WRITE;
            for (;;) {
                ResetEvent(ov.hEvent);
                if (!ReadDirectoryChangesW(dir, buf, bufBytes, TRUE, filter, nullptr, &ov, nullptr)) break;
                HANDLE waits[2] = {stop, ov.hEvent};
                DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (w != WAIT_OBJECT_0 + 1) {
                    CancelIoEx(dir, &ov);
                    DWORD dummy = 0;
                    GetOverlappedResult(dir, &ov, &dummy, TRUE);
                    break;
                }
                DWORD bytes = 0;
                if (!GetOverlappedResult(dir, &ov, &bytes, FALSE)) break;
                bool relevant = bytes == 0; // 缓冲区溢出:变化太多,当作有变化
                for (DWORD off = 0; bytes && !relevant;) {
                    auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf + off);
                    QString name = QString::fromWCharArray(info->FileName, int(info->FileNameLength / sizeof(WCHAR)));
                    if (Relevant(path, name, info->Action)) relevant = true;
                    if (!info->NextEntryOffset) break;
                    off += info->NextEntryOffset;
                }
                // 析构时会先停掉并等待这些线程,所以这里 self 一定还在
                if (relevant)
                    if (FolderWatcher* w = self.data())
                        QMetaObject::invokeMethod(w, [self, path]() { if (self) emit self->changed(path); }, Qt::QueuedConnection);
            }
            CloseHandle(ov.hEvent);
            CloseHandle(dir);
        });
        d_->roots.push_back(std::move(r));
    }
}
#else
struct FolderWatcher::Impl {
    QFileSystemWatcher fsw;
};

FolderWatcher::FolderWatcher(QObject* parent) : QObject(parent), d_(std::make_unique<Impl>()) {
    connect(&d_->fsw, &QFileSystemWatcher::directoryChanged, this, [this](const QString& dir) {
        const QString n = QDir::cleanPath(dir);
        for (const QString& r : roots_) {
            const QString rn = QDir::cleanPath(r);
            if (n == rn || n.startsWith(rn + "/")) {
                // 新建的子目录也要纳入监控
                QStringList add;
                QDirIterator it(n, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
                while (it.hasNext()) add << it.next();
                if (!add.isEmpty()) d_->fsw.addPaths(add);
                emit changed(r);
                return;
            }
        }
    });
}

FolderWatcher::~FolderWatcher() = default;

void FolderWatcher::stopAll() {
    const QStringList dirs = d_->fsw.directories();
    if (!dirs.isEmpty()) d_->fsw.removePaths(dirs);
}

void FolderWatcher::setRoots(const QStringList& roots) {
    if (roots == roots_) return;
    stopAll();
    roots_ = roots;
    QStringList dirs;
    for (const QString& r : roots) {
        if (!QFileInfo(r).isDir()) continue;
        dirs << r;
        QDirIterator it(r, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext() && dirs.size() < 4000) dirs << it.next();
    }
    if (!dirs.isEmpty()) d_->fsw.addPaths(dirs);
}
#endif
