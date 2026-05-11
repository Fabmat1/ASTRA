#pragma once

#include <QDialog>
#include <QList>
#include <memory>
#include "rv_mcmc/api.h"

class RVFit;
class QListWidget;
class QPushButton;
class QDoubleSpinBox;
class QGridLayout;
class QCustomPlot;
class QCPColorMap;
class QCPGraph;
class QLabel;

class RVMCMCResultsDialog : public QDialog
{
    Q_OBJECT
public:
    RVMCMCResultsDialog(rv_mcmc::FitResult result,
                        QString curveId,
                        QWidget* parent = nullptr);

    QList<std::shared_ptr<RVFit>> selectedFits() const { return _selected; }

private slots:
    void onAddSelectedPeaks();
    void onAddCustomRegion();
    void onPeakActivated(int row);
    void onCornerRangeChanged();   // when user zooms a corner panel
    void onResetView();

private:
    void buildUi();
    void renderCornerGrid();
    void rebuildHistogramsForActiveFilter();
    std::vector<bool> currentFilterMask() const;

    std::shared_ptr<RVFit> fitFromSubChain(
        const std::vector<std::vector<double>>& sub,
        const QString& methodTag) const;

    rv_mcmc::FitResult _result;
    QString            _curveId;
    QList<std::shared_ptr<RVFit>> _selected;

    // ── UI ──
    QListWidget*    _peakList    = nullptr;
    QPushButton*    _addPeaksBtn = nullptr;
    QPushButton*    _addRegionBtn= nullptr;
    QDoubleSpinBox* _pMinSpin    = nullptr;
    QDoubleSpinBox* _pMaxSpin    = nullptr;
    QLabel*         _filterInfo  = nullptr;

    // Corner panel grid: diag[k] is QCPGraph; off[i][j] (i>j) is colormap.
    QGridLayout*                          _cornerGrid = nullptr;
    std::vector<QCustomPlot*>             _diagPlots;
    std::vector<std::vector<QCustomPlot*>> _offPlots;
    std::vector<QCPColorMap*>             _colormaps; // flat list, parallels offPlots
    bool _suppressRangeSync = false;

        struct CustomRegion {
        double pmin = 0.0;
        double pmax = 0.0;
        int    nSamples = 0;
    };
    void addPeakListItem(int solutionIndex);
    void addCustomListItem(int customIndex);

    QList<CustomRegion> _customRegions;
    QPushButton*        _resetViewBtn = nullptr;
};