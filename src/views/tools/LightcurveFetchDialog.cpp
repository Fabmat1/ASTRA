#include "LightcurveFetchDialog.h"
#include "views/panels/LCPanel.h"
#include "models/Star.h"
#include "utils/Logger.h"
#include "views/panels/PeriodogramPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QLabel>
#include <QDialogButtonBox>

LightcurveFetchDialog::LightcurveFetchDialog(std::shared_ptr<Star> star,
                                             DatabaseManager*       dbm,
                                             ApplicationController* controller,
                                             const QString&         projectId,
                                             QWidget*               parent)
    : QDialog(parent)
    , _star(star)
    , _dbm(dbm)
    , _controller(controller)
    , _projectId(projectId)
{
    setupUi();
    LOG_INFO("Tools", QString("Lightcurve dialog opened for star %1").arg(_star->getSourceId()));
}

LightcurveFetchDialog::~LightcurveFetchDialog() = default;

void LightcurveFetchDialog::setupUi()
{
    setWindowTitle(QString("Light Curves — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(1200, 800);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    _tabs = new QTabWidget;
    _tabs->addTab(buildViewerTab(),      "Viewer");
    _tabs->addTab(buildPeriodogramTab(), "Periodogram");
    _tabs->addTab(buildFetchTab(),       "Fetch");
    _tabs->addTab(buildFitTab(),         "Fit");
    layout->addWidget(_tabs, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

QWidget* LightcurveFetchDialog::buildViewerTab()
{
    DetailPanel::Context ctx;
    ctx.star       = _star;
    ctx.dbm        = _dbm;
    ctx.controller = _controller;
    ctx.projectId  = _projectId;

    _lcPanel = new LCPanel(ctx);
    return _lcPanel;
}

QWidget* LightcurveFetchDialog::buildPeriodogramTab()
{
    _periodogramPanel = new PeriodogramPanel;

    // Push current LC series into the panel whenever this tab becomes active
    connect(_tabs, &QTabWidget::currentChanged, this, [this](int idx) {
        if (!_periodogramPanel || _tabs->widget(idx) != _periodogramPanel) return;
        if (!_lcPanel) return;
        const auto src = _lcPanel->seriesData(false);
        QList<PeriodogramPanel::Series> conv;
        conv.reserve(src.size());
        for (const auto& s : src)
            conv.append({s.source, s.filter, s.t, s.y, s.e});
        _periodogramPanel->setSeries(conv);
    });

    // Double-click peak → fold LC viewer and switch tab
    connect(_periodogramPanel, &PeriodogramPanel::periodSelected,
            this, [this](double period) {
        if (!_lcPanel || period <= 0) return;
        _lcPanel->setFoldPeriod(period);
        _lcPanel->setFolded(true);
        _tabs->setCurrentIndex(0);
    });

    return _periodogramPanel;
}

QWidget* LightcurveFetchDialog::buildFetchTab()
{
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    auto* ph = new QLabel("🚧  Fetch tab — coming in Task 3.");
    ph->setAlignment(Qt::AlignCenter);
    ph->setStyleSheet("color: gray; font-size: 14px;");
    l->addWidget(ph);
    return w;
}

QWidget* LightcurveFetchDialog::buildFitTab()
{
    auto* w = new QWidget;
    auto* l = new QVBoxLayout(w);
    auto* ph = new QLabel("🚧  Fit tab — coming in Task 4.");
    ph->setAlignment(Qt::AlignCenter);
    ph->setStyleSheet("color: gray; font-size: 14px;");
    l->addWidget(ph);
    return w;
}