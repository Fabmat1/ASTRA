#include "sync/ui/SyncWindowGlue.h"

#include "app/ui/ApplicationController.h"
#include "app/ui/BackgroundTaskManager.h"
#include "sync/ui/SyncSettingsPage.h"

#include <QApplication>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QToolButton>

namespace astra::sync::ui {

SyncWindowGlue::SyncWindowGlue(QMainWindow* window, ApplicationController* controller,
                               std::function<void(const QString&)> openSettings)
    : QObject(window),
      _window(window),
      _controller(controller),
      _service(controller->syncService()),
      _openSettings(std::move(openSettings))
{
    if (!_service) return;

    _button = new QToolButton(window);
    _button->setAutoRaise(true);
    _button->setFocusPolicy(Qt::NoFocus);
    window->statusBar()->addPermanentWidget(_button);
    connect(_button, &QToolButton::clicked, this, [this] { _openSettings(QStringLiteral("Sync")); });

    connect(_service, &SyncService::statusChanged, this, &SyncWindowGlue::showStatus);
    connect(_service, &SyncService::message, this, [this](const QString& text) {
        _window->statusBar()->showMessage(text, 15000);
    });

    // What is open changes with every window; twice a second is often enough
    // for a rule whose point is "not while you are looking at it".
    _heldTimer = new QTimer(this);
    _heldTimer->setInterval(500);
    connect(_heldTimer, &QTimer::timeout, this, &SyncWindowGlue::reportHeld);
    _heldTimer->start();
    reportHeld();

    showStatus(_service->status());
}

void SyncWindowGlue::reportHeld()
{
    QSet<QString> stars;
    bool everything = false;

    for (QWidget* w : QApplication::topLevelWidgets()) {
        if (!w->isVisible() || w == _window) continue;
        // Menus, tool tips and the like are windows too, but hold no data.
        const Qt::WindowType type = w->windowType();
        if (type != Qt::Window && type != Qt::Dialog && type != Qt::Tool) continue;
        if (w->property("astra.syncSafe").toBool()) continue;

        // A star's own window holds that star. Anything else that is open
        // (a fit dialog, an import wizard, the plot tool) may hold copies of
        // any rows, so nothing changes underneath it.
        const QString star = w->property("astra.starId").toString();
        if (!star.isEmpty() && !w->inherits("QDialog")) stars.insert(star);
        else everything = true;
    }
    if (_controller->backgroundTaskManager() && _controller->backgroundTaskManager()->hasActiveTasks())
        everything = true;
    if (_controller->massFitActive()) everything = true;

    _service->setHeld(stars, everything);
}

void SyncWindowGlue::showStatus(const SyncStatus& status)
{
    if (!_button) return;
    // Nothing in the status bar for people who do not use sync.
    _button->setVisible(status.enabled || !status.requests.isEmpty());
    _button->setText(status.summary());

    QStringList tip;
    for (const PeerStatus& p : status.peers) {
        QString line = p.name.isEmpty() ? p.destination : p.name;
        switch (p.state) {
        case PeerStatus::InSync:  line += QStringLiteral(": in sync"); break;
        case PeerStatus::Syncing: line += QStringLiteral(": syncing"); break;
        case PeerStatus::Offline: line += QStringLiteral(": not reachable (%1)").arg(p.detail); break;
        case PeerStatus::Paused:  line += QStringLiteral(": paused"); break;
        case PeerStatus::WaitingForApproval:
            line += QStringLiteral(": confirm the code %1 there").arg(spacedCode(p.detail));
            break;
        case PeerStatus::NeedsChoice: line += QStringLiteral(": needs your choice"); break;
        default: break;
        }
        tip << line;
    }
    if (status.waitingToApply > 0)
        tip << QStringLiteral("Changes from another machine are waiting until the windows they "
                              "concern are closed.");
    if (status.conflicts > 0)
        tip << QStringLiteral("%1 values were changed on two machines at once; see Settings > Sync.")
                   .arg(status.conflicts);
    tip << QStringLiteral("Click to open the sync settings.");
    _button->setToolTip(tip.join(QLatin1Char('\n')));

    // Asked from the event loop, not from inside this signal.
    if (status.needsAttention() && !_asking)
        QTimer::singleShot(0, this, [this] { askWhatIsNeeded(_service->status()); });
}

void SyncWindowGlue::askWhatIsNeeded(const SyncStatus& status)
{
    if (_asking) return;
    _asking = true;

    if (status.restored && !_asked.contains(QStringLiteral("restored"))) {
        _asked.insert(QStringLiteral("restored"));
        bool catchUp = true;
        if (askRestoreChoice(_window, &catchUp)) {
            _service->decideRestore(catchUp);
            _asked.remove(QStringLiteral("restored"));     // a later restore asks again
        }
    }

    for (const PairingRequest& r : status.requests) {
        const QString key = QStringLiteral("request:") + r.code;
        if (_asked.contains(key)) continue;
        _asked.insert(key);
        QMessageBox box(_window);
        box.setProperty("astra.syncSafe", true);
        box.setWindowTitle(QStringLiteral("Sync"));
        box.setIcon(QMessageBox::Question);
        box.setText(QStringLiteral("%1 asks to sync with this database.")
                        .arg(r.name.isEmpty() ? QStringLiteral("Another machine") : r.name));
        box.setInformativeText(QStringLiteral(
            "Allow it only if you started this on that machine, and it shows the code\n\n"
            "        %1\n\nOnce allowed, the two databases are kept the same.").arg(spacedCode(r.code)));
        auto* allow  = box.addButton(QStringLiteral("Allow"), QMessageBox::AcceptRole);
        auto* refuse = box.addButton(QStringLiteral("Refuse"), QMessageBox::DestructiveRole);
        box.addButton(QStringLiteral("Later"), QMessageBox::RejectRole);
        box.exec();
        const QAbstractButton* clicked = box.clickedButton();
        if (clicked == allow) _service->approveRequest(r.code);
        else if (clicked == refuse) _service->rejectRequest(r.code);
    }

    for (const PeerStatus& p : status.peers) {
        if (p.state != PeerStatus::NeedsChoice) continue;
        const QString key = QStringLiteral("choice:") + p.id;
        if (_asked.contains(key)) continue;
        _asked.insert(key);
        bool keepMine = true;
        if (askPairingChoice(_window, p.name, p.detail, &keepMine))
            _service->resolvePairing(p.id, keepMine);
    }

    _asking = false;
}

bool SyncWindowGlue::confirmClose()
{
    if (!_service) return true;
    const SyncStatus status = _service->status();
    if (!status.enabled || status.unsent == 0) return true;

    QStringList reachable;
    for (const PeerStatus& p : status.peers)
        if (p.state == PeerStatus::Syncing || p.state == PeerStatus::InSync) reachable << p.name;
    if (reachable.isEmpty()) return true;       // nobody to send to; it goes next time

    return QMessageBox::question(
               _window, QStringLiteral("Sync"),
               QStringLiteral("Changes made here are still on their way to %1.\n\n"
                              "Quitting now is safe: they are sent the next time ASTRA runs. "
                              "But until then the other machine does not have them. Quit now?")
                   .arg(reachable.join(QStringLiteral(", "))),
               QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}

}   // namespace astra::sync::ui
