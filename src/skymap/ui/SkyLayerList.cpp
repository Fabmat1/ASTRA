#include "skymap/ui/SkyLayerList.h"

#include "app/ui/panels/PanelUtils.h"
#include "skymap/Healpix.h"
#include "skymap/HipsSurvey.h"
#include "skymap/ui/HipsRender.h"
#include "skymap/ui/HipsTileLoader.h"

#include <QLinearGradient>
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
// Previews span at least this field across, widened for coarse surveys so a
// few of their pixels fit (a 5' strip of Fermi would be one flat colour).
constexpr double kPreviewMinFov = 300.0;          // arcsec
constexpr double kPreviewMaxFov = 3.0 * 3600.0;
constexpr double kPreviewSurveyPixels = 40.0;
constexpr int    kCardHeight = 40;                // logical, excluding the gap
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

bool isGreyed(SkyLayerList::State s)
{
    return s == SkyLayerList::State::NoCoverage || s == SkyLayerList::State::Unavailable;
}

// The part of `img` with the aspect ratio of `target`, centred.
QRectF centreCrop(const QImage& img, const QRectF& target)
{
    const double iw = img.width(), ih = img.height();
    const double a = target.width() / target.height();
    if (iw / ih > a) {
        const double w = ih * a;
        return { (iw - w) / 2, 0, w, ih };
    }
    const double h = iw / a;
    return { 0, (ih - h) / 2, iw, h };
}

class CardDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        const QFontMetrics fm(opt.font);
        if (idx.data(SkyLayerList::HeaderRole).toBool()) return { 200, fm.height() + 10 };
        if (idx.data(SkyLayerList::FooterRole).toBool()) return { 200, fm.height() + 18 };
        return { 200, kCardHeight + 5 };
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override
    {
        using PanelUtils::mix;
        const QColor bg = PanelUtils::themeBg();
        const QColor fg = PanelUtils::themeFg();
        const QColor accent = PanelUtils::themeAccent();
        const bool hover = opt.state & QStyle::State_MouseOver;

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);
        p->setRenderHint(QPainter::SmoothPixmapTransform, true);

        if (idx.data(SkyLayerList::HeaderRole).toBool()) {
            QFont f = opt.font;
            f.setBold(true);
            f.setPointSizeF(opt.font.pointSizeF() * 0.9);
            p->setFont(f);
            p->setPen(mix(fg, bg, 0.45));
            p->drawText(opt.rect.adjusted(9, 0, -8, -3), Qt::AlignLeft | Qt::AlignBottom,
                        idx.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }

        if (idx.data(SkyLayerList::FooterRole).toBool()) {
            QFont f = opt.font;
            f.setUnderline(hover);
            p->setFont(f);
            p->setPen(hover ? accent : mix(fg, bg, 0.35));
            p->drawText(opt.rect, Qt::AlignCenter, idx.data(Qt::DisplayRole).toString());
            p->restore();
            return;
        }

        const auto state = SkyLayerList::State(idx.data(SkyLayerList::StateRole).toInt());
        const bool greyed = isGreyed(state);
        const bool selected = opt.state & QStyle::State_Selected;
        const QRectF card = QRectF(opt.rect).adjusted(6, 2.5, -6, -2.5);
        QPainterPath shape;
        shape.addRoundedRect(card, 5, 5);

        // The preview is the card: cropped to its shape, centred on the star.
        const QImage preview = idx.data(SkyLayerList::ThumbRole).value<QImage>();
        const bool hasImage = !preview.isNull();
        p->save();
        p->setClipPath(shape);
        p->fillRect(card, hasImage ? QColor(kSkyBackground) : mix(bg, fg, 0.07));
        if (hasImage) {
            if (greyed) p->setOpacity(0.4);
            const QImage shown = greyed ? preview.convertToFormat(QImage::Format_Grayscale8) : preview;
            p->drawImage(card, shown, centreCrop(shown, card));
            p->setOpacity(1.0);
            // Darken toward the label so it reads on any image.
            QLinearGradient g(card.topLeft(), card.topRight());
            g.setColorAt(0.0, QColor(0, 0, 0, 185));
            g.setColorAt(0.55, QColor(0, 0, 0, 95));
            g.setColorAt(1.0, QColor(0, 0, 0, 15));
            p->fillRect(card, g);
        }
        if (selected) p->fillRect(card, QColor(accent.red(), accent.green(), accent.blue(), 45));
        p->restore();

        // Label
        const QColor ink = hasImage ? QColor(255, 255, 255) : fg;
        const QColor inkDim = hasImage ? QColor(255, 255, 255, 175) : mix(fg, bg, 0.45);
        QFont titleFont = opt.font;
        titleFont.setBold(true);
        QFont smallFont = opt.font;
        smallFont.setPointSizeF(opt.font.pointSizeF() * 0.85);
        smallFont.setItalic(true);
        const QFontMetricsF tfm(titleFont), sfm(smallFont);
        const double tx = card.left() + 10, tw = card.width() - 20;
        const QString title = idx.data(Qt::DisplayRole).toString();
        const QString status = idx.data(SkyLayerList::StatusRole).toString();

        p->setFont(titleFont);
        p->setPen(greyed ? inkDim : ink);
        if (status.isEmpty()) {
            p->drawText(QRectF(tx, card.top(), tw, card.height()), Qt::AlignLeft | Qt::AlignVCenter,
                        tfm.elidedText(title, Qt::ElideRight, tw));
        } else {
            const double y = card.center().y() - (tfm.height() + sfm.height()) / 2;
            p->drawText(QPointF(tx, y + tfm.ascent()), tfm.elidedText(title, Qt::ElideRight, tw));
            p->setFont(smallFont);
            p->setPen(inkDim);
            p->drawText(QPointF(tx, y + tfm.height() + sfm.ascent() - 1),
                        sfm.elidedText(status, Qt::ElideRight, tw));
        }

        // Outline
        p->setBrush(Qt::NoBrush);
        if (selected)   p->setPen(QPen(accent, 2.0));
        else if (hover) p->setPen(QPen(mix(bg, fg, 0.45), 1.0));
        else            p->setPen(QPen(mix(bg, fg, 0.14), 1.0));
        p->drawPath(shape);
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
    setMinimumWidth(180);

    _footer = new QStandardItem;
    _footer->setData(true, FooterRole);
    _footer->setFlags(Qt::ItemIsEnabled);
    _model->appendRow(_footer);

    _timer = new QTimer(this);
    _timer->setSingleShot(true);
    _timer->setInterval(60);
    connect(_timer, &QTimer::timeout, this, [this] {
        const QSet<QString> ids = std::exchange(_dirty, {});
        for (const QString& id : ids) renderPreview(id);
    });

    _visibilityTimer = new QTimer(this);
    _visibilityTimer->setSingleShot(true);
    _visibilityTimer->setInterval(0);
    connect(_visibilityTimer, &QTimer::timeout, this, &SkyLayerList::updateVisibility);

    connect(_loader, &HipsTileLoader::surveyChanged, this, [this](const QString& id) {
        _final.remove(id);
        schedulePreview(id);
    });
    connect(_loader, &HipsTileLoader::tileArrived, this, &SkyLayerList::schedulePreview);

    connect(selectionModel(), &QItemSelectionModel::currentChanged, this,
            [this](const QModelIndex& cur) {
        const QString key = cur.data(KeyRole).toString();
        if (!key.isEmpty()) emit currentKeyChanged(key);
        _visibilityTimer->start();   // the current card always stays visible
    });
    connect(this, &QAbstractItemView::clicked, this, [this](const QModelIndex& idx) {
        if (!idx.data(FooterRole).toBool()) return;
        _showUncovered = !_showUncovered;
        updateVisibility();
        if (_showUncovered) scrollTo(idx);
    });

    updateVisibility();
}

void SkyLayerList::setTarget(double raDeg, double decDeg)
{
    _ra = raDeg;
    _dec = decDeg;
    _final.clear();
    if (_started)
        for (const HipsSurvey& s : hipsSurveys()) schedulePreview(s.id);
}

void SkyLayerList::appendRow(QStandardItem* it)
{
    _model->insertRow(_footer->row(), it);
}

void SkyLayerList::addHeader(const QString& title)
{
    auto* it = new QStandardItem(title);
    it->setData(true, HeaderRole);
    it->setFlags(Qt::NoItemFlags);
    appendRow(it);
}

void SkyLayerList::addLayer(const QString& key, const QString& title, const QString& tooltip)
{
    auto* it = new QStandardItem(title);
    it->setData(key, KeyRole);
    it->setData(int(State::Pending), StateRole);
    it->setToolTip(tooltip);
    it->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
    appendRow(it);
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
        addLayer(kHipsLayerPrefix + s.id, s.title,
                 QStringLiteral("%1, %2\n%3").arg(s.title, s.band, s.id));
    }
    updateVisibility();
}

QStandardItem* SkyLayerList::itemFor(const QString& key) const
{
    return _items.value(key, nullptr);
}

void SkyLayerList::setState(QStandardItem* it, State state, const QString& status)
{
    const bool changed = it->data(StateRole).toInt() != int(state);
    if (changed) it->setData(int(state), StateRole);
    if (it->data(StatusRole).toString() != status) it->setData(status, StatusRole);
    if (changed) _visibilityTimer->start();
}

void SkyLayerList::updateVisibility()
{
    const QString current = currentKey();
    int uncovered = 0;
    // Walk bottom-up so each header knows whether a card below it is visible.
    bool cardBelow = false;
    for (int r = _model->rowCount() - 1; r >= 0; --r) {
        const QStandardItem* it = _model->item(r);
        if (it == _footer) continue;
        if (it->data(HeaderRole).toBool()) {
            setRowHidden(r, !cardBelow);
            cardBelow = false;
            continue;
        }
        const QString key = it->data(KeyRole).toString();
        const bool greyed = isGreyed(State(it->data(StateRole).toInt()));
        if (greyed) ++uncovered;
        const bool hide = greyed && !_showUncovered && key != current;
        setRowHidden(r, hide);
        if (!hide) cardBelow = true;
    }
    _footer->setText(_showUncovered
        ? tr("Hide sources without coverage")
        : tr("%n more without coverage here", nullptr, uncovered));
    setRowHidden(_footer->row(), uncovered == 0);
}

void SkyLayerList::setLayerPreview(const QString& key, const QImage& preview, State state,
                                   const QString& status)
{
    QStandardItem* it = itemFor(key);
    if (!it) return;
    it->setData(preview, ThumbRole);
    setState(it, state, status);
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
    const int w = std::max(16, int(std::lround(kPreviewWidth * dpr)));
    const int h = std::max(8, int(std::lround(kPreviewHeight * dpr)));

    QImage img(w, h, QImage::Format_ARGB32);
    const HipsRenderStats stats = renderHips(*_loader, id, p, tangentBasisAt(_ra, _dec),
                                             fov * kArcsec / w, img, true, 0, false);
    _loader->requestBackground(id, stats.wanted);

    // Does the survey cover the star itself? Its tile is absent, or present
    // but transparent (PNG footprint) or black fill (JPEG) at the star.
    const TileKey starTile = tileAt(p, stats.order, _ra, _dec);
    const bool haveStarTile = _loader->tile(id, starTile) != nullptr;
    const bool starMissing = _loader->isMissing(id, starTile);
    const bool complete = stats.have + stats.missing == stats.needed;

    State state = State::Ready;
    QString status;
    if (starMissing
        || (haveStarTile && (qAlpha(img.pixel(w / 2, h / 2)) == 0 || allBlack(img)))) {
        state = State::NoCoverage;
        status = tr("No coverage at the star");
    } else if (!haveStarTile) {
        state = State::Pending;
        status = st.fromCache ? tr("Offline, not cached") : tr("Loading...");
    } else if (st.fromCache) {
        status = tr("Offline, cached tiles");
    }

    QImage shown(w, h, QImage::Format_RGB32);
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
    updateVisibility();
    scrollTo(it->index(), QAbstractItemView::PositionAtCenter);
    return true;
}

void SkyLayerList::step(int delta)
{
    const int n = _model->rowCount();
    if (n == 0) return;
    int i = std::max(0, currentIndex().row());
    for (int k = 0; k < n; ++k) {
        i = ((i + delta) % n + n) % n;
        const QStandardItem* it = _model->item(i);
        if (it->data(KeyRole).toString().isEmpty() || isRowHidden(i)) continue;
        if (isGreyed(State(it->data(StateRole).toInt())) && !_showUncovered) continue;
        setCurrentIndex(it->index());
        scrollTo(it->index());
        return;
    }
}

} // namespace SkyMap
