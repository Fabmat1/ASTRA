#include "QuantityLabel.h"

#include "CopyToast.h"
#include "utils/QuantityFormat.h"

#include <QAction>
#include <QApplication>
#include <QContextMenuEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <cmath>

using QuantityFormat::CopyContent;
using QuantityFormat::CopyStyle;

namespace {
// Horizontal breathing room so a selection highlight is not clipped by the
// widget edge. Vertically there is none: the widget has to be exactly as tall
// as the equivalent QLabel, or every property row in a grid grows.
constexpr int kPadX = 2;
constexpr int kPadY = 0;

std::function<void(QWidget *)> g_settingsInvoker;

QString styleName(CopyStyle s)
{
    return s == CopyStyle::Latex ? QObject::tr("LaTeX") : QObject::tr("plain text");
}
} // namespace

QuantityLabel::QuantityLabel(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
}

QuantityLabel::QuantityLabel(const Quantity &q, QWidget *parent)
    : QuantityLabel(parent)
{
    setQuantity(q);
}

void QuantityLabel::setSettingsInvoker(std::function<void(QWidget *)> fn)
{
    g_settingsInvoker = std::move(fn);
}

void QuantityLabel::setQuantity(const Quantity &q)
{
    _q         = q;
    _selection = 0;
    _hover     = -1;
    relayout();
}

void QuantityLabel::setAlignment(Qt::Alignment a)
{
    _align = a;
    update();
}

void QuantityLabel::setTextPixelSize(int px, bool bold)
{
    _valueBold = bold;
    setStyleSheet(QString("QuantityLabel { font-size: %1px;%2 background: "
                          "transparent; border: none; }")
                      .arg(px)
                      .arg(bold ? " font-weight: 700;" : ""));
    relayout();
}

void QuantityLabel::setValueBold(bool bold)
{
    _valueBold = bold;
    relayout();
}

void QuantityLabel::setColors(const QColor &fg, const QColor &unit)
{
    _fg     = fg;
    _unitFg = unit;
    update();
}

void QuantityLabel::relayout()
{
    QFont f = font();
    f.setBold(_valueBold || font().bold());
    _layout = QuantityRenderer::layout(_q, f);
    updateGeometry();
    setToolTip(_q.hasValue()
                   ? QuantityFormat::plainText(_q) +
                         tr("\nClick to copy, drag to select a part")
                   : QString());
    update();
}

QSize QuantityLabel::sizeHint() const
{
    if (_layout.isEmpty())
        return QSize(0, fontMetrics().height());
    return QSize(static_cast<int>(std::ceil(_layout.size.width())) + 2 * kPadX,
                 static_cast<int>(std::ceil(_layout.size.height())) + 2 * kPadY);
}

QSize QuantityLabel::minimumSizeHint() const { return sizeHint(); }

QRectF QuantityLabel::contentRect() const
{
    return QRectF(rect()).adjusted(kPadX, kPadY, -kPadX, -kPadY);
}

void QuantityLabel::paintEvent(QPaintEvent *)
{
    if (_layout.isEmpty())
        return;

    QPainter p(this);
    QFont    f = font();
    f.setBold(_valueBold || font().bold());

    QuantityRenderer::PaintOptions o;
    o.fg          = _fg.isValid() ? _fg : palette().color(QPalette::WindowText);
    o.unitFg      = _unitFg;
    o.selectionBg = palette().color(QPalette::Highlight);
    o.selectionFg = palette().color(QPalette::HighlightedText);
    o.hoverBg     = QColor(o.fg.red(), o.fg.green(), o.fg.blue(), 28);
    o.selection   = _selection;
    o.hover       = _hover;
    o.valueBold   = _valueBold;

    QuantityRenderer::paint(p, contentRect(), _layout, f, o, _align);
}

unsigned QuantityLabel::effectiveMask() const
{
    if (_selection)
        return _selection;
    unsigned mask = 0;
    const QuantityFormat::Parts parts =
        QuantityFormat::partsFor(QuantityFormat::prefs().content);
    for (int s = 0; s < QuantityRenderer::SegCount; ++s)
        if (_layout.on[s] && parts.testFlag(QuantityRenderer::partOf(s)))
            mask |= QuantityRenderer::bit(s);
    return mask;
}

void QuantityLabel::copyMask(unsigned mask, CopyStyle style, const QString &note)
{
    const QString text =
        QuantityFormat::copyText(_q, QuantityRenderer::partsOf(mask), style);
    if (text.isEmpty())
        return;
    CopyToast::copy(text, note);
    emit copied(text);
}

void QuantityLabel::copyWhole(CopyStyle style)
{
    copyMask(effectiveMask(), style, QString());
}

void QuantityLabel::mousePressEvent(QMouseEvent *ev)
{
    if (ev->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(ev);
        return;
    }
    _pressPos  = ev->pos();
    _dragging  = false;
    if (_selection) {
        _selection = 0;
        update();
    }
    ev->accept();
}

void QuantityLabel::mouseMoveEvent(QMouseEvent *ev)
{
    if (ev->buttons() & Qt::LeftButton) {
        const int dist = (ev->pos() - _pressPos).manhattanLength();
        if (_dragging || dist >= QApplication::startDragDistance()) {
            _dragging = true;
            const QRectF drag(QPointF(_pressPos), QPointF(ev->pos()));
            const unsigned mask = QuantityRenderer::segmentsIn(
                contentRect(), _layout, drag, _align);
            if (mask != _selection) {
                _selection = mask;
                update();
            }
        }
        ev->accept();
        return;
    }

    const int seg =
        QuantityRenderer::segmentAt(contentRect(), _layout, ev->pos(), _align);
    if (seg != _hover) {
        _hover = seg;
        update();
    }
    QWidget::mouseMoveEvent(ev);
}

void QuantityLabel::mouseReleaseEvent(QMouseEvent *ev)
{
    if (ev->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(ev);
        return;
    }
    if (!_dragging) {
        // A plain click copies the whole quantity, the way the old
        // click-to-copy labels did - only now it carries the errors and unit.
        setFocus(Qt::MouseFocusReason);
        copyWhole(QuantityFormat::prefs().style);
    }
    _dragging = false;
    ev->accept();
}

void QuantityLabel::selectAll()
{
    unsigned mask = 0;
    for (int s = 0; s < QuantityRenderer::SegCount; ++s)
        if (_layout.on[s])
            mask |= QuantityRenderer::bit(s);
    if (mask == _selection)
        return;
    _selection = mask;
    update();
}

void QuantityLabel::mouseDoubleClickEvent(QMouseEvent *ev)
{
    selectAll();
    ev->accept();
}

void QuantityLabel::leaveEvent(QEvent *ev)
{
    if (_hover != -1) {
        _hover = -1;
        update();
    }
    QWidget::leaveEvent(ev);
}

void QuantityLabel::changeEvent(QEvent *ev)
{
    QWidget::changeEvent(ev);
    if (ev->type() == QEvent::FontChange || ev->type() == QEvent::StyleChange)
        relayout();
}

void QuantityLabel::keyPressEvent(QKeyEvent *ev)
{
    if (ev->matches(QKeySequence::Copy)) {
        copyWhole(QuantityFormat::prefs().style);
        ev->accept();
        return;
    }
    if (ev->matches(QKeySequence::SelectAll)) {
        selectAll();
        ev->accept();
        return;
    }
    QWidget::keyPressEvent(ev);
}

void QuantityLabel::contextMenuEvent(QContextMenuEvent *ev)
{
    if (!_q.hasValue())
        return;

    const CopyStyle pref  = QuantityFormat::prefs().style;
    const CopyStyle other = pref == CopyStyle::Latex ? CopyStyle::Plain
                                                     : CopyStyle::Latex;
    QMenu menu(this);

    if (_selection) {
        const unsigned sel = _selection;
        menu.addAction(tr("Copy selection"), this,
                       [this, sel, pref] { copyMask(sel, pref, tr("selection")); });
        menu.addSeparator();
    }

    auto addContent = [&](const QString &label, CopyContent c) {
        unsigned mask = 0;
        const QuantityFormat::Parts parts = QuantityFormat::partsFor(c);
        for (int s = 0; s < QuantityRenderer::SegCount; ++s)
            if (_layout.on[s] && parts.testFlag(QuantityRenderer::partOf(s)))
                mask |= QuantityRenderer::bit(s);
        if (!mask)
            return;
        QAction *a = menu.addAction(label, this,
                                    [this, mask, pref] { copyMask(mask, pref, QString()); });
        if (c == QuantityFormat::prefs().content)
            a->setText(label + tr("   (default)"));
    };

    addContent(tr("Copy value"), CopyContent::Value);
    if (_q.hasError())
        addContent(tr("Copy value and error"), CopyContent::ValueError);
    if (!_q.unit.isEmpty())
        addContent(tr("Copy value, error and unit"), CopyContent::ValueErrorUnit);

    menu.addSeparator();
    menu.addAction(tr("Copy as %1").arg(styleName(other)), this,
                   [this, other] { copyWhole(other); });

    if (g_settingsInvoker) {
        menu.addSeparator();
        menu.addAction(tr("Copy format settings..."), this,
                       [this] { g_settingsInvoker(this); });
    }

    menu.exec(ev->globalPos());
    ev->accept();
}
