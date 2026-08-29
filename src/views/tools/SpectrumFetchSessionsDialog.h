#pragma once

#include <QDialog>

#include <memory>
#include <vector>

#include "utils/SpectrumFetchService.h"

class Star;
class AppSettings;

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

/**
 * Overview of the background spectrum fetch sessions: per-session state, the
 * session log, progress with a rough ETA, and cancel controls. Sessions that
 * finished their archive search and wait for the review step can be resumed
 * from here. Opened from the Data menu or the status-bar widget.
 */
class SpectrumFetchSessionsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SpectrumFetchSessionsDialog(SpectrumFetchService* service,
                                         QWidget*              parent = nullptr);

    /// Show the review step for a session in AwaitingSelection (also called
    /// internally when a bulk discovery finishes while the dialog is open).
    void reviewSession(const QString& sessionId);

    /// Session ids whose discovery this dialog should follow up with a
    /// review dialog (bulk sessions started via the setup dialog).
    void watchSession(const QString& sessionId);

signals:
    /// "New Fetch..." was clicked; MainWindow wires this to the Project
    /// View's batch setup action.
    void newFetchRequested();

private slots:
    void rebuildList();
    void onSelectionChanged();
    void onLogUpdated(const QString& id);
    void onDiscoveryFinished(const QString& id,
                             const QList<SpecFetch::RemoteSpectrum>& results);
    void onReviewSelected();
    void onCancelSelected();
    void onCancelAll();
    void refreshProgressLabel();

private:
    QString selectedSessionId() const;

    SpectrumFetchService* _service = nullptr;
    QSet<QString>         _watched;

    QListWidget*    _list        = nullptr;
    QPlainTextEdit* _log         = nullptr;
    QLabel*         _summary     = nullptr;
    QPushButton*    _reviewBtn   = nullptr;
    QPushButton*    _cancelBtn   = nullptr;
    QPushButton*    _cancelAllBtn = nullptr;
    QTimer*         _ticker      = nullptr;
};

/**
 * Setup dialog for the batch "Fetch Spectra" Data-menu action: scope (all
 * project / filtered / selected stars), archives with their per-archive
 * options, search radius and download parallelism. The last-used state is
 * persisted in AppSettings as a JSON blob.
 */
class BatchSpectrumFetchSetupDialog : public QDialog
{
    Q_OBJECT
public:
    BatchSpectrumFetchSetupDialog(
        const std::vector<std::shared_ptr<Star>>& allStars,
        const std::vector<std::shared_ptr<Star>>& filteredStars,
        const std::vector<std::shared_ptr<Star>>& selectedStars,
        AppSettings* settings, QWidget* parent = nullptr);

    SpectrumFetchService::Options options() const;
    const std::vector<std::shared_ptr<Star>>& scopeStars() const;

    void accept() override;   // persists the dialog state

private:
    void restoreState();
    QString stateToJson() const;

    AppSettings* _settings = nullptr;
    std::vector<std::shared_ptr<Star>> _all, _filtered, _selected;

    QComboBox* _scopeCombo = nullptr;

    // Archives
    QCheckBox* _esoCb    = nullptr;
    QListWidget* _esoCollections = nullptr;
    QCheckBox* _lrsCb    = nullptr;
    QCheckBox* _mrsCb    = nullptr;
    QComboBox* _lamostDr = nullptr;
    QCheckBox* _sdssCb   = nullptr;
    QComboBox* _sdssDr   = nullptr;
    QCheckBox* _mastCb   = nullptr;
    QListWidget* _mastMissions = nullptr;
    QCheckBox* _apogeeCb = nullptr;

    // Common options
    QDoubleSpinBox* _radiusSpin  = nullptr;
    QSpinBox*       _parallelSpin = nullptr;
    QCheckBox*      _exposuresCb = nullptr;
    QCheckBox*      _vacToAirCb  = nullptr;
    QCheckBox*      _joinArmsCb  = nullptr;
    QCheckBox*      _redownloadCb = nullptr;
};

/**
 * Review step between discovery and download of a bulk fetch: per-archive
 * product counts and estimated volume, per-archive enable, and an optional
 * per-star cap (newest first). Returns the filtered pick list.
 */
class SpectrumFetchReviewDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SpectrumFetchReviewDialog(
        const QList<SpecFetch::RemoteSpectrum>& results,
        QWidget* parent = nullptr);

    QList<SpecFetch::RemoteSpectrum> picks() const;

private:
    QList<SpecFetch::RemoteSpectrum> _results;
    QTableWidget* _table   = nullptr;   // one row per archive, with checkbox
    QSpinBox*     _capSpin = nullptr;   // 0 = unlimited
};
