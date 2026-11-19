#include "skymap/ui/SkyLayerList.h"

#include "app/ui/panels/PanelUtils.h"
#include "skymap/Healpix.h"
#include "skymap/HipsSurvey.h"
#include "skymap/ui/HipsRender.h"
#include "skymap/ui/HipsTileLoader.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace SkyMap {

namespace {

constexpr double kArcsec = 3.14159265358979323846 / 180.0 / 3600.0;
// Card previews show at least this field, widened for coarse surveys so a few
// of their pixels fit (a 4' cutout of Fermi would be one flat colour).
constexpr double kPreviewMinFov = 240.0;          // arcsec
constexpr double kPreviewMaxFov = 3.0 * 3600.0;
constexpr double kPreviewSurveyPixels = 40.0;
const QRgb kSkyBackground = qRgb(11, 14, 20);

// Every pixel that has data is (near) black: a JPEG survey's fill outside its
// footprint.
bool allBlack(const QImage& img)
{
    int seen = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* row = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            if (qAlpha(row[x]) == 0) continue;
            ++seen;
            if (qRed(row[x]) > 4 || qGreen(row[x]) > 4 || qBlue(row[x]) > 4) return false;
        }
    }
    return seen > 0;
}

class CardDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        const QFontMetrics fm(opt.font);
        if (idx.data(SkyLayerList::HeaderRole).toBool())
            return { 200, fm.height() + 16 };
        return { 200, SkyLayerList::kThumbPx + 14 };
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        using PanelUtils::mix;
        const QColor bg = PanelUtils::themeBg();
        const QColor fg = PanelUtils::themeFg();
        const QColor accent = PanelUtils::themeAccent();

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setRenderHint(QPainter::SmoothPixmapTransform, true);

        if (idx.data(SkyLayerList::HeaderRole).toBool()) {
            QFont f = opt.font;
            f.setBold(true);
            p->setFont(f);
            p->setPen(mix(fg, bg, 0.4));
            const QRect r = opt.rect.adjusted(10, 0, -8, -3);
            p->drawText(r, Qt::AlignLeft | Qt::AlignBottom, idx.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }

        const auto state = SkyLayerList::State(idx.data(SkyLayerList::StateRole).toInt());
        const bool greyed = state == SkyLayerList::State::NoCoverage
                         || state == SkyLayerList::State::Unavailable;
        const bool selected = opt.state & QStyle::State_Selected;
        const bool hover = opt.state & QStyle::State_MouseOver;

        // Card
        const QRectF card = QRectF(opt.rect).adjusted(6.5, 3.5, -6.5, -3.5);
        QColor fill = mix(bg, fg, 0.035);
        if (hover) fill = mix(bg, fg, 0.07);
        if (selected) fill = mix(bg, accent, 0.22);
        p->setPen(QPen(selected ? accent : mix(bg, fg, 0.12), selected ? 1.6 : 1.0));
        p->setBrush(fill);
        p->drawRoundedRect(card, 6, 6);

        // Preview
        const int t = SkyLayerList::kThumbPx;
        const QRectF thumbRect(card.left() + 5, card.center().y() - t / 2.0, t, t);
        QPainterPath clip;
        clip.addRoundedRect(thumbRect, 4, 4);
        p->save();
        p->setClipPath(clip);
        const QImage thumb = idx.data(SkyLayerList::ThumbRole).value<QImage>();
        if (thumb.isNull()) {
            p->fillRect(thumbRect, mix(bg, fg, 0.08));
        } else {
            QImage shown = thumb;
            if (greyed) {
                shown = thumb.convertToFormat(QImage::Format_Grayscale8);
                p->setOpacity(0.45);
            }
            p->drawImage(thumbRect, shown);
            p->setOpacity(1.0);
        }
        p->restore();

        // Text
        const double tx = thumbRect.right() + 10;
        const double tw = card.right() - 8 - tx;
        QFont titleFont = opt.font;
        titleFont.setBold(true);
        QFont smallFont = opt.font;
        smallFont.setPointSizeF(opt.font.pointSizeF() * 0.9);
        const QFontMetricsF tfm(titleFont), sfm(smallFont);

        const QString title = idx.data(Qt::DisplayRole).toString();
        const QString sub = idx.data(SkyLayerList::SubtitleRole).toString();
        const QString status = idx.data(SkyLayerList::StatusRole).toString();

        const double blockH = tfm.height() + 2 * sfm.height() + 2;
        double y = card.center().y() - blockH / 2;
        p->setFont(titleFont);
        p->setPen(greyed ? mix(fg, bg, 0.5) : fg);
        p->drawText(QPointF(tx, y + tfm.ascent()), tfm.elidedText(title, Qt::ElideRight, tw));
        y += tfm.height() + 2;
        p->setFont(smallFont);
        p->setPen(mix(fg, bg, greyed ? 0.6 : 0.4));
        p->drawText(QPointF(tx, y + sfm.ascent()), sfm.elidedText(sub, Qt::ElideRight, tw));
        y += sfm.height();
        if (!status.isEmpty()) {
            QFont st = smallFont;
            st.setItalic(greyed || state == SkyLayerList::State::Pending);
            p->setFont(st);
            p->setPen(mix(fg, bg, 0.5));
            p->drawText(QPointF(tx, y + sfm.ascent()), QFontMetricsF(st).elidedText(status, Qt::ElideRight, tw));
        }
        p->restore();
    }
};

} // namespace

SkyLayerList::SkyLayerList(HipsTileLoader* loader, QWidget* parent)
    : QListView(parent), _loader(loader), _model(new QStandardItemModel(this))
{
    setModel(_model);
    setItemDelegate(new CardDelegate(this));
    setSelectionMode(QAbstractItemView::SingleSelection);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setMouseTracking(true);
    setUniformItemSizes(false);
    setMinimumWidth(200);

    _timer = new QTimer(this);
    _timer->setSingleShot(true);
    _timer->setInterval(60);
    connect(_timer, &QTimer::timeout, this, [this] {
        const QSet<QString> ids = std::exchange(_dirty, {});
        for (const QString& id : ids) renderPreview(id);
    });

    connect(_loader, &HipsTileLoader::surveyChanged, this, [this](const QString& id) {
        _final.remove(id);
        schedulePreview(id);
    });
    connect(_loader, &HipsTileLoader::tileArrived, this, &SkyLayerList::schedulePreview);

    connect(selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& cur) {
        const QString key = cur.data(KeyRole).toString();
        if (!key.isEmpty()) emit currentKeyChanged(key);
    });
}

void SkyLayerList::setTarget(double raDeg, double decDeg)
{
    _ra = raDeg;
    _dec = decDeg;
    _final.clear();
    if (_started)
        for (const HipsSurvey& s : hipsSurveys()) schedulePreview(s.id);
}

void SkyLayerList::addHeader(const QString& title)
{
    auto* it = new QStandardItem(title);
    it->setData(true, HeaderRole);
    it->setFlags(Qt::NoItemFlags);
    _model->appendRow(it);
}

void SkyLayerList::addLayer(const QString& key, const QString& title, const QString& subtitle,
                            const QString& tooltip)
{
    auto* it = new QStandardItem(title);
    it->setData(key, KeyRole);
    it->setData(subtitle, SubtitleRole);
    it->setData(int(State::Pending), StateRole);
    it->setToolTip(tooltip);
    it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    _model->appendRow(it);
    _items.insert(key, it);
}

void SkyLayerList::addHipsSurveys()
{
    bool first = true;
    Regime regime = Regime::Gamma;
    for (const HipsSurvey& s : hipsSurveys()) {
        if (first || s.regime != regime) {
            addHeader(regimeName(s.regime));
            regime = s.regime;
            first = false;
        }
        addLayer(kHipsLayerPrefix + s.id, s.title, s.band, s.id);
    }
}

QStandardItem* SkyLayerList::itemFor(const QString& key) const
{
    return _items.value(key, nullptr);
}

void SkyLayerList::setState(QStandardItem* it, State state, const QString& status)
{
    if (it->data(StateRole).toInt() != int(state)) it->setData(int(state), StateRole);
    if (it->data(StatusRole).toString() != status) it->setData(status, StatusRole);
}

void SkyLayerList::setLayerPreview(const QString& key, const QImage& thumb, State state,
                                   const QString& status)
{
    QStandardItem* it = itemFor(key);
    if (!it) return;
    it->setData(thumb, ThumbRole);
    setState(it, state, status);
}

void SkyLayerList::setLayerTitle(const QString& key, const QString& title)
{
    if (QStandardItem* it = itemFor(key)) it->setText(title);
}

SkyLayerList::State SkyLayerList::layerState(const QString& key) const
{
    const QStandardItem* it = itemFor(key);
    return it ? State(it->data(StateRole).toInt()) : State::Unavailable;
}

void SkyLayerList::startPreviews()
{
    if (_started) return;
    _started = true;
    for (const HipsSurvey& s : hipsSurveys()) {
        _loader->survey(s.id);   // probe the mirrors
        schedulePreview(s.id);
    }
}

void SkyLayerList::schedulePreview(const QString& surveyId)
{
    if (!_started || _final.contains(surveyId) || !_items.contains(kHipsLayerPrefix + surveyId))
        return;
    _dirty.insert(surveyId);
    if (!_timer->isActive()) _timer->start();
}

void SkyLayerList::renderPreview(const QString& id)
{
    QStandardItem* it = itemFor(kHipsLayerPrefix + id);
    if (!it || !std::isfinite(_ra) || !std::isfinite(_dec)) return;

    const HipsTileLoader::SurveyState& st = _loader->survey(id);
    if (st.status == HipsTileLoader::Status::Failed) {
        setState(it, State::Unavailable, tr("No server answered"));
        _final.insert(id);
        return;
    }
    if (st.status != HipsTileLoader::Status::Ready) {
        setState(it, State::Pending, tr("Finding a server..."));
        return;
    }

    const HipsProperties& p = st.props;
    int bits = 0;
    while ((1 << bits) < p.tileWidth) ++bits;
    const double native = Healpix::cellSizeArcsec(p.maxOrder + bits);
    const double fov = std::clamp(kPreviewSurveyPixels * native, kPreviewMinFov, kPreviewMaxFov);
    const qreal dpr = devicePixelRatioF();
    const int px = std::max(16, int(std::lround(kThumbPx * dpr)));

    QImage img(px, px, QImage::Format_ARGB32);
    const HipsRenderStats stats = renderHips(*_loader, id, p, tangentBasisAt(_ra, _dec),
                                             fov * kArcsec / px, img, true, 0, false);
    _loader->requestBackground(id, stats.wanted);

    // Does the survey cover the star itself? Its tile is absent, or present
    // but transparent (PNG footprint) or black fill (JPEG) at the star.
    const TileKey starTile = tileAt(p, stats.order, _ra, _dec);
    const bool haveStarTile = _loader->tile(id, starTile) != nullptr;
    const bool starMissing = _loader->isMissing(id, starTile);
    const bool complete = stats.have + stats.missing == stats.needed;

    State state = State::Ready;
    QString status = tr("%1 around the star").arg(formatSkyAngle(fov));
    if (starMissing
        || (haveStarTile && (qAlpha(img.pixel(px / 2, px / 2)) == 0 || allBlack(img)))) {
        state = State::NoCoverage;
        status = tr("No coverage at the star");
    } else if (!haveStarTile) {
        state = State::Pending;
        status = st.fromCache ? tr("Offline, not cached") : tr("Loading preview...");
    } else if (st.fromCache) {
        status = tr("Offline, cached tiles");
    }

    QImage shown(px, px, QImage::Format_RGB32);
    shown.fill(kSkyBackground);
    {
        QPainter sp(&shown);
        sp.drawImage(0, 0, img);
    }
    shown.setDevicePixelRatio(dpr);
    it->setData(shown, ThumbRole);
    setState(it, state, status);

    if (state == State::NoCoverage || complete || (st.fromCache && !haveStarTile))
        _final.insert(id);
}

QString SkyLayerList::currentKey() const
{
    return currentIndex().data(KeyRole).toString();
}

bool SkyLayerList::setCurrentKey(const QString& key)
{
    QStandardItem* it = itemFor(key);
    if (!it) return false;
    setCurrentIndex(it->index());
    scrollTo(it->index(), QAbstractItemView::PositionAtCenter);
    return true;
}

void SkyLayerList::step(int delta)
{
    const int n = _model->rowCount();
    if (n == 0) return;
    const int start = std::max(0, currentIndex().row());
    // Prefer cards that show something; fall back to any card.
    for (bool strict : { true, false }) {
        int i = start;
        for (int k = 0; k < n; ++k) {
            i = ((i + delta) % n + n) % n;
            const QStandardItem* it = _model->item(i);
            if (it->data(KeyRole).toString().isEmpty()) continue;
            const State s = State(it->data(StateRole).toInt());
            if (strict && (s == State::NoCoverage || s == State::Unavailable)) continue;
            setCurrentIndex(it->index());
            scrollTo(it->index());
            return;
        }
    }
}

} // namespace SkyMap
