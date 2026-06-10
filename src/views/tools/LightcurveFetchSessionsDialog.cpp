#include "LightcurveFetchSessionsDialog.h"

#include "views/widgets/AnsiTerminalWidget.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QSplitter>
#include <QVBoxLayout>

// ───────────────────────────────────────────────────────────────────
// LightcurveFetchSessionsDialog
// ───────────────────────────────────────────────────────────────────

namespace {

QString stateGlyph(LightcurveFetchService::State s)
{
    using State = LightcurveFetchService::State;
    switch (s) {
        case State::Queued:    return QStringLiteral("◌");
        case State::Running:   return QStringLiteral("▶");
        case State::Finished:  return QStringLiteral("✔");
        case State::Failed:    return QStringLiteral("✘");
        case State::Cancelled: return QStringLiteral("◼");
    }
    return {};
}

} // namespace

LightcurveFetchSessionsDialog::LightcurveFetchSessionsDialog(
    LightcurveFetchService* service, QWidget* parent)
    : QDialog(parent)
    , _service(service)
{
    setWindowTitle(tr("Lightcurve Fetch Sessions"));
    resize(1100, 620);

    auto* root = new QVBoxLayout(this);

    _summary = new QLabel;
    _summary->setStyleSheet("color: gray;");
    root->addWidget(_summary);

    auto* splitter = new QSplitter(Qt::Horizontal);

    _list = new QListWidget;
    _list->setSelectionMode(QAbstractItemView::SingleSelection);
    _list->setAlternatingRowColors(true);
    _list->setMinimumWidth(260);
    splitter->addWidget(_list);

    auto* right = new QWidget;
    auto* rv    = new QVBoxLayout(right);
    rv->setContentsMargins(0, 0, 0, 0);
    _terminal = new AnsiTerminalWidget;
    rv->addWidget(_terminal, 1);
    _statusLbl = new QLabel;
    _statusLbl->setStyleSheet("color: gray;");
    _statusLbl->setWordWrap(true);
    rv->addWidget(_statusLbl);
    splitter->addWidget(right);

    splitter->setStretchFactor(0, 0);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({300, 800});
    root->addWidget(splitter, 1);

    auto* btnRow  = new QHBoxLayout;
    _cancelBtn    = new QPushButton(tr("Cancel Selected"));
    _cancelAllBtn = new QPushButton(tr("Cancel All"));
    auto* closeBtn = new QPushButton(tr("Close"));
    btnRow->addWidget(_cancelBtn);
    btnRow->addWidget(_cancelAllBtn);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    root->addLayout(btnRow);

    connect(_cancelBtn,    &QPushButton::clicked, this, &LightcurveFetchSessionsDialog::onCancelSelected);
    connect(_cancelAllBtn, &QPushButton::clicked, this, &LightcurveFetchSessionsDialog::onCancelAll);
    connect(closeBtn,      &QPushButton::clicked, this, &QDialog::accept);

    connect(_list, &QListWidget::itemSelectionChanged,
            this, &LightcurveFetchSessionsDialog::onSelectionChanged);

    connect(_service, &LightcurveFetchService::sessionsChanged,
            this, &LightcurveFetchSessionsDialog::rebuildList);
    connect(_service, &LightcurveFetchService::sessionOutput,
            this, &LightcurveFetchSessionsDialog::onSessionOutput);
    connect(_service, &LightcurveFetchService::progressChanged,
            this, [this](int, int, int) { updateSummaryLabel(); });

    rebuildList();
    if (_list->count() > 0 && !_list->currentItem())
        _list->setCurrentRow(0);
}

QString LightcurveFetchSessionsDialog::selectedSessionId() const
{
    auto* it = _list->currentItem();
    return it ? it->data(Qt::UserRole).toString() : QString{};
}

void LightcurveFetchSessionsDialog::updateSummaryLabel()
{
    const int running = _service->runningCount();
    const int queued  = _service->queuedCount();
    if (running + queued > 0) {
        _summary->setText(tr("%1 of %2 done  ·  %3 running, %4 queued")
                              .arg(_service->waveDone())
                              .arg(_service->waveTotal())
                              .arg(running)
                              .arg(queued));
    } else if (_service->waveTotal() > 0) {
        _summary->setText(tr("All %1 sessions finished.").arg(_service->waveTotal()));
    } else {
        _summary->setText(tr("No fetch sessions yet."));
    }
}

void LightcurveFetchSessionsDialog::rebuildList()
{
    const QString prevSel = selectedSessionId();

    QSignalBlocker block(_list);
    _list->clear();

    const auto sessions = _service->sessions();
    // Newest first.
    for (auto it = sessions.crbegin(); it != sessions.crend(); ++it) {
        const auto& info = *it;
        auto* item = new QListWidgetItem(
            QString("%1  %2 — %3")
                .arg(stateGlyph(info.state),
                     info.starLabel,
                     LightcurveFetchService::stateLabel(info.state)));
        item->setData(Qt::UserRole, info.id);
        if (!info.summary.isEmpty())
            item->setToolTip(info.summary);
        _list->addItem(item);
        if (info.id == prevSel)
            _list->setCurrentItem(item);
    }

    if (!_list->currentItem() && _list->count() > 0)
        _list->setCurrentRow(0);

    block.unblock();
    onSelectionChanged();
    updateSummaryLabel();
}

void LightcurveFetchSessionsDialog::onSelectionChanged()
{
    const QString id = selectedSessionId();
    _terminal->clearTerminal();
    _statusLbl->clear();
    _cancelBtn->setEnabled(false);
    if (id.isEmpty()) return;

    const QByteArray buf = _service->sessionBuffer(id);
    if (!buf.isEmpty()) _terminal->feed(buf);

    bool found = false;
    const auto info = _service->sessionInfo(id, &found);
    if (found) {
        _statusLbl->setText(QString("%1 — Gaia DR3 %2 — %3%4")
                                .arg(info.starLabel,
                                     info.gaiaId,
                                     LightcurveFetchService::stateLabel(info.state),
                                     info.summary.isEmpty()
                                         ? QString()
                                         : QString("  ·  %1").arg(info.summary)));
        _cancelBtn->setEnabled(_service->isSessionActive(id));
    }
}

void LightcurveFetchSessionsDialog::onSessionOutput(const QString& id,
                                                    const QByteArray& chunk)
{
    if (id == selectedSessionId())
        _terminal->feed(chunk);
}

void LightcurveFetchSessionsDialog::onCancelSelected()
{
    const QString id = selectedSessionId();
    if (!id.isEmpty())
        _service->cancelSession(id);
}

void LightcurveFetchSessionsDialog::onCancelAll()
{
    _service->cancelAll();
}

// ───────────────────────────────────────────────────────────────────
// BatchLightcurveFetchSetupDialog
// ───────────────────────────────────────────────────────────────────

BatchLightcurveFetchSetupDialog::BatchLightcurveFetchSetupDialog(int starCount,
                                                                 QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Fetch Lightcurves"));

    auto* root = new QVBoxLayout(this);

    auto* hdr = new QLabel(
        tr("Fetch public lightcurves for <b>%n selected star(s)</b> via the "
           "bundled <i>lightcurvequery</i> tool. The fetches run in the "
           "background; progress is shown in the status bar.",
           nullptr, starCount));
    hdr->setWordWrap(true);
    root->addWidget(hdr);

    auto* srcBox = new QGroupBox(tr("Sources"));
    auto* srcLay = new QHBoxLayout(srcBox);
    _tess  = new QCheckBox("TESS");     _tess->setChecked(true);
    _ztf   = new QCheckBox("ZTF");      _ztf->setChecked(true);
    _atlas = new QCheckBox("ATLAS");    _atlas->setChecked(true);
    _gaia  = new QCheckBox("Gaia");     _gaia->setChecked(true);
    _bg    = new QCheckBox("BlackGEM"); _bg->setChecked(false);
    _bg->setToolTip(tr("Requires the BlackGEM query script to be configured."));
    for (auto* cb : { _tess, _ztf, _atlas, _gaia, _bg })
        srcLay->addWidget(cb);
    srcLay->addStretch();
    root->addWidget(srcBox);

    auto* form = new QFormLayout;
    _workers = new QSpinBox;
    _workers->setRange(1, 4);
    _workers->setValue(2);
    _workers->setToolTip(tr("How many stars are queried concurrently. "
                            "Capped at 4 to avoid hammering the remote archives."));
    form->addRow(tr("Parallel workers:"), _workers);
    root->addLayout(form);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    bb->button(QDialogButtonBox::Ok)->setText(tr("Start Fetch"));
    connect(bb, &QDialogButtonBox::accepted, this, [this] {
        if (options().sources.isEmpty()) return; // need at least one source
        accept();
    });
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(bb);
}

LightcurveFetcher::Options BatchLightcurveFetchSetupDialog::options() const
{
    LightcurveFetcher::Options opt;
    if (_tess->isChecked())  opt.sources << "TESS";
    if (_ztf->isChecked())   opt.sources << "ZTF";
    if (_atlas->isChecked()) opt.sources << "ATLAS";
    if (_gaia->isChecked())  opt.sources << "Gaia";
    if (_bg->isChecked())    opt.sources << "BlackGEM";
    return opt;
}

int BatchLightcurveFetchSetupDialog::parallelWorkers() const
{
    return _workers->value();
}
