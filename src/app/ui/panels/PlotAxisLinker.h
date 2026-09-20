// src/app/ui/panels/PlotAxisLinker.h
#pragma once

#include "app/ui/panels/DetailPanel.h"

#include <QMargins>
#include <QObject>
#include <QPair>
#include <QPointer>
#include <QVector>

class QCustomPlot;
class QTimer;

/// Locks two stacked DetailPanels onto one visually shared x axis.
///
/// The Star Detail grid can put the RV panel directly above (or below) the
/// light-curve panel, which invites reading the two plots as one figure - but
/// the panels sit in independent splitter cells and size their own y-axis
/// labels, so their plotting areas start and end at different screen columns
/// and a feature at phase 0.4 in one plot is not above phase 0.4 in the other.
///
/// While linked, this
///   * pins the outer edges of both panels' plotting areas to the same two
///     screen columns, by overriding the left/right axis-rect margins (never
///     below what each plot needs for its own labels, so nothing is clipped),
///     and
///   * mirrors the x range between them whenever both axes show the same
///     quantity, correcting for the different zero points the two panels use.
///
/// Both are re-established after either panel rebuilds its plots, resizes, or
/// changes its axis ranges. Unlinking hands the margins back to QCustomPlot's
/// own automatic calculation.
class PlotAxisLinker : public QObject
{
    Q_OBJECT
public:
    /// `reference` is the common ancestor the plots' positions are compared in
    /// (the grid host of the star detail window).
    PlotAxisLinker(DetailPanel* first, DetailPanel* second,
                   QWidget* reference, QObject* parent = nullptr);
    ~PlotAxisLinker() override;

    bool isLinked() const { return _linked; }

    /// True when both panels currently show something linkable. The host uses
    /// this to hide the chain button while, say, the RV panel has no data.
    bool isAvailable() const;

    /// Horizontal span, in `reference` coordinates, that the two panels have in
    /// common - the region the chain button should be centred over. Null when
    /// the panels do not overlap horizontally at all.
    QPair<int, int> overlapInReference() const;

public slots:
    void setLinked(bool on);

    /// Re-run the alignment on the next event-loop turn. Cheap when unlinked.
    void scheduleRealign();

signals:
    /// Either panel gained or lost linkable plots.
    void availabilityChanged();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// Re-read both panels' link descriptors and re-attach everything that
    /// holds QCustomPlot pointers.
    void rewire();
    void realign();
    /// Give every plot we touched its automatic margins back.
    void releaseMargins();
    /// Push `src`'s x range onto every other plot in the sync group.
    void syncFrom(QCustomPlot* src);

    QPointer<DetailPanel> _first;
    QPointer<DetailPanel> _second;
    QPointer<QWidget>     _reference;

    bool _linked  = false;
    bool _syncing = false;

    QTimer* _realignTimer = nullptr;

    struct SyncEntry {
        QPointer<QCustomPlot> plot;
        double                zero = 0.0;
    };
    /// Plots whose x ranges mirror each other; empty when the two axes show
    /// different quantities (e.g. one folded, one on a timeline).
    QVector<SyncEntry> _syncGroup;

    /// Plots whose margins we overrode, so they can be handed back.
    QVector<QPointer<QCustomPlot>> _overridden;

    /// Connections and event filters owned by the current wiring.
    QVector<QMetaObject::Connection> _plotConnections;
    QVector<QPointer<QCustomPlot>>   _watched;
};
