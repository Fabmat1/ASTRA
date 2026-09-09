// src/views/widgets/ElidedLabel.h
//
// A QLabel that shortens its own text with an ellipsis instead of forcing its
// container to stay wide. The size hint still reports the full text, so a
// roomy layout renders it in full; the minimum size hint is a small floor, so
// a narrow layout may squeeze it and the text elides to whatever fits.
//
// The untruncated text is kept as the tool tip whenever it does not fit, so
// nothing is lost when the panel is narrow.

#ifndef ELIDEDLABEL_H
#define ELIDEDLABEL_H

#include <QLabel>
#include <QString>

class ElidedLabel : public QLabel
{
    Q_OBJECT
public:
    explicit ElidedLabel(const QString& text = QString(),
                         QWidget* parent = nullptr);

    /// Sets the text this label represents; what is painted is derived from it.
    void setFullText(const QString& text);
    QString fullText() const { return _full; }

    void setElideMode(Qt::TextElideMode mode);
    /// Smallest width the label will report; below it the text is clipped.
    void setMinimumTextWidth(int px);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    void updateElision();

    QString           _full;
    Qt::TextElideMode _mode     = Qt::ElideRight;
    int               _minWidth = 48;
};

#endif   // ELIDEDLABEL_H
