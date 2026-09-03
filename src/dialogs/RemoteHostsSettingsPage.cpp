#include "dialogs/RemoteHostsSettingsPage.h"

#include "remote/RemoteHostRegistry.h"
#include "utils/AppSettings.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <QFutureWatcher>

namespace astra::remote {

RemoteHostsSettingsPage::RemoteHostsSettingsPage(QWidget* parent)
    : QWidget(parent)
{
    _hosts = RemoteHostRegistry::instance().hosts();

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16);

    auto* intro = new QLabel(
        "Machines reachable over SSH that ASTRA can use for fitting. A host "
        "can stream its model grids to this computer (fitting still runs "
        "locally), run whole fits itself, or both. Connections use your "
        "<code>~/.ssh/config</code>, so aliases and jump hosts work as usual.");
    intro->setWordWrap(true);
    outer->addWidget(intro);

    // ── Host list + editor ───────────────────────────────────────────────
    auto* split = new QHBoxLayout;

    auto* leftCol = new QVBoxLayout;
    _list = new QListWidget;
    _list->setFixedWidth(170);
    leftCol->addWidget(_list, 1);
    auto* listBtns = new QHBoxLayout;
    auto* addBtn = new QPushButton("Add");
    auto* remBtn = new QPushButton("Remove");
    listBtns->addWidget(addBtn);
    listBtns->addWidget(remBtn);
    leftCol->addLayout(listBtns);
    split->addLayout(leftCol);

    auto* editor = new QWidget;
    auto* ev = new QVBoxLayout(editor);
    ev->setContentsMargins(12, 0, 0, 0);

    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight);

    _nameEdit = new QLineEdit;
    _nameEdit->setPlaceholderText(tr("gridserver"));
    _nameEdit->setToolTip("Shown in fitting dialogs and used inside remote "
                          "grid paths, so keep it short and unique.");
    form->addRow("Name:", _nameEdit);

    _destEdit = new QLineEdit;
    _destEdit->setPlaceholderText("ssh alias or user@host");
    form->addRow("SSH destination:", _destEdit);

    _typeCombo = new QComboBox;
    _typeCombo->addItem("Plain machine", "plain");
    _typeCombo->addItem("Slurm cluster", "slurm");
    form->addRow("Type:", _typeCombo);

    _workDirEdit = new QLineEdit;
    _workDirEdit->setPlaceholderText("$HOME/.astra");
    _workDirEdit->setToolTip(
        "Where ASTRA stages jobs and installs its worker on this host. On "
        "clusters this belongs on the work filesystem, e.g. /work/$USER/astra.");
    form->addRow("Work directory:", _workDirEdit);

    ev->addLayout(form);

    auto* useBox = new QGroupBox("Use this host for");
    auto* uv = new QVBoxLayout(useBox);
    _streamCheck = new QCheckBox("Streaming model grids to this computer");
    _fitCheck    = new QCheckBox("Running fits remotely");
    uv->addWidget(_streamCheck);
    uv->addWidget(_fitCheck);
    ev->addWidget(useBox);

    auto* gridsBox = new QGroupBox("Grid base paths on this host");
    auto* gv = new QVBoxLayout(gridsBox);
    _gridPathsEdit = new QPlainTextEdit;
    _gridPathsEdit->setPlaceholderText(
        QStringLiteral("/data/grids\n/shared/model_grids"));
    _gridPathsEdit->setToolTip("One absolute remote path per line. These are "
                               "searched for grid.fits markers.");
    _gridPathsEdit->setMaximumHeight(70);
    gv->addWidget(_gridPathsEdit);
    ev->addWidget(gridsBox);

    _slurmBox = new QGroupBox("Slurm defaults");
    auto* sf = new QFormLayout(_slurmBox);
    _partitionEdit = new QLineEdit;
    _partitionEdit->setPlaceholderText("all");
    sf->addRow("Partition:", _partitionEdit);
    _accountEdit = new QLineEdit;
    _accountEdit->setPlaceholderText("(scheduler default)");
    sf->addRow("Account:", _accountEdit);
    _timeLimitEdit = new QLineEdit;
    _timeLimitEdit->setPlaceholderText("24:00:00");
    sf->addRow("Time limit:", _timeLimitEdit);
    _cpusSpin = new QSpinBox;
    _cpusSpin->setRange(1, 1024);
    _cpusSpin->setValue(16);
    sf->addRow("CPUs per fit:", _cpusSpin);
    _memPerCpuEdit = new QLineEdit;
    _memPerCpuEdit->setPlaceholderText("(scheduler default), e.g. 4G");
    sf->addRow("Memory per CPU:", _memPerCpuEdit);
    _extraSbatchEdit = new QPlainTextEdit;
    _extraSbatchEdit->setPlaceholderText("#SBATCH --qos=...");
    _extraSbatchEdit->setMaximumHeight(50);
    sf->addRow("Extra sbatch lines:", _extraSbatchEdit);
    ev->addWidget(_slurmBox);

    auto* envBox = new QGroupBox("Shell setup before each fit (optional)");
    auto* env = new QVBoxLayout(envBox);
    _envSetupEdit = new QPlainTextEdit;
    _envSetupEdit->setPlaceholderText("module load ...");
    _envSetupEdit->setMaximumHeight(50);
    env->addWidget(_envSetupEdit);
    ev->addWidget(envBox);

    _bundleLabel = new QLabel;
    _bundleLabel->setWordWrap(true);
    ev->addWidget(_bundleLabel);

    auto* testRow = new QHBoxLayout;
    _testBtn = new QPushButton("Test connection");
    testRow->addWidget(_testBtn);
    testRow->addStretch();
    ev->addLayout(testRow);

    _testResult = new QLabel;
    _testResult->setWordWrap(true);
    _testResult->setTextInteractionFlags(Qt::TextSelectableByMouse);
    ev->addWidget(_testResult);

    ev->addStretch();
    split->addWidget(editor, 1);
    outer->addLayout(split, 1);

    // ── Local grid cache ─────────────────────────────────────────────────
    auto* cacheBox = new QGroupBox("Local cache for streamed grids");
    auto* cf = new QFormLayout(cacheBox);
    auto* dirRow = new QHBoxLayout;
    _cacheDirEdit = new QLineEdit;
    AppSettings settings;
    _cacheDirEdit->setText(settings.remoteGridCacheDir());
    _cacheDirEdit->setPlaceholderText(settings.effectiveRemoteGridCacheDir());
    auto* browse = new QPushButton("Browse...");
    dirRow->addWidget(_cacheDirEdit, 1);
    dirRow->addWidget(browse);
    cf->addRow("Directory:", dirRow);
    _cacheCapSpin = new QSpinBox;
    _cacheCapSpin->setRange(1, 10000);
    _cacheCapSpin->setSuffix(" GiB");
    _cacheCapSpin->setValue(settings.remoteGridCacheCapGb());
    _cacheCapSpin->setToolTip("Grid files fetched from remote hosts are kept "
                              "here; the oldest are dropped past this size.");
    cf->addRow("Maximum size:", _cacheCapSpin);
    outer->addWidget(cacheBox);

    connect(browse, &QPushButton::clicked, this, [this] {
        const QString d = QFileDialog::getExistingDirectory(
            this, "Grid cache directory", _cacheDirEdit->text());
        if (!d.isEmpty()) _cacheDirEdit->setText(d);
    });
    connect(addBtn, &QPushButton::clicked, this,
            &RemoteHostsSettingsPage::onAddHost);
    connect(remBtn, &QPushButton::clicked, this,
            &RemoteHostsSettingsPage::onRemoveHost);
    connect(_list, &QListWidget::currentRowChanged, this,
            [this](int row) { loadEditor(row); });
    connect(_testBtn, &QPushButton::clicked, this,
            &RemoteHostsSettingsPage::onTestConnection);
    connect(_typeCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        _slurmBox->setVisible(
            _typeCombo->currentData().toString() == QLatin1String("slurm"));
    });
    // The name doubles as the key inside remote grid paths, so keep the list
    // label in step while it is being typed.
    connect(_nameEdit, &QLineEdit::textChanged, this, [this](const QString& t) {
        if (_editing >= 0 && _editing < _list->count())
            _list->item(_editing)->setText(t);
    });

    reloadList(_hosts.isEmpty() ? -1 : 0);
}

void RemoteHostsSettingsPage::reloadList(int selectRow)
{
    const QSignalBlocker block(_list);
    _list->clear();
    for (const auto& h : _hosts) _list->addItem(h.name);
    _editing = -1;
    if (selectRow >= 0 && selectRow < _list->count()) {
        _list->setCurrentRow(selectRow);
        loadEditor(selectRow);
    } else {
        setEditorEnabled(false);
    }
}

void RemoteHostsSettingsPage::setEditorEnabled(bool on)
{
    for (QWidget* w : {static_cast<QWidget*>(_nameEdit),
                       static_cast<QWidget*>(_destEdit),
                       static_cast<QWidget*>(_typeCombo),
                       static_cast<QWidget*>(_workDirEdit),
                       static_cast<QWidget*>(_gridPathsEdit),
                       static_cast<QWidget*>(_streamCheck),
                       static_cast<QWidget*>(_fitCheck),
                       static_cast<QWidget*>(_slurmBox),
                       static_cast<QWidget*>(_envSetupEdit),
                       static_cast<QWidget*>(_testBtn)})
        w->setEnabled(on);
    if (!on) {
        _testResult->clear();
        _bundleLabel->clear();
    }
}

void RemoteHostsSettingsPage::loadEditor(int row)
{
    storeEditor();
    if (row < 0 || row >= _hosts.size()) {
        _editing = -1;
        setEditorEnabled(false);
        return;
    }
    const RemoteHost& h = _hosts[row];
    _nameEdit->setText(h.name);
    _destEdit->setText(h.destination);
    _typeCombo->setCurrentIndex(
        _typeCombo->findData(RemoteHost::typeName(h.type)));
    _workDirEdit->setText(h.workDir);
    _gridPathsEdit->setPlainText(h.gridBasePaths.join(QLatin1Char('\n')));
    _streamCheck->setChecked(h.useGridsForStreaming);
    _fitCheck->setChecked(h.useForFitting);
    _partitionEdit->setText(h.slurm.partition);
    _accountEdit->setText(h.slurm.account);
    _timeLimitEdit->setText(h.slurm.timeLimit);
    _cpusSpin->setValue(h.slurm.cpusPerTask);
    _memPerCpuEdit->setText(h.slurm.memPerCpu);
    _extraSbatchEdit->setPlainText(h.slurm.extraSbatchLines);
    _envSetupEdit->setPlainText(h.envSetup);
    _slurmBox->setVisible(h.type == RemoteHost::Type::Slurm);
    _bundleLabel->setText(
        h.installedBundleVersion.isEmpty()
            ? QStringLiteral("Worker: not installed yet. Remote fitting "
                             "installs it on first use.")
            : QStringLiteral("Worker installed: %1")
                  .arg(h.installedBundleVersion));
    _testResult->clear();
    setEditorEnabled(true);
    _editing = row;
}

void RemoteHostsSettingsPage::storeEditor()
{
    if (_editing < 0 || _editing >= _hosts.size()) return;
    RemoteHost& h = _hosts[_editing];
    h.name        = _nameEdit->text().trimmed();
    h.destination = _destEdit->text().trimmed();
    h.type        = RemoteHost::typeFromName(
        _typeCombo->currentData().toString());
    h.workDir     = _workDirEdit->text().trimmed();
    h.gridBasePaths.clear();
    for (const QString& line :
         _gridPathsEdit->toPlainText().split(QLatin1Char('\n'),
                                             Qt::SkipEmptyParts)) {
        const QString p = line.trimmed();
        if (!p.isEmpty()) h.gridBasePaths << p;
    }
    h.useGridsForStreaming = _streamCheck->isChecked();
    h.useForFitting        = _fitCheck->isChecked();
    h.slurm.partition        = _partitionEdit->text().trimmed();
    h.slurm.account          = _accountEdit->text().trimmed();
    h.slurm.timeLimit        = _timeLimitEdit->text().trimmed();
    h.slurm.cpusPerTask      = _cpusSpin->value();
    h.slurm.memPerCpu        = _memPerCpuEdit->text().trimmed();
    h.slurm.extraSbatchLines = _extraSbatchEdit->toPlainText().trimmed();
    h.envSetup               = _envSetupEdit->toPlainText().trimmed();
}

void RemoteHostsSettingsPage::onAddHost()
{
    storeEditor();
    RemoteHost h;
    h.id   = QUuid::createUuid().toString(QUuid::WithoutBraces);
    h.name = QStringLiteral("new host");
    _hosts.push_back(h);
    reloadList(_hosts.size() - 1);
    _nameEdit->setFocus();
    _nameEdit->selectAll();
}

void RemoteHostsSettingsPage::onRemoveHost()
{
    const int row = _list->currentRow();
    if (row < 0 || row >= _hosts.size()) return;
    _editing = -1;                       // do not write back the removed host
    _hosts.remove(row);
    reloadList(qMin(row, _hosts.size() - 1));
}

void RemoteHostsSettingsPage::onTestConnection()
{
    storeEditor();
    if (_editing < 0) return;
    const RemoteHost host = _hosts[_editing];
    if (host.destination.isEmpty()) {
        _testResult->setText("Set an SSH destination first.");
        return;
    }

    _testBtn->setEnabled(false);
    _testResult->setText(QStringLiteral("Connecting to %1 ...")
                             .arg(host.destination));

    // probeHost() blocks (it may prompt for credentials), so it runs off the
    // GUI thread; the watcher brings the result back.
    auto* watcher = new QFutureWatcher<RemoteHostRegistry::ProbeResult>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
        const auto r = watcher->result();
        _testResult->setText(r.summary());
        _testBtn->setEnabled(true);
        // Adopt what the probe learned: it is more reliable than a guess.
        if (r.reachable && _editing >= 0) {
            if (r.hasSlurm &&
                _typeCombo->currentData().toString() == QLatin1String("plain"))
                _testResult->setText(
                    r.summary() +
                    QStringLiteral("\nThis host runs Slurm; consider setting "
                                   "Type to \"Slurm cluster\"."));
        }
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([host] {
        return RemoteHostRegistry::instance().probeHost(host);
    }));
}

void RemoteHostsSettingsPage::apply()
{
    storeEditor();

    // Names key the ssh:// grid paths, so blank or duplicate ones would make
    // a grid unresolvable; fix them up rather than silently saving them.
    QSet<QString> seen;
    for (int i = 0; i < _hosts.size(); ++i) {
        RemoteHost& h = _hosts[i];
        if (h.name.trimmed().isEmpty())
            h.name = QStringLiteral("host%1").arg(i + 1);
        h.name.replace(QLatin1Char('/'), QLatin1Char('_'));
        h.name.replace(QLatin1Char(' '), QLatin1Char('_'));
        QString base = h.name;
        int n = 2;
        while (seen.contains(h.name))
            h.name = QStringLiteral("%1_%2").arg(base).arg(n++);
        seen.insert(h.name);
    }

    RemoteHostRegistry::instance().setHosts(_hosts);

    AppSettings settings;
    settings.setRemoteGridCacheDir(_cacheDirEdit->text().trimmed());
    settings.setRemoteGridCacheCapGb(_cacheCapSpin->value());

    reloadList(_list->currentRow());
}

} // namespace astra::remote
