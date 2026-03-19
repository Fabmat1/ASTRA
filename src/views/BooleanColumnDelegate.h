#ifndef BOOLEANCOLUMNDELEGATE_H
#define BOOLEANCOLUMNDELEGATE_H

#include <QStyledItemDelegate>
#include <QPainter>
#include <QSet>

// ─────────────────────────────────────────────────────────────────────────────
// Renders boolean columns as centred ✓ (green) or ✗ (muted red).
// All other columns fall through to the default delegate.
//
// Usage:
//   delegate->setBoolColumns({3, 7, 8, 9});   // column indices
// ─────────────────────────────────────────────────────────────────────────────
class BooleanColumnDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit BooleanColumnDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void setBoolColumns(const QSet<int>& cols) { _boolCols = cols; }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        if (!_boolCols.contains(index.column())) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }

        // Draw background / selection highlight
        initStyleOption(const_cast<QStyleOptionViewItem*>(&option), index);
        if (option.state & QStyle::State_Selected)
            painter->fillRect(option.rect, option.palette.highlight());
        else if (option.features & QStyleOptionViewItem::Alternate)
            painter->fillRect(option.rect, option.palette.alternateBase());

        // Determine value
        QVariant v = index.data(Qt::DisplayRole);
        bool isTrue = false;
        if (v.typeId() == QMetaType::Bool)
            isTrue = v.toBool();
        else if (v.typeId() == QMetaType::Double || v.typeId() == QMetaType::Int)
            isTrue = v.toInt() != 0;
        else if (v.typeId() == QMetaType::QString)
            isTrue = (v.toString() == "1" || v.toString().toLower() == "true");

        // Draw centred glyph
        painter->save();
        QFont f = painter->font();
        f.setPixelSize(14);
        f.setBold(true);
        painter->setFont(f);

        if (isTrue) {
            painter->setPen(QColor(0x2E, 0xCC, 0x71));   // green
            painter->drawText(option.rect, Qt::AlignCenter, "✓");
        } else {
            painter->setPen(QColor(0xCC, 0x5B, 0x5B));   // muted red
            painter->drawText(option.rect, Qt::AlignCenter, "✗");
        }
        painter->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem& option,
                   const QModelIndex& index) const override
    {
        if (_boolCols.contains(index.column()))
            return QSize(40, option.fontMetrics.height() + 4);
        return QStyledItemDelegate::sizeHint(option, index);
    }

private:
    QSet<int> _boolCols;
};

#endif // BOOLEANCOLUMNDELEGATE_H