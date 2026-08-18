#include "CopyToast.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QLabel>
#include <QTimer>

namespace CopyToast {

void flash(const QString &note)
{
    auto *popup = new QLabel(note.isEmpty()
                                 ? QStringLiteral("\xe2\x9c\x93 Copied")
                                 : QStringLiteral("\xe2\x9c\x93 Copied ") + note);
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
