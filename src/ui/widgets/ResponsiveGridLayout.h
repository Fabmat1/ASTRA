// src/views/widgets/ResponsiveGridLayout.h
//
// A label/value grid that picks its column count from the width it is given.
//
// The summary panel's property blocks are pairs of (name, measured value). A
// fixed QGridLayout with two pair-columns reads well in a wide panel and is
// clipped in a narrow one, because the column count is baked in. This layout
// derives it instead: it measures the widest label and the widest value, and
// fits as many pair-columns as the available width allows, down to one. Pairs
// are filled column-major, so the reading order stays "down the first column,
// then the next" at every width.

#ifndef RESPONSIVEGRIDLAYOUT_H
#define RESPONSIVEGRIDLAYOUT_H

#include <QLayout>
#include <QList>

class ResponsiveGridLayout : public QLayout
{
public:
    explicit ResponsiveGridLayout(QWidget* parent = nullptr);
    ~ResponsiveGridLayout() override;

    /// Appends one row: `label` on the left, `value` next to it.
    void addPair(QWidget* label, QWidget* value);

    void setMaxColumns(int n);      // upper bound on pair-columns (default 2)
    void setColumnSpacing(int px);  // gap between pair-columns
    void setRowSpacing(int px);     // gap between rows
    void setPairSpacing(int px);    // gap between a label and its value

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int i) const override;
    QLayoutItem* takeAt(int i) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int w) const override;
    void setGeometry(const QRect& r) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;

private:
    struct Extents {
        int labelHint = 0, labelMin = 0;
        int valueHint = 0, valueMin = 0;
    };
    Extents extents() const;
    int     minContentWidth() const;
    int     doLayout(const QRect& rect, bool testOnly) const;

    QList<QLayoutItem*> _items;   // label, value, label, value, ...
    int _maxColumns  = 2;
    int _colSpacing  = 16;
    int _rowSpacing  = 4;
    int _pairSpacing = 8;
};

#endif   // RESPONSIVEGRIDLAYOUT_H
