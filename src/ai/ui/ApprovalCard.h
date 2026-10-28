#pragma once

/*  Asks the user to approve an AI agent's dangerous action without blocking
 *  anything: a small tool window over the main window, one request at a
 *  time, the rest queued. It is invisible to the AI tools themselves (an
 *  agent must not be able to press "Allow" on its own request).             */

#include "ai/PermissionGate.h"

#include <QPointer>
#include <QWidget>
#include <deque>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QToolButton;

namespace astra::ai::ui {

class ApprovalCard : public QWidget, public ApprovalPrompter {
    Q_OBJECT
public:
    explicit ApprovalCard(QWidget* anchor);

    void ask(const ToolDef& tool, const ToolCall& call, const QString& summary,
             std::function<void(Answer)> done) override;

    void sessionEnded(const QString& sessionId) override;

    int pending() const { return int(_queue.size()) + (_current ? 1 : 0); }
    /// Denies everything waiting (the "Stop agents" button).
    void denyAll();

private:
    struct Request {
        QString     tool;
        QString     risk;
        QString     caller;
        QString     summary;
        QString     args;
        QString     session;
        std::function<void(Answer)> done;
    };
    void showNext();
    void answer(Answer a);
    void place();

    QPointer<QWidget> _anchor;
    std::deque<Request> _queue;
    std::unique_ptr<Request> _current;

    QLabel*         _title;
    QLabel*         _summary;
    QLabel*         _queueLabel;
    QToolButton*    _detailsToggle;
    QPlainTextEdit* _details;
    QPushButton*    _deny;
    QPushButton*    _allowOnce;
    QPushButton*    _allowSession;
};

QString riskVerb(Risk r);

} // namespace astra::ai::ui
