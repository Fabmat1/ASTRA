// src/app/ui/panels/PlotAxisLinker.cpp
#include "app/ui/panels/PlotAxisLinker.h"

#include "plotting/qcustomplot.h"

#include <QEvent>
#include <QPair>
#include <QPoint>
#include <QTimer>

#include <algorithm>
#include <limits>

namespace {

/// Smallest plotting area the link is allowed to squeeze the panels down to.
/// Below this the two panels barely overlap horizontally and aligning them
/// would leave a sliver of a plot, so the link stands down instead.
constexpr int kMinAlignedWidth = 60;

/// The margins QCustomPlot would compute for `plot` right now, i.e. exactly
/// what its tick labels and axis labels need. Asking for them is the only way
/// to know how far a forced margin may be pushed without clipping, and the
/// answer moves as the axis ranges (and so the tick label widths) change.
///
/// The two layout phases are the same ones replot() runs, minus the painting.
QMargins naturalMargins(QCustomPlot* plot)
{
    QCPAxisRect* rect = plot->axisRect();

    const QCP::MarginSides sides = rect->autoMargins();
    const QMargins         held  = rect->margins();

    rect->setAutoMargins(QCP::msAll);
    plot->plotLayout()->update(QCPLayoutElement::upPreparation);
    plot->plotLayout()->update(QCPLayoutElement::upMargins);
    const QMargins natural = rect->margins();

    if (sides != QCP::msAll) {
        rect->setAutoMargins(sides);
        rect->setMargins(held);
    }
    return natural;
}

} // namespace


PlotAxisLinker::PlotAxisLinker(DetailPanel* first, DetailPanel* second,
                               QWidget* reference, QObject* parent)
    : QObject(parent)
    , _first(first)
    , _second(second)
    , _reference(reference)
{
    _realignTimer = new QTimer(this);
    _realignTimer->setSingleShot(true);
    _realignTimer->setInterval(0);
    connect(_realignTimer, &QTimer::timeout, this, &PlotAxisLinker::realign);

    for (DetailPanel* panel : { first, second }) {
        if (!panel) continue;
        connect(panel, &DetailPanel::plotsRebuilt, this, [this] {
            rewire();
            emit availabilityChanged();
        });
        connect(panel, &QObject::destroyed, this, [this] {
            _syncGroup.clear();
            _overridden.clear();
            _linked = false;
            emit availabilityChanged();
        });
        panel->installEventFilter(this);
    }

    rewire();
}

PlotAxisLinker::~PlotAxisLinker()
{
    releaseMargins();
}

bool PlotAxisLinker::isAvailable() const
{
    if (!_first || !_second) return false;
    auto usable = [](const DetailPanel::XAxisLink& link) {
        return link.kind != DetailPanel::XAxisLink::Kind::None
            && !link.plots.isEmpty();
    };
    return usable(_first->xAxisLink()) && usable(_second->xAxisLink());
}

QPair<int, int> PlotAxisLinker::overlapInReference() const
{
    if (!_first || !_second || !_reference) return { 0, -1 };

    auto span = [this](DetailPanel* panel) -> QPair<int, int> {
        const auto link = panel->xAxisLink();
        if (link.plots.isEmpty()) return { 0, -1 };
        int lo = std::numeric_limits<int>::max();
        int hi = std::numeric_limits<int>::lowest();
        for (const auto& entry : link.plots) {
            if (!entry.plot) continue;
            const int x = entry.plot->mapTo(_reference, QPoint(0, 0)).x();
            lo = std::min(lo, x);
            hi = std::max(hi, x + entry.plot->width());
        }
        return (hi > lo) ? QPair<int, int>{ lo, hi } : QPair<int, int>{ 0, -1 };
    };

    const auto a = span(_first);
    const auto b = span(_second);
    if (a.second <= a.first || b.second <= b.first) return { 0, -1 };

    const int lo = std::max(a.first, b.first);
    const int hi = std::min(a.second, b.second);
    return (hi > lo) ? QPair<int, int>{ lo, hi } : QPair<int, int>{ 0, -1 };
}

void PlotAxisLinker::setLinked(bool on)
{
    if (_linked == on) return;
    _linked = on;

    if (!_linked) {
        releaseMargins();
        return;
    }
    rewire();
    realign();

    // Start from the first panel's view, so linking is a single visible jump
    // rather than whichever plot happens to move next winning.
    if (!_syncGroup.isEmpty() && _syncGroup.first().plot)
        syncFrom(_syncGroup.first().plot);
}

void PlotAxisLinker::scheduleRealign()
{
    if (_linked && _realignTimer) _realignTimer->start();
}

void PlotAxisLinker::rewire()
{
    for (const auto& c : _plotConnections) disconnect(c);
    _plotConnections.clear();

    for (const auto& plot : _watched)
        if (plot) plot->removeEventFilter(this);
    _watched.clear();

    _syncGroup.clear();

    if (!_first || !_second) return;

    if (!isAvailable()) { releaseMargins(); return; }

    const auto a = _first->xAxisLink();
    const auto b = _second->xAxisLink();

    // Mirroring the range only makes sense when both axes carry the same
    // quantity, and when neither panel chops its x axis into side-by-side
    // segments (the RV broken-axis view) - there is no single range to share.
    const bool syncable = a.kind == b.kind && !a.segmented && !b.segmented;

    for (const auto& link : { a, b }) {
        for (const auto& entry : link.plots) {
            if (!entry.plot) continue;

            if (syncable) _syncGroup.append({ entry.plot, entry.zero });

            // A range change moves the tick labels, and with them the margin
            // each plot needs, so the alignment has to be recomputed.
            QCustomPlot* plot = entry.plot;
            _plotConnections.append(connect(
                plot->xAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
                this, [this, plot] {
                    if (_linked && !_syncing) syncFrom(plot);
                    scheduleRealign();
                }));
            _plotConnections.append(connect(
                plot->yAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
                this, [this] { scheduleRealign(); }));

            plot->installEventFilter(this);
            _watched.append(plot);
        }
    }

    if (_linked) scheduleRealign();
}

void PlotAxisLinker::realign()
{
    if (!_linked || !_reference || !_first || !_second) return;

    if (!isAvailable()) { releaseMargins(); return; }

    const auto a = _first->xAxisLink();
    const auto b = _second->xAxisLink();

    // Which plots own an outer edge of their panel's x axis. A segmented panel
    // only pins its first and last segment; the breaks in between keep their
    // own margins so the segment proportions are left alone.
    struct Edge { QCustomPlot* plot; bool left; bool right; int originX; };
    QVector<Edge> edges;

    auto collect = [&](const DetailPanel::XAxisLink& link) {
        for (int i = 0; i < link.plots.size(); ++i) {
            QCustomPlot* plot = link.plots[i].plot;
            if (!plot) continue;
            const bool first = (i == 0);
            const bool last  = (i == link.plots.size() - 1);
            edges.append({ plot,
                           link.segmented ? first : true,
                           link.segmented ? last  : true,
                           plot->mapTo(_reference, QPoint(0, 0)).x() });
        }
    };
    collect(a);
    collect(b);
    if (edges.isEmpty()) { releaseMargins(); return; }

    // The innermost edge each side allows: pushing past it would clip a plot's
    // own tick or axis labels.
    int targetLeft  = std::numeric_limits<int>::lowest();
    int targetRight = std::numeric_limits<int>::max();
    QVector<QMargins> natural;
    natural.reserve(edges.size());

    for (const Edge& e : edges) {
        const QMargins nat = naturalMargins(e.plot);
        natural.append(nat);
        if (e.left)
            targetLeft = std::max(targetLeft, e.originX + nat.left());
        if (e.right)
            targetRight = std::min(targetRight,
                                   e.originX + e.plot->width() - nat.right());
    }

    if (targetRight - targetLeft < kMinAlignedWidth) { releaseMargins(); return; }

    QVector<QPointer<QCustomPlot>> stillOverridden;
    for (int i = 0; i < edges.size(); ++i) {
        const Edge&     e   = edges[i];
        QCPAxisRect*    ar  = e.plot->axisRect();
        const QMargins& nat = natural[i];

        QCP::MarginSides autoSides = QCP::msTop | QCP::msBottom;
        QMargins         wanted    = ar->margins();
        wanted.setTop(nat.top());
        wanted.setBottom(nat.bottom());

        if (e.left) wanted.setLeft(targetLeft - e.originX);
        else      { wanted.setLeft(nat.left());  autoSides |= QCP::msLeft; }

        if (e.right) wanted.setRight(e.originX + e.plot->width() - targetRight);
        else       { wanted.setRight(nat.right()); autoSides |= QCP::msRight; }

        ar->setAutoMargins(autoSides);
        ar->setMargins(wanted);
        e.plot->replot(QCustomPlot::rpQueuedReplot);
        stillOverridden.append(e.plot);
    }

    // Anything that dropped out of the wiring since the last pass (a plot that
    // was hidden, say) gets its automatic margins back.
    for (const auto& plot : _overridden) {
        if (!plot) continue;
        const bool kept = std::any_of(
            stillOverridden.cbegin(), stillOverridden.cend(),
            [&](const QPointer<QCustomPlot>& p) { return p == plot; });
        if (kept) continue;
        plot->axisRect()->setAutoMargins(QCP::msAll);
        plot->replot(QCustomPlot::rpQueuedReplot);
    }
    _overridden = stillOverridden;
}

void PlotAxisLinker::releaseMargins()
{
    for (const auto& plot : _overridden) {
        if (!plot) continue;
        plot->axisRect()->setAutoMargins(QCP::msAll);
        plot->replot(QCustomPlot::rpQueuedReplot);
    }
    _overridden.clear();
}

void PlotAxisLinker::syncFrom(QCustomPlot* src)
{
    if (!_linked || _syncing || !src || _syncGroup.size() < 2) return;

    double srcZero = 0.0;
    bool   found   = false;
    for (const auto& e : _syncGroup)
        if (e.plot == src) { srcZero = e.zero; found = true; break; }
    if (!found) return;

    const QCPRange r = src->xAxis->range();

    _syncing = true;
    for (const auto& e : _syncGroup) {
        if (!e.plot || e.plot == src) continue;
        // Both ends expressed in the shared quantity, then back into the
        // target plot's own offset.
        const double shift = srcZero - e.zero;
        const QCPRange target(r.lower + shift, r.upper + shift);
        if (qFuzzyCompare(target.lower, e.plot->xAxis->range().lower)
            && qFuzzyCompare(target.upper, e.plot->xAxis->range().upper))
            continue;
        e.plot->xAxis->setRange(target);
        e.plot->replot(QCustomPlot::rpQueuedReplot);
    }
    _syncing = false;
}

bool PlotAxisLinker::eventFilter(QObject* watched, QEvent* event)
{
    switch (event->type()) {
    case QEvent::Resize:
    case QEvent::Move:
    case QEvent::Show:
        scheduleRealign();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}
