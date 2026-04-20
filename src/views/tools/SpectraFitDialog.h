#pragma once

#include <QDialog>
#include <memory>
#include <vector>

class Star;
class Spectrum;
class SpectralFit;
class DatabaseManager;
class SpectraPanel;
class QTreeWidget;
class QTreeWidgetItem;
class QSplitter;

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

private slots:
    void onTreeItemChanged(QTreeWidgetItem* item, int column);
    void onTreeItemClicked(QTreeWidgetItem* item, int column);
    void onPanelSelectionChanged(const QString& spectrumId,
                                 const QString& fitId);

private:
    void setupUi();
    void rebuildTree();
    void refreshTreeStyling();
    void updateBestMarkers();
    void setBestFitTied(const QString& fitId, bool markBest);
    void propagateBestFitParams(const std::shared_ptr<SpectralFit>& fit);
    void syncTreeSelectionTo(const QString& spectrumId, const QString& fitId);

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm = nullptr;
    QString                _projectId;

    std::vector<std::shared_ptr<Spectrum>> _spectra;

    QSplitter*    _splitter = nullptr;
    SpectraPanel* _panel    = nullptr;
    QTreeWidget*  _tree     = nullptr;

    bool _updatingTree   = false;
    bool _syncingFromPanel = false;
};