#include "cover_service.h"

#include "audio_tag_parser.hpp"
#include "cover_fetcher.hpp"
#include "cover_writer.hpp"
#include "library_service.h"

#include <QDateTime>
#include <QMap>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QUrl>
#include <QVariantMap>

#include <cwctype>
#include <atomic>
#include <thread>
#include <chrono>
#include <mutex>

namespace {

// 该文件当前是否已有内嵌封面(按容器类型判断)
bool HasCover(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    std::wstring e = path.substr(dot);
    for (wchar_t& c : e) c = static_cast<wchar_t>(::towlower(c));
    if (e == L".flac") return cover_core::FlacHasCover(path);
    // 其它容器用工程自己的解析器(惰性封面:只定位不读图)
    ParseOptions po;
    po.load_cover = false;
    AudioMetadata m;
    if (!AudioTagParser::ParseFile(path, m, po)) return false;
    return m.has_cover;
}

// 专辑名归一化:去掉括号里的版本说明、空白与标点,ASCII 转小写。
// 「桜色ダイアリー (Special Edition)」与「桜色ダイアリー」应视为同一张。
std::wstring NormalizeKey(const QString& text) {
    const std::wstring in = text.toStdWString();
    std::wstring out;
    out.reserve(in.size());
    int depth = 0;
    for (wchar_t c : in) {
        if (c == L'(' || c == L'[' || c == L'<' || c == L'\uFF08' || c == L'\u3010') { ++depth; continue; }
        if (c == L')' || c == L']' || c == L'>' || c == L'\uFF09' || c == L'\u3011') { if (depth) --depth; continue; }
        if (depth) continue;
        if (c == L' ' || c == L'\t' || c == L'-' || c == L'_' || c == L'.' || c == L',' || c == L'!' ||
            c == L'?' || c == L'\'' || c == L'"' || c == L'/' || c == L':' || c == L';' || c == L'&')
            continue;
        out.push_back(static_cast<wchar_t>(::towlower(c)));
    }
    return out;
}

bool IsFlacPath(const std::wstring& p) {
    if (p.size() < 5) return false;
    std::wstring e = p.substr(p.size() - 5);
    for (wchar_t& c : e) c = static_cast<wchar_t>(::towlower(c));
    return e == L".flac";
}

} // namespace

CoverService::CoverService(LibraryService* lib, QObject* parent) : QObject(parent), lib_(lib) {
    // 曲库变了(扫描 / 补完封面后重扫) → 横幅上的「缺封面」小圆点跟着更新
    if (lib_) connect(lib_, &LibraryService::libraryChanged, this, &CoverService::missingCountChanged);
}

CoverService::~CoverService() {
    if (cancel_) cancel_->store(true);
    if (staleCancel_) staleCancel_->store(true);
    if (worker_.joinable()) worker_.join();
    if (staleWorker_.joinable()) staleWorker_.join();
}

QVariantList CoverService::albumStates() const {
    QVariantList out;
    auto s = lib_->snapshot();
    if (!s) return out;
    const int n = static_cast<int>(s->albums.size());
    int pending = 0;
    // 只收缺封面的:已齐的不进面板 (专辑上千张时这是性能关键)
    for (int i = 0; i < n; ++i) {
        const AlbumInfo& a = s->albums[i];
        int missing = 0, fixable = 0;
        for (const TrackItem* t : a.tracks) {
            if (t->has_cover) continue;
            ++missing;
            if (cover_core::CanEmbedCover(t->file_path)) ++fixable;
        }
        if (missing == 0) continue;
        ++pending;
        QVariantMap m;
        m["index"] = i;
        m["title"] = a.title;
        m["cover"] = QString("image://cover/a/%1/%2").arg(lib_->generation()).arg(i);
        m["total"] = static_cast<int>(a.tracks.size());
        m["missing"] = missing;
        m["fixable"] = fixable;   // 0 = 这张没法补(缺封面的是 WAV/DSF 等)
        out.push_back(m);
    }
    if (pending != pendingAlbums_) {
        pendingAlbums_ = pending;
        // const 方法里只改计数,不发信号(由 missingCountChanged 的调用方触发刷新)
    }
    return out;
}

int CoverService::missingCount() const {
    auto s = lib_->snapshot();
    if (!s) return 0;
    int n = 0;
    for (const AlbumInfo& a : s->albums) {
        for (const TrackItem* t : a.tracks)
            if (!t->has_cover) { ++n; break; }
    }
    return n;
}

void CoverService::cancel() {
    if (cancel_) cancel_->store(true);
}

void CoverService::fetchAlbums(const QVariantList& indexes) {
    if (busy_) return;
    if (worker_.joinable()) worker_.join();

    auto s = lib_->snapshot();
    if (!s) return;

    // 先决定要处理哪些专辑
    std::vector<int> want;
    if (indexes.isEmpty()) {
        for (int i = 0; i < static_cast<int>(s->albums.size()); ++i) {
            for (const TrackItem* t : s->albums[i].tracks)
                if (!t->has_cover) { want.push_back(i); break; }
        }
    } else {
        for (const QVariant& v : indexes) {
            const int i = v.toInt();
            if (i >= 0 && i < static_cast<int>(s->albums.size())) want.push_back(i);
        }
    }
    if (want.empty()) return;

    // 在主线程把要用的数据拷出来(快照只读,但后台线程不该再碰 Qt 对象)
    std::vector<Job> jobs;
    jobs.reserve(want.size());
    for (int idx : want) {
        const AlbumInfo& a = s->albums[idx];
        Job j;
        j.index = idx;
        j.title = a.title.toStdWString();
        // 代表曲目:第一首有歌名的;没有就记一笔失败(用户要能看到原因)
        const TrackItem* rep = nullptr;
        for (const TrackItem* t : a.tracks) {
            if (!t->title.empty()) { rep = t; break; }
        }
        if (!rep) {
            j.skipReason = "曲目没有标题标签,无法检索";
            jobs.push_back(std::move(j));
            continue;
        }
        j.searchTitle = rep->title;
        j.searchArtist = rep->artist;
        j.probe = rep->file_path;
        // 从文件名提取备用标题:「艺人 - 标题」取后半段(标签被翻成罗马字时靠它搜索)
        {
            const QString stem = QFileInfo(QString::fromStdWString(rep->file_name.empty() ? rep->file_path
                                                                                          : rep->file_name))
                                     .completeBaseName();
            QString alt = stem;
            const int sep = stem.indexOf(QStringLiteral(" - "));
            if (sep > 0) alt = stem.mid(sep + 3);
            if (!alt.isEmpty() && alt != QString::fromStdWString(rep->title)) j.altTitle = alt.toStdWString();
        }
        for (const TrackItem* t : a.tracks) {
            if (t->has_cover) continue;              // 已有内嵌封面,不动
            if (!cover_core::CanEmbedCover(t->file_path)) continue; // 该容器不支持写封面
            j.targets.push_back(t->file_path);
        }
        // 同一张专辑常有多个版本(日文原名 / 罗马字 / Special Edition),库里只要有一个带封面,
        // 直接借过来 —— 不用联网,也不会拿错(专辑名归一化后相同)。
        if (!j.targets.empty()) {
            const std::wstring wantAlbum = NormalizeKey(a.title);
            for (int k = 0; k < static_cast<int>(s->albums.size()) && j.borrowFrom.empty(); ++k) {
                if (k == idx) continue;
                const AlbumInfo& other = s->albums[k];
                if (NormalizeKey(other.title) != wantAlbum || wantAlbum.empty()) continue;
                if (!other.coverTrack) continue;
                if (!other.coverTrack->has_cover) continue;   // 只借"文件里真有封面"的
                j.borrowFrom = other.coverTrack->file_path;
            }
        }
        if (j.targets.empty()) {
            // 把「哪些格式挡住的」写清楚,不然用户只看到"没动"却不知道为什么
            std::map<std::wstring, int> fmts;
            for (const TrackItem* t : a.tracks) {
                if (t->has_cover) continue;
                fmts[t->format.empty() ? L"未知" : t->format]++;
            }
            std::wstring desc;
            for (const auto& kv : fmts) {
                if (!desc.empty()) desc += L" / ";
                desc += kv.first + L"×" + std::to_wstring(kv.second);
            }
            j.skipReason = "容器不支持写入(" + AudioTagParser::WideToUtf8(desc) + ")";
        }
        jobs.push_back(std::move(j));
    }
    if (jobs.empty()) return;

    start(std::move(jobs));
}

void CoverService::start(std::vector<Job> jobs) {
    busy_ = true;
    total_ = static_cast<int>(jobs.size());
    lastJobCount_ = total_;
    done_ = 0;
    progress_ = 0;
    ok_ = fail_ = 0;
    skip_ = 0;
    current_.clear();
    failures_.clear();
    skips_.clear();
    elapsed_.restart();
    emit jobChanged();
    emit progressChanged();

    cancel_ = std::make_shared<std::atomic<bool>>(false);
    auto cancel = cancel_;
    QPointer<CoverService> self(this);

    // 并发数:取一半核心,限制在 2~6。
    // 每个请求都是独立的 WinHTTP session,互不干扰;再高只会被服务端限流,反而更慢。
    const int cores = static_cast<int>(std::thread::hardware_concurrency());
    const int want = std::max(2, std::min(6, cores / 2));
    const int nThreads = std::max(1, std::min(want, static_cast<int>(jobs.size())));
    threadCount_ = nThreads;
    qWarning("KPCV start jobs=%zu threads=%d", jobs.size(), nThreads);

    worker_ = std::thread([self, jobs = std::move(jobs), cancel, nThreads]() {
        // ---- 共享状态(worker 之间要加锁) ----
        std::atomic<size_t> next{0};
        std::atomic<int> ok{0}, fail{0}, skip{0}, done{0};
        std::mutex mtx;
        std::vector<std::pair<QString, QString>> fails;
        std::vector<std::pair<QString, QString>> skips;
        std::vector<QString> everHad;
        const double n = static_cast<double>(jobs.size());

        // 单个任务的处理(原串行逻辑抽出来)
        auto runOne = [&](const Job& j) {
            const QString jtitle = QString::fromStdWString(j.title);
            if (!j.skipReason.empty()) {
                std::lock_guard<std::mutex> l(mtx);
                ++skip;
                skips.emplace_back(jtitle, QString::fromStdString(j.skipReason));
                return;
            }
            // ★ 不看快照的 has_cover,直接问磁盘 —— 快照可能是补封面之前建的
            std::vector<std::wstring> todo;
            for (const std::wstring& t : j.targets)
                if (!HasCover(t)) todo.push_back(t);
            if (todo.empty()) {
                std::lock_guard<std::mutex> l(mtx);
                ++ok;
                everHad.emplace_back(jtitle);
                return;
            }
            if (!j.borrowFrom.empty()) {
                ParseOptions po;
                po.load_cover = true;
                AudioMetadata bm;
                if (AudioTagParser::ParseFile(j.borrowFrom, bm, po) && bm.has_cover && !bm.cover_data.empty()) {
                    int wrote = 0;
                    QString lastErr;
                    const std::string img(bm.cover_data.begin(), bm.cover_data.end());
                    for (const std::wstring& t : todo) {
                        std::string e;
                        if (cover_core::EmbedCover(t, img, bm.cover_mime_type, &e)) ++wrote;
                        else lastErr = QString::fromStdString(e);
                    }
                    std::lock_guard<std::mutex> l(mtx);
                    if (wrote > 0) ++ok;
                    else { ++fail; fails.emplace_back(jtitle, QStringLiteral("借用写入失败: ") + lastErr); }
                    return;
                }
                std::lock_guard<std::mutex> l(mtx);
                ++fail;
                fails.emplace_back(jtitle, QStringLiteral("同专辑版本没有可借的封面"));
                return;
            }

            cover_core::FlacTags tags;
            tags.title = AudioTagParser::WideToUtf8(j.searchTitle);
            tags.artist = AudioTagParser::WideToUtf8(j.searchArtist);
            tags.album = AudioTagParser::WideToUtf8(j.title);
            tags.alt_title = AudioTagParser::WideToUtf8(j.altTitle);

            cover_core::CoverResult r;
            std::string err;
            if (cover_core::FetchOnline(tags, j.probe, r, &err)) {
                int wrote = 0;
                QString lastErr;
                for (const std::wstring& t : todo) {
                    std::string e;
                    if (cover_core::EmbedCover(t, r.jpeg, r.mime, &e)) ++wrote;
                    else lastErr = QString::fromStdString(e);
                }
                std::lock_guard<std::mutex> l(mtx);
                if (wrote > 0) ++ok;
                else { ++fail; fails.emplace_back(jtitle, QStringLiteral("写入失败: ") + lastErr); }
            } else {
                std::lock_guard<std::mutex> l(mtx);
                ++fail;
                fails.emplace_back(jtitle, QString::fromStdString(err));
            }
        };

        // ---- 线程池 ----
        std::vector<std::thread> pool;
        pool.reserve(static_cast<size_t>(nThreads));
        for (int t = 0; t < nThreads; ++t) {
            pool.emplace_back([&]() {
                for (;;) {
                    const size_t i = next.fetch_add(1);
                    if (i >= jobs.size() || cancel->load()) break;
                    const Job& j = jobs[i];
                    // 开发验证用:人为放慢,便于截图检查「运行中」的面板外观(默认 0 = 不生效)
                    static const int kDelayMs = qEnvironmentVariableIntValue("KP_COVER_DELAY_MS");
                    if (kDelayMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(kDelayMs));
                    runOne(j);
                    const int d = ++done;
                    // 进度节流:每 80ms 最多上报一次,避免几十个 queued 调用把 UI 事件循环挤满
                    static thread_local qint64 lastPost = 0;
                    const qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (now - lastPost < 80 && d < static_cast<int>(jobs.size())) continue;
                    lastPost = now;

                    QVariantList sf, ss;
                    QString cur, lastTitle;
                    int cOk = 0, cFail = 0, cSkip = 0;
                    {
                        std::lock_guard<std::mutex> l(mtx);
                        for (const auto& f : fails) { QVariantMap m; m["title"] = f.first; m["reason"] = f.second; sf.push_back(m); }
                        for (const auto& f : skips) { QVariantMap m; m["title"] = f.first; m["reason"] = f.second; ss.push_back(m); }
                        cOk = ok; cFail = fail; cSkip = skip;
                        lastTitle = QString::fromStdWString(j.title);
                    }
                    QMetaObject::invokeMethod(self, [self, d, n, cOk, cFail, cSkip, sf, ss, lastTitle]() {
                        if (!self) return;
                        self->done_ = d;
                        self->progress_ = static_cast<double>(d) / n;
                        self->ok_ = cOk;
                        self->fail_ = cFail;
                        self->skip_ = cSkip;
                        self->failures_ = sf;
                        self->skips_ = ss;
                        self->current_ = lastTitle;
                        emit self->progressChanged();
                        emit self->jobChanged();
                    }, Qt::QueuedConnection);
                }
            });
        }
        for (auto& th : pool) th.join();

        const bool cancelled = cancel->load();
        QVariantList alreadyOk;
        {
            std::lock_guard<std::mutex> l(mtx);
            for (const QString& t : everHad) alreadyOk.push_back(t);
        }
        QVariantList finalFails, finalSkips;
        int fOk = 0, fFail = 0, fSkip = 0;
        {
            std::lock_guard<std::mutex> l(mtx);
            for (const auto& f : fails) { QVariantMap m; m["title"] = f.first; m["reason"] = f.second; finalFails.push_back(m); }
            for (const auto& f : skips) { QVariantMap m; m["title"] = f.first; m["reason"] = f.second; finalSkips.push_back(m); }
            fOk = ok; fFail = fail; fSkip = skip;
        }
        QMetaObject::invokeMethod(self, [self, fOk, fFail, fSkip, cancelled, finalFails, finalSkips, alreadyOk]() {
            if (!self) return;
            self->busy_ = false;
            self->progress_ = cancelled ? self->progress_ : 1.0;
            self->ok_ = fOk;
            self->fail_ = fFail;
            self->skip_ = fSkip;
            self->failures_ = finalFails;
            self->skips_ = finalSkips;
            self->alreadied_ = alreadyOk;
            self->current_.clear();
            emit self->jobChanged();
            emit self->progressChanged();
            qWarning("KPCV finish done=%d total=%d ok=%d fail=%d skip=%d cancelled=%d",
                     self->done_, self->total_, fOk, fFail, fSkip, cancelled ? 1 : 0);
            emit self->finished(fOk, fFail);
            // ★ 无论成功 / 跳过 / 失败都重扫:
            //   文件可能在上次扫描之后就被别的途径补过封面,快照里的 has_cover 是旧的。
            if (!cancelled && self->lib_) self->lib_->rescan();
        }, Qt::QueuedConnection);
    });
}

void CoverService::openFailureLog() {
    const QString dir = LibraryService::dataDir();
    QDir().mkpath(dir);
    const QString path = dir + QStringLiteral("/cover_failures.txt");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);

    // 按原因归类统计,一眼看出瓶颈在哪
    QMap<QString, int> failByReason, skipByReason;
    for (const QVariant& v : failures_) ++failByReason[v.toMap().value("reason").toString()];
    for (const QVariant& v : skips_) ++skipByReason[v.toMap().value("reason").toString()];

    ts << "补封面报告  " << QDateTime::currentDateTime().toString(Qt::ISODate) << "\n";
    ts << "================================================\n";
    ts << "本次处理    " << lastJobCount_ << " 张专辑\n";
    ts << "  成功      " << ok_ << "\n";
    ts << "  失败      " << fail_ << "\n";
    ts << "  跳过      " << skip_ << "\n";
    ts << "面板剩余    " << pendingAlbums_ << " 张缺封面(未进入本次任务,或跳过了)\n\n";

    if (!failByReason.isEmpty()) {
        ts << "---- 失败原因归类 ----\n";
        for (auto it = failByReason.constBegin(); it != failByReason.constEnd(); ++it)
            ts << "  " << it.value() << " 次   " << it.key() << "\n";
        ts << "\n";
    }
    if (!alreadied_.isEmpty()) {
        ts << "---- 此前已补好,本次确认无需动作 ----\n";
        ts << "  " << alreadied_.size() << " 张(点「重建索引」后从面板消失)\n";
        for (const QVariant& v : alreadied_) ts << "    " << v.toString() << "\n";
        ts << "\n";
    }
    if (!skipByReason.isEmpty()) {
        ts << "---- 跳过原因归类(不算失败) ----\n";
        for (auto it = skipByReason.constBegin(); it != skipByReason.constEnd(); ++it)
            ts << "  " << it.value() << " 次   " << it.key() << "\n";
        ts << "\n";
    }

    if (!failures_.isEmpty()) {
        ts << "---- 失败明细 ----\n";
        for (const QVariant& v : failures_) {
            const QVariantMap m = v.toMap();
            ts << "[" << m.value("title").toString() << "]\n    " << m.value("reason").toString() << "\n\n";
        }
    }
    if (!skips_.isEmpty()) {
        ts << "---- 跳过明细 ----\n";
        for (const QVariant& v : skips_) {
            const QVariantMap m = v.toMap();
            ts << "[" << m.value("title").toString() << "]\n    " << m.value("reason").toString() << "\n\n";
        }
    }
    f.close();
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void CoverService::checkStale() {
    if (busy_) return;
    if (staleWorker_.joinable()) staleWorker_.join();
    auto s = lib_->snapshot();
    if (!s) return;

    // 把快照里"说没封面"的 FLAC 挑出来(主线程拷,后台只碰 std::wstring)
    struct Item { std::wstring path; QString album; };
    std::vector<Item> todo;
    constexpr size_t kMaxProbe = 400;   // 只抽查前 400 个,足够判断,又不至于太慢
    for (const AlbumInfo& a : s->albums) {
        bool any = false;
        for (const TrackItem* t : a.tracks) {
            if (t->has_cover) continue;
            if (!cover_core::CanEmbedCover(t->file_path)) continue;
            any = true;
            if (todo.size() < kMaxProbe) todo.push_back({t->file_path, a.title});
            break;   // 每张专辑只验一首就够判断
        }
        if (!any) continue;
    }
    if (todo.empty()) {
        if (!alreadied_.isEmpty()) {
            alreadied_.clear();
            emit jobChanged();
        }
        return;
    }

    staleCancel_ = std::make_shared<std::atomic<bool>>(false);
    auto cancel = staleCancel_;
    QPointer<CoverService> self(this);
    staleWorker_ = std::thread([self, todo = std::move(todo), cancel]() {
        QStringList hits;
        for (const auto& it : todo) {
            if (cancel->load()) break;
            if (HasCover(it.path)) hits << it.album;
        }
        const bool cancelled = cancel->load();
        QMetaObject::invokeMethod(self, [self, hits, cancelled]() {
            if (!self || cancelled) return;
            QVariantList list;
            for (const QString& h : hits) list.push_back(h);
            if (list.size() != self->alreadied_.size()) {
                self->alreadied_ = list;
                emit self->jobChanged();
            }
        }, Qt::QueuedConnection);
    });
}
