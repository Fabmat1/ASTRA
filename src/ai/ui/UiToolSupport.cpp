#include "ai/ui/UiToolSupport.h"

#include "ai/ui/UiWindows.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QFrame>
#include <QImage>
#include <QJsonArray>
#include <QPixmap>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QWidget>

namespace astra::ai::ui {

void runUiAction(const ToolCallPtr& call, std::function<QJsonObject()> action, int settleMs)
{
    QSet<QWidget*> before;
    for (QWidget* w : visibleWindows()) before.insert(w);
    QWidget* blockingBefore = blockingWidget();

    struct State {
        bool        done = false;
        bool        failed = false;
        QString     error;
        QJsonObject result;
        QElapsedTimer sinceDone;
        QElapsedTimer total;
    };
    auto st = std::make_shared<State>();
    st->total.start();

    auto report = [call, before, st](bool waiting) {
        if (call->finished()) return;
        if (st->failed) { call->fail(st->error); return; }
        QJsonObject o = st->result;
        QJsonArray fresh;
        for (QWidget* w : visibleWindows())
            if (!before.contains(w)) fresh.append(describeWindow(w));
        if (!fresh.isEmpty()) o.insert(QStringLiteral("new_windows"), fresh);
        const QJsonObject b = blockingState();
        if (!b.isEmpty()) {
            o.insert(QStringLiteral("waiting_for_input"), b);
            o.insert(QStringLiteral("hint"),
                     QStringLiteral("A dialog is waiting for input. Inspect it with ui_snapshot "
                                    "(window id above), fill fields with ui_set_value and answer "
                                    "with dialog_respond."));
        }
        if (waiting && !st->done) o.insert(QStringLiteral("status"), QStringLiteral("waiting_for_input"));
        else if (!o.contains(QStringLiteral("status"))) o.insert(QStringLiteral("status"), QStringLiteral("done"));
        call->finish(ToolResult::json(o));
    };

    // The poller lives on the application, not on the call: the action may
    // run a nested event loop for minutes.
    auto* poll = new QTimer;
    poll->setInterval(40);
    QObject::connect(poll, &QTimer::timeout, poll, [poll, st, report, blockingBefore, settleMs] {
        QWidget* b = blockingWidget();
        if (!st->done && b && b != blockingBefore) {
            report(true);
            poll->deleteLater();
            return;
        }
        if (st->done && st->sinceDone.elapsed() >= settleMs) {
            report(false);
            poll->deleteLater();
            return;
        }
        if (st->total.elapsed() > 30000 && !st->done) {
            // Still inside the action (a long computation): report and let
            // it finish on its own.
            st->result.insert(QStringLiteral("status"), QStringLiteral("still_running"));
            report(false);
            poll->deleteLater();
        }
    });
    poll->start();

    QTimer::singleShot(0, [st, action = std::move(action)] {
        try {
            st->result = action();
        } catch (const ToolError& e) {
            st->failed = true;
            st->error = QString::fromStdString(e.what());
        } catch (const std::exception& e) {
            st->failed = true;
            st->error = QString::fromStdString(e.what());
        }
        st->done = true;
        st->sinceDone.start();
    });
}

QByteArray grabPng(QWidget* w, int maxWidth)
{
    if (!w) return {};
    QImage img = w->grab().toImage();
    if (img.width() > maxWidth) img = img.scaledToWidth(maxWidth, Qt::SmoothTransformation);
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}

void flashWidget(QWidget* w)
{
    if (!w || !w->isVisible()) return;
    QWidget* top = w->window();
    auto* frame = new QFrame(top);
    frame->setAttribute(Qt::WA_TransparentForMouseEvents);
    frame->setProperty("astra.internal", true);
    frame->setStyleSheet(QStringLiteral("border: 2px solid #4c8df6; border-radius: 4px; background: transparent;"));
    const QPoint p = w->mapTo(top, QPoint(0, 0));
    frame->setGeometry(QRect(p, w->size()).adjusted(-3, -3, 3, 3));
    frame->show();
    frame->raise();
    QTimer::singleShot(700, frame, &QObject::deleteLater);
}

} // namespace astra::ai::ui
