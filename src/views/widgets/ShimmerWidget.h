#pragma once

#include <QWidget>

class QTimer;

// A lightweight skeleton-loading placeholder: it paints a silhouette of the
// content that is about to appear (stacked "cards") and runs a soft highlight
// band sweeping left-to-right across it, the familiar shimmer loading effect.
//
// Theme-aware (colours derived from PanelUtils). The animation only runs while
// the widget is visible, so it costs nothing when hidden.
class ShimmerWidget : public QWidget
{
    Q_OBJECT
public:
    explicit ShimmerWidget(QWidget* parent = nullptr);

    // Number of skeleton "cards" (e.g. periodogram panels) to silhouette.
    void setCardCount(int n);

protected:
    void paintEvent(QPaintEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;

private:
    QTimer* _timer = nullptr;
    double  _phase = 0.0;   // 0..1 sweep position
    int     _cards = 3;
};
