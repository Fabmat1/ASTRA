#include "skymap/ui/SkyMapWidget.h"

#include "skymap/HipsSurvey.h"
#include "skymap/ui/HipsTileLoader.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolTip>
#include <QUrl>
#include <QWheelEvent>
#include <QtConcurrent/QtConcurrentMap>

#include <algorithm>
#include <cmath>
#include <vector>

namespace SkyMap {

using Healpix::Vec3;

namespace {

constexpr double kPi          = 3.14159265358979323846;
constexpr double kDeg         = kPi / 180.0;
constexpr double kArcsec      = kDeg / 3600.0;
constexpr double kDefaultFov  = 180.0;     // arcsec across the shorter side
constexpr double kMinScale    = 0.02;      // arcsec per logical pixel
constexpr double kMaxScale    = 60.0;
constexpr double kGaiaMaxViewRadiusDeg = 0.25;
constexpr double kGaiaMaxQueryRadiusDeg = 0.3;
constexpr int    kGaiaMaxRows = 5000;
// Neighbours within this many magnitudes of the target add at least ~10 % of
// its flux to a blended aperture, so they are the ones a crowding check is for.
constexpr double kBrightNeighbourDeltaG = 2.5;

const QColor kSkyBackground(11, 14, 20);
const QColor kReticleColor(255, 70, 70);
const QColor kGaiaFaint(80, 216, 255);
const QColor kGaiaBright(255, 166, 60);
const QColor kSelectedColor(255, 255, 255);
const QColor kPanelBg(0, 0, 0, 165);
const QColor kPanelText(235, 238, 245);
const QColor kPanelDim(170, 178, 192);

inline Vec3 add(const Vec3& a, const Vec3& b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
inline Vec3 mul(const Vec3& a, double s) { return { a[0] * s, a[1] * s, a[2] * s }; }
inline double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline Vec3 normalized(const Vec3& a)
{
    const double l = std::sqrt(dot(a, a));
    return l > 0 ? mul(a, 1.0 / l) : a;
}
inline double angleBetween(const Vec3& a, const Vec3& b)
{
    // atan2 of |a x b| and a.b stays accurate at tiny separations.
    const Vec3 c{ a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
    return std::atan2(std::sqrt(dot(c, c)), dot(a, b));
}

// Bilinear sample at continuous pixel position (fx, fy), pixel centres at
// integer + 0.5, clamped to the image. Returns non-premultiplied ARGB.
inline QRgb sampleBilinear(const QImage& img, double fx, double fy)
{
    const int w = img.width(), h = img.height();
    fx = std::clamp(fx - 0.5, 0.0, double(w - 1));
    fy = std::clamp(fy - 0.5, 0.0, double(h - 1));
    const int x0 = int(fx), y0 = int(fy);
    const int x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const double tx = fx - x0, ty = fy - y0;
    const QRgb* r0 = reinterpret_cast<const QRgb*>(img.constScanLine(y0));
    const QRgb* r1 = reinterpret_cast<const QRgb*>(img.constScanLine(y1));
    const QRgb a = r0[x0], b = r0[x1], c = r1[x0], d = r1[x1];
    auto lerp = [&](int ca, int cb, int cc, int cd) {
        const double top = ca + (cb - ca) * tx;
        const double bot = cc + (cd - cc) * tx;
        return int(top + (bot - top) * ty + 0.5);
    };
    return qRgba(lerp(qRed(a), qRed(b), qRed(c), qRed(d)),
                 lerp(qGreen(a), qGreen(b), qGreen(c), qGreen(d)),
                 lerp(qBlue(a), qBlue(b), qBlue(c), qBlue(d)),
                 lerp(qAlpha(a), qAlpha(b), qAlpha(c), qAlpha(d)));
}

inline QRgb overBackground(QRgb c)
{
    const int a = qAlpha(c);
    if (a == 255) return c | 0xff000000u;
    auto mix = [a](int fg, int bg) { return (fg * a + bg * (255 - a) + 127) / 255; };
    return qRgb(mix(qRed(c), kSkyBackground.red()),
                mix(qGreen(c), kSkyBackground.green()),
                mix(qBlue(c), kSkyBackground.blue()));
}

// Rows [y0, y1) of the frame, rendered by one worker.
struct Strip { int y0, y1; };

std::vector<Strip> makeStrips(int h)
{
    std::vector<Strip> strips;
    const int step = std::max(8, h / 64);
    for (int y = 0; y < h; y += step) strips.push_back({ y, std::min(h, y + step) });
    return strips;
}

QString formatSexagesimal(double value, int fieldsPrecision, bool sign)
{
    // value in hours or degrees; rounds the last field and carries.
    const bool neg = value < 0;
    double v = std::fabs(value);
    const double unit = std::pow(10.0, fieldsPrecision);
    double totalSec = std::round(v * 3600.0 * unit) / unit;
    int a = int(totalSec / 3600.0);
    totalSec -= a * 3600.0;
    int b = int(totalSec / 60.0);
    double c = totalSec - b * 60.0;
    if (c >= 60.0 - 0.5 / unit) { c = 0.0; ++b; }
    if (b >= 60) { b -= 60; ++a; }
    QString s = QString::asprintf("%02d:%02d:%0*.*f", a, b,
                                  fieldsPrecision > 0 ? fieldsPrecision + 3 : 2,
                                  fieldsPrecision, c);
    if (sign) s.prepend(neg ? '-' : '+');
    return s;
}

QString formatAngle(double arcsec)
{
    if (arcsec >= 3600.0) return QString::number(arcsec / 3600.0, 'g', 3) + QStringLiteral("°");
    if (arcsec >= 60.0)   return QString::number(arcsec / 60.0, 'g', 3) + QStringLiteral("′");
    if (arcsec >= 1.0)    return QString::number(arcsec, 'g', 3) + QStringLiteral("″");
    return QString::number(arcsec, 'f', 2) + QStringLiteral("″");
}

void drawPanel(QPainter& p, const QRectF& r)
{
    p.save();
    p.setPen(Qt::NoPen);
    p.setBrush(kPanelBg);
    p.drawRoundedRect(r, 4, 4);
    p.restore();
}

} // namespace

// ── construction ────────────────────────────────────────────────────────────

SkyMapWidget::SkyMapWidget(QWidget* parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setCursor(Qt::OpenHandCursor);

    _nam = new QNetworkAccessManager(this);
    _loader = new HipsTileLoader(_nam, this);
    connect(_loader, &HipsTileLoader::tileArrived, this, [this](const QString& id) {
        if (_layer == Layer::Hips && id == _surveyId) scheduleRepaint();
    });
    connect(_loader, &HipsTileLoader::surveyChanged, this, [this](const QString& id) {
        if (_layer == Layer::Hips && id == _surveyId) {
            scheduleRepaint();
            emit surveyStateChanged();
        }
    });

    // Tile arrivals come in bursts; render at most once per frame interval.
    _repaintTimer = new QTimer(this);
    _repaintTimer->setSingleShot(true);
    _repaintTimer->setInterval(30);
    connect(_repaintTimer, &QTimer::timeout, this, [this] {
        _frameDirty = true;
        update();
    });

    _gaiaQuery = new GaiaFieldQuery(_nam, this);
    connect(_gaiaQuery, &GaiaFieldQuery::finished, this, &SkyMapWidget::onGaiaFinished);
    _gaiaTimer = new QTimer(this);
    _gaiaTimer->setSingleShot(true);
    _gaiaTimer->setInterval(400);
    connect(_gaiaTimer, &QTimer::timeout, this, &SkyMapWidget::ensureGaiaCoverage);
}

SkyMapWidget::~SkyMapWidget() = default;

// ── public API ──────────────────────────────────────────────────────────────

void SkyMapWidget::setTarget(double raDeg, double decDeg, qint64 gaiaSourceId, double gmag)
{
    const bool changed = !_hasTarget || raDeg != _targetRa || decDeg != _targetDec;
    _hasTarget  = std::isfinite(raDeg) && std::isfinite(decDeg);
    _targetRa   = raDeg;
    _targetDec  = decDeg;
    _targetGaia = gaiaSourceId;
    _targetGmag = gmag;
    if (changed) {
        _gaiaSources.clear();
        _gaiaCones.clear();
        _gaiaError.clear();
        _selectedSource = 0;
        _viewInitialised = false;
        resetView();
    }
}

void SkyMapWidget::showHipsSurvey(const QString& id)
{
    _layer = Layer::Hips;
    _surveyId = id;
    const HipsSurvey* s = findHipsSurvey(id);
    _layerTitle = s ? s->title : id;
    _layerMessage.clear();
    _localImage = QImage();
    _loader->survey(id);   // start resolving the mirrors now
    _frameDirty = true;
    update();
    emit surveyStateChanged();
}

void SkyMapWidget::showLocalImage(const QImage& image, double raDeg, double decDeg,
                                  double arcsecPerPixel, const QString& title)
{
    _layer = Layer::Local;
    _surveyId.clear();
    _layerTitle = title;
    _layerMessage.clear();
    _localImage = image.convertToFormat(QImage::Format_ARGB32);
    _localRa = raDeg;
    _localDec = decDeg;
    _localScale = arcsecPerPixel;
    _frameDirty = true;
    update();
    emit surveyStateChanged();
}

void SkyMapWidget::showFlatImage(const QImage& image, const QString& title, const QString& note)
{
    _layer = Layer::Flat;
    _surveyId.clear();
    _layerTitle = title;
    _layerMessage = note;
    _localImage = image;
    _selectedSource = 0;
    QToolTip::hideText();
    _frameDirty = true;
    update();
    emit surveyStateChanged();
}

void SkyMapWidget::showNothing(const QString& title, const QString& message)
{
    _layer = Layer::None;
    _surveyId.clear();
    _layerTitle = title;
    _layerMessage = message;
    _localImage = QImage();
    _frameDirty = true;
    update();
    emit surveyStateChanged();
}

void SkyMapWidget::retryCurrentSurvey()
{
    if (_layer != Layer::Hips) return;
    _loader->retry(_surveyId);
    _frameDirty = true;
    update();
}

bool SkyMapWidget::currentSurveyFailed() const
{
    if (_layer != Layer::Hips) return false;
    return _loader->survey(_surveyId).status == HipsTileLoader::Status::Failed;
}

void SkyMapWidget::setGaiaMarkersVisible(bool on)
{
    if (_gaiaVisible == on) return;
    _gaiaVisible = on;
    if (on) ensureGaiaCoverage();
    update();
}

void SkyMapWidget::resetView()
{
    if (_hasTarget) {
        _centerRa = _targetRa;
        _centerDec = _targetDec;
    }
    const double side = std::max(64, std::min(width(), height()));
    _scale = std::clamp(kDefaultFov / side, kMinScale, kMaxScale);
    _viewInitialised = width() > 64 && height() > 64;
    viewChanged();
}

void SkyMapWidget::zoomBy(double factor)
{
    if (factor <= 0) return;
    setScale(_scale / factor);
    viewChanged();
}

double SkyMapWidget::fieldOfViewArcsec() const
{
    return _scale * std::min(width(), height());
}

// ── view geometry ───────────────────────────────────────────────────────────

SkyMapWidget::Basis SkyMapWidget::basisAt(double raDeg, double decDeg)
{
    const double a = raDeg * kDeg, d = decDeg * kDeg;
    Basis b;
    b.c = { std::cos(d) * std::cos(a), std::cos(d) * std::sin(a), std::sin(d) };
    b.e = { -std::sin(a), std::cos(a), 0.0 };
    b.n = { -std::sin(d) * std::cos(a), -std::sin(d) * std::sin(a), std::cos(d) };
    return b;
}

Vec3 SkyMapWidget::widgetToVec(const QPointF& p) const
{
    const Basis b = basisAt(_centerRa, _centerDec);
    const double s = _scale * kArcsec;
    const double xi  = -(p.x() - width() / 2.0) * s;    // east is left
    const double eta = -(p.y() - height() / 2.0) * s;   // north is up
    return normalized(add(b.c, add(mul(b.e, xi), mul(b.n, eta))));
}

bool SkyMapWidget::vecToWidget(const Vec3& v, QPointF& p) const
{
    const Basis b = basisAt(_centerRa, _centerDec);
    const double dc = dot(v, b.c);
    if (dc <= 1e-9) return false;   // behind the tangent plane
    const double s = _scale * kArcsec;
    const double xi  = dot(v, b.e) / dc;
    const double eta = dot(v, b.n) / dc;
    p = QPointF(width() / 2.0 - xi / s, height() / 2.0 - eta / s);
    return true;
}

QPointF SkyMapWidget::skyToWidget(double raDeg, double decDeg, bool* visible) const
{
    QPointF p;
    const bool ok = vecToWidget(Healpix::vecFromLonLat(raDeg, decDeg), p);
    if (visible) *visible = ok;
    return p;
}

void SkyMapWidget::setCenter(const Vec3& v)
{
    Healpix::lonLatFromVec(v, _centerRa, _centerDec);
}

void SkyMapWidget::setScale(double arcsecPerPixel)
{
    _scale = std::clamp(arcsecPerPixel, kMinScale, kMaxScale);
}

void SkyMapWidget::viewChanged()
{
    _frameDirty = true;
    update();
    if (_gaiaVisible) _gaiaTimer->start();
}

void SkyMapWidget::scheduleRepaint()
{
    if (!_repaintTimer->isActive()) _repaintTimer->start();
}

// ── rendering ───────────────────────────────────────────────────────────────

void SkyMapWidget::renderFrame()
{
    const qreal dpr = devicePixelRatioF();
    const int w = std::max(1, int(std::lround(width() * dpr)));
    const int h = std::max(1, int(std::lround(height() * dpr)));
    if (_frame.width() != w || _frame.height() != h || _frame.format() != QImage::Format_RGB32)
        _frame = QImage(w, h, QImage::Format_RGB32);
    _frame.setDevicePixelRatio(dpr);

    const Basis b = basisAt(_centerRa, _centerDec);
    const double radPerDevPx = _scale * kArcsec / dpr;

    _hipsStatus.clear();
    _hipsDetail.clear();
    _noCoverage = false;

    switch (_layer) {
    case Layer::Hips:  renderHips(b, radPerDevPx, w, h); break;
    case Layer::Local: renderLocal(b, radPerDevPx, w, h); break;
    case Layer::None:  _frame.fill(kSkyBackground); break;
    case Layer::Flat: {
        _frame.fill(kSkyBackground);
        if (!_localImage.isNull()) {
            QPainter fp(&_frame);
            fp.setRenderHint(QPainter::SmoothPixmapTransform, true);
            const QSizeF avail(width() - 20.0, height() - 20.0);
            const QSizeF sz = QSizeF(_localImage.size()).scaled(avail, Qt::KeepAspectRatio);
            fp.drawImage(QRectF(QPointF((width() - sz.width()) / 2, (height() - sz.height()) / 2), sz),
                         _localImage);
        }
        break;
    }
    }
    _frameDirty = false;
}

void SkyMapWidget::renderHips(const Basis& b, double radPerDevPx, int w, int h)
{
    const HipsTileLoader::SurveyState& st = _loader->survey(_surveyId);
    if (st.status != HipsTileLoader::Status::Ready) {
        _frame.fill(kSkyBackground);
        if (st.status == HipsTileLoader::Status::Failed)
            _hipsStatus = tr("No server for this survey answered");
        else
            _hipsStatus = tr("Looking for a server...");
        return;
    }

    const HipsProperties& p = st.props;
    const bool galactic = p.galactic;
    int bits = 0;
    while ((1 << bits) < p.tileWidth) ++bits;

    // The coarsest tile order whose pixels are no larger than a screen pixel;
    // the deepest order when even that is coarser (the image is magnified).
    const double devArcsec = radPerDevPx / kArcsec;
    int order = p.maxOrder;
    for (int o = p.minOrder; o <= p.maxOrder; ++o) {
        if (Healpix::cellSizeArcsec(o + bits) <= devArcsec) { order = o; break; }
    }

    auto pixelVec = [&](double x, double y) {
        const double xi  = -(x - w / 2.0) * radPerDevPx;
        const double eta = -(y - h / 2.0) * radPerDevPx;
        Vec3 v = add(b.c, add(mul(b.e, xi), mul(b.n, eta)));
        return galactic ? Healpix::icrsToGalactic(v) : v;
    };

    // Which tiles does the view touch? Sample a grid fine enough that no tile
    // at this order (at least ~one screen tile wide) slips between samples.
    struct Need { qint64 npix; double dist2; };
    QHash<qint64, double> needed;
    const int step = 16;
    for (int y = 0; y <= h + step - 1; y += step) {
        const double yy = std::min(y, h - 1) + 0.5;
        for (int x = 0; x <= w + step - 1; x += step) {
            const double xx = std::min(x, w - 1) + 0.5;
            const Healpix::FacePos fp = Healpix::facePos(pixelVec(xx, yy));
            const double ns = std::ldexp(1.0, order);
            const qint64 npix = Healpix::xyf2nest(order, qint64(fp.x * ns), qint64(fp.y * ns), fp.face);
            const double d2 = (xx - w / 2.0) * (xx - w / 2.0) + (yy - h / 2.0) * (yy - h / 2.0);
            auto it = needed.find(npix);
            if (it == needed.end()) needed.insert(npix, d2);
            else if (d2 < *it) *it = d2;
        }
    }
    std::vector<Need> list;
    list.reserve(needed.size());
    for (auto it = needed.cbegin(); it != needed.cend(); ++it) list.push_back({ it.key(), it.value() });
    std::sort(list.begin(), list.end(), [](const Need& a, const Need& c) { return a.dist2 < c.dist2; });

    // Ask for coarse ancestors first: they arrive quickly and fill the view
    // with a soft preview while the sharp tiles load.
    QVector<TileKey> wanted;
    const int coarse = std::max(p.minOrder, order - 2);
    if (coarse < order) {
        QSet<qint64> seen;
        for (const Need& n : list) {
            const qint64 a = n.npix >> (2 * (order - coarse));
            if (!seen.contains(a)) { seen.insert(a); wanted.push_back({ coarse, a }); }
        }
    }
    for (const Need& n : list) wanted.push_back({ order, n.npix });
    _loader->setWanted(_surveyId, wanted);

    // Every tile the renderer may use, at this order or any coarser one
    // already in memory.
    auto lutKey = [](int o, qint64 npix) { return (quint64(o) << 58) | quint64(npix); };
    QHash<quint64, const QImage*> lut;
    int haveSharp = 0, missingSharp = 0;
    for (const Need& n : list) {
        for (int o = order; o >= p.minOrder; --o) {
            const qint64 np = n.npix >> (2 * (order - o));
            const quint64 key = lutKey(o, np);
            if (lut.contains(key)) break;
            if (const QImage* img = _loader->tile(_surveyId, { o, np })) {
                lut.insert(key, img);
                if (o == order) ++haveSharp;
                break;
            }
            if (o == order && _loader->isMissing(_surveyId, { o, np })) ++missingSharp;
        }
    }

    const int minOrder = p.minOrder;
    const int tileW = p.tileWidth;
    std::vector<Strip> strips = makeStrips(h);
    QtConcurrent::blockingMap(strips, [&](const Strip& s) {
        quint64 lastKey = ~quint64(0);
        const QImage* lastImg = nullptr;
        for (int y = s.y0; y < s.y1; ++y) {
            QRgb* out = reinterpret_cast<QRgb*>(_frame.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const Healpix::FacePos fp = Healpix::facePos(pixelVec(x + 0.5, y + 0.5));
                QRgb col = kSkyBackground.rgb();
                for (int o = order; o >= minOrder; --o) {
                    const double ns = std::ldexp(1.0, o);
                    const double fx = fp.x * ns, fy = fp.y * ns;
                    const qint64 tx = qint64(fx), ty = qint64(fy);
                    const quint64 key = lutKey(o, Healpix::xyf2nest(o, tx, ty, fp.face));
                    const QImage* img;
                    if (key == lastKey) img = lastImg;
                    else {
                        img = lut.value(key, nullptr);
                        if (img) { lastKey = key; lastImg = img; }
                    }
                    if (!img) continue;
                    // HiPS tile layout: image column follows the face y axis,
                    // image row (from the top) the face x axis.
                    const double row = (fx - tx) * tileW;
                    const double colPx = (fy - ty) * tileW;
                    col = overBackground(sampleBilinear(*img, colPx, row));
                    break;
                }
                out[x] = col;
            }
        }
    });

    const int pending = _loader->pendingCount(_surveyId);
    if (pending > 0)
        _hipsStatus = tr("Loading %n tile(s)...", nullptr, pending);
    if (missingSharp > 0 && haveSharp == 0 && missingSharp == int(list.size())) {
        _noCoverage = true;
        _hipsStatus = tr("No coverage at this position");
    } else if (_loader->errorCount(_surveyId) > 0 && pending == 0 && haveSharp < int(list.size()) - missingSharp) {
        _hipsStatus = tr("Some tiles could not be downloaded");
    }
    _hipsDetail = QStringLiteral("%1 · order %2%3")
                      .arg(QUrl(st.baseUrl).host())
                      .arg(order)
                      .arg(st.fromCache ? tr(" · offline, cached tiles only") : QString());
}

void SkyMapWidget::renderLocal(const Basis& b, double radPerDevPx, int w, int h)
{
    const QImage& img = _localImage;
    if (img.isNull()) { _frame.fill(kSkyBackground); return; }

    const Basis lb = basisAt(_localRa, _localDec);
    const double radPerImgPx = _localScale * kArcsec;
    const double refX = img.width() / 2.0;
    const double refY = img.height() / 2.0;
    const QRgb bg = kSkyBackground.rgb();

    std::vector<Strip> strips = makeStrips(h);
    QtConcurrent::blockingMap(strips, [&](const Strip& s) {
        for (int y = s.y0; y < s.y1; ++y) {
            QRgb* out = reinterpret_cast<QRgb*>(_frame.scanLine(y));
            const double eta = -(y + 0.5 - h / 2.0) * radPerDevPx;
            for (int x = 0; x < w; ++x) {
                const double xi = -(x + 0.5 - w / 2.0) * radPerDevPx;
                const Vec3 v = add(b.c, add(mul(b.e, xi), mul(b.n, eta)));
                const double dc = dot(v, lb.c);
                QRgb col = bg;
                if (dc > 0) {
                    const double ix = refX - (dot(v, lb.e) / dc) / radPerImgPx;
                    const double iy = refY - (dot(v, lb.n) / dc) / radPerImgPx;
                    if (ix >= 0 && iy >= 0 && ix < img.width() && iy < img.height())
                        col = overBackground(sampleBilinear(img, ix, iy));
                }
                out[x] = col;
            }
        }
    });
}

// ── painting ────────────────────────────────────────────────────────────────

void SkyMapWidget::paintEvent(QPaintEvent*)
{
    if (!_viewInitialised && width() > 64 && height() > 64) resetView();
    if (_frameDirty || _frame.isNull()) renderFrame();

    QPainter p(this);
    p.drawImage(QPointF(0, 0), _frame);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (_layer == Layer::Flat) {   // no sky geometry: nothing can be placed on it
        drawInfoPanels(p);
        return;
    }
    if (_layer == Layer::Local) drawLocalFootprint(p);
    if (_gaiaVisible) drawGaiaMarkers(p);
    drawReticle(p);
    drawCompass(p);
    drawScaleBar(p);
    drawInfoPanels(p);
}

void SkyMapWidget::drawLocalFootprint(QPainter& p)
{
    if (_localImage.isNull()) return;
    // Outline the image's extent, so its edge is not mistaken for empty sky.
    const Basis lb = basisAt(_localRa, _localDec);
    const double r = _localScale * kArcsec;
    const double hw = _localImage.width() / 2.0 * r, hh = _localImage.height() / 2.0 * r;
    QPolygonF poly;
    const double corners[4][2] = { { hw, hh }, { -hw, hh }, { -hw, -hh }, { hw, -hh } };
    for (const auto& c : corners) {
        const Vec3 v = normalized(add(lb.c, add(mul(lb.e, c[0]), mul(lb.n, c[1]))));
        QPointF q;
        if (!vecToWidget(v, q)) return;
        poly << q;
    }
    p.save();
    QPen pen(QColor(255, 255, 255, 110), 1.0, Qt::DashLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(poly);
    p.restore();
}

double SkyMapWidget::markerRadius(const GaiaFieldSource& s) const
{
    if (!std::isfinite(s.gmag)) return 3.0;
    return std::clamp(2.5 + 0.9 * (19.5 - s.gmag), 2.5, 14.0);
}

bool SkyMapWidget::isTargetSource(const GaiaFieldSource& s) const
{
    return _targetGaia != 0 && s.sourceId == _targetGaia;
}

void SkyMapWidget::drawGaiaMarkers(QPainter& p)
{
    // Zoomed out past what is queried, the loaded sources would only pile up
    // in a blob around the target.
    if (_gaiaSources.isEmpty() || _gaiaTooWide) return;
    const QRectF bounds = QRectF(rect()).adjusted(-20, -20, 20, 20);

    // Faint first, so bright neighbours are drawn on top.
    std::vector<const GaiaFieldSource*> order;
    order.reserve(_gaiaSources.size());
    for (const GaiaFieldSource& s : _gaiaSources) order.push_back(&s);
    std::sort(order.begin(), order.end(), [](const GaiaFieldSource* a, const GaiaFieldSource* b) {
        const double ga = std::isfinite(a->gmag) ? a->gmag : 99.0;
        const double gb = std::isfinite(b->gmag) ? b->gmag : 99.0;
        return ga > gb;
    });

    p.save();
    for (const GaiaFieldSource* s : order) {
        if (isTargetSource(*s)) continue;
        bool vis = false;
        const QPointF q = skyToWidget(s->ra, s->dec, &vis);
        if (!vis || !bounds.contains(q)) continue;
        const double r = markerRadius(*s);
        const bool bright = std::isfinite(_targetGmag) && std::isfinite(s->gmag)
                            && s->gmag - _targetGmag < kBrightNeighbourDeltaG;
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 150), 3.2));
        p.drawEllipse(q, r, r);
        p.setPen(QPen(bright ? kGaiaBright : kGaiaFaint, 1.5));
        p.drawEllipse(q, r, r);
        if (s->sourceId == _selectedSource) {
            p.setPen(QPen(QColor(0, 0, 0, 160), 4.0));
            p.drawEllipse(q, r + 4, r + 4);
            p.setPen(QPen(kSelectedColor, 2.0));
            p.drawEllipse(q, r + 4, r + 4);
        }
    }
    p.restore();
}

void SkyMapWidget::drawReticle(QPainter& p)
{
    if (!_hasTarget) return;
    bool vis = false;
    const QPointF c = skyToWidget(_targetRa, _targetDec, &vis);
    if (!vis) return;
    // Four ticks with a gap, so the target itself is never covered.
    const double gap = 9, len = 16;
    p.save();
    for (const QPointF d : { QPointF(0, -1), QPointF(0, 1), QPointF(-1, 0), QPointF(1, 0) }) {
        const QPointF a = c + d * gap, e = c + d * (gap + len);
        p.setPen(QPen(QColor(0, 0, 0, 150), 4.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, e);
        p.setPen(QPen(kReticleColor, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, e);
    }
    p.restore();
}

void SkyMapWidget::drawCompass(QPainter& p)
{
    // North is up and east left at the view centre by construction; the
    // compass states it, as on the printed previews.
    const double arm = 34;
    const QPointF o(width() - 22, 30 + arm);
    auto arrow = [&](const QPointF& tip, const QColor& col, const QString& label, bool labelLeft) {
        p.setPen(QPen(QColor(0, 0, 0, 160), 4.5, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(o, tip);
        p.setPen(QPen(col, 2.2, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(o, tip);
        const QPointF dir = (tip - o) / arm;
        const QPointF perp(-dir.y(), dir.x());
        QPolygonF head;
        head << tip + dir * 2 << tip - dir * 7 + perp * 4 << tip - dir * 7 - perp * 4;
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawPolygon(head);
        QFont f = font();
        f.setBold(true);
        p.setFont(f);
        const QFontMetricsF fm(f);
        QPointF tp = labelLeft ? tip + QPointF(-fm.horizontalAdvance(label) - 6, fm.ascent() / 2 - 1)
                               : tip + QPointF(-fm.horizontalAdvance(label) / 2, -6);
        QPainterPath path;
        path.addText(tp, f, label);
        p.setPen(QPen(QColor(0, 0, 0, 170), 3.0));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setPen(Qt::NoPen);
        p.setBrush(col);
        p.drawPath(path);
    };
    p.save();
    arrow(o + QPointF(0, -arm), QColor(110, 170, 255), QStringLiteral("N"), false);
    arrow(o + QPointF(-arm, 0), kReticleColor, QStringLiteral("E"), true);
    p.restore();
}

void SkyMapWidget::drawScaleBar(QPainter& p)
{
    // A round length spanning roughly a fifth of the view.
    static const double nice[] = { 0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 20, 30, 60, 120, 180, 300,
                                   600, 900, 1200, 1800, 3600, 7200, 10800, 18000, 36000 };
    const double target = 0.2 * width() * _scale;
    double len = nice[0];
    for (double v : nice) if (v <= target * 1.25) len = v;
    const double px = len / _scale;

    const QPointF a(16, height() - 40), b(16 + px, height() - 40);
    p.save();
    p.setPen(QPen(QColor(0, 0, 0, 170), 6, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(a, b);
    p.setPen(QPen(QColor(255, 90, 230), 3, Qt::SolidLine, Qt::FlatCap));
    p.drawLine(a, b);
    QFont f = font();
    f.setBold(true);
    p.setFont(f);
    const QString label = formatAngle(len);
    const QFontMetricsF fm(f);
    QPainterPath path;
    path.addText(QPointF((a.x() + b.x()) / 2 - fm.horizontalAdvance(label) / 2, a.y() - 7), f, label);
    p.setPen(QPen(QColor(0, 0, 0, 170), 3.0));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 90, 230));
    p.drawPath(path);
    p.restore();
}

void SkyMapWidget::drawInfoPanels(QPainter& p)
{
    p.save();
    QFont f = font();
    const QFontMetricsF fm(f);
    const double pad = 6, lh = fm.height();

    // Top left: layer, where it comes from, what it is doing.
    QStringList lines;
    QList<QColor> colors;
    lines << _layerTitle;
    colors << kPanelText;
    if (!_hipsDetail.isEmpty()) { lines << _hipsDetail; colors << kPanelDim; }
    if (_layer == Layer::Hips) {
        const auto& st = _loader->survey(_surveyId);
        if (st.status == HipsTileLoader::Status::Failed && !st.error.isEmpty()) {
            lines << fm.elidedText(st.error, Qt::ElideRight, width() * 0.6);
            colors << kPanelDim;
        }
    }
    if (!_hipsStatus.isEmpty()) { lines << _hipsStatus; colors << QColor(255, 210, 120); }
    const bool sky = _layer != Layer::Flat;
    if ((_layer == Layer::None || _layer == Layer::Flat) && !_layerMessage.isEmpty()) {
        lines << _layerMessage; colors << QColor(255, 210, 120);
    }
    if (_gaiaVisible && sky) {
        QString g;
        if (_gaiaQuery->isRunning()) g = tr("Gaia DR3: loading...");
        else if (!_gaiaError.isEmpty()) g = tr("Gaia DR3: %1").arg(_gaiaError);
        else if (_gaiaTooWide) g = tr("Gaia DR3: zoom in to load sources");
        else if (_gaiaTruncated) g = tr("Gaia DR3: brightest %1 sources only").arg(kGaiaMaxRows);
        if (!g.isEmpty()) { lines << fm.elidedText(g, Qt::ElideRight, width() * 0.6); colors << kPanelDim; }
    }

    double wMax = 0;
    for (const QString& l : lines) wMax = std::max(wMax, fm.horizontalAdvance(l));
    QRectF box(10, 10, wMax + 2 * pad, lines.size() * lh + 2 * pad);
    drawPanel(p, box);
    for (int i = 0; i < lines.size(); ++i) {
        p.setPen(colors[i]);
        p.drawText(QPointF(box.left() + pad, box.top() + pad + fm.ascent() + i * lh), lines[i]);
    }
    double nextTop = box.bottom() + 6;

    // Gaia legend, under the layer panel.
    if (_gaiaVisible && sky && !_gaiaTooWide && !_gaiaSources.isEmpty()) {
        const QString bright = std::isfinite(_targetGmag)
            ? tr("ΔG < %1").arg(kBrightNeighbourDeltaG, 0, 'f', 1)
            : QString();
        const QString faint = std::isfinite(_targetGmag) ? tr("fainter") : tr("Gaia DR3");
        const double rowW = 18 + fm.horizontalAdvance(bright) + 14 + 18 + fm.horizontalAdvance(faint);
        QRectF lb(10, nextTop, rowW + 2 * pad, lh + 2 * pad);
        drawPanel(p, lb);
        double x = lb.left() + pad;
        const double cy = lb.top() + pad + lh / 2;
        if (!bright.isEmpty()) {
            p.setPen(QPen(kGaiaBright, 1.5));
            p.setBrush(Qt::NoBrush);
            p.drawEllipse(QPointF(x + 6, cy), 5, 5);
            p.setPen(kPanelText);
            p.drawText(QPointF(x + 16, lb.top() + pad + fm.ascent()), bright);
            x += 18 + fm.horizontalAdvance(bright) + 14;
        }
        p.setPen(QPen(kGaiaFaint, 1.5));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QPointF(x + 6, cy), 5, 5);
        p.setPen(kPanelText);
        p.drawText(QPointF(x + 16, lb.top() + pad + fm.ascent()), faint);
        nextTop = lb.bottom() + 6;
    }

    // Selected Gaia source details.
    if (_gaiaVisible && sky && _selectedSource != 0) {
        auto it = _gaiaSources.constFind(_selectedSource);
        if (it != _gaiaSources.cend()) {
            const QStringList info = gaiaSourceText(*it, false).split('\n');
            double iw = 0;
            for (const QString& l : info) iw = std::max(iw, fm.horizontalAdvance(l));
            QRectF ib(10, nextTop, iw + 2 * pad, info.size() * lh + 2 * pad);
            drawPanel(p, ib);
            p.setPen(kPanelText);
            for (int i = 0; i < info.size(); ++i)
                p.drawText(QPointF(ib.left() + pad, ib.top() + pad + fm.ascent() + i * lh), info[i]);
        }
    }

    if (!sky) { p.restore(); return; }

    // Bottom right: cursor position and field of view.
    QString pos;
    if (_cursorInside) pos = formatCoords(widgetToVec(_cursor));
    const QString fov = tr("FoV %1 × %2").arg(formatAngle(_scale * width()),
                                                    formatAngle(_scale * height()));
    const QString text = pos.isEmpty() ? fov : pos + QStringLiteral("   ") + fov;
    const double tw = fm.horizontalAdvance(text);
    QRectF cb(width() - tw - 2 * pad - 10, height() - lh - 2 * pad - 10, tw + 2 * pad, lh + 2 * pad);
    drawPanel(p, cb);
    p.setPen(kPanelText);
    p.drawText(QPointF(cb.left() + pad, cb.top() + pad + fm.ascent()), text);

    p.restore();
}

QString SkyMapWidget::formatCoords(const Vec3& v) const
{
    double ra, dec;
    Healpix::lonLatFromVec(v, ra, dec);
    return QStringLiteral("%1 %2").arg(formatSexagesimal(ra / 15.0, 2, false),
                                       formatSexagesimal(dec, 1, true));
}

// ── Gaia ────────────────────────────────────────────────────────────────────

void SkyMapWidget::ensureGaiaCoverage()
{
    if (!_gaiaVisible || !_hasTarget) return;
    const Vec3 c = Healpix::vecFromLonLat(_centerRa, _centerDec);
    const double halfDiag = 0.5 * std::hypot(width(), height()) * _scale * kArcsec;

    _gaiaTooWide = halfDiag > kGaiaMaxViewRadiusDeg * kDeg;
    if (_gaiaTooWide) { update(); return; }

    for (const Cone& k : _gaiaCones)
        if (angleBetween(c, k.c) + halfDiag <= k.radiusRad) return;   // already loaded

    // Load a margin around the view, so small pans need no new query.
    const double radiusDeg = std::clamp(halfDiag * 1.6 / kDeg, 3.0 / 60.0, kGaiaMaxQueryRadiusDeg);
    _gaiaError.clear();
    _gaiaQuery->query(_centerRa, _centerDec, radiusDeg, kGaiaMaxRows);
    update();
}

void SkyMapWidget::onGaiaFinished(double ra, double dec, double radius,
                                  const QVector<GaiaFieldSource>& sources, bool truncated,
                                  const QString& error)
{
    if (!error.isEmpty()) {
        _gaiaError = error;
        update();
        return;
    }
    for (const GaiaFieldSource& s : sources) _gaiaSources.insert(s.sourceId, s);
    _gaiaTruncated = truncated;
    // A truncated cone is missing its faint sources, so it does not count as
    // covered for anything but its own extent of bright stars; still record
    // it, or the same query would repeat on every pan.
    _gaiaCones.push_back({ Healpix::vecFromLonLat(ra, dec), radius * kDeg });
    if (!std::isfinite(_targetGmag) && _targetGaia != 0) {
        auto it = _gaiaSources.constFind(_targetGaia);
        if (it != _gaiaSources.cend()) _targetGmag = it->gmag;
    }
    update();
}

const GaiaFieldSource* SkyMapWidget::gaiaSourceAt(const QPointF& p) const
{
    if (!_gaiaVisible || _gaiaTooWide || _layer == Layer::Flat) return nullptr;
    const GaiaFieldSource* best = nullptr;
    double bestD = 1e9;
    for (const GaiaFieldSource& s : _gaiaSources) {
        if (isTargetSource(s)) continue;
        bool vis = false;
        const QPointF q = skyToWidget(s.ra, s.dec, &vis);
        if (!vis) continue;
        const double d = std::hypot(q.x() - p.x(), q.y() - p.y());
        const double reach = std::max(6.0, markerRadius(s) + 3.0);
        if (d <= reach && d < bestD) { bestD = d; best = &s; }
    }
    return best;
}

QString SkyMapWidget::gaiaSourceText(const GaiaFieldSource& s, bool html) const
{
    QStringList l;
    l << tr("Gaia DR3 %1").arg(s.sourceId);
    if (std::isfinite(s.gmag)) {
        QString g = tr("G = %1").arg(s.gmag, 0, 'f', 2);
        if (std::isfinite(_targetGmag))
            g += tr("  (ΔG = %1%2 vs. target)")
                     .arg(s.gmag - _targetGmag >= 0 ? "+" : "")
                     .arg(s.gmag - _targetGmag, 0, 'f', 2);
        l << g;
    }
    if (std::isfinite(s.bpRp)) l << tr("BP-RP = %1").arg(s.bpRp, 0, 'f', 2);
    if (std::isfinite(s.parallax)) {
        QString plx = tr("Parallax = %1").arg(s.parallax, 0, 'f', 3);
        if (std::isfinite(s.parallaxError)) plx += tr(" ± %1").arg(s.parallaxError, 0, 'f', 3);
        l << plx + tr(" mas");
    }
    if (std::isfinite(s.pmra) && std::isfinite(s.pmdec))
        l << tr("PM = (%1, %2) mas/yr").arg(s.pmra, 0, 'f', 2).arg(s.pmdec, 0, 'f', 2);
    if (std::isfinite(s.ruwe)) l << tr("RUWE = %1").arg(s.ruwe, 0, 'f', 2);
    if (_hasTarget) {
        const double sep = angleBetween(Healpix::vecFromLonLat(s.ra, s.dec),
                                        Healpix::vecFromLonLat(_targetRa, _targetDec)) / kArcsec;
        l << tr("%1 from the target").arg(formatAngle(sep));
    }
    if (html) {
        l[0] = QStringLiteral("<b>") + l[0].toHtmlEscaped() + QStringLiteral("</b>");
        for (int i = 1; i < l.size(); ++i) l[i] = l[i].toHtmlEscaped();
        return l.join(QStringLiteral("<br>"));
    }
    return l.join('\n');
}

// ── interaction ─────────────────────────────────────────────────────────────

void SkyMapWidget::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (!_viewInitialised) resetView();
    _frameDirty = true;
    if (_gaiaVisible) _gaiaTimer->start();
}

void SkyMapWidget::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    _dragging = true;
    _dragMoved = false;
    _pressPos = e->position().toPoint();
    _pressCenter = Healpix::vecFromLonLat(_centerRa, _centerDec);
    _pressScale = _scale;
    setCursor(Qt::ClosedHandCursor);
}

void SkyMapWidget::mouseMoveEvent(QMouseEvent* e)
{
    _cursor = e->position();
    _cursorInside = true;

    if (_dragging) {
        const QPoint d = e->position().toPoint() - _pressPos;
        if (!_dragMoved && d.manhattanLength() < 3) return;
        _dragMoved = true;
        QToolTip::hideText();
        // Pan in the projection the drag started in, so the point under the
        // cursor stays under it.
        const Vec3 pc = _pressCenter;
        double ra0, dec0;
        Healpix::lonLatFromVec(pc, ra0, dec0);
        const Basis b = basisAt(ra0, dec0);
        const double s = _pressScale * kArcsec;
        const double xi = d.x() * s, eta = d.y() * s;   // move the centre opposite to the drag
        setCenter(normalized(add(b.c, add(mul(b.e, xi), mul(b.n, eta)))));
        viewChanged();
        return;
    }

    if (const GaiaFieldSource* s = gaiaSourceAt(_cursor))
        QToolTip::showText(e->globalPosition().toPoint(), gaiaSourceText(*s, true), this);
    else
        QToolTip::hideText();
    update();   // cursor readout
}

void SkyMapWidget::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(e); return; }
    const bool wasClick = _dragging && !_dragMoved;
    _dragging = false;
    setCursor(Qt::OpenHandCursor);
    if (wasClick) {
        const GaiaFieldSource* s = gaiaSourceAt(e->position());
        _selectedSource = s ? s->sourceId : 0;
        update();
    }
}

void SkyMapWidget::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) return;
    setCenter(widgetToVec(e->position()));
    viewChanged();
}

void SkyMapWidget::wheelEvent(QWheelEvent* e)
{
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0) return;
    const double oldScale = _scale;
    setScale(_scale * std::pow(1.0 / 1.25, steps));
    if (_scale == oldScale) return;
    // Keep the sky point under the cursor where it is.
    const QPointF c(width() / 2.0, height() / 2.0);
    const QPointF cur = e->position();
    const QPointF shift = (cur - c) * (1.0 - _scale / oldScale);
    const double keep = _scale;
    _scale = oldScale;
    const Vec3 nc = widgetToVec(c + shift);
    _scale = keep;
    setCenter(nc);
    viewChanged();
    e->accept();
}

void SkyMapWidget::keyPressEvent(QKeyEvent* e)
{
    switch (e->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomBy(1.25); break;
    case Qt::Key_Minus: zoomBy(1.0 / 1.25); break;
    case Qt::Key_Home:
    case Qt::Key_0:     resetView(); break;
    default:            QWidget::keyPressEvent(e); return;
    }
    e->accept();
}

void SkyMapWidget::leaveEvent(QEvent* e)
{
    _cursorInside = false;
    update();
    QWidget::leaveEvent(e);
}

void SkyMapWidget::contextMenuEvent(QContextMenuEvent* e)
{
    QMenu menu(this);
    const QPointF pos = e->pos();
    const Vec3 v = widgetToVec(pos);
    if (const GaiaFieldSource* s = gaiaSourceAt(pos)) {
        const qint64 id = s->sourceId;
        menu.addAction(tr("Copy Gaia DR3 source id"), this, [id] {
            QApplication::clipboard()->setText(QString::number(id));
        });
        menu.addSeparator();
    }
    menu.addAction(tr("Copy coordinates here"), this, [this, v] {
        double ra, dec;
        Healpix::lonLatFromVec(v, ra, dec);
        QApplication::clipboard()->setText(QStringLiteral("%1 %2")
            .arg(ra, 0, 'f', 6).arg(dec, 0, 'f', 6));
    });
    menu.addAction(tr("Centre here"), this, [this, v] { setCenter(v); viewChanged(); });
    menu.addAction(tr("Reset view"), this, [this] { resetView(); });
    menu.exec(e->globalPos());
}

} // namespace SkyMap
