#include "ui/widgets/CopyToast.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QLabel>
#include <QTimer>

namespace CopyToast {

void flash(const QString &note)
{
    // fromUtf8, not QStringLiteral: the latter compiles its argument as a
    // UTF-16 literal, so the three UTF-8 bytes of the check mark would each
    // become their own code unit and the toast would read "\u00e2" followed by
    // two invisible control characters instead of a tick.
    const QString tick = QString::fromUtf8("\xe2\x9c\x93");
    auto *popup = new QLabel(note.isEmpty()
                                 ? tick + QStringLiteral(" Copied")
                                 : tick + QStringLiteral(" Copied ") + note);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setAttribute(Qt::WA_ShowWithoutActivating);
    popup->setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint);
    popup->setStyleSheet(
        "QLabel { background: #4CAF50; color: white; font-weight: bold;"
        " padding: 4px 12px; border-radius: 4px; font-size: 12px; }");
    popup->adjustSize();
    popup->move(QCursor::pos() + QPoint(12, 12));
    popup->show();
    QTimer::singleShot(1000, popup, &QLabel::close);
}

void copy(const QString &text, const QString &note)
{
    if (text.isEmpty())
        return;
    QApplication::clipboard()->setText(text);
    flash(note);
}

} // namespace CopyToast
