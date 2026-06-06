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

public slots:
    // Highlight the RV point that corresponds to the spectrum currently shown
    // in the Spectra Panel. Pass an empty id to clear the highlight. The fit id
    // is accepted for signal compatibility but unused (matching is per-spectrum).
    void highlightSpectrum(const QString& spectrumId, const QString& fitId = {});

private slots:
    void onToggleFolded();

private:
    void setupUi();
    void populate();

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
    };
    std::vector<HighlightTarget> _highlightTargets;

    QPushButton* _toggleButton  = nullptr;
    QWidget*     _content       = nullptr;
    QVBoxLayout* _contentLayout = nullptr;
    bool         _folded        = false;

    std::shared_ptr<RVFit> _displayedFit;

    QCheckBox* _showFlaggedCheck = nullptr;
    bool       _showFlagged      = false;

    // Spectrum currently shown in the Spectra Panel; its matching RV point is
    // emphasised in the plot. Empty → no highlight.
    QString    _highlightSpectrumId;

    bool _foldDefaultApplied = false; 

    RadialVelocityCurve::ListenerToken _rvChangeToken =
        RadialVelocityCurve::kInvalidToken;
};