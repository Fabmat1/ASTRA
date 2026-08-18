#pragma once

#include "QuantityRenderer.h"
#include "models/Quantity.h"

#include <QColor>
#include <QPoint>
#include <QWidget>

#include <functional>

// ─────────────────────────────────────────────────────────────────────────────
// Displays one measured value with its uncertainty and unit, with the
// asymmetric error sides stacked (see QuantityRenderer).
//
// Interaction:
//   click            copy the whole quantity, formatted per Settings
//   drag             select individual pieces (value / +up / −down / unit)
//   Ctrl+C           copy the selection, or the whole quantity when empty
//   right-click      copy menu with the other content levels and notations
// ─────────────────────────────────────────────────────────────────────────────
class QuantityLabel : public QWidget {
    Q_OBJECT

  public:
    explicit QuantityLabel(QWidget *parent = nullptr);
    explicit QuantityLabel(const Quantity &q, QWidget *parent = nullptr);

    void            setQuantity(const Quantity &q);
    const Quantity &quantity() const { return _q; }

    void setAlignment(Qt::Alignment a);
    /// Sets the text size the way the panels do it. The themes carry a global
    /// `* { font-size: 10pt; }` rule that overrides QWidget::setFont, so the
    /// size has to come from the widget's own style sheet to take effect and
    /// to match the QLabel next to it.
    void setTextPixelSize(int px, bool bold = false);
    void setValueBold(bool bold);
    /// Text colour; `unit` falls back to `fg` when left invalid.
    void setColors(const QColor &fg, const QColor &unit = QColor());

    /// Opens the copy-format preferences. Installed once by MainWindow so the
    /// context menu can offer the shortcut without plumbing the controller
    /// through every panel.
    static void setSettingsInvoker(std::function<void(QWidget *)> fn);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  signals:
    void copied(const QString &text);

  protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void contextMenuEvent(QContextMenuEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void leaveEvent(QEvent *) override;
    void changeEvent(QEvent *) override;

  private:
    void     relayout();
    void     selectAll();
    QRectF   contentRect() const;
    void     copyMask(unsigned mask, QuantityFormat::CopyStyle style,
                      const QString &note);
    void     copyWhole(QuantityFormat::CopyStyle style);
    unsigned effectiveMask() const;

    Quantity                 _q;
    QuantityRenderer::Layout _layout;
    Qt::Alignment            _align = Qt::AlignLeft | Qt::AlignVCenter;
    QColor                   _fg;
    QColor                   _unitFg;
    bool                     _valueBold = false;

    unsigned _selection = 0;
    int      _hover     = -1;
    QPoint   _pressPos;
    bool     _dragging = false;
};
