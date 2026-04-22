#pragma once

#include <QDialog>
#include "fitting/FitTypes.h"

class QProgressBar;
class QPlainTextEdit;
class QPushButton;
class QLabel;

class FitProgressDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FitProgressDialog(QWidget* parent = nullptr);

public slots:
    void appendLog(const QString& line);
    void setProgress(const QString& stage, double frac);
    void setFinished(const astra::fitting::SpectralFitResult& r);
    void setError(const QString& msg);

signals:
    void abortRequested();

private:
    QLabel*         _status   = nullptr;
    QProgressBar*   _bar      = nullptr;
    QPlainTextEdit* _log      = nullptr;
    QPushButton*    _abortBtn = nullptr;
    QPushButton*    _closeBtn = nullptr;
};