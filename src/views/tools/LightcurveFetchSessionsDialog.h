#pragma once

#include <QDialog>

#include "utils/LightcurveFetchService.h"

class QListWidget;
class QLabel;
class QPushButton;
class QCheckBox;
class QSpinBox;
class AnsiTerminalWidget;

/**
 * Overview of all background lightcurve fetch sessions: per-session state,
 * the live (or final) terminal output of the selected session, and
 * interrupt/cancel controls. Opened from the main-window status bar widget
 * or the Analysis menu.
 */
class LightcurveFetchSessionsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LightcurveFetchSessionsDialog(LightcurveFetchService* service,
                                           QWidget*                parent = nullptr);

signals:
    /// Emitted when the user clicks "New Fetch…" - the owner (MainWindow) wires
    /// this to the Project View's batch-fetch action for the selected stars.
    void newFetchRequested();

private slots:
    void rebuildList();
    void onSelectionChanged();
    void onSessionOutput(const QString& id, const QByteArray& chunk);
    void onCancelSelected();
    void onCancelAll();

private:
    QString selectedSessionId() const;
    void    updateSummaryLabel();

    LightcurveFetchService* _service = nullptr;

    QListWidget*        _list      = nullptr;
    AnsiTerminalWidget* _terminal  = nullptr;
    QLabel*             _summary   = nullptr;
    QLabel*             _statusLbl = nullptr;
    QPushButton*        _cancelBtn = nullptr;
    QPushButton*        _cancelAllBtn = nullptr;
};

/**
 * Small setup dialog for the batch "Fetch Lightcurves" Analysis action:
 * pick the sources to query and how many parallel workers to use (max 4,
 * to not hammer the remote archives).
 */
class BatchLightcurveFetchSetupDialog : public QDialog
{
    Q_OBJECT
public:
    explicit BatchLightcurveFetchSetupDialog(int      starCount,
                                             QWidget* parent = nullptr);

    LightcurveFetcher::Options options() const;
    int parallelWorkers() const;

private:
    QCheckBox* _tess  = nullptr;
    QCheckBox* _ztf   = nullptr;
    QCheckBox* _atlas = nullptr;
    QCheckBox* _gaia  = nullptr;
    QCheckBox* _bg    = nullptr;
    QSpinBox*  _workers = nullptr;
};
