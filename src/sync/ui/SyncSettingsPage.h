#pragma once

// Settings > Sync: switch sync on for this database, name this machine, add
// and remove the devices it syncs with, answer devices that ask to pair, and
// look at what incoming changes replaced.
//
// Nothing here waits for OK: every control acts at once through the
// SyncService, and the page shows the service's status as it changes.

#include "sync/SyncService.h"

#include <QDialog>
#include <QWidget>

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QGroupBox;

namespace astra::sync::ui {

class SyncSettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SyncSettingsPage(SyncService* service, QWidget* parent = nullptr);

private:
    void showStatus(const SyncStatus& status);
    void addDevice();
    void removeDevice();
    void togglePause();
    QString selectedPeer() const;

    SyncService* _service;
    SyncStatus   _status;

    QLabel*      _state        = nullptr;
    QPushButton* _switch       = nullptr;
    QLineEdit*   _name         = nullptr;
    QGroupBox*   _devicesBox   = nullptr;
    QTreeWidget* _devices      = nullptr;
    QPushButton* _add          = nullptr;
    QPushButton* _remove       = nullptr;
    QPushButton* _pause        = nullptr;
    QPushButton* _syncNow      = nullptr;
    QGroupBox*   _requestsBox  = nullptr;
    QTreeWidget* _requests     = nullptr;
    QPushButton* _allow        = nullptr;
    QPushButton* _refuse       = nullptr;
    QLabel*      _historyLabel = nullptr;
    QPushButton* _history      = nullptr;
};

/// What incoming changes replaced or deleted here, with a way to put it back.
class SyncHistoryDialog : public QDialog {
    Q_OBJECT
public:
    explicit SyncHistoryDialog(SyncService* service, QWidget* parent = nullptr);

private:
    void fill(const QList<HistoryEntry>& entries);

    SyncService* _service;
    QTreeWidget* _list    = nullptr;
    QPushButton* _restore = nullptr;
};

/// The one question of a first pairing: the databases differ, whose rows
/// are kept. Returns true when the user chose; `keepMine` holds the choice.
bool askPairingChoice(QWidget* parent, const QString& deviceName, const QString& summary,
                      bool* keepMine);

/// A backup was restored on this machine. Returns true when the user chose.
bool askRestoreChoice(QWidget* parent, bool* catchUp);

/// "123-456".
QString spacedCode(const QString& code);

}   // namespace astra::sync::ui
