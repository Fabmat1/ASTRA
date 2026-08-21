// src/views/widgets/FlowLayout.h
//
// A layout that places items left-to-right and wraps to a new row when the
// available width runs out, so a long run of small widgets never dictates the
// minimum width of its container. Rows are either left-aligned (default) or
// centered (the project-card grid look).

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

    QList<QLayoutItem*> _items;
    int  _hSpace;
    int  _vSpace;
    bool _centerRows;
};

#endif   // FLOWLAYOUT_H
