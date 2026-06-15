#pragma once

#include <QDialog>
#include <memory>
#include <vector>

class Star;
class Spectrum;
class SpectralFit;
class Instrument;
class DatabaseManager;
class SpectraPanel;
class QTreeWidget;
class QTreeWidgetItem;
class QSplitter;
class FitSetupWidget;
class QTabWidget;
class QPushButton;
class CheckStateDragger;

class SpectraFitDialog : public QDialog
{
    Q_OBJECT
public:
    SpectraFitDialog(std::shared_ptr<Star> star,
                     DatabaseManager* dbm,
                     const QString& projectId,
                     QWidget* parent = nullptr);
    ~SpectraFitDialog() override;

signals:
    void starParametersChanged();
    void spectraUpdated();

private slots:
    void onTreeItemChanged(QTreeWidgetItem* item, int column);
    void onTreeItemClicked(QTreeWidgetItem* item, int column);
    void onPanelSelectionChanged(const QString& spectrumId,
                                 const QString& fitId);

    void onTreeContextMenu(const QPoint& pos);
    void onAddSpectraClicked();
    void onAddFitClicked();
    void onRedetectAllClicked();

private:
    // Instrument/mode (re)detection
    bool autodetectInstrument(const std::shared_ptr<Spectrum>& spec,
                              const std::vector<std::shared_ptr<Instrument>>& instruments);
    void redetectSpectrumById(const QString& spectrumId);
    void defineInstrumentManually(const QString& spectrumId);

    void setupUi();
    void rebuildTree();
    void refreshTreeStyling();
    void updateBestMarkers();
    void setBestFitTied(const QString& fitId, bool markBest);
    void propagateBestFitParams(const std::shared_ptr<SpectralFit>& fit);
    void syncTreeSelectionTo(const QString& spectrumId, const QString& fitId);

    void removeSpectrum(const QString& spectrumId);
    void removeFit(const QString& spectrumId, const QString& fitId);

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm = nullptr;
    QString                _projectId;

    std::vector<std::shared_ptr<Spectrum>> _spectra;

    QSplitter*    _splitter = nullptr;
    SpectraPanel* _panel    = nullptr;
    QTreeWidget*  _tree     = nullptr;
    QTabWidget*    _rightTabs  = nullptr;
    FitSetupWidget* _setup     = nullptr;

    QPushButton*  _addSpectraBtn = nullptr;
    QPushButton*  _addFitBtn     = nullptr;
    QPushButton*  _redetectBtn   = nullptr;
    CheckStateDragger* _flagDragger = nullptr;

    bool _updatingTree    = false;
    bool _syncingFromPanel = false;
};