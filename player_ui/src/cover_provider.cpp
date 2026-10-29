#include "cover_provider.h"

#include <QBuffer>
#include <QCache>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QPainter>
#include <QRunnable>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtMath>
#include <cmath>
#include <mutex>

#include "library_service.h"
#include "music_catalog.hpp"

const char* const kDefaultAccent = "#c8a46e"; // 香槟金

// 强调色「驯化」:限制饱和度与亮度,蓝紫 / 品红色系额外降饱和,杜绝电光霓虹色
QColor TameAccent(const QColor& in) {
    float h, s, l;
    in.getHslF(&h, &s, &l);
    if (h < 0) return QColor(kDefaultAccent);
    s = std::clamp(s, 0.28f, 0.55f);
    if (h > 0.52f && h < 0.86f) s *= 0.68f;        // 青蓝 ~ 紫:雾霾蓝 / 灰紫
    else if (h >= 0.86f && h < 0.97f) s *= 0.78f;  // 品红 ~ 玫瑰:脏粉
    else if (h > 0.22f && h <= 0.52f) s *= 0.85f;  // 绿 ~ 青绿:鼠尾草 / 松石灰
    l = std::clamp(l, 0.62f, 0.70f);
    return QColor::fromHslF(h, s, l);
}

namespace {

// ---------------- 内存 LRU (跨线程共享) ----------------
// 只存缩略图的 JPEG 字节 (512px 约 40KB),不存解码后的位图 (512px 约 1MB):
// 界面里正在显示的封面 Qt Quick 自己还持有一份位图,这里再存一份位图会让大曲库翻一遍后常驻上百 MB。
// 命中时在工作线程里重新解码,一张 512px JPEG 只需 1~2ms
std::mutex g_cacheMutex;
QCache<QString, QByteArray>& MemCache() {
    static QCache<QString, QByteArray> c(24 * 1024); // 以 KB 计,约 24MB,可容纳数百张缩略图
    return c;
}

QByteArray EncodeJpeg(const QImage& img) {
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "JPG", 90);
    return bytes;
}

QString DiskCacheDir() {
    static QString dir = [] {
        QString d = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/thumbs";
        QDir().mkpath(d);
        return d;
    }();
    return dir;
}

QString CoverIdentity(const TrackItem& t) {
    QByteArray key = QString::fromStdWString(t.file_path).toUtf8();
    key += '|' + QByteArray::number(static_cast<qulonglong>(t.file_size));
    key += '|' + QByteArray::number(static_cast<qulonglong>(t.cover_offset));
    key += '|' + QByteArray::number(t.cover_size);
    key += '|' + QString::fromStdWString(t.external_cover_path).toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
}

int Bucket(int edge) {
    // 768 一档:播放页大封面 (约 520 逻辑像素 × 1.25~1.5 缩放) 与专辑详情 (640) 不必解到 1024
    for (int b : {128, 256, 512, 768, 1024})
        if (edge <= b) return b;
    return 1600;
}

class Response : public QQuickImageResponse, public QRunnable {
public:
    Response(QString id, QSize req) : id_(std::move(id)), req_(req) { setAutoDelete(false); }
    QQuickTextureFactory* textureFactory() const override { return QQuickTextureFactory::textureFactoryForImage(image_); }

    void run() override {
        image_ = Produce();
        emit finished();
    }

private:
    QImage Produce() {
        // id: <kind>/<gen>/<ref>
        QStringList parts = id_.split('/');
        if (parts.size() < 3) return CoverProvider::Placeholder(id_, 256);
        const QString kind = parts[0];
        LibraryService* lib = LibraryService::instance();
        auto snap = lib ? lib->snapshot() : nullptr;
        if (!snap) return CoverProvider::Placeholder(id_, 256);

        const TrackItem* track = nullptr;
        QString seed;
        if (kind == "a") {
            int idx = parts[2].toInt();
            if (idx >= 0 && idx < static_cast<int>(snap->albums.size())) {
                track = snap->albums[idx].coverTrack;
                seed = snap->albums[idx].title;
            }
        } else {
            track = snap->catalog.FindById(parts[2].toULongLong());
            if (track) seed = QString::fromStdWString(track->album);
        }

        int edge = std::max(req_.width(), req_.height());
        if (edge <= 0) edge = 512;
        if (kind == "b") edge = 256;
        int bucket = Bucket(edge);

        QImage img;
        if (track) {
            QString memKey = CoverIdentity(*track) + QString::number(bucket);
            QByteArray jpeg;
            {
                std::lock_guard<std::mutex> lock(g_cacheMutex);
                if (QByteArray* hit = MemCache().object(memKey)) jpeg = *hit;
            }
            if (jpeg.isEmpty()) {
                QString diskPath = DiskCacheDir() + "/" + memKey + ".jpg";
                QFile f(diskPath);
                if (f.open(QIODevice::ReadOnly)) jpeg = f.readAll();
                if (jpeg.isEmpty() || !img.loadFromData(jpeg, "JPG")) {
                    jpeg.clear();
                    img = CoverProvider::LoadTrackCover(*track, bucket);
                    if (!img.isNull()) {
                        jpeg = EncodeJpeg(img);
                        QSaveFile out(diskPath);
                        if (out.open(QIODevice::WriteOnly) && out.write(jpeg) == jpeg.size()) out.commit();
                    }
                }
                if (!jpeg.isEmpty()) {
                    std::lock_guard<std::mutex> lock(g_cacheMutex);
                    MemCache().insert(memKey, new QByteArray(jpeg), std::max<qsizetype>(1, jpeg.size() / 1024));
                }
            } else {
                img.loadFromData(jpeg, "JPG");
            }
        }
        if (img.isNull()) img = CoverProvider::Placeholder(seed.isEmpty() ? id_ : seed, bucket);
        if (kind == "b") return CoverProvider::BlurSmall(img);
        // 在工作线程里精确缩放到显示尺寸:渲染时无需 mipmap,纹理上传更小,滚动更顺
        if (req_.isValid() && req_.width() > 0 && req_.height() > 0 &&
            (img.width() > req_.width() * 1.15 || img.height() > req_.height() * 1.15))
            img = img.scaled(req_, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        return img;
    }

    QString id_;
    QSize req_;
    QImage image_;
};

// 三次盒式模糊近似高斯
void BoxBlur(QImage& img, int radius) {
    const int w = img.width(), h = img.height();
    QImage tmp(img.size(), QImage::Format_ARGB32);
    auto pass = [&](const QImage& src, QImage& dst, bool horizontal) {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int r = 0, g = 0, b = 0, n = 0;
                for (int k = -radius; k <= radius; ++k) {
                    int sx = horizontal ? std::clamp(x + k, 0, w - 1) : x;
                    int sy = horizontal ? y : std::clamp(y + k, 0, h - 1);
                    QRgb p = reinterpret_cast<const QRgb*>(src.constScanLine(sy))[sx];
                    r += qRed(p);
                    g += qGreen(p);
                    b += qBlue(p);
                    ++n;
                }
                reinterpret_cast<QRgb*>(dst.scanLine(y))[x] = qRgb(r / n, g / n, b / n);
            }
        }
    };
    for (int i = 0; i < 3; ++i) {
        pass(img, tmp, true);
        pass(tmp, img, false);
    }
}

} // namespace

// 进后台时释放缩略图缓存。定义在匿名 namespace 外面 —— 成员函数不能在里面定义。
// 切回来时封面会按需重新解码,翻页可能有一两帧空白,换来的是真金白银的内存。
void CoverProvider::ClearMemoryCache() {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    MemCache().clear();
}

CoverProvider::CoverProvider() : QQuickAsyncImageProvider() { pool_.setMaxThreadCount(4); }

QQuickImageResponse* CoverProvider::requestImageResponse(const QString& id, const QSize& requestedSize) {
    auto* r = new Response(id, requestedSize);
    pool_.start(r);
    return r;
}

QImage CoverProvider::LoadTrackCover(const TrackItem& t, int maxEdge) {
    std::vector<uint8_t> data;
    std::string mime;
    if (!MusicCatalog::LoadCover(t, data, mime) || data.empty()) return {};
    QByteArray bytes = QByteArray::fromRawData(reinterpret_cast<const char*>(data.data()), static_cast<qsizetype>(data.size()));
    QBuffer buf(&bytes);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);
    // 内嵌封面常见 3000px 以上:先让解码器直接按目标尺寸解码 (JPEG 走 DCT 缩放),
    // 避免把整张原图 (3000x3000 约 36MB) 解成位图再缩小;解码器缩得较粗,预留 2 倍余量后再平滑缩放
    const QSize full = reader.size();
    if (full.isValid() && std::max(full.width(), full.height()) > maxEdge * 2) {
        const double k = double(maxEdge * 2) / std::max(full.width(), full.height());
        reader.setScaledSize(QSize(std::max(1, qRound(full.width() * k)), std::max(1, qRound(full.height() * k))));
    }
    QImage img = reader.read();
    if (img.isNull()) return {};
    if (std::max(img.width(), img.height()) > maxEdge)
        img = img.scaled(maxEdge, maxEdge, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return img.convertToFormat(QImage::Format_RGB32);
}

QImage CoverProvider::Placeholder(const QString& seed, int edge) {
    // 由标题哈希决定色相的柔和双色渐变,每张无封面专辑都有稳定且不同的观感
    // 低饱和大地色系 (陶土 / 鼠尾草 / 驼色 / 石板),与整体暖中性色调一致
    static const char* const kEarth[][2] = {{"#8a6f5a", "#3a2e27"}, {"#7d8a74", "#2c3229"}, {"#a08a6a", "#3b3226"},
                                            {"#6f7a80", "#262b2e"}, {"#94736b", "#36292a"}, {"#8c8270", "#302c25"}};
    uint h = qHash(seed);
    QColor a(kEarth[h % 6][0]);
    QColor b(kEarth[h % 6][1]);
    QImage img(edge, edge, QImage::Format_RGB32);
    QPainter p(&img);
    QLinearGradient g(0, 0, edge, edge);
    g.setColorAt(0, a);
    g.setColorAt(1, b);
    p.fillRect(img.rect(), g);
    QRadialGradient glow(edge * 0.3, edge * 0.25, edge * 0.7);
    glow.setColorAt(0, QColor(255, 255, 255, 50));
    glow.setColorAt(1, QColor(255, 255, 255, 0));
    p.fillRect(img.rect(), glow);
    // 专辑名首字作为大号字形,无封面专辑也能一眼区分
    QString glyph = seed.trimmed().left(1).toUpper();
    if (!glyph.isEmpty() && !seed.contains('/')) {
        QFont f;
        f.setPixelSize(int(edge * 0.46));
        f.setWeight(QFont::DemiBold);
        p.setFont(f);
        p.setRenderHint(QPainter::TextAntialiasing);
        p.setPen(QColor(255, 255, 255, 150));
        p.drawText(img.rect(), Qt::AlignCenter, glyph);
    }
    return img;
}

QImage CoverProvider::BlurSmall(const QImage& src) {
    QImage small = src.scaled(64, 64, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_ARGB32);
    BoxBlur(small, 3);
    return small;
}

QColor ExtractAccentColor(const QImage& src) {
    if (src.isNull()) return QColor(kDefaultAccent);
    QImage img = src.scaled(48, 48, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGB32);
    // 色相直方图 (36 桶),权重 = 饱和度 × 亮度适中度
    struct Bin { double w = 0, r = 0, g = 0, b = 0; };
    Bin bins[36];
    double total = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            QColor c = QColor::fromRgb(line[x]);
            float hh, s, l;
            c.getHslF(&hh, &s, &l);
            if (hh < 0) continue; // 无彩色
            double mid = 1.0 - std::abs(l - 0.55) * 1.8;
            if (mid <= 0 || s < 0.18) continue;
            double w = s * s * mid;
            Bin& bin = bins[std::min(35, int(hh * 36))];
            bin.w += w;
            bin.r += c.redF() * w;
            bin.g += c.greenF() * w;
            bin.b += c.blueF() * w;
            total += w;
        }
    }
    int best = -1;
    for (int i = 0; i < 36; ++i)
        if (best < 0 || bins[i].w + 0.5 * (bins[(i + 35) % 36].w + bins[(i + 1) % 36].w) >
                            bins[best].w + 0.5 * (bins[(best + 35) % 36].w + bins[(best + 1) % 36].w))
            best = i;
    if (best < 0 || total < 1.0 || bins[best].w <= 0) {
        // 近乎黑白的封面:用暖白色强调
        return QColor(kDefaultAccent);
    }
    const Bin& b = bins[best];
    QColor c = QColor::fromRgbF(float(b.r / b.w), float(b.g / b.w), float(b.b / b.w));
    float hh, s, l;
    c.getHslF(&hh, &s, &l);
    return TameAccent(QColor::fromHslF(hh, s, l));
}

QList<QColor> ExtractPalette(const QImage& src) {
    // 一个主色 + 一个辅助色 + 暖中性色:全部由驯化后的强调色推导,保证和谐,不让封面杂色乱入
    QColor accent = ExtractAccentColor(src);
    float h, s, l;
    accent.getHslF(&h, &s, &l);
    if (h < 0) h = 0.1f;
    // 辅助色:邻近色相向暖色方向偏移约 25°,低饱和深色
    float warmDir = (h > 0.1f && h < 0.6f) ? -1.f : 1.f;
    float hs = std::fmod(h + warmDir * 0.07f + 1.f, 1.f);
    QList<QColor> out;
    out << QColor::fromHslF(h, s * 0.62f, 0.22f)          // 主色深调
        << QColor::fromHslF(hs, s * 0.45f, 0.17f)         // 辅助色深调
        << QColor::fromHslF(h, s * 0.50f, 0.27f)          // 主色稍亮,形成层次
        << QColor("#1b1816");                             // 暖中性
    return out;
}
