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
};
