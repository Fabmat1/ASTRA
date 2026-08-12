#pragma once

#include "DetailPanel.h"
#include <QMetaObject>
#include <vector>
#include "views/widgets/FitPreviewOverlay.h"

class Spectrum;
class SpectralFit;
class QTabBar;
class QComboBox;
class QCheckBox;
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
    QComboBox*   _displayMode  = nullptr;
    QCheckBox*   _componentsCheck = nullptr;  // per-component model curves
    QCheckBox*   _telluricCheck   = nullptr;  // fitted telluric transmission
    QCheckBox*   _solarRelCheck   = nullptr;  // abundance view: [X/H]
    QLabel*      _modelLabel   = nullptr;
    QCustomPlot* _mainPlot     = nullptr;
    QCustomPlot* _residualPlot = nullptr;
    QCustomPlot* _abundancePlot = nullptr;
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

    bool   _axisSyncInProgress = false;
    bool   _hasCustomZoom      = false;
    bool   _syncFromMain       = false;
    double _pendingSyncRangeMin = 0.0;
    double _pendingSyncRangeMax = 1.0;
};