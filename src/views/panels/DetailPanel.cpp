#include "DetailPanel.h"
#include "views/widgets/ShimmerWidget.h"

#include <QResizeEvent>

DetailPanel::DetailPanel(const Context& ctx, QWidget* parent)
    : QWidget(parent), _ctx(ctx) {}

DetailPanel::~DetailPanel() = default;

void DetailPanel::showLoadingShimmer(int cards)
{
    if (!_loadingShimmer)
        _loadingShimmer = new ShimmerWidget(this);
    _loadingShimmer->setCardCount(qMax(1, cards));
    _loadingShimmer->setGeometry(rect());
    _loadingShimmer->show();
    _loadingShimmer->raise();
}

void DetailPanel::hideLoadingShimmer()
{
    if (!_loadingShimmer) return;
    _loadingShimmer->hide();
    _loadingShimmer->deleteLater();
    _loadingShimmer = nullptr;
}

void DetailPanel::populateNow()
{
    populate();
    hideLoadingShimmer();
    emit populated();
}

void DetailPanel::resizeEvent(QResizeEvent* e)
{
    QWidget::resizeEvent(e);
    if (_loadingShimmer)
        _loadingShimmer->setGeometry(rect());
}
