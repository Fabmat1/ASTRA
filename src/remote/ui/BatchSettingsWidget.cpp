#include "remote/ui/BatchSettingsWidget.h"

#include "remote/RemoteScheduler.h"

#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTableWidget>
#include <QVBoxLayout>

namespace astra::remote {

namespace {

enum TierColumn { kColSpectra = 0, kColMemory, kColTime, kColCount };

QSpinBox* limitSpin(int max, const QString& zeroText)
{
    auto* s = new QSpinBox;
    s->setRange(0, max);
    s->setSpecialValueText(zeroText);   // shown for 0
    return s;
}

} // namespace

BatchSettingsWidget::BatchSettingsWidget(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);

    auto* intro = new QLabel(tr(
        "How mass fitting packs many fits onto this host. Each task runs a "
        "batch of fits one after another; memory and wall time are chosen "
        "per tier from the number of spectra in a fit, and a fit killed for "
        "running out of either is retried one tier up. All values depend on "
        "your cluster."));
    intro->setWordWrap(true);
    v->addWidget(intro);

    _enabled = new QCheckBox(tr("Batch mass-fit work on this host"));
    _enabled->setToolTip(tr("Off, every fit becomes a job of its own, which "
                            "only suits a handful of stars."));
    v->addWidget(_enabled);

    auto* packBox = new QGroupBox(tr("Packing"));
    auto* pf = new QFormLayout(packBox);
    _fitsPerTask = new QSpinBox;
    _fitsPerTask->setRange(1, 100000);
    _fitsPerTask->setToolTip(tr("Upper bound; a task is also cut short of its "
                                "tier's time limit using measured fit times."));
    pf->addRow(tr("Fits per task, at most:"), _fitsPerTask);
    _cpusPerTask = new QSpinBox;
    _cpusPerTask->setRange(1, 1024);
    _cpusPerTask->setToolTip(tr("Threads per fit. Mass fitting parallelises "
                                "over fits, so 1 is usually best."));
    pf->addRow(tr("CPUs per task:"), _cpusPerTask);
    _maxConcurrent = limitSpin(1000000, tr("no limit"));
    _maxConcurrent->setToolTip(tr("Tasks running at the same time. On a Slurm "
                                  "cluster this becomes the array's %N "
                                  "throttle; on a plain host it is the number "
                                  "of parallel runners."));
    pf->addRow(tr("Concurrent tasks:"), _maxConcurrent);
    _timeGuard = new QSpinBox;
    _timeGuard->setRange(0, 86400);
    _timeGuard->setSuffix(tr(" s"));
    _timeGuard->setToolTip(tr("A task stops starting new fits when less than "
                              "this plus the next fit's expected duration is "
                              "left of its wall time."));
    pf->addRow(tr("Time guard:"), _timeGuard);
    _retries = new QSpinBox;
    _retries->setRange(0, 10);
    pf->addRow(tr("Retries after a kill:"), _retries);
    _learn = new QCheckBox(tr("Learn memory and time from finished fits"));
    pf->addRow(QString(), _learn);
    v->addWidget(packBox);

    // ── Slurm ceilings ──────────────────────────────────────────────────
    auto* slurmBox = new QGroupBox(tr("Scheduler limits"));
    auto* sv = new QVBoxLayout(slurmBox);
    auto* sf = new QFormLayout;
    _maxArray = limitSpin(10000000, tr("cluster default"));
    _maxArray->setToolTip(tr("Largest job array one submission may carry "
                             "(Slurm's MaxArraySize)."));
    sf->addRow(tr("Max array size:"), _maxArray);
    _maxQueued = limitSpin(10000000, tr("no limit"));
    _maxQueued->setToolTip(tr("Most tasks this host may have queued or running "
                              "at once. Keep it under your per-user submit "
                              "limit (MaxSubmitJobs): every array element "
                              "counts, and sbatch refuses jobs past it."));
    sf->addRow(tr("Max queued tasks:"), _maxQueued);
    sv->addLayout(sf);
    auto* detectRow = new QHBoxLayout;
    _detectBtn = new QPushButton(tr("Detect from cluster"));
    _detectBtn->setToolTip(tr("Reads MaxArraySize and your association and QoS "
                              "limits with scontrol and sacctmgr, and fills "
                              "the limits above and the concurrency."));
    detectRow->addWidget(_detectBtn);
    detectRow->addStretch();
    sv->addLayout(detectRow);
    _detectResult = new QLabel;
    _detectResult->setWordWrap(true);
    _detectResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    sv->addWidget(_detectResult);
    v->addWidget(slurmBox);
    _slurmOnly << slurmBox;

    // ── Tiers ───────────────────────────────────────────────────────────
    auto* tierBox = new QGroupBox(tr("Resource tiers"));
    auto* tv = new QVBoxLayout(tierBox);
    _tiers = new QTableWidget(0, kColCount);
    _tiers->setHorizontalHeaderLabels(
        { tr("Spectra per fit, up to"), tr("Memory"), tr("Wall time") });
    _tiers->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    _tiers->verticalHeader()->setVisible(false);
    _tiers->setSelectionBehavior(QAbstractItemView::SelectRows);
    _tiers->setToolTip(tr("A fit goes to the first tier whose bound covers "
                          "its number of spectra; 0 means no bound (last "
                          "tier). Memory like 4G, wall time like 05:00:00 "
                          "or 1-00:00:00."));
    _tiers->setMinimumHeight(130);
    tv->addWidget(_tiers);
    auto* tierBtns = new QHBoxLayout;
    auto* addTier = new QPushButton(tr("Add tier"));
    auto* remTier = new QPushButton(tr("Remove tier"));
    auto* resetTiers = new QPushButton(tr("Reset to defaults"));
    tierBtns->addWidget(addTier);
    tierBtns->addWidget(remTier);
    tierBtns->addStretch();
    tierBtns->addWidget(resetTiers);
    tv->addLayout(tierBtns);
    v->addWidget(tierBox);

    _validation = new QLabel;
    _validation->setWordWrap(true);
    _validation->setStyleSheet(QStringLiteral("color: #c0392b;"));
    v->addWidget(_validation);
    v->addStretch();

    connect(_detectBtn, &QPushButton::clicked, this,
            &BatchSettingsWidget::detectRequested);
    connect(addTier, &QPushButton::clicked, this, [this] {
        ResourceTier t;
        if (_tiers->rowCount() > 0) {
            // Start from the row above so only the bound needs typing.
            const int r = _tiers->rowCount() - 1;
            t.memory    = _tiers->item(r, kColMemory)->text();
            t.timeLimit = _tiers->item(r, kColTime)->text();
        }
        addTierRow(t);
        refreshValidation();
    });
    connect(remTier, &QPushButton::clicked, this, [this] {
        const int r = _tiers->currentRow();
        if (r >= 0) _tiers->removeRow(r);
        refreshValidation();
    });
    connect(resetTiers, &QPushButton::clicked, this, [this] {
        _tiers->setRowCount(0);
        for (const auto& t : BatchSettings::defaultTiers()) addTierRow(t);
        refreshValidation();
    });
    connect(_tiers, &QTableWidget::itemChanged, this,
            [this] { refreshValidation(); });
    for (QSpinBox* s : { _fitsPerTask, _cpusPerTask, _maxArray, _maxConcurrent,
                         _maxQueued, _timeGuard, _retries })
        connect(s, &QSpinBox::valueChanged, this, [this] { refreshValidation(); });

    setSettings(BatchSettings{});
}

void BatchSettingsWidget::addTierRow(const ResourceTier& t)
{
    const QSignalBlocker block(_tiers);
    const int r = _tiers->rowCount();
    _tiers->insertRow(r);
    _tiers->setItem(r, kColSpectra,
                    new QTableWidgetItem(QString::number(t.maxSpectra)));
    _tiers->setItem(r, kColMemory, new QTableWidgetItem(t.memory));
    _tiers->setItem(r, kColTime, new QTableWidgetItem(t.timeLimit));
}

void BatchSettingsWidget::setSettings(const BatchSettings& s)
{
    _enabled->setChecked(s.enabled);
    _fitsPerTask->setValue(s.fitsPerTask);
    _cpusPerTask->setValue(s.cpusPerTask);
    _maxArray->setValue(s.maxArraySize);
    _maxConcurrent->setValue(s.maxConcurrentTasks);
    _maxQueued->setValue(s.maxQueuedTasks);
    _timeGuard->setValue(s.timeGuardSeconds);
    _retries->setValue(s.maxRetries);
    _learn->setChecked(s.learnFromUsage);
    _tiers->setRowCount(0);
    for (const auto& t : s.tiers) addTierRow(t);
    _detectResult->clear();
    refreshValidation();
}

BatchSettings BatchSettingsWidget::settings() const
{
    BatchSettings s;
    s.enabled            = _enabled->isChecked();
    s.fitsPerTask        = _fitsPerTask->value();
    s.cpusPerTask        = _cpusPerTask->value();
    s.maxArraySize       = _maxArray->value();
    s.maxConcurrentTasks = _maxConcurrent->value();
    s.maxQueuedTasks     = _maxQueued->value();
    s.timeGuardSeconds   = _timeGuard->value();
    s.maxRetries         = _retries->value();
    s.learnFromUsage     = _learn->isChecked();
    s.tiers.clear();
    for (int r = 0; r < _tiers->rowCount(); ++r) {
        ResourceTier t;
        auto text = [&](int c) {
            const QTableWidgetItem* it = _tiers->item(r, c);
            return it ? it->text().trimmed() : QString();
        };
        t.maxSpectra = text(kColSpectra).toInt();
        t.memory     = text(kColMemory);
        t.timeLimit  = text(kColTime);
        s.tiers.append(t);
    }
    return s;
}

void BatchSettingsWidget::setSlurm(bool slurm)
{
    for (QWidget* w : _slurmOnly) w->setVisible(slurm);
}

void BatchSettingsWidget::showDetected(const SlurmLimits& limits)
{
    BatchSettings s = settings();
    applySlurmLimits(limits, s);
    _maxArray->setValue(s.maxArraySize);
    _maxQueued->setValue(s.maxQueuedTasks);
    _maxConcurrent->setValue(s.maxConcurrentTasks);
    QString text = limits.describe();
    if (limits.maxWallSeconds > 0) {
        for (const auto& t : s.tiers) {
            if (parseSlurmTime(t.timeLimit) > limits.maxWallSeconds) {
                text += tr("\nA tier's wall time (%1) is longer than this QoS "
                           "allows; such tasks would never start.")
                            .arg(t.timeLimit);
                break;
            }
        }
    }
    _detectResult->setText(text);
}

void BatchSettingsWidget::showDetectError(const QString& message)
{
    _detectResult->setText(tr("Could not read the limits: %1").arg(message));
}

void BatchSettingsWidget::setDetectBusy(bool busy)
{
    _detectBtn->setEnabled(!busy);
    if (busy) _detectResult->setText(tr("Asking the cluster ..."));
}

void BatchSettingsWidget::refreshValidation()
{
    const QStringList problems = settings().validate();
    _validation->setText(problems.join(QLatin1Char('\n')));
    _validation->setVisible(!problems.isEmpty());
}

} // namespace astra::remote
