#include "ai/ui/ApprovalCard.h"

#include <QApplication>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QToolButton>
#include <QVBoxLayout>

namespace astra::ai::ui {

QString riskVerb(Risk r)
{
    switch (r) {
    case Risk::Destructive: return QObject::tr("delete or overwrite data");
    case Risk::Execute:     return QObject::tr("run code on this computer");
    case Risk::Remote:      return QObject::tr("contact an archive or a remote host");
    case Risk::Modify:      return QObject::tr("change data");
    default:                return QObject::tr("act");
    }
}

ApprovalCard::ApprovalCard(QWidget* anchor)
    : QWidget(anchor, Qt::Tool | Qt::WindowStaysOnTopHint), _anchor(anchor)
{
    setProperty("astra.internal", true);
    setWindowTitle(tr("ASTRA: approve AI action"));
    setAttribute(Qt::WA_ShowWithoutActivating);
    setMinimumWidth(380);
    setMaximumWidth(520);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(14, 12, 14, 12);
    lay->setSpacing(8);

    _title = new QLabel;
    _title->setWordWrap(true);
    QFont tf = _title->font();
    tf.setBold(true);
    _title->setFont(tf);
    lay->addWidget(_title);

    _summary = new QLabel;
    _summary->setWordWrap(true);
    _summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(_summary);

    _detailsToggle = new QToolButton;
    _detailsToggle->setText(tr("Show details"));
    _detailsToggle->setCheckable(true);
    _detailsToggle->setAutoRaise(true);
    lay->addWidget(_detailsToggle, 0, Qt::AlignLeft);

    _details = new QPlainTextEdit;
    _details->setReadOnly(true);
    _details->setMaximumHeight(160);
    _details->setVisible(false);
    lay->addWidget(_details);
    connect(_detailsToggle, &QToolButton::toggled, this, [this](bool on) {
        _details->setVisible(on);
        _detailsToggle->setText(on ? tr("Hide details") : tr("Show details"));
        adjustSize();
        place();
    });

    _queueLabel = new QLabel;
    _queueLabel->setVisible(false);
    lay->addWidget(_queueLabel);

    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    _deny = new QPushButton(tr("Deny"));
    _allowOnce = new QPushButton(tr("Allow"));
    _allowSession = new QPushButton(tr("Allow for this session"));
    _allowSession->setToolTip(tr("Allow this kind of action from this agent until it disconnects"));
    buttons->addWidget(_deny);
    buttons->addWidget(_allowSession);
    buttons->addWidget(_allowOnce);
    lay->addLayout(buttons);

    connect(_deny, &QPushButton::clicked, this, [this] { answer(Answer::Deny); });
    connect(_allowOnce, &QPushButton::clicked, this, [this] { answer(Answer::Allow); });
    connect(_allowSession, &QPushButton::clicked, this, [this] { answer(Answer::AllowSession); });

    hide();
}

void ApprovalCard::ask(const ToolDef& tool, const ToolCall& call, const QString& summary,
                       std::function<void(Answer)> done)
{
    Request r;
    r.tool = tool.name;
    r.risk = riskVerb(tool.risk);
    r.caller = call.caller().name.isEmpty() ? tr("An AI agent") : call.caller().name;
    r.summary = summary;
    r.args = QString::fromUtf8(QJsonDocument(call.args()).toJson(QJsonDocument::Indented));
    r.session = call.caller().sessionId;
    r.done = std::move(done);
    _queue.push_back(std::move(r));
    if (!_current) {
        showNext();
        return;
    }
    _queueLabel->setText(tr("%1 more waiting").arg(_queue.size()));
    _queueLabel->setVisible(true);
}

void ApprovalCard::showNext()
{
    if (_queue.empty()) {
        _current.reset();
        hide();
        return;
    }
    _current = std::make_unique<Request>(std::move(_queue.front()));
    _queue.pop_front();
    _title->setText(tr("%1 wants to %2").arg(_current->caller, _current->risk));
    _summary->setText(_current->summary);
    _details->setPlainText(tr("Tool: %1\n\n%2").arg(_current->tool, _current->args));
    _queueLabel->setVisible(!_queue.empty());
    _queueLabel->setText(tr("%1 more waiting").arg(_queue.size()));
    _allowOnce->setDefault(false);
    _deny->setDefault(true);
    adjustSize();
    place();
    show();
    raise();
    QApplication::alert(_anchor ? _anchor->window() : this);
}

void ApprovalCard::answer(Answer a)
{
    if (!_current) return;
    auto done = std::move(_current->done);
    _current.reset();
    if (done) done(a);
    showNext();
}

void ApprovalCard::sessionEnded(const QString& sessionId)
{
    for (auto it = _queue.begin(); it != _queue.end();) {
        if (it->session == sessionId) {
            auto done = std::move(it->done);
            it = _queue.erase(it);
            if (done) done(Answer::Deny);
        } else {
            ++it;
        }
    }
    if (_current && _current->session == sessionId) answer(Answer::Deny);
    else if (_current) _queueLabel->setVisible(!_queue.empty());
}

void ApprovalCard::denyAll()
{
    while (!_queue.empty()) {
        auto r = std::move(_queue.front());
        _queue.pop_front();
        if (r.done) r.done(Answer::Deny);
    }
    if (_current) answer(Answer::Deny);
}

void ApprovalCard::place()
{
    QWidget* a = _anchor ? _anchor->window() : nullptr;
    QRect area = a ? a->frameGeometry()
                   : (QApplication::primaryScreen() ? QApplication::primaryScreen()->availableGeometry() : QRect());
    const QSize s = sizeHint().expandedTo(minimumSizeHint());
    move(area.right() - s.width() - 24, area.bottom() - s.height() - 48);
}

} // namespace astra::ai::ui
