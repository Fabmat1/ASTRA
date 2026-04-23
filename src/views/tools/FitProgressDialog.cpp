#include "FitProgressDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>

FitProgressDialog::FitProgressDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Running spectral fit");
    resize(700, 500);
    setModal(false);

    auto* v = new QVBoxLayout(this);

    _status = new QLabel("Starting fit…");
    v->addWidget(_status);

    _bar = new QProgressBar;
    _bar->setRange(0, 1000);
    _bar->setValue(0);
    v->addWidget(_bar);

    _log = new QPlainTextEdit;
    _log->setReadOnly(true);
    _log->setStyleSheet("font-family: monospace; font-size: 11px;");
    v->addWidget(_log, 1);

    auto* row = new QHBoxLayout;
    _abortBtn = new QPushButton("Abort");
    _closeBtn = new QPushButton("Close");
    _closeBtn->setEnabled(false);
    connect(_abortBtn, &QPushButton::clicked, this, [this]{
        _abortBtn->setEnabled(false);
        _status->setText("Aborting…");
        emit abortRequested();
    });
    connect(_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    row->addStretch();
    row->addWidget(_abortBtn);
    row->addWidget(_closeBtn);
    v->addLayout(row);
}

void FitProgressDialog::appendLog(const QString& line)
{
    _log->appendPlainText(line);
}

void FitProgressDialog::setProgress(const QString& stage, double frac)
{
    if (!stage.isEmpty()) _status->setText(stage);

    if (frac < 0.0) {
        // Indeterminate: Qt shows a marquee animation when min == max == 0.
        if (_bar->maximum() != 0) _bar->setRange(0, 0);
    } else {
        if (_bar->maximum() == 0) _bar->setRange(0, 1000);
        _bar->setValue(std::clamp(int(frac * 1000.0), 0, 1000));
    }
}

void FitProgressDialog::setFinished(const astra::fitting::SpectralFitResult& r)
{
    _abortBtn->setEnabled(false);
    _closeBtn->setEnabled(true);
    _bar->setRange(0, 1000);
    _bar->setValue(1000);
    QString summary = QString(
        "✔ Finished — χ² = %1, iter = %2, free = %3, points = %4, converged = %5")
        .arg(r.finalChi2, 0, 'f', 3)
        .arg(r.iterations)
        .arg(r.nFreeParameters)
        .arg(r.nDataPoints)
        .arg(r.converged ? "yes" : "no");
    _status->setText(summary);
    appendLog("\n" + summary);
    if (!r.rejectedFiles.isEmpty())
        appendLog("Rejected: " + r.rejectedFiles.join(", "));
}

void FitProgressDialog::setError(const QString& msg)
{
    _abortBtn->setEnabled(false);
    _closeBtn->setEnabled(true);
    _bar->setRange(0, 1000);
    _bar->setValue(0);
    _status->setText("✘ Failed: " + msg);
    appendLog("ERROR: " + msg);
}
