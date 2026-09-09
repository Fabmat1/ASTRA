// src/views/widgets/ElidedLabel.cpp

#include "ElidedLabel.h"

#include <QEvent>
#include <QFontMetrics>
#include <QResizeEvent>

ElidedLabel::ElidedLabel(const QString& text, QWidget* parent)
    : QLabel(parent)
{
    setFullText(text);
}

void ElidedLabel::setFullText(const QString& text)
{
    _full = text;
    updateElision();
    // The hints are derived from the full text, so a re-elision never changes
    // them; a new text does.
    updateGeometry();
}

void ElidedLabel::setElideMode(Qt::TextElideMode mode)
{
    if (_mode == mode) return;
    _mode = mode;
    updateElision();
}

void ElidedLabel::setMinimumTextWidth(int px)
{
    if (_minWidth == px) return;
    _minWidth = px;
    updateGeometry();
}

QSize ElidedLabel::sizeHint() const
{
    const QFontMetrics fm(fontMetrics());
    const QMargins     m = contentsMargins();
    return QSize(fm.horizontalAdvance(_full) + m.left() + m.right() + 2,
                 fm.height() + m.top() + m.bottom());
}

QSize ElidedLabel::minimumSizeHint() const
{
    QSize s = sizeHint();
    s.setWidth(qMin(s.width(), _minWidth));
    return s;
}

void ElidedLabel::resizeEvent(QResizeEvent* e)
{
    QLabel::resizeEvent(e);
    updateElision();
}

void ElidedLabel::changeEvent(QEvent* e)
{
    QLabel::changeEvent(e);
    // A theme switch restyles the font, which changes what fits.
    if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange)
        updateElision();
}

void ElidedLabel::updateElision()
{
    const QMargins m     = contentsMargins();
    const int      avail = width() - m.left() - m.right() - 2;
    if (avail <= 0) {
        // Not laid out yet: show the full text and let the first resize elide.
        QLabel::setText(_full);
        return;
    }
    const QString shown = fontMetrics().elidedText(_full, _mode, avail);
    if (shown != text())
        QLabel::setText(shown);
    setToolTip(shown == _full ? QString() : _full);
}
