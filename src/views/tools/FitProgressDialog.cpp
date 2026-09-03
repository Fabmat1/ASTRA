#include "FitProgressDialog.h"
#include "utils/UiIcons.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QTimer>

#include <algorithm>

namespace {

QString formatDuration(double seconds)
{
    if (!(seconds >= 0.0)) return QStringLiteral("--");
    const qint64 s = static_cast<qint64>(seconds + 0.5);
    if (s >= 3600)
        return QStringLiteral("%1:%2:%3")
            .arg(s / 3600)
            .arg((s % 3600) / 60, 2, 10, QLatin1Char('0'))
            .arg(s % 60,          2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2")
        .arg(s / 60)
        .arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

FitProgressDialog::FitProgressDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle("Running spectral fit");
    resize(700, 520);
    setModal(false);

    auto* v = new QVBoxLayout(this);

    _status = new QLabel("Starting fit…");
    QFont statusFont = _status->font();
    statusFont.setBold(true);
    _status->setFont(statusFont);
    v->addWidget(_status);

    _bar = new QProgressBar;
    _bar->setRange(0, 1000);
    _bar->setValue(0);
    v->addWidget(_bar);

    // Sub-stage line: which LM iteration, how many free parameters, the chi2
    // the solver is sitting on. Dimmed, because it changes several times a
    // second and must not compete with the stage name above.
    _detail = new QLabel;
    _detail->setTextFormat(Qt::PlainText);
    _detail->setStyleSheet("color: palette(mid);");
    v->addWidget(_detail);

    _timing = new QLabel;
    _timing->setStyleSheet("color: palette(mid);");
    v->addWidget(_timing);

    _log = new QPlainTextEdit;
    _log->setReadOnly(true);
    _log->setStyleSheet("font-family: monospace; font-size: 11px;");
    v->addWidget(_log, 1);

    auto* row = new QHBoxLayout;
    _abortBtn = new QPushButton("Abort");
    _closeBtn = new QPushButton("Close");
    UiIcons::apply(_closeBtn, UiIcons::Role::Dismiss);
    _closeBtn->setEnabled(false);
    connect(_abortBtn, &QPushButton::clicked, this, [this]{
        _abortBtn->setEnabled(false);
        _status->setText("Aborting…");
        _detail->setText("waiting for the solver to reach a safe stopping point");
        emit abortRequested();
    });
    connect(_closeBtn, &QPushButton::clicked, this, &QDialog::accept);
    row->addStretch();
    row->addWidget(_abortBtn);
    row->addWidget(_closeBtn);
    v->addLayout(row);

    // The elapsed clock has to tick on its own: a fit can spend a long time
    // inside one LM iteration, and a frozen timer reads as a hung dialog.
    _clock.start();
    _ticker = new QTimer(this);
    connect(_ticker, &QTimer::timeout, this, &FitProgressDialog::refreshTiming);
    _ticker->start(500);
    refreshTiming();
}

void FitProgressDialog::appendLog(const QString& line)
{
    _log->appendPlainText(line);
}

void FitProgressDialog::refreshTiming()
{
    if (!_running) return;
    const double elapsed = _clock.elapsed() / 1000.0;
    QString t = QStringLiteral("elapsed %1").arg(formatDuration(elapsed));
    if (_stepTotal > 1)
        t += QStringLiteral("  ·  spectrum %1 of %2")
                 .arg(_stepIndex + 1).arg(_stepTotal);
    if (_eta >= 0.0)
        t += _stepTotal > 1
                 ? QStringLiteral("  ·  ~%1 left on this fit").arg(formatDuration(_eta))
                 : QStringLiteral("  ·  ~%1 remaining").arg(formatDuration(_eta));
    _timing->setText(t);
}

void FitProgressDialog::setProgress(const astra::fitting::FitProgressInfo& info)
{
    if (!info.stage.isEmpty()) {
        _status->setText(_stepTotal > 1
            ? QStringLiteral("[%1/%2] %3  ·  %4")
                  .arg(_stepIndex + 1).arg(_stepTotal).arg(info.stage, _stepLabel)
            : info.stage);
    }
    _detail->setText(info.detail);

    if (info.fraction < 0.0 && _stepTotal <= 1) {
        // Indeterminate: Qt shows a marquee animation when min == max == 0.
        if (_bar->maximum() != 0) _bar->setRange(0, 0);
    } else {
        if (_bar->maximum() == 0) _bar->setRange(0, 1000);
        // In a sequence the bar tracks the whole queue: a step with no
        // fraction of its own simply parks at its own starting edge, so the
        // bar never runs backwards when the next spectrum begins.
        const double within = std::max(info.fraction, 0.0);
        const double f = _stepTotal > 1
                             ? (_stepIndex + within) / double(_stepTotal)
                             : within;
        _bar->setValue(std::clamp(int(f * 1000.0), 0, 1000));
    }

    _eta = info.etaSeconds;
    refreshTiming();
}

void FitProgressDialog::beginStep(int index, int total, const QString& label)
{
    _stepIndex = index;
    _stepTotal = total;
    _stepLabel = label;
    _eta       = -1.0;

    if (_bar->maximum() == 0) _bar->setRange(0, 1000);
    _bar->setValue(std::clamp(int(index / double(std::max(total, 1)) * 1000.0),
                              0, 1000));

    _status->setText(QStringLiteral("[%1/%2] Starting  ·  %3")
                         .arg(index + 1).arg(total).arg(label));
    _detail->clear();
    appendLog(QStringLiteral("\n── [%1/%2] %3 ──────────────")
                  .arg(index + 1).arg(total).arg(label));
    refreshTiming();
}

void FitProgressDialog::appendStepResult(const QString& line)
{
    appendLog(line);
}

void FitProgressDialog::setSequenceFinished(int nOk, int nFailed, bool aborted)
{
    const double elapsed = _clock.elapsed() / 1000.0;
    stopRunning();
    if (!aborted && nFailed == 0) _bar->setValue(1000);

    QString summary;
    if (aborted && nOk == 0)
        summary = QStringLiteral("Aborted - no fit was saved.");
    else if (aborted)
        summary = QStringLiteral("Aborted - the %1 fit(s) that had finished "
                                 "are saved, the rest were not run").arg(nOk);
    else if (nFailed == 0)
        summary = QStringLiteral("✓ Finished - %1 of %2 fits succeeded")
                      .arg(nOk).arg(nOk + nFailed);
    else
        summary = QStringLiteral("Finished - %1 of %2 fits succeeded, %3 failed")
                      .arg(nOk).arg(nOk + nFailed).arg(nFailed);
    _status->setText(summary);
    _detail->clear();
    _timing->setText(QStringLiteral("took %1").arg(formatDuration(elapsed)));
    appendLog("\n" + summary);
}

void FitProgressDialog::stopRunning()
{
    _running = false;
    _ticker->stop();
    _abortBtn->setEnabled(false);
    _closeBtn->setEnabled(true);
    _bar->setRange(0, 1000);
}

void FitProgressDialog::setFinished(const astra::fitting::SpectralFitResult& r)
{
    const double elapsed = _clock.elapsed() / 1000.0;
    stopRunning();
    _bar->setValue(1000);

    QString summary = QString(
        "✓ Finished - χ² = %1, iter = %2, free = %3, points = %4, converged = %5")
        .arg(r.finalChi2, 0, 'f', 3)
        .arg(r.iterations)
        .arg(r.nFreeParameters)
        .arg(r.nDataPoints)
        .arg(r.converged ? "yes" : "no");
    _status->setText(summary);
    _detail->clear();
    _timing->setText(QStringLiteral("took %1").arg(formatDuration(elapsed)));
    appendLog("\n" + summary);
    if (!r.rejectedFiles.isEmpty())
        appendLog("Rejected: " + r.rejectedFiles.join(", "));
}

void FitProgressDialog::setError(const QString& msg)
{
    stopRunning();
    _bar->setValue(0);
    _status->setText("✗ Failed: " + msg);
    _detail->clear();
    appendLog("ERROR: " + msg);
}

void FitProgressDialog::setAborted()
{
    const double elapsed = _clock.elapsed() / 1000.0;
    stopRunning();
    _status->setText("Aborted - no fit was saved.");
    _detail->clear();
    _timing->setText(QStringLiteral("stopped after %1").arg(formatDuration(elapsed)));
    appendLog("\nAborted on request; nothing was saved.");
}
