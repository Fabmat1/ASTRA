#pragma once

#include <QDialog>

class AppSettings;
class LcqueryEnvironment;
class QLineEdit;
class QPushButton;
class QPlainTextEdit;
class QProgressBar;
class QLabel;

/**
 * Guided one-click setup for the bundled lightcurvequery Python environment.
 *
 * Unpacks the bundled scripts into a writable per-user directory, creates a
 * virtualenv on top of the chosen system Python and pip-installs the
 * requirements, streaming progress to a log view. On success it writes the
 * resulting interpreter / script paths back into AppSettings so the Lightcurve
 * Fetch dialog can use them immediately.
 */
class LcquerySetupDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LcquerySetupDialog(AppSettings* settings, QWidget* parent = nullptr);

private slots:
    void onStartClicked();

private:
    void appendLog(const QString& html);
    void setRunning(bool running);

    AppSettings*        _settings = nullptr;
    LcqueryEnvironment* _env      = nullptr;

    QLineEdit*      _pythonEdit = nullptr;
    QPushButton*    _browseBtn  = nullptr;
    QPushButton*    _startBtn   = nullptr;
    QLabel*         _stageLabel = nullptr;
    QProgressBar*   _progress   = nullptr;
    QPlainTextEdit* _log        = nullptr;
    QPushButton*    _closeBtn   = nullptr;
};
