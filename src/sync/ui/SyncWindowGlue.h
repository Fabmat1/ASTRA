#pragma once

// What ties sync to the main window:
//   - the entry in the status bar ("In sync", "Syncing, 12 files to fetch",
//     ...) that opens Settings > Sync;
//   - the report of what is open, which is what holds incoming changes back:
//     a star whose detail window is up keeps its rows as they are, and any
//     other tool window or running background work holds everything;
//   - the questions sync cannot answer itself: a device asking to pair, whose
//     rows win at a first pairing, what a restored backup means.

#include "sync/SyncService.h"

#include <QObject>
#include <QSet>

class ApplicationController;
class QMainWindow;
class QTimer;
class QToolButton;

namespace astra::sync::ui {

class SyncWindowGlue : public QObject {
    Q_OBJECT
public:
    /// `openSettings` opens the settings dialog on the given page.
    SyncWindowGlue(QMainWindow* window, ApplicationController* controller,
                   std::function<void(const QString& page)> openSettings);

    /// Called when the window is about to close: false to keep it open.
    bool confirmClose();

private:
    void showStatus(const SyncStatus& status);
    void reportHeld();
    void askWhatIsNeeded(const SyncStatus& status);

    QMainWindow*           _window;
    ApplicationController* _controller;
    SyncService*           _service;
    std::function<void(const QString&)> _openSettings;
    QToolButton*           _button = nullptr;
    QTimer*                _heldTimer = nullptr;
    QSet<QString>          _asked;          ///< questions already put, by key
    bool                   _asking = false;
};

}   // namespace astra::sync::ui
