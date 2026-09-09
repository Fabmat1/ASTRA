// src/views/widgets/FlowLayout.h
//
// A layout that places items left-to-right and wraps to a new row when the
// available width runs out, so a long run of small widgets never dictates the
// minimum width of its container. Rows are either left-aligned (default) or
// centered (the project-card grid look).
//
// Two extras keep it usable for panel content that has to survive a narrow
// container:
//   * an item is never given more width than the row has, and items that
//     report height-for-width (wrapping labels, nested flow layouts) are
//     measured at the width they actually get;
//   * uniform mode gives every item in a row the same width and picks the
//     column count from a preferred item width, which is what a row of equal
//     cards wants: 4 across when wide, 2x2 when medium, stacked when narrow.

#ifndef FLOWLAYOUT_H
#define FLOWLAYOUT_H

#include <QLayout>
#include <QList>

class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget* parent = nullptr, int margin = 0,
                        int hSpacing = -1, int vSpacing = -1,
                        bool centerRows = false);
    ~FlowLayout() override;

    /// Every item in a row shares the row width equally (card-grid mode).
    void setUniformItemWidths(bool on);
    /// Target width of one item; the column count is the number of these that
    /// fit. Only used in uniform mode; <= 0 means "widest item size hint".
    void setPreferredItemWidth(int px);
    /// Upper bound on the columns per row (0 = as many as fit).
    void setMaxColumns(int n);
    /// The last item on a row that reflows its own content (a wrapping label)
    /// is stretched to the end of the row, instead of stopping at the width
    /// its size hint happens to report. Off by default.
    void setExpandsTrailingItem(bool on);

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
    int doLayout(const QRect& rect, bool testOnly) const;
    static int itemHeightFor(const QLayoutItem* item, int width);
    static int itemWidthFor(const QLayoutItem* item, int avail);

    QList<QLayoutItem*> _items;
    int  _hSpace;
    int  _vSpace;
    bool _centerRows;
    bool _uniform        = false;
    int  _preferredItemW = 0;
    int  _maxColumns     = 0;
    bool _expandTrailing = false;
};

#endif   // FLOWLAYOUT_H
