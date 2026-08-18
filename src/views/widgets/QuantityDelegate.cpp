#include "QuantityDelegate.h"

#include "QuantityRenderer.h"
#include "utils/QuantityFormat.h"

#include <QApplication>
#include <QPainter>

QuantityDelegate::QuantityDelegate(QObject *parent)
    : QStyledItemDelegate(parent) {}

Quantity QuantityDelegate::quantityOf(const QModelIndex &index)
{
    const QVariant v = index.data(QuantityRole);
    if (v.canConvert<Quantity>())
        return v.value<Quantity>();
    return Quantity();
}

QString QuantityDelegate::copyTextFor(const QModelIndex &index)
{
    const Quantity q = quantityOf(index);
    if (q.hasValue())
        return QuantityFormat::copyText(q);
    return index.data(Qt::DisplayRole).toString();
}

void QuantityDelegate::paint(QPainter *p, const QStyleOptionViewItem &opt,
                             const QModelIndex &index) const
{
    const Quantity q = quantityOf(index);
    if (!q.hasValue()) {
        QStyledItemDelegate::paint(p, opt, index);
        return;
    }

    QStyleOptionViewItem o = opt;
    initStyleOption(&o, index);
    // The renderer draws the text, so let the style paint only the chrome.
    o.text.clear();
    const QWidget *w  = o.widget;
    QStyle        *st = w ? w->style() : QApplication::style();
    st->drawControl(QStyle::CE_ItemViewItem, &o, p, w);

    const bool selected = o.state & QStyle::State_Selected;

    QuantityRenderer::PaintOptions po;
    po.fg = o.palette.color(selected ? QPalette::HighlightedText
                                     : QPalette::Text);

    Qt::Alignment align = o.displayAlignment;
    if (!(align & Qt::AlignHorizontal_Mask))
        align |= Qt::AlignLeft;
    if (!(align & Qt::AlignVertical_Mask))
        align |= Qt::AlignVCenter;

    const QuantityRenderer::Layout l = QuantityRenderer::layout(q, o.font);
    const QRectF target = st->subElementRect(QStyle::SE_ItemViewItemText, &o, w);
    QuantityRenderer::paint(*p, target, l, o.font, po, align);
}

QSize QuantityDelegate::sizeHint(const QStyleOptionViewItem &opt,
                                 const QModelIndex &index) const
{
    const Quantity q = quantityOf(index);
    if (!q.hasValue())
        return QStyledItemDelegate::sizeHint(opt, index);

    QStyleOptionViewItem o = opt;
    initStyleOption(&o, index);
    const QuantityRenderer::Layout l = QuantityRenderer::layout(q, o.font);
    const QSize base = QStyledItemDelegate::sizeHint(opt, index);

    // The default hint covers one line of text plus the view's margins. A
    // stacked pair reaches above and below that line box, so the cell grows by
    // exactly that overflow - otherwise the lower error side is clipped.
    const int overflow = static_cast<int>(
        std::ceil(std::max(0.0, l.size.height() - l.lineHeight)));
    return QSize(std::max(base.width(),
                          static_cast<int>(std::ceil(l.size.width())) + 8),
                 base.height() + overflow + 1);
}
