#pragma once

/*  The round AI button floating over the bottom right of the star table.
 *  Keeps its corner when the table resizes, and follows the theme.          */

#include <QAbstractButton>
#include <QPointer>

namespace astra::ai::ui {

class AiFab : public QAbstractButton {
    Q_OBJECT
public:
    /// Floats over `host` (the star table), inside its scroll bars.
    explicit AiFab(QWidget* host);

    QSize sizeHint() const override { return {48, 48}; }

protected:
    void paintEvent(QPaintEvent*) override;
    bool eventFilter(QObject* o, QEvent* e) override;
    void enterEvent(QEnterEvent*) override { _hover = true; update(); }
    void leaveEvent(QEvent*) override { _hover = false; update(); }

private:
    void reposition();
    QPointer<QWidget> _host;
    bool _hover = false;
};

} // namespace astra::ai::ui
