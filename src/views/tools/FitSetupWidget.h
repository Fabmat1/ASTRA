#pragma once

#include <QWidget>
#include <QHash>
#include <QVector>
#include <QStringList>
#include <memory>
#include <vector>

#include "fitting/FitTypes.h"

class Star;
class Spectrum;
class DatabaseManager;
class SpectraPanel;
class GridSelectorWidget;

class QVBoxLayout;
class QListWidget;
class QListWidgetItem;
class QDoubleSpinBox;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QPushButton;
class QLineEdit;
class QGroupBox;
class QScrollArea;

namespace astra::fitting { class FitWorker; }

class FitSetupWidget : public QWidget
{
    Q_OBJECT
public:
    struct Context {
        std::shared_ptr<Star>  star;
        DatabaseManager*       dbm = nullptr;
        QString                projectId;
        SpectraPanel*          panel = nullptr;
    };

    explicit FitSetupWidget(const Context& ctx, QWidget* parent = nullptr);
    ~FitSetupWidget() override;

    void refreshSpectraList();

signals:
    void fitCompleted();

private slots:
    void onSpectrumListRowChanged(int row);
    void onRunFit();
    void onCopyToAll();

private:
    struct PerSpec {
        bool   enabled = true;
        double wlMin = 3600.0;
        double wlMax = 5250.0;
        QVector<astra::fitting::IgnoreRegion>    ignore;
        QVector<astra::fitting::ContinuumAnchor> anchors;
        double resOffset = 0.0;
        double resSlope  = 0.37037;
        bool   inferFromFits = false;
    };

    void setupUi();
    QGroupBox* buildComponentsSection();
    QGroupBox* buildSpectraListSection();
    QGroupBox* buildPerSpectrumSection();
    QGroupBox* buildGlobalSection();

    void rebuildComponentRows();
    void rebuildIgnoreRows();
    void rebuildAnchorRows();

    // Per-spectrum state lifecycle
    void commitEditorToState();       // numeric fields → _configs[_currentId]
    void loadStateToEditor();         // _configs[_currentId] → numeric fields
    void inferFromBestFit(PerSpec& cfg, const std::shared_ptr<Spectrum>& s) const;

    astra::fitting::SpectralFitJob buildJob(QStringList& tempFilesOut) const;
    QString exportSpectrumToTemp(const std::shared_ptr<Spectrum>& s,
                                  const QString& dir) const;

    void persistResult(const astra::fitting::SpectralFitResult& result,
                        const astra::fitting::SpectralFitJob&  job);

    // ── State ──────────────────────────────────────────────────
    Context _ctx;
    std::vector<std::shared_ptr<Spectrum>> _sortedSpectra;
    QHash<QString, PerSpec>                _configs;
    QString                                 _currentId;

    QVector<astra::fitting::StellarComponent> _components;

    QVector<GridSelectorWidget*> _componentSelectors;

    // ── UI ─────────────────────────────────────────────────────
    QListWidget*    _spectraList        = nullptr;

    // Components section
    QVBoxLayout*    _componentsLayout   = nullptr;
    QPushButton*    _addComponentBtn    = nullptr;

    // Per-spectrum editor
    QWidget*        _perSpectrumHost    = nullptr;
    QDoubleSpinBox* _wlMinSpin          = nullptr;
    QDoubleSpinBox* _wlMaxSpin          = nullptr;
    QCheckBox*      _inferCheck         = nullptr;
    QVBoxLayout*    _ignoreListLayout   = nullptr;
    QPushButton*    _addIgnoreBtn       = nullptr;
    QVBoxLayout*    _anchorListLayout   = nullptr;
    QPushButton*    _addAnchorBtn       = nullptr;
    QDoubleSpinBox* _resOffsetSpin      = nullptr;
    QDoubleSpinBox* _resSlopeSpin       = nullptr;
    QPushButton*    _copyToAllBtn       = nullptr;

    // Global options
    QComboBox*      _backendCombo       = nullptr;
    QLineEdit*      _untiedEdit         = nullptr;
    QDoubleSpinBox* _filterSnrSpin      = nullptr;
    QDoubleSpinBox* _requireBlueSpin    = nullptr;
    QSpinBox*       _nitNoiseMaxSpin    = nullptr;
    QDoubleSpinBox* _outlierLoSpin      = nullptr;
    QDoubleSpinBox* _outlierHiSpin      = nullptr;
    QCheckBox*      _verboseCheck       = nullptr;

    // Run
    QPushButton*    _runButton          = nullptr;

    astra::fitting::FitWorker* _worker = nullptr;
};