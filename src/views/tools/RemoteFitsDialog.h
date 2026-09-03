#pragma once

#include "remote/RemoteFitService.h"

#include <QDialog>

class QLabel;
class QPushButton;
class QTableWidget;
class QTimer;

namespace astra::remote {

/*  The fits currently running on other machines.
 *
 *  A remote job outlives the window that started it, and an ASTRA restart:
 *  runs adopted from an earlier session show up here alongside the ones this
 *  session started, and both can be stopped from here.  Stopping asks the
 *  worker to stop rather than killing it, so whatever it already produced is
 *  written out and collected.                                               */
class RemoteFitsDialog : public QDialog {
    Q_OBJECT
public:
    explicit RemoteFitsDialog(RemoteFitService* service,
                              QWidget* parent = nullptr);

private:
    void refresh();
    void onStop();
    void onForget();
    QString selectedRunId() const;

    RemoteFitService* _service;
    QTableWidget*     _table = nullptr;
    QLabel*           _summary = nullptr;
    QPushButton*      _stopBtn = nullptr;
    QPushButton*      _forgetBtn = nullptr;
    QTimer*           _ticker = nullptr;
};

} // namespace astra::remote
