#pragma once

#include "app/ui/panels/DetailPanel.h"
#include <QHash>
#include <QMetaObject>
#include <vector>
#include "fitting/ui/FitPreviewOverlay.h"
#include "core/AsymmetricErrors.h"

class Spectrum;
class SpectralFit;
class QTabBar;
class QComboBox;
class QCheckBox;
class QButtonGroup;
class QScrollArea;
class QCPAxisRect;
class QCPTextElement;
class QCPAxis;
class QCPGraph;
class QCustomPlot;
class QLabel;
class QPushButton;
class QTimer;
class PlotKeyNavigator;

class SpectraPanel : public DetailPanel
{
    Q_OBJECT
public:
    explicit SpectraPanel(const Context& ctx, QWidget* parent = nullptr,
                          bool deferPopulate = false);

    void refresh() override;
    void refreshTheme() override;
    void setFitPreview(const FitPreviewConfig& cfg);
    void clearFitPreview();

    // ── Co-add overlay ──────────────────────────────────────────────────────
    // While a co-add is set the panel shows the stacked spectrum instead of the
    // per-epoch view; the spectrum tabs and fit selector are hidden, since
    // neither applies to it.
    struct CoaddDisplay {
        std::vector<double> wavelengths;
        std::vector<double> fluxes;
        std::vector<double> sigmas;
        std::vector<int>    counts;    ///< contributing spectra per pixel
        QString             caption;   ///< rich text for the info strip
    };
    void showCoadd(const CoaddDisplay& coadd);
    void clearCoadd();
    bool showingCoadd() const { return _coaddActive; }

    /// Normalized: the fit's rebinned flux and model divided by its fitted
    /// continuum. Rebinned ("Flux"): the same without the division. Raw: the
    /// spectrum as observed, the model scaled onto it.
    enum DisplayMode {
        DisplayNormalized = 0,
        DisplayRebinned   = 1,
        DisplayRaw        = 2,
    };

    // Navigation / control
    void selectSpectrumById(const QString& spectrumId);
    void selectFitById(const QString& fitId);          // navigates + Normalized
    void setDisplayMode(DisplayMode mode);
    void clearFitSelection();                           // fit combo → "None"
    void refreshCurrentView();                          // rebuild combo labels
    void refreshFitPreviewData();
    void resetCustomZoom() { _hasCustomZoom = false; }

    void onSummaryChanged() override { /* curves unchanged by summary metrics */ }

protected:
    void changeEvent(QEvent* ev) override;
    bool eventFilter(QObject* watched, QEvent* ev) override;

public:
    
    // State
    QString currentSpectrumId() const;
    QString currentFitId() const;
    
signals:
    void selectionChanged(const QString& spectrumId, const QString& fitId);
    void fitPreviewEdited(const FitPreviewConfig& cfg);

private:

    void setupUi();
    void populate() override;
    /// Drop the user's zoom and redraw at the auto-fitted ranges.
    void resetZoomView();
    /// Remember that the user zoomed/panned and reveal the reset button.
    void markCustomZoom();
    void displaySpectrum(int index);
    void updateSpectrumDisplay();
    void updateCoaddDisplay();

    // ── Abundance view ──────────────────────────────────────────────────────
    // The tab bar carries one extra tab after the spectrum tabs; selecting it
    // swaps the spectrum/residual plots for the abundance plot while leaving
    // _currentSpectrumIndex (and therefore the fit selector) untouched.
    void onTabChanged(int index);
    void setAbundanceViewActive(bool on);
    void updateAbundanceDisplay();
    /// Undo the parts of PanelUtils::stylePlot() that assume a numeric x axis.
    void styleAbundanceAxes();
    bool starHasAbundances() const;
    /// Fit currently picked in the combo, or null for "None"/no spectrum.
    std::shared_ptr<SpectralFit> currentFit() const;
    /// "Component 1 (28 500 K)", falling back to "Component 1".
    static QString componentLabel(const SpectralFit& fit, int which);
    /// Show/hide the model-related toolbar widgets for the active view.
    void updateToolbarVisibility();

    // ── Display helpers ─────────────────────────────────────────────────────
    /// The spectrum as drawn in the current mode, before any normalization:
    /// data, error, fit mask and every model curve on one wavelength grid.
    struct Series;
    struct SliceCurves;
    /// One panel of the Panels view and its home window.
    struct PanelInfo {
        QString         key;
        QCPAxisRect*    rect  = nullptr;
        QCPTextElement* title = nullptr;
        double wlLow = 0.0, wlHigh = 0.0;
        double homeX0 = 0.0, homeX1 = 1.0, homeY0 = 0.0, homeY1 = 1.0;
        bool   custom = false;          ///< the user zoomed or panned it
    };
    Series buildSeries(const std::shared_ptr<Spectrum>& spec,
                       const SpectralFit* fit, int mode);
    /// Draw pixels [i0, i1) of `s` onto one pair of axes, each value divided
    /// by `div` (empty: as is).
    SliceCurves drawSlice(QCustomPlot* plot, QCPAxis* xa, QCPAxis* ya,
                          const Series& s, int i0, int i1,
                          const std::vector<double>& div, bool showCont,
                          const SpectralFit* fit);
    void renderFull(const Series& s, const SpectralFit* fit, bool normalized);
    void renderPanels(const Series& s, const SpectralFit* fit, bool normalized,
                      const std::shared_ptr<Spectrum>& spec);
    /// Switch to the full view zoomed to [lo, hi].
    void openInFull(double lo, double hi);
    void resetPanel(int index);
    void showPanelMenu(const QPoint& pos);
    int  panelAt(const QPoint& pos, bool* onTitle = nullptr) const;
    /// Whether the Panels view is what is on screen right now: the user's
    /// choice, unless a co-add, the abundances or the fit-setup overlay
    /// (which edits the full plot) needs the full view.
    bool panelsShown() const;
    void applyPlotVisibility();
    int  currentMode() const;
    void setModeButton(int mode);
    void updateToggleSwatches();
    void saveViewSettings() const;

    QString formatTabLabel(const std::shared_ptr<Spectrum>& s, int i) const;
    QString formatInfo(const std::shared_ptr<Spectrum>& s) const;
    std::vector<double> interpolateModel(
        const std::vector<double>& mw, const std::vector<double>& mf,
        const std::vector<double>& tw);
    double computeRenormFactor(const std::vector<double>& d,
                                const std::vector<double>& m);

    QTabBar*     _tabBar       = nullptr;
    QWidget*     _toolbar      = nullptr;
    QComboBox*   _fitCombo     = nullptr;
    // ── View controls ──
    QWidget*      _viewBar       = nullptr;
    QButtonGroup* _modeGroup     = nullptr;   // Normalized / Flux / Raw
    QButtonGroup* _layoutGroup   = nullptr;   // Panels / Full
    QCheckBox*   _modelCheck      = nullptr;  // combined model
    QCheckBox*   _contCheck       = nullptr;  // fitted continuum
    QCheckBox*   _componentsCheck = nullptr;  // per-component model curves
    QCheckBox*   _telluricCheck   = nullptr;  // fitted telluric transmission
    QCheckBox*   _errCheck        = nullptr;  // error band
    QCheckBox*   _residCheck      = nullptr;  // residual strip (full view)
    QCheckBox*   _solarRelCheck   = nullptr;  // abundance view: [X/H]
    QLabel*      _modelLabel   = nullptr;
    QCustomPlot* _mainPlot     = nullptr;
    QCustomPlot* _residualPlot = nullptr;
    QCustomPlot* _abundancePlot = nullptr;
    QScrollArea* _panelScroll  = nullptr;   ///< hosts _panelPlot
    QCustomPlot* _panelPlot    = nullptr;   ///< the Panels view, one axis rect per panel
    QLabel*      _infoLabel    = nullptr;
    QPushButton* _resetZoomButton = nullptr;
    QTimer*      _axisSyncTimer   = nullptr;
    FitPreviewOverlay* _fitOverlay = nullptr;
    PlotKeyNavigator*  _keyNav     = nullptr;

    int  _currentSpectrumIndex = -1;
    std::vector<std::shared_ptr<Spectrum>> _sortedSpectra;

    int  _abundanceTabIndex  = -1;     ///< tab index of "Abundances", -1 = none
    bool _showingAbundances  = false;
    bool _toolbarHasFits     = false;  ///< last hasFits state of the fit combo

    bool         _coaddActive = false;
    CoaddDisplay _coadd;
    QMetaObject::Connection _tabConnection;
    QMetaObject::Connection _axisSyncConn1, _axisSyncConn2;

    // Panels view state. A panel the user zoomed keeps its window across
    // redraws of the same spectrum (toggles, theme, resize).
    QVector<PanelInfo> _panelInfos;
    struct PanelZoom { double x0, x1, y0, y1; };
    QHash<QString, PanelZoom> _panelZoom;
    int  _panelCols      = 0;
    int  _preferredMode  = DisplayNormalized;  ///< the user's last pick
    bool _modeForced     = false;              ///< setDisplayMode() override

    bool   _axisSyncInProgress = false;
    bool   _hasCustomZoom      = false;
    bool   _syncFromMain       = false;
    double _pendingSyncRangeMin = 0.0;
    double _pendingSyncRangeMax = 1.0;
};