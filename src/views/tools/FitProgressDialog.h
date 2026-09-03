#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include "fitting/FitTypes.h"

class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class QLabel;
class QTimer;

class FitProgressDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FitProgressDialog(QWidget* parent = nullptr);

public slots:
    void appendLog(const QString& line);
    void setProgress(const astra::fitting::FitProgressInfo& info);
    void setFinished(const astra::fitting::SpectralFitResult& r);
    void setError(const QString& msg);
    void setAborted();

    // ── Sequential runs (one fit per spectrum, back to back) ───────────
    /// Announces the step that is about to start. The progress bar is
    /// rescaled into that step's slice of the whole queue, so it keeps
    /// running from 0 to 100 % across the sequence rather than per fit.
    void beginStep(int index, int total, const QString& label);
    /// Records the outcome of one step without ending the run.
    void appendStepResult(const QString& line);
    /// The whole queue is done; @p nFailed counts the steps that did not
    /// produce a fit. @p aborted says the queue was cut short on request,
    /// in which case the fits already saved still stand.
    void setSequenceFinished(int nOk, int nFailed, bool aborted = false);

signals:
    void abortRequested();

private:
    // Elapsed is the dialog's own clock (it starts when the dialog does, so
    // it covers the backend's start-up too); the estimate comes from the
    // backend, and is only shown once it has offered one.
    void refreshTiming();
    void stopRunning();

    QLabel*         _status   = nullptr;
    QLabel*         _detail   = nullptr;
    QLabel*         _timing   = nullptr;
    QProgressBar*   _bar      = nullptr;
    QPlainTextEdit* _log      = nullptr;
    QPushButton*    _abortBtn = nullptr;
    QPushButton*    _closeBtn = nullptr;

    QElapsedTimer   _clock;
    QTimer*         _ticker   = nullptr;
    double          _eta      = -1.0;   // seconds, < 0 = unknown
    bool            _running  = true;

    // Sequential runs only; _stepTotal stays 0 for a single joint fit, which
    // is what every "are we in a sequence" test below keys on.
    int             _stepIndex = 0;
    int             _stepTotal = 0;
    QString         _stepLabel;
};
