#pragma once

#include "DetailPanel.h"
#include <QMetaObject>
#include <vector>

class Spectrum;
class QTabBar;
class QComboBox;
class QCustomPlot;
class QLabel;
class QPushButton;
class QTimer;

class SpectraPanel : public DetailPanel
{
    Q_OBJECT
public:
    explicit SpectraPanel(const Context& ctx, QWidget* parent = nullptr);

    void refresh() override;
    void refreshTheme() override;

private:
    enum DisplayMode {
        DisplayNormalized = 0,
        DisplayRebinned   = 1,
        DisplayRaw        = 2,
    };

    void setupUi();
    void populate();
    void displaySpectrum(int index);
    void updateSpectrumDisplay();

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
    QLabel*      _modelLabel   = nullptr;
    QCustomPlot* _mainPlot     = nullptr;
    QCustomPlot* _residualPlot = nullptr;
    QLabel*      _infoLabel    = nullptr;
    QPushButton* _resetZoomButton = nullptr;
    QTimer*      _axisSyncTimer   = nullptr;

    int  _currentSpectrumIndex = -1;
    std::vector<std::shared_ptr<Spectrum>> _sortedSpectra;
    QMetaObject::Connection _tabConnection;
    QMetaObject::Connection _axisSyncConn1, _axisSyncConn2;

    bool   _axisSyncInProgress = false;
    bool   _hasCustomZoom      = false;
    bool   _syncFromMain       = false;
    double _pendingSyncRangeMin = 0.0;
    double _pendingSyncRangeMax = 1.0;
};