#pragma once

#include <QDialog>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <memory>
#include <vector>

class QTimer;

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
class CoAddWidget;
class ArchiveFetchWidget;
class ApplicationController;
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
                     QWidget* parent = nullptr,
                     ApplicationController* controller = nullptr);
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
    void redetectSpectra(const QStringList& spectrumIds);
    void defineInstrumentManually(const QStringList& spectrumIds);

    void setupUi();
    /// Reload the star's spectra from the DB and refresh every view (after a
    /// fit completed or archive spectra were imported).
    void reloadStarSpectra();
    void rebuildTree();
    void refreshTreeStyling();
    void styleFlagRow(QTreeWidgetItem* item);
    void flushPendingFlagChanges();
    void updateBestMarkers();
    void setBestFitTied(const QString& fitId, bool markBest);
    void propagateBestFitParams(const std::shared_ptr<SpectralFit>& fit);
    void syncTreeSelectionTo(const QString& spectrumId, const QString& fitId);

    void removeSpectrum(const QString& spectrumId);
    void removeSpectra(const QStringList& spectrumIds);
    void removeFit(const QString& spectrumId, const QString& fitId);

    /// Delete the RV points derived from the given (already deleted) spectra,
    /// in memory and in the DB. Call only after the spectra have been dropped
    /// from the star.
    void purgeRVPointsFor(const QSet<QString>& spectrumIds);

    std::shared_ptr<Star>  _star;
    DatabaseManager*       _dbm = nullptr;
    QString                _projectId;
    ApplicationController* _controller = nullptr;

    std::vector<std::shared_ptr<Spectrum>> _spectra;

    QSplitter*    _splitter = nullptr;
    SpectraPanel* _panel    = nullptr;
    QTreeWidget*  _tree     = nullptr;
    QTabWidget*    _rightTabs  = nullptr;
    FitSetupWidget* _setup     = nullptr;
    CoAddWidget*    _coadd     = nullptr;
    ArchiveFetchWidget* _archives = nullptr;

    QPushButton*  _addSpectraBtn = nullptr;
    QPushButton*  _addFitBtn     = nullptr;
    QPushButton*  _redetectBtn   = nullptr;
    CheckStateDragger* _flagDragger = nullptr;

    // Flag toggles are applied in-memory immediately but the DB writes,
    // RV-point mirroring and summary recompute are coalesced here, so
    // drag-flagging many rows costs one update instead of one per row.
    QHash<QString, bool> _pendingSpectrumFlags;   // spectrumId → flagged
    QHash<QString, bool> _pendingFitFlags;        // fitId → flagged
    QTimer* _flagFlushTimer = nullptr;

    bool _updatingTree    = false;
    bool _syncingFromPanel = false;
};