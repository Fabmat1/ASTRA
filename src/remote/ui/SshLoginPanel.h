#pragma once

#include <QHash>
#include <QStringList>
#include <QWidget>

class QVBoxLayout;

namespace astra::remote {

/*  Inline SSH login for remote hosts, embedded wherever remote work needs a
 *  logged-in host (the grid selector).  Replaces the askpass popups: one
 *  row per host that needs a login, with a Log in button; ssh's questions
 *  (password, key passphrase, one-time code, host key confirmation) then
 *  appear in that row, routed through SshAuthBroker.
 *
 *  A successful login is announced through RemoteHostRegistry::
 *  hostLoggedIn, so every panel and selector showing that host updates, not
 *  just the one that was used.                                              */
class SshLoginPanel : public QWidget {
    Q_OBJECT
public:
    explicit SshLoginPanel(QWidget* parent = nullptr);

    /*  Host names (RemoteHost::name) that need a login, with the reason ssh
     *  gave, shown as a tooltip.  An empty list hides the panel.  Rows of
     *  hosts that stay listed keep their state (a login in progress).       */
    void setHosts(const QStringList& hostNames,
                  const QHash<QString, QString>& reasons = {});
    QStringList hosts() const;

signals:
    void loggedIn(const QString& hostName);

private:
    class HostRow;
    QVBoxLayout*             _layout = nullptr;
    QHash<QString, HostRow*> _rows;
};

} // namespace astra::remote
