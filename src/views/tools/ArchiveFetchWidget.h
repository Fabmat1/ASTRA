#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// "Archives" tab of the spectral analysis dialog.
//
// Queries the online spectrum archives (ESO Phase 3, LAMOST, SDSS, ...) for
// products at the star's position, lists what is available, and downloads
// the user's picks through the app-wide SpectrumFetchService. Imported
// spectra appear in the Browse tab via the dialog's reload path.
// ─────────────────────────────────────────────────────────────────────────────

#include <QWidget>

#include <memory>

#include "utils/SpectrumFetchService.h"

class Star;
class DatabaseManager;
class ApplicationController;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QTableWidget;

class ArchiveFetchWidget : public QWidget
{
    Q_OBJECT
public:
    struct Context {
        std::shared_ptr<Star>  star;
        DatabaseManager*       dbm        = nullptr;
        ApplicationController* controller = nullptr;
        QString                projectId;
    };

    explicit ArchiveFetchWidget(const Context& ctx, QWidget* parent = nullptr);

signals:
    /// Spectra for this star were imported; the dialog should reload.
    void spectraImported();

private slots:
    void onSearch();
    void onDownloadSelected();
    void onDownloadAll();
    void onDiscoveryProgress(const QString& id, const QString& archiveLabel,
                             int done, int total);
    void onDiscoveryFinished(const QString&                          id,
                             const QList<SpecFetch::RemoteSpectrum>& results);
    void onItemFinished(const QString& id,
                        const SpecFetch::RemoteSpectrum& item, bool ok,
                        bool skippedDuplicate, const QString& message);
    void onStarSpectraUpdated(const QString& starId);

private:
    void setupUi();
    void populateTable(const QList<SpecFetch::RemoteSpectrum>& results);
    void setRowStatus(int row, const QString& text);
    void queuePicks(const QList<SpecFetch::RemoteSpectrum>& picks);
    void setBusy(bool busy);
    bool alreadyImported(const QString& originId) const;
    QList<SpecFetch::Archive> checkedArchives() const;

    SpectrumFetchService* service() const;

    Context _ctx;
    QString _sessionId;
    QList<SpecFetch::RemoteSpectrum> _results;   // row i <-> _results[i]
    QSet<QString> _importedOriginIds;

    // ── UI ──────────────────────────────────────────────────────────────────
    QHash<SpecFetch::Archive, QCheckBox*> _archiveChecks;
    QDoubleSpinBox* _radiusSpin   = nullptr;
    QComboBox*      _lamostDr     = nullptr;
    QCheckBox*      _exposuresCb  = nullptr;
    QCheckBox*      _redownloadCb = nullptr;
    QPushButton*    _searchBtn    = nullptr;   // "Search" / "Stop"
    bool            _searching    = false;
    QTableWidget*   _table        = nullptr;
    QPushButton*    _downloadBtn  = nullptr;
    QPushButton*    _downloadAllBtn = nullptr;
    QLabel*         _statusLabel  = nullptr;
};
