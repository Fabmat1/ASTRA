#pragma once

#include "models/Quantity.h"

#include <QStyledItemDelegate>

// Renders table cells that carry a Quantity in QuantityDelegate::QuantityRole
// exactly like QuantityLabel does, so asymmetric errors stack in tables too.
// Cells without that role fall back to the default item painting.
//
// Keep the cell's Qt::DisplayRole set to the plain text form: existing copy
// and export paths read it, and it is what the fallback painting shows.
class QuantityDelegate : public QStyledItemDelegate {
    Q_OBJECT

  public:
    static constexpr int QuantityRole = Qt::UserRole + 0x51;

    explicit QuantityDelegate(QObject *parent = nullptr);

    void  paint(QPainter *p, const QStyleOptionViewItem &opt,
                const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &opt,
                   const QModelIndex &index) const override;

    /// The Quantity stored on `index`, or an unset one.
    static Quantity quantityOf(const QModelIndex &index);
    /// Copy text for `index` per the user's preferences; falls back to the
    /// cell's display text when it carries no Quantity.
    static QString copyTextFor(const QModelIndex &index);
};
