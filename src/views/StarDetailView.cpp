#include "StarDetailView.h"
#include "models/Star.h"
#include "models/Project.h"
#include "controllers/ApplicationController.h"
#include "db/DatabaseManager.h"
#include "utils/Logger.h"
#include "utils/AppSettings.h"

#include "views/panels/DetailPanel.h"
#include "views/panels/DetailPanelFactory.h"
#include "views/panels/PanelUtils.h"

#include "views/tools/RVInspectorDialog.h"
#include "views/tools/SpectraFitDialog.h"
#include "views/tools/LightcurveFetchDialog.h"
#include "views/tools/SEDFitDialog.h"
#include "views/tools/CMDDialog.h"
#include "views/tools/GalacticOrbitDialog.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitter>
#include <QPushButton>
#include <QTimer>
#include <QEvent>
#include <QDesktopServices>
#include <QUrl>

StarDetailView::StarDetailView(std::shared_ptr<Star> star,
                               DatabaseManager* dbm,
                               ApplicationController* controller,
                               const QString& projectId,
                               QWidget* parent)
    : QWidget(parent, Qt::Window)
    , _star(star)
    , _dbm(dbm)
    , _controller(controller)
    , _projectId(projectId)
{
    setupUi();
    buildGrid();

    _star->computeSummaryMetricsFull([this]() {
        if (_dbm) _dbm->updateStarRow(_projectId, _star);
    });

    if (_controller && _controller->settings()) {
        connect(_controller->settings(), &AppSettings::detailGridChanged,
                this, &StarDetailView::onSettingsGridChanged);
    }

    QTimer::singleShot(0, this, &StarDetailView::refreshAllThemes);
}

StarDetailView::~StarDetailView() = default;

void StarDetailView::setupUi()
{
    QString title = _star->getAlias().isEmpty()
        ? QString("Star Detail — %1").arg(_star->getSourceId())
        : QString("Star Detail — %1").arg(_star->getAlias());
    setWindowTitle(title);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1400, 900);

    auto* outer = new QHBoxLayout(this);
    outer->setContentsMargins(6, 6, 6, 6);
    outer->setSpacing(6);

    _gridHost = new QWidget;
    auto* hostLayout = new QVBoxLayout(_gridHost);
    hostLayout->setContentsMargins(0, 0, 0, 0);

    outer->addWidget(_gridHost, 1);
    outer->addWidget(createButtonSidebar());
}

void StarDetailView::tearDownGrid()
{
    for (auto* p : _panels) { if (p) p->deleteLater(); }
    _panels.clear();
    if (_rootVSplit) { _rootVSplit->deleteLater(); _rootVSplit = nullptr; }
}

void StarDetailView::buildGrid()
{
    tearDownGrid();

    AppSettings* settings = _controller ? _controller->settings() : nullptr;
    const auto grid = settings ? settings->detailGrid()
                                : QVector<QVector<AppSettings::DetailPanel>>{
                                      { AppSettings::DetailPanel::Summary,
                                        AppSettings::DetailPanel::RadialVelocity },
                                      { AppSettings::DetailPanel::Spectra,
                                        AppSettings::DetailPanel::LightCurve } };

    _rootVSplit = new QSplitter(Qt::Vertical, _gridHost);
    _rootVSplit->setOpaqueResize(false);

    DetailPanel::Context ctx { _star, _dbm, _controller, _projectId };

    for (const auto& row : grid) {
        auto* hSplit = new QSplitter(Qt::Horizontal, _rootVSplit);
        hSplit->setOpaqueResize(false);

        bool anyInRow = false;
        for (auto which : row) {
            DetailPanel* panel = DetailPanelFactory::create(which, ctx, hSplit);
            if (panel) {
                hSplit->addWidget(panel);
                _panels.append(panel);
                anyInRow = true;
            } else {
                // placeholder for "None" cells so row proportions are preserved
                auto* empty = new QWidget(hSplit);
                empty->setMinimumSize(40, 40);
                hSplit->addWidget(empty);
            }
        }
        // Equal stretch per column
        for (int i = 0; i < hSplit->count(); ++i)
            hSplit->setStretchFactor(i, 1);

        _rootVSplit->addWidget(hSplit);
        if (!anyInRow) hSplit->setMaximumHeight(1);  // collapse empty rows visually
    }
    for (int i = 0; i < _rootVSplit->count(); ++i)
        _rootVSplit->setStretchFactor(i, 1);

    _gridHost->layout()->addWidget(_rootVSplit);

    // Force equal partition at startup
    QTimer::singleShot(0, this, [this]() {
        if (!_rootVSplit) return;
        int h = _rootVSplit->height();
        QList<int> sizes;
        for (int i = 0; i < _rootVSplit->count(); ++i)
            sizes << h / _rootVSplit->count();
        _rootVSplit->setSizes(sizes);

        for (int r = 0; r < _rootVSplit->count(); ++r) {
            if (auto* hs = qobject_cast<QSplitter*>(_rootVSplit->widget(r))) {
                int w = hs->width();
                QList<int> cs;
                for (int i = 0; i < hs->count(); ++i) cs << w / hs->count();
                hs->setSizes(cs);
            }
        }
    });
}

void StarDetailView::onSettingsGridChanged()
{
    buildGrid();
    QTimer::singleShot(0, this, &StarDetailView::refreshAllThemes);
}

void StarDetailView::refreshAllThemes()
{
    for (auto* p : _panels) if (p) p->refreshTheme();
}

QWidget* StarDetailView::createButtonSidebar()
{
    QWidget* sidebar = new QWidget;
    sidebar->setFixedWidth(180);
    QVBoxLayout* layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(2, 0, 2, 0);
    layout->setSpacing(8);

    auto makeButton = [&](const QString& text, const QString& tooltip) -> QPushButton* {
        QPushButton* btn = new QPushButton(text);
        btn->setToolTip(tooltip);
        btn->setMinimumHeight(36);
        layout->addWidget(btn);
        return btn;
    };

    _simbadButton = makeButton("Show in SIMBAD", "Open SIMBAD page for this star");
    connect(_simbadButton, &QPushButton::clicked, this, &StarDetailView::onShowInSimbad);

    layout->addSpacing(8);

    _viewAdjustRVButton = makeButton("View / Adjust RV", "View and adjust radial velocity data");
    connect(_viewAdjustRVButton, &QPushButton::clicked, this, &StarDetailView::onViewAdjustRV);

    _viewFitSpectraButton = makeButton("View / Fit Spectra", "View and fit spectra");
    connect(_viewFitSpectraButton, &QPushButton::clicked, this, &StarDetailView::onViewFitSpectra);

    _fetchLCButton = makeButton("Fetch / Fit LC", "Fetch and fit light curves");
    connect(_fetchLCButton, &QPushButton::clicked, this, &StarDetailView::onFetchLightcurves);

    _viewFitSEDButton = makeButton("View / Fit SED", "View and fit SED");
    connect(_viewFitSEDButton, &QPushButton::clicked, this, &StarDetailView::onViewFitSED);

    layout->addSpacing(8);

    _cmdButton = makeButton("Show CMD", "Show colour–magnitude diagram");
    connect(_cmdButton, &QPushButton::clicked, this, &StarDetailView::onShowCMD);

    _calcOrbitButton = makeButton("Galactic Orbit", "Calculate galactic orbit");
    connect(_calcOrbitButton, &QPushButton::clicked, this, &StarDetailView::onCalculateOrbit);

    layout->addStretch();
    return sidebar;
}


bool StarDetailView::event(QEvent* e)
{
    if (e->type() == QEvent::ApplicationPaletteChange ||
        e->type() == QEvent::StyleChange) {
        QTimer::singleShot(0, this, &StarDetailView::refreshAllThemes);
    }
    return QWidget::event(e);
}

void StarDetailView::onViewFitSED()
{
    auto* dlg = new SEDFitDialog(_star, _dbm, _projectId, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    connect(dlg, &SEDFitDialog::fitDataChanged, this, [this] {
        for (auto* p : _panels) if (p) p->refresh();
    });
    dlg->show();
}

void StarDetailView::onViewAdjustRV()
{
    auto* dialog = new RVInspectorDialog(_star, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void StarDetailView::onViewFitSpectra()
{
    auto* dialog = new SpectraFitDialog(_star, _dbm, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void StarDetailView::onFetchLightcurves()
{
    auto* dialog = new LightcurveFetchDialog(_star, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void StarDetailView::onShowCMD()
{
    std::vector<std::shared_ptr<Star>> projectStars;
    if (_controller) {
        if (auto proj = _controller->getCurrentProject()) {
            projectStars = proj->getAllStars();
        }
    }
    auto* dialog = new CMDDialog(_star, std::move(projectStars), _projectId, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void StarDetailView::onCalculateOrbit()
{
    auto* dialog = new GalacticOrbitDialog(_star, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}

void StarDetailView::onShowInSimbad()
{
    if (!_star) return;
    QString url = QString("https://simbad.cds.unistra.fr/simbad/sim-id?Ident=Gaia+DR3+%1&submit=submit+id")
                      .arg(_star->getSourceId());
    QDesktopServices::openUrl(QUrl(url));
}