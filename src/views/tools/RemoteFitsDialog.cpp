#include "views/tools/RemoteFitsDialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace astra::remote {

namespace {

QString prettyState(const RemoteFitService::RunInfo& r)
{
    if (r.stopping) return QObject::tr("Stopping");
    if (r.state == QLatin1String("queued"))   return QObject::tr("Queued");
    if (r.state == QLatin1String("running"))  return QObject::tr("Running");
    if (r.state == QLatin1String("staged"))   return QObject::tr("Starting");
    if (r.state == QLatin1String("aborting")) return QObject::tr("Stopping");
    if (r.state == QLatin1String("done"))     return QObject::tr("Finished");
    if (r.state == QLatin1String("harvested"))return QObject::tr("Collected");
    if (r.state == QLatin1String("aborted"))  return QObject::tr("Stopped");
    if (r.state == QLatin1String("failed"))   return QObject::tr("Failed");
    return r.state;
}

} // namespace

RemoteFitsDialog::RemoteFitsDialog(RemoteFitService* service, QWidget* parent)
    : QDialog(parent), _service(service)
{
    setWindowTitle(tr("Remote Fits"));
    resize(880, 420);

    auto* root = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("Fits running on other machines. These keep going when ASTRA is "
           "closed; anything still unfinished is picked up again on the next "
           "start and its result stored automatically."));
    intro->setWordWrap(true);
    root->addWidget(intro);

    _table = new QTableWidget(0, 6, this);
    _table->setHorizontalHeaderLabels({tr("Host"), tr("State"), tr("Progress"),
                                       tr("Stage"), tr("Job"), tr("Started")});
    _table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _table->setSelectionMode(QAbstractItemView::SingleSelection);
    _table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _table->verticalHeader()->setVisible(false);
    _table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    root->addWidget(_table, 1);

    _summary = new QLabel;
    _summary->setWordWrap(true);
    root->addWidget(_summary);

    auto* buttons = new QHBoxLayout;
    _stopBtn = new QPushButton(tr("Stop fit"));
    _stopBtn->setToolTip(tr("Ask the remote worker to stop. Work that already "
                            "finished is kept."));
    _forgetBtn = new QPushButton(tr("Remove from list"));
    _forgetBtn->setToolTip(tr("Stop tracking this run. Nothing on the remote "
                              "host is touched."));
    buttons->addWidget(_stopBtn);
    buttons->addWidget(_forgetBtn);
    buttons->addStretch();
    auto* close = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->addWidget(close);
    root->addLayout(buttons);

    connect(close, &QDialogButtonBox::rejected, this, &QDialog::accept);
    connect(_stopBtn, &QPushButton::clicked, this, &RemoteFitsDialog::onStop);
    connect(_forgetBtn, &QPushButton::clicked, this,
            &RemoteFitsDialog::onForget);
    if (_service)
        connect(_service, &RemoteFitService::runsChanged, this,
                &RemoteFitsDialog::refresh);

    // The progress inside a run changes without the run list changing, so the
    // view ticks as well as listening.
    _ticker = new QTimer(this);
    _ticker->setInterval(1000);
    connect(_ticker, &QTimer::timeout, this, &RemoteFitsDialog::refresh);
    _ticker->start();

    refresh();
}

QString RemoteFitsDialog::selectedRunId() const
{
    const int row = _table->currentRow();
    if (row < 0 || row >= _table->rowCount()) return {};
    const auto* item = _table->item(row, 0);
    return item ? item->data(Qt::UserRole).toString() : QString();
}

void RemoteFitsDialog::refresh()
{
    if (!_service) return;
    const auto list = _service->runs();

    const QString keep = selectedRunId();
    _table->setRowCount(list.size());
    int selectRow = -1;

    for (int i = 0; i < list.size(); ++i) {
        const auto& r = list[i];

        auto* host = new QTableWidgetItem(
            r.reattached ? tr("%1 (earlier session)").arg(r.hostName)
                         : r.hostName);
        host->setData(Qt::UserRole, r.id);
        if (!r.error.isEmpty()) host->setToolTip(r.error);
        _table->setItem(i, 0, host);

        _table->setItem(i, 1, new QTableWidgetItem(prettyState(r)));

        auto* pct = new QTableWidgetItem(
            r.fraction >= 0.0
                ? QStringLiteral("%1 %").arg(100.0 * r.fraction, 0, 'f', 1)
                : QStringLiteral("-"));
        pct->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        _table->setItem(i, 2, pct);

        _table->setItem(i, 3, new QTableWidgetItem(
            r.detail.isEmpty() ? r.stage
                               : QStringLiteral("%1  %2").arg(r.stage, r.detail)));
        _table->setItem(i, 4, new QTableWidgetItem(
            r.slurmJobId.isEmpty() ? QStringLiteral("-") : r.slurmJobId));
        _table->setItem(i, 5, new QTableWidgetItem(r.createdAt));

        if (r.id == keep) selectRow = i;
    }
    if (selectRow >= 0) _table->selectRow(selectRow);
    _table->resizeColumnsToContents();
    _table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);

    _summary->setText(list.isEmpty()
                          ? tr("No fits are running remotely.")
                          : tr("%n fit(s) running remotely.", "", list.size()));
    const bool hasSel = !selectedRunId().isEmpty();
    _stopBtn->setEnabled(hasSel);
    _forgetBtn->setEnabled(hasSel);
}

void RemoteFitsDialog::onStop()
{
    const QString id = selectedRunId();
    if (id.isEmpty() || !_service) return;
    if (QMessageBox::question(
            this, tr("Stop fit"),
            tr("Ask the remote worker to stop this fit?\n\n"
               "It stops at the next safe point, so results it already "
               "produced are kept.")) != QMessageBox::Yes)
        return;
    _service->requestStop(id);
    refresh();
}

void RemoteFitsDialog::onForget()
{
    const QString id = selectedRunId();
    if (id.isEmpty() || !_service) return;
    if (QMessageBox::question(
            this, tr("Remove from list"),
            tr("Stop tracking this run?\n\nThe job on the remote host is left "
               "alone, and its files are not deleted.")) != QMessageBox::Yes)
        return;
    _service->forget(id);
    refresh();
}

} // namespace astra::remote
