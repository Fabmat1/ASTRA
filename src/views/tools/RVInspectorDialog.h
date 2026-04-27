#pragma once

#include <QDialog>
#include <QWidget>
#include <QAbstractTableModel>
#include <QHash>
#include <memory>
#include <vector>

class Star;
class Spectrum;
class DatabaseManager;
class ApplicationController;
class RadialVelocityCurve;
class RadialVelocityPoint;
class RVFit;
class RVPanel;
class QTableView;
class QListWidget;
class QPushButton;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;

// ─────────────────────────────────────────────────────────────────────────────
//   Points table
// ─────────────────────────────────────────────────────────────────────────────
class RVPointsTableModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column {
        ColMJD = 0, ColBJD,
        ColRV, ColErrFormal, ColErrSystematic,
        ColInstrument, ColSource, ColFlagged,
        ColCount
    };

    RVPointsTableModel(std::shared_ptr<Star> star,
                       std::shared_ptr<RadialVelocityCurve> curve,
                       DatabaseManager* dbm,
                       QObject* parent = nullptr);

    void reload();

    int rowCount(const QModelIndex& parent = {}) const override;
    int columnCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& idx, int role) const override;
    bool setData(const QModelIndex& idx, const QVariant& value, int role) override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    Qt::ItemFlags flags(const QModelIndex& idx) const override;

    bool canResetToFit(int row) const;
    void resetToFit(int row);

signals:
    void pointEdited(const QModelIndex& row);

private:
    void computeMissingBJDs();
    QString resolveInstrumentName(const std::shared_ptr<RadialVelocityPoint>& p) const;
    std::shared_ptr<class Instrument> resolveInstrumentObject(
        const std::shared_ptr<RadialVelocityPoint>& p) const;
    std::shared_ptr<Spectrum> linkedSpectrum(
        const std::shared_ptr<RadialVelocityPoint>& p) const;

    std::shared_ptr<Star>                              _star;
    std::shared_ptr<RadialVelocityCurve>               _curve;
    DatabaseManager*                                   _dbm = nullptr;
    std::vector<std::shared_ptr<RadialVelocityPoint>>  _points;
    QHash<QString, std::shared_ptr<Spectrum>>          _spectraById;
};

// ─────────────────────────────────────────────────────────────────────────────
//   Solutions sidebar
// ─────────────────────────────────────────────────────────────────────────────
class RVSolutionsWidget : public QWidget
{
    Q_OBJECT
public:
    RVSolutionsWidget(std::shared_ptr<Star> star,
                      DatabaseManager* dbm,
                      QWidget* parent = nullptr);

    void reload();
    std::shared_ptr<RVFit> displayedFit() const { return _displayed; }

signals:
    void displayedFitChanged(std::shared_ptr<RVFit> fit);
    void fitsChanged();   // best-toggle / add / delete

private slots:
    void onSelectionChanged();
    void onAddSolution();
    void onDeleteSolution();
    void onSetAsBest();
    void onApply();
    void onRevert();
    void onEccentricToggled(bool on);
    void onParamChanged();

    private:
    void buildUi();
    void rebuildList();
    void loadIntoEditor(std::shared_ptr<RVFit> fit);
    void writeBackToFit(std::shared_ptr<RVFit> fit);
    void recomputeStats(std::shared_ptr<RVFit> fit);
    void updateStatsLabel(std::shared_ptr<RVFit> fit);
    void takeSnapshot(std::shared_ptr<RVFit> fit);
    std::shared_ptr<RVFit> currentFit() const;

    std::shared_ptr<Star>   _star;
    DatabaseManager*        _dbm = nullptr;

    QListWidget*    _list      = nullptr;
    QPushButton*    _addBtn    = nullptr;
    QPushButton*    _delBtn    = nullptr;
    QPushButton*    _bestBtn   = nullptr;
    QPushButton*    _applyBtn  = nullptr;
    QPushButton*    _revertBtn = nullptr;

    QCheckBox*      _eccCheck    = nullptr;
    QDoubleSpinBox* _periodSpin  = nullptr;
    QDoubleSpinBox* _kSpin       = nullptr;
    QDoubleSpinBox* _gammaSpin   = nullptr;
    QDoubleSpinBox* _phiSpin     = nullptr;     // phase ∈ [0,1]
    QDoubleSpinBox* _eccSpin     = nullptr;
    QDoubleSpinBox* _omegaSpin   = nullptr;
    QLabel*         _statsLabel  = nullptr;

    std::shared_ptr<RVFit> _displayed;
    bool _suppressSignals = false;

    struct Snapshot {
        QString id;
        double P=0, K=0, gamma=0, phi=0, e=0, omega=0;
        bool ecc = false;
    } _snapshot;
};

// ─────────────────────────────────────────────────────────────────────────────
//   Dialog
// ─────────────────────────────────────────────────────────────────────────────
class RVInspectorDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RVInspectorDialog(std::shared_ptr<Star> star,
                               DatabaseManager* dbm = nullptr,
                               ApplicationController* controller = nullptr,
                               const QString& projectId = {},
                               QWidget* parent = nullptr);
    ~RVInspectorDialog() override;

private:
    void setupUi();
    void onTableContextMenu(const QPoint& pos);

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm        = nullptr;
    ApplicationController* _controller = nullptr;
    QString                _projectId;

    RVPanel*            _plotPanel   = nullptr;
    RVSolutionsWidget*  _solutions   = nullptr;
    QTableView*         _pointsTable = nullptr;
    RVPointsTableModel* _pointsModel = nullptr;
};