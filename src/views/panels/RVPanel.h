#pragma once

#include "DetailPanel.h"
#include "models/RadialVelocity.h"   // for RadialVelocityCurve::ListenerToken

#include <QPointer>
#include <QString>
#include <QVector>

#include <vector>

class QPushButton;
class QVBoxLayout;
class QCustomPlot;
class QCPGraph;
class QCheckBox;

class RVPanel : public DetailPanel
{
    Q_OBJECT
public:
    explicit RVPanel(const Context& ctx, QWidget* parent = nullptr);
    ~RVPanel() override;

    void refresh() override;
    void refreshTheme() override;

    void setDisplayedFit(std::shared_ptr<RVFit> fit);

protected:
    bool eventFilter(QObject* obj, QEvent* event) override;

public slots:
    // Highlight the RV point that corresponds to the spectrum currently shown
    // in the Spectra Panel. Pass an empty id to clear the highlight. The fit id
    // is accepted for signal compatibility but unused (matching is per-spectrum).
    void highlightSpectrum(const QString& spectrumId, const QString& fitId = {});

    // Emphasise a specific RV point in the plot - used by the RV Inspector to
    // mark the row the user has selected in the points table. Matches by source
    // spectrum id when present, otherwise by epoch (sortValue). Pass
    // hasEpoch=false with an empty id to clear the highlight.
    void highlightRVPoint(const QString& spectrumId, double epoch, bool hasEpoch);

private slots:
    void onToggleFolded();
    void resetZoom();

private:
    void setupUi();
    void populate();

    // Show/hide & reposition the floating "reset zoom" button depending on
    // whether any plot is currently away from its computed home range.
    void updateResetZoomButton();

    // Update only the highlight markers in-place (no widget rebuild) so that
    // scrolling through spectra does not flash the whole plot.
    void applyHighlight();

    // One per plot/segment: a dedicated, persistent highlight graph plus the
    // coordinates and identity of every plotted point, so applyHighlight() can
    // recompute which marker to show without repopulating.
    struct HighlightTarget {
        QPointer<QCustomPlot> plot;
        QCPGraph*             graph = nullptr;
        std::vector<double>   xs;       // plotted x (phase or days-from-first)
        std::vector<double>   ys;       // plotted y (RV)
        std::vector<QString>  specIds;  // source spectrum id ("" if none)
        std::vector<double>   epochs;   // epoch for the time-based fallback
        double                xMin = 0.0;
        double                xMax = 0.0;
        // "Home" (auto-computed) axis ranges, so the reset-zoom button can
        // restore them and we can tell when the user has zoomed/panned away.
        double homeXLo = 0.0, homeXHi = 0.0, homeYLo = 0.0, homeYHi = 0.0;
    };
    std::vector<HighlightTarget> _highlightTargets;

    QPushButton* _toggleButton  = nullptr;
    QPushButton* _resetZoomBtn   = nullptr;   // floating overlay on the plot
    QWidget*     _content       = nullptr;
    QVBoxLayout* _contentLayout = nullptr;
    bool         _folded        = false;

    // When set, the next populate() restores the previous per-plot axis ranges
    // instead of the freshly computed home ranges - so scrolling through RV
    // solutions keeps the user's zoom level.
    bool         _preserveZoomOnNextPopulate = false;

    std::shared_ptr<RVFit> _displayedFit;

    QCheckBox* _showFlaggedCheck = nullptr;
    bool       _showFlagged      = false;

    // Spectrum currently shown in the Spectra Panel; its matching RV point is
    // emphasised in the plot. Empty → no highlight.
    QString    _highlightSpectrumId;

    // Optional epoch-based highlight (for points with no source spectrum, e.g.
    // manual/imported points). When set, applyHighlight() also matches points
    // whose epoch is within a small tolerance of _highlightEpoch.
    double     _highlightEpoch    = 0.0;
    bool       _highlightHasEpoch = false;

    bool _foldDefaultApplied = false;

    RadialVelocityCurve::ListenerToken _rvChangeToken =
        RadialVelocityCurve::kInvalidToken;
};