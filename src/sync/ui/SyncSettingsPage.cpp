#include "sync/ui/SyncSettingsPage.h"

#include "remote/RemoteHostRegistry.h"
#include "sync/BlobIndex.h"
#include "sync/SyncSchema.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace astra::sync::ui {

namespace {

QString peerStateText(const PeerStatus& p)
{
    switch (p.state) {
    case PeerStatus::Idle:
        return p.detail.isEmpty() ? QStringLiteral("Paired") : p.detail;
    case PeerStatus::Connecting:
        return QStringLiteral("Connecting");
    case PeerStatus::WaitingForApproval:
        return QStringLiteral("Confirm the code %1 on that machine").arg(spacedCode(p.detail));
    case PeerStatus::NeedsChoice:
        return QStringLiteral("The databases differ: choose whose rows are kept");
    case PeerStatus::Syncing:
        return QStringLiteral("Syncing");
    case PeerStatus::InSync:
        return p.lastSync.isValid()
                   ? QStringLiteral("In sync since %1").arg(p.lastSync.toString(QStringLiteral("HH:mm")))
                   : QStringLiteral("In sync");
    case PeerStatus::Offline:
        return QStringLiteral("Not reachable: %1").arg(p.detail);
    case PeerStatus::Paused:
        return QStringLiteral("Paused");
    }
    return {};
}

QLabel* note(const QString& text)
{
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

}   // namespace

QString spacedCode(const QString& code)
{
    return code.size() == 6 ? code.left(3) + QLatin1Char('-') + code.mid(3) : code;
}

SyncSettingsPage::SyncSettingsPage(SyncService* service, QWidget* parent)
    : QWidget(parent), _service(service)
{
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->addWidget(scroll);

    auto* content = new QWidget;
    scroll->setWidget(content);
    auto* outer = new QVBoxLayout(content);
    outer->setContentsMargins(16, 16, 16, 16);
    outer->setSpacing(12);

    outer->addWidget(note(QStringLiteral(
        "Keeps this database and its data files the same on your machines. Changes are "
        "merged row by row as they happen, also when both machines were used while apart. "
        "The data folder must not be synced by another tool (Syncthing, a cloud folder) at "
        "the same time.")));

    // ── this database ───────────────────────────────────────────────────
    auto* self = new QGroupBox(QStringLiteral("This machine"));
    auto* selfLayout = new QFormLayout(self);
    _state = note(QString());
    _switch = new QPushButton;
    auto* stateRow = new QHBoxLayout;
    stateRow->addWidget(_state, 1);
    stateRow->addWidget(_switch);
    selfLayout->addRow(stateRow);
    _name = new QLineEdit;
    _name->setToolTip(QStringLiteral("What this machine is called on your other devices."));
    selfLayout->addRow(QStringLiteral("Name:"), _name);
    outer->addWidget(self);

    connect(_switch, &QPushButton::clicked, this, [this] {
        if (!_status.enabled) {
            _service->enable();
            return;
        }
        if (QMessageBox::question(
                this, QStringLiteral("Switch sync off"),
                QStringLiteral("This removes sync from this database: the list of devices and "
                               "everything sync knows about what changed where. The data itself "
                               "stays. To sync again later, the devices have to be paired anew.\n\n"
                               "Switch sync off?"),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
            _service->disable();
    });
    connect(_name, &QLineEdit::editingFinished, this, [this] {
        if (_name->text().trimmed() != _status.deviceName && !_name->text().trimmed().isEmpty())
            _service->setDeviceName(_name->text());
    });

    // ── devices ─────────────────────────────────────────────────────────
    _devicesBox = new QGroupBox(QStringLiteral("Devices"));
    auto* devLayout = new QVBoxLayout(_devicesBox);
    _devices = new QTreeWidget;
    _devices->setHeaderLabels({QStringLiteral("Device"), QStringLiteral("Reached through"),
                               QStringLiteral("State")});
    _devices->setRootIsDecorated(false);
    _devices->setUniformRowHeights(true);
    _devices->setTextElideMode(Qt::ElideMiddle);
    _devices->header()->setStretchLastSection(true);
    _devices->setMinimumHeight(110);
    devLayout->addWidget(_devices);
    auto* devButtons = new QHBoxLayout;
    _add     = new QPushButton(QStringLiteral("Add device..."));
    _remove  = new QPushButton(QStringLiteral("Remove"));
    _pause   = new QPushButton(QStringLiteral("Pause"));
    _syncNow = new QPushButton(QStringLiteral("Sync now"));
    devButtons->addWidget(_add);
    devButtons->addWidget(_remove);
    devButtons->addWidget(_pause);
    devButtons->addStretch();
    devButtons->addWidget(_syncNow);
    devLayout->addLayout(devButtons);
    devLayout->addWidget(note(QStringLiteral(
        "Add a machine this one can reach over SSH. ASTRA has to be installed there. "
        "The first time, that machine shows a code to confirm, here in its own settings or "
        "with \"ASTRA --sync-allow\" on its command line.")));
    outer->addWidget(_devicesBox);

    connect(_add, &QPushButton::clicked, this, &SyncSettingsPage::addDevice);
    connect(_remove, &QPushButton::clicked, this, &SyncSettingsPage::removeDevice);
    connect(_pause, &QPushButton::clicked, this, &SyncSettingsPage::togglePause);
    connect(_syncNow, &QPushButton::clicked, _service, &SyncService::syncNow);
    connect(_devices, &QTreeWidget::itemSelectionChanged, this, [this] { showStatus(_status); });
    connect(_devices, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item) {
        // A device waiting for the pairing choice: ask it here.
        for (const PeerStatus& p : _status.peers) {
            if (p.id != item->data(0, Qt::UserRole).toString() || p.state != PeerStatus::NeedsChoice)
                continue;
            bool keepMine = true;
            if (askPairingChoice(this, p.name, p.detail, &keepMine))
                _service->resolvePairing(p.id, keepMine);
        }
    });

    // ── requests ────────────────────────────────────────────────────────
    _requestsBox = new QGroupBox(QStringLiteral("Devices asking to sync with this database"));
    auto* reqLayout = new QVBoxLayout(_requestsBox);
    _requests = new QTreeWidget;
    _requests->setHeaderLabels({QStringLiteral("Device"), QStringLiteral("Code")});
    _requests->setRootIsDecorated(false);
    _requests->setMaximumHeight(90);
    reqLayout->addWidget(_requests);
    auto* reqButtons = new QHBoxLayout;
    _allow  = new QPushButton(QStringLiteral("Allow"));
    _refuse = new QPushButton(QStringLiteral("Refuse"));
    reqButtons->addWidget(note(QStringLiteral(
        "Allow only when the code is the one shown on the machine that asks.")), 1);
    reqButtons->addWidget(_allow);
    reqButtons->addWidget(_refuse);
    reqLayout->addLayout(reqButtons);
    outer->addWidget(_requestsBox);

    auto requestCode = [this] {
        const auto* item = _requests->currentItem() ? _requests->currentItem()
                                                     : _requests->topLevelItem(0);
        return item ? item->data(0, Qt::UserRole).toString() : QString();
    };
    connect(_allow, &QPushButton::clicked, this, [this, requestCode] {
        if (const QString code = requestCode(); !code.isEmpty()) _service->approveRequest(code);
    });
    connect(_refuse, &QPushButton::clicked, this, [this, requestCode] {
        if (const QString code = requestCode(); !code.isEmpty()) _service->rejectRequest(code);
    });

    // ── history ─────────────────────────────────────────────────────────
    auto* historyBox = new QGroupBox(QStringLiteral("Replaced and deleted data"));
    auto* histLayout = new QHBoxLayout(historyBox);
    _historyLabel = note(QString());
    _history = new QPushButton(QStringLiteral("Show..."));
    histLayout->addWidget(_historyLabel, 1);
    histLayout->addWidget(_history);
    outer->addWidget(historyBox);
    connect(_history, &QPushButton::clicked, this, [this] {
        SyncHistoryDialog dialog(_service, this);
        dialog.exec();
    });

    outer->addStretch();

    connect(_service, &SyncService::statusChanged, this, &SyncSettingsPage::showStatus);
    connect(_service, &SyncService::message, this, [this](const QString& text) {
        if (isVisible()) QMessageBox::information(this, QStringLiteral("Sync"), text);
    });
    showStatus(_service->status());
}

QString SyncSettingsPage::selectedPeer() const
{
    const auto* item = _devices->currentItem();
    return item ? item->data(0, Qt::UserRole).toString() : QString();
}

void SyncSettingsPage::showStatus(const SyncStatus& status)
{
    _status = status;

    if (!status.enabled) {
        _state->setText(status.blocked.isEmpty()
                            ? QStringLiteral("Sync is off for this database.")
                            : status.blocked + QStringLiteral(" Remove it from there before "
                                                              "switching sync on here."));
        _switch->setText(QStringLiteral("Switch on"));
        _switch->setEnabled(status.blocked.isEmpty());
    } else {
        QString text = status.summary() + QLatin1Char('.');
        if (status.restored)
            text = QStringLiteral("This database was restored from a backup. Nothing is "
                                  "exchanged until you chose how to continue.");
        else if (!status.blocked.isEmpty())
            text += QLatin1Char(' ') + status.blocked + QStringLiteral(" Two tools moving the "
                                                                       "same database undo each other.");
        _state->setText(text);
        _switch->setText(QStringLiteral("Switch off..."));
        _switch->setEnabled(true);
    }
    if (!_name->hasFocus()) _name->setText(status.deviceName);
    _name->setEnabled(true);

    // Devices, keeping the selection.
    const QString selected = selectedPeer();
    _devices->clear();
    for (const PeerStatus& p : status.peers) {
        auto* item = new QTreeWidgetItem(_devices);
        item->setText(0, p.name.isEmpty() ? p.destination : p.name);
        item->setText(1, p.destination.isEmpty() ? QStringLiteral("it connects here") : p.destination);
        item->setToolTip(1, p.destination);
        item->setText(2, peerStateText(p));
        item->setToolTip(2, peerStateText(p));
        item->setData(0, Qt::UserRole, p.id);
        item->setData(1, Qt::UserRole, p.state == PeerStatus::Paused);
        if (p.id == selected) _devices->setCurrentItem(item);
    }
    // The state is what matters; a long address must not push it out of view.
    for (int c = 0; c < 2; ++c) {
        _devices->resizeColumnToContents(c);
        _devices->setColumnWidth(c, qMin(_devices->columnWidth(c) + 12, 220));
    }
    _devicesBox->setEnabled(status.enabled);
    const bool hasSelection = _devices->currentItem() != nullptr;
    _remove->setEnabled(hasSelection);
    _pause->setEnabled(hasSelection);
    _pause->setText(hasSelection && _devices->currentItem()->data(1, Qt::UserRole).toBool()
                        ? QStringLiteral("Resume") : QStringLiteral("Pause"));
    _syncNow->setEnabled(!status.peers.isEmpty());

    _requests->clear();
    for (const PairingRequest& r : status.requests) {
        auto* item = new QTreeWidgetItem(_requests);
        item->setText(0, r.name.isEmpty() ? r.device : r.name);
        item->setText(1, spacedCode(r.code));
        item->setData(0, Qt::UserRole, r.code);
    }
    _requestsBox->setVisible(!status.requests.isEmpty());

    _historyLabel->setText(
        status.conflicts > 0
            ? QStringLiteral("%1 values were changed on two machines at once; the later change "
                             "was kept and the other one is here, for 30 days.").arg(status.conflicts)
            : QStringLiteral("What a change from another machine replaced or deleted here is "
                             "kept for 30 days and can be put back."));
    _history->setEnabled(status.enabled);
}

void SyncSettingsPage::addDevice()
{
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Add device"));
    dialog.setProperty("astra.syncSafe", true);
    auto* layout = new QVBoxLayout(&dialog);
    layout->addWidget(note(QStringLiteral(
        "The other machine as you would give it to ssh: an alias from your ssh "
        "configuration, or user@host.")));
    auto* form = new QFormLayout;
    auto* host = new QComboBox;
    host->setEditable(true);
    host->setMinimumWidth(320);
    for (const auto& h : astra::remote::RemoteHostRegistry::instance().hosts())
        if (!h.destination.isEmpty() && host->findText(h.destination) < 0)
            host->addItem(h.destination);
    host->setCurrentText(QString());
    auto* rootEdit = new QLineEdit;
    rootEdit->setPlaceholderText(QStringLiteral("default: where ASTRA keeps its data there"));
    auto* command = new QLineEdit;
    command->setPlaceholderText(QStringLiteral("default: ASTRA"));
    form->addRow(QStringLiteral("Machine:"), host);
    form->addRow(QStringLiteral("Data folder there:"), rootEdit);
    form->addRow(QStringLiteral("ASTRA command there:"), command);
    layout->addLayout(form);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) return;
    const QString destination = host->currentText().trimmed();
    if (destination.isEmpty()) return;
    _service->addPeer(destination, rootEdit->text(), command->text());
}

void SyncSettingsPage::removeDevice()
{
    const QString id = selectedPeer();
    if (id.isEmpty()) return;
    if (QMessageBox::question(
            this, QStringLiteral("Remove device"),
            QStringLiteral("Stop syncing with %1? The data on both machines stays as it is.")
                .arg(_devices->currentItem()->text(0)),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes)
        _service->removePeer(id);
}

void SyncSettingsPage::togglePause()
{
    const QString id = selectedPeer();
    if (id.isEmpty()) return;
    const bool paused = _devices->currentItem()->data(1, Qt::UserRole).toBool();
    _service->setPeerEnabled(id, paused);
}

// ── history ─────────────────────────────────────────────────────────────────

SyncHistoryDialog::SyncHistoryDialog(SyncService* service, QWidget* parent)
    : QDialog(parent), _service(service)
{
    setWindowTitle(QStringLiteral("Replaced and deleted data"));
    setProperty("astra.syncSafe", true);
    resize(860, 460);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(note(QStringLiteral(
        "What changes from your other machines replaced or deleted on this one. Entries "
        "marked as conflict were changed on both machines before they had synced; the later "
        "change was kept. Putting an entry back makes it the current value on all machines.")));
    _list = new QTreeWidget;
    _list->setHeaderLabels({QStringLiteral("When"), QStringLiteral("What"), QStringLiteral("Row"),
                            QStringLiteral("Changed by"), QStringLiteral("Was")});
    _list->setRootIsDecorated(false);
    _list->setUniformRowHeights(true);
    layout->addWidget(_list, 1);
    auto* buttons = new QHBoxLayout;
    _restore = new QPushButton(QStringLiteral("Put back"));
    _restore->setEnabled(false);
    auto* close = new QPushButton(QStringLiteral("Close"));
    buttons->addWidget(_restore);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);

    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    connect(_list, &QTreeWidget::itemSelectionChanged, this,
            [this] { _restore->setEnabled(_list->currentItem() != nullptr); });
    connect(_restore, &QPushButton::clicked, this, [this] {
        if (const auto* item = _list->currentItem())
            _service->restoreFromHistory(item->data(0, Qt::UserRole).toLongLong());
    });
    connect(_service, &SyncService::historyLoaded, this, &SyncHistoryDialog::fill);
    connect(_service, &SyncService::message, this, [this](const QString& text) {
        QMessageBox::information(this, QStringLiteral("Sync"), text);
    });
    _service->loadHistory();
}

void SyncHistoryDialog::fill(const QList<HistoryEntry>& entries)
{
    _list->clear();
    for (const HistoryEntry& e : entries) {
        auto* item = new QTreeWidgetItem(_list);
        item->setText(0, e.at.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
        QString what = e.cell == kDeletedCell ? QStringLiteral("deleted")
                     : e.cell == kRowCell ? QStringLiteral("whole row replaced")
                     : e.cell.startsWith(kBlobCellPrefix) ? QStringLiteral("data file replaced")
                     : e.cell;
        if (e.conflict) what += QStringLiteral(" (conflict)");
        item->setText(1, e.table + QStringLiteral(": ") + what);
        item->setText(2, e.pk);
        item->setText(3, e.byDevice);
        item->setText(4, e.preview);
        item->setToolTip(4, e.preview);
        item->setData(0, Qt::UserRole, e.id);
    }
    for (int c = 0; c < 4; ++c) _list->resizeColumnToContents(c);
    _restore->setEnabled(false);
}

// ── questions ───────────────────────────────────────────────────────────────

bool askPairingChoice(QWidget* parent, const QString& deviceName, const QString& summary,
                      bool* keepMine)
{
    QMessageBox box(parent);
    box.setProperty("astra.syncSafe", true);
    box.setWindowTitle(QStringLiteral("First sync with %1").arg(deviceName));
    box.setIcon(QMessageBox::Question);
    box.setText(QStringLiteral("This database and the one on %1 are not the same.").arg(deviceName));
    box.setInformativeText(QStringLiteral(
        "Rows that only one of them has are copied to the other; nothing is deleted. For "
        "rows that exist on both but differ, one side has to win. The other side's version "
        "of those rows is kept for 30 days under Settings > Sync.\n\n%1").arg(summary));
    auto* mine   = box.addButton(QStringLiteral("Keep this machine's"), QMessageBox::AcceptRole);
    auto* theirs = box.addButton(QStringLiteral("Keep %1's").arg(deviceName), QMessageBox::AcceptRole);
    box.addButton(QStringLiteral("Decide later"), QMessageBox::RejectRole);
    box.exec();
    const QAbstractButton* clicked = box.clickedButton();
    if (clicked == mine) { *keepMine = true; return true; }
    if (clicked == theirs) { *keepMine = false; return true; }
    return false;
}

bool askRestoreChoice(QWidget* parent, bool* catchUp)
{
    QMessageBox box(parent);
    box.setProperty("astra.syncSafe", true);
    box.setWindowTitle(QStringLiteral("Restored database"));
    box.setIcon(QMessageBox::Question);
    box.setText(QStringLiteral("This database is older than what this machine already synced: "
                               "it was restored from a backup."));
    box.setInformativeText(QStringLiteral(
        "Your other machines still have the changes made since that backup.\n\n"
        "Catch up: take those changes again. This undoes the restore for everything that "
        "had been synced.\n\n"
        "Keep this state: your other machines become copies of this database as it is now; "
        "what they have beyond it is removed there (and kept for 30 days in their history)."));
    auto* up   = box.addButton(QStringLiteral("Catch up"), QMessageBox::AcceptRole);
    auto* keep = box.addButton(QStringLiteral("Keep this state"), QMessageBox::DestructiveRole);
    box.addButton(QStringLiteral("Decide later"), QMessageBox::RejectRole);
    box.exec();
    const QAbstractButton* clicked = box.clickedButton();
    if (clicked == up) { *catchUp = true; return true; }
    if (clicked == keep) { *catchUp = false; return true; }
    return false;
}

}   // namespace astra::sync::ui
