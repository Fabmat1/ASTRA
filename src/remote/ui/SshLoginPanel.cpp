#include "remote/ui/SshLoginPanel.h"

#include "remote/RemoteHostRegistry.h"
#include "remote/SshAuthBroker.h"

#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QVBoxLayout>
#include <QtConcurrent>

namespace astra::remote {

/*  One host: idle ("needs a login" + Log in), connecting (busy bar), asking
 *  (ssh's prompt with an input field or Yes/No), failed (reason + Try
 *  again).                                                                  */
class SshLoginPanel::HostRow : public QFrame {
public:
    HostRow(QString hostName, SshLoginPanel* panel)
        : QFrame(panel), _name(std::move(hostName)), _panel(panel)
    {
        setFrameShape(QFrame::StyledPanel);
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(6, 4, 6, 4);
        v->setSpacing(4);

        _message = new QLabel;
        _message->setWordWrap(true);
        _message->setTextFormat(Qt::PlainText);
        v->addWidget(_message);

        auto* inputRow = new QHBoxLayout;
        _input = new QLineEdit;
        _input->setEchoMode(QLineEdit::Password);
        inputRow->addWidget(_input, 1);
        _busy = new QProgressBar;
        _busy->setRange(0, 0);
        _busy->setTextVisible(false);
        _busy->setMaximumHeight(_input->sizeHint().height() / 2);
        inputRow->addWidget(_busy, 1);
        inputRow->addStretch(0);
        _primary = new QPushButton;
        _secondary = new QPushButton;
        inputRow->addWidget(_primary);
        inputRow->addWidget(_secondary);
        v->addLayout(inputRow);

        connect(_primary, &QPushButton::clicked, this, [this] { onPrimary(); });
        connect(_secondary, &QPushButton::clicked, this, [this] { onSecondary(); });
        connect(_input, &QLineEdit::returnPressed, this, [this] { onPrimary(); });
        connect(&SshAuthBroker::instance(), &SshAuthBroker::promptClosed, this,
                [this](int id) {
                    if (id == _promptId) { _promptId = 0; showConnecting(); }
                });

        showIdle();
    }

    ~HostRow() override
    {
        // A prompt nobody can answer any more would hold ssh until it times out.
        if (_promptId) SshAuthBroker::instance().answer(_promptId, false);
        SshAuthBroker::instance().unregisterHandler(this);
    }

    void setReason(const QString& reason) { _reason = reason; if (_state == State::Idle) showIdle(); }
    bool busy() const { return _state == State::Connecting || _state == State::Asking; }

private:
    enum class State { Idle, Connecting, Asking, Failed, Done };

    void showIdle()
    {
        _state = State::Idle;
        _message->setText(tr("%1 needs an SSH login before its grids can be listed.")
                              .arg(_name));
        _message->setToolTip(_reason);
        setControls(false, false, tr("Log in"), {});
    }

    void showConnecting()
    {
        _state = State::Connecting;
        _message->setText(tr("Connecting to %1...").arg(_name));
        _message->setToolTip({});
        setControls(false, true, {}, {});
    }

    void showPrompt(const SshAuthBroker::Prompt& p)
    {
        if (_promptId && _promptId != p.id)   // superseded: ssh moved on
            SshAuthBroker::instance().answer(_promptId, false);
        _state     = State::Asking;
        _promptId  = p.id;
        _confirm   = p.confirm;
        _message->setText(QStringLiteral("%1: %2").arg(_name, p.text.trimmed()));
        _message->setToolTip({});
        if (_confirm) {
            setControls(false, false, tr("Yes"), tr("No"));
        } else {
            _input->clear();
            setControls(true, false, tr("Log in"), tr("Cancel"));
            _input->setFocus(Qt::OtherFocusReason);
        }
    }

    void showFailed(const QString& why)
    {
        _state = State::Failed;
        _message->setText(why.isEmpty()
                              ? tr("Login to %1 failed.").arg(_name)
                              : tr("Login to %1 failed: %2").arg(_name, why));
        _message->setToolTip({});
        setControls(false, false, tr("Try again"), {});
    }

    void setControls(bool input, bool busy, const QString& primary,
                     const QString& secondary)
    {
        _input->setVisible(input);
        _busy->setVisible(busy);
        _primary->setVisible(!primary.isEmpty());
        _primary->setText(primary);
        _primary->setDefault(input);
        _secondary->setVisible(!secondary.isEmpty());
        _secondary->setText(secondary);
    }

    void onPrimary()
    {
        switch (_state) {
        case State::Idle:
        case State::Failed:
            startLogin();
            break;
        case State::Asking:
            answer(true);
            break;
        case State::Connecting:
        case State::Done:
            break;
        }
    }

    void onSecondary()
    {
        if (_state == State::Asking) answer(false);
    }

    void answer(bool accepted)
    {
        const int id = _promptId;
        _promptId = 0;
        const QString text = _confirm ? QString() : _input->text();
        _input->clear();
        showConnecting();
        SshAuthBroker::instance().answer(id, accepted, text);
    }

    void startLogin()
    {
        RemoteHost host;
        auto& reg = RemoteHostRegistry::instance();
        if (!reg.hostByName(_name, &host)) {
            showFailed(tr("the host is no longer configured"));
            return;
        }
        SshConnection* conn = reg.connection(host.id);
        if (!conn) {
            showFailed(tr("the host is no longer configured"));
            return;
        }

        QPointer<HostRow> self(this);
        SshAuthBroker::instance().registerHandler(
            host.destination, this, [self](const SshAuthBroker::Prompt& p) {
                if (self) self->showPrompt(p);
            });
        showConnecting();

        struct Result { bool ok = false; QString err; };
        auto* watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher] {
            const Result r = watcher->result();
            watcher->deleteLater();
            SshAuthBroker::instance().unregisterHandler(this);
            if (_promptId) {
                SshAuthBroker::instance().answer(_promptId, false);
                _promptId = 0;
            }
            if (r.ok) {
                const QString name = _name;
                QPointer<SshLoginPanel> panel(_panel);
                _state = State::Done;
                _message->setText(tr("Logged in to %1.").arg(name));
                setControls(false, false, {}, {});
                // Listeners typically rebuild the panel, which deletes this
                // row: nothing may touch `this` after the announcement.
                RemoteHostRegistry::instance().announceLoggedIn(name);
                if (panel) emit panel->loggedIn(name);
                return;
            }
            showFailed(r.err);
        });
        watcher->setFuture(QtConcurrent::run([conn] {
            Result r;
            r.ok = conn->ensureMaster(&r.err, /*allowPrompt=*/true);
            return r;
        }));
    }

    QString         _name;
    QString         _reason;
    SshLoginPanel*  _panel;
    State           _state = State::Idle;
    int             _promptId = 0;
    bool            _confirm = false;

    QLabel*         _message   = nullptr;
    QLineEdit*      _input     = nullptr;
    QProgressBar*   _busy      = nullptr;
    QPushButton*    _primary   = nullptr;
    QPushButton*    _secondary = nullptr;
};

SshLoginPanel::SshLoginPanel(QWidget* parent) : QWidget(parent)
{
    _layout = new QVBoxLayout(this);
    _layout->setContentsMargins(0, 0, 0, 0);
    _layout->setSpacing(4);
    setVisible(false);
}

void SshLoginPanel::setHosts(const QStringList& hostNames,
                             const QHash<QString, QString>& reasons)
{
    // Rows of hosts no longer listed go, unless a login is running in them:
    // its prompt may still be on its way.
    for (auto it = _rows.begin(); it != _rows.end();) {
        if (!hostNames.contains(it.key()) && !it.value()->busy()) {
            it.value()->deleteLater();
            it.value()->hide();
            it = _rows.erase(it);
        } else {
            ++it;
        }
    }
    for (const QString& name : hostNames) {
        HostRow* row = _rows.value(name);
        if (!row) {
            row = new HostRow(name, this);
            _rows.insert(name, row);
            _layout->addWidget(row);
        }
        row->setReason(reasons.value(name));
    }
    setVisible(!_rows.isEmpty());
}

QStringList SshLoginPanel::hosts() const
{
    return _rows.keys();
}

} // namespace astra::remote
