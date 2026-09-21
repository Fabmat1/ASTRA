#include "app/ui/StarDetailView.h"
#include "core/Star.h"
#include "core/Project.h"
#include "app/ui/ApplicationController.h"
#include "db/DatabaseManager.h"
#include "app/Logger.h"
#include "app/AppSettings.h"

#include "app/ui/panels/DetailPanel.h"
#include "app/ui/panels/DetailPanelFactory.h"
#include "app/ui/panels/PanelUtils.h"
#include "app/ui/panels/PlotAxisLinker.h"
#include "app/ui/UiIcons.h"
#include "lightcurve/ui/LCPanel.h"
#include "spectra/ui/SpectraPanel.h"
#include "rv/ui/RVPanel.h"

#include "io/ui/StarShare.h"
#include "catalog/ui/CMDDialog.h"
#include "kinematics/ui/GalacticOrbitDialog.h"
#include "lightcurve/ui/LightcurveFetchDialog.h"
#include "observing/ui/ObservabilityDialog.h"
#include "rv/ui/RVInspectorDialog.h"
#include "sed/ui/SEDFitDialog.h"
#include "fitting/ui/SpectraFitDialog.h"

#include <QDesktopServices>
#include <QEvent>
#include <QHBoxLayout>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QSplitterHandle>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cstdlib>

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

    _star->setSummaryChangedCallback([this]() {
        for (auto* p : _panels) if (p) p->onSummaryChanged();
    });

    _star->computeSummaryMetricsFull([this]() {
        if (_dbm) _dbm->updateStarRow(_projectId, _star);
    });

    if (_controller && _controller->settings()) {
        connect(_controller->settings(), &AppSettings::detailGridChanged,
                this, &StarDetailView::onSettingsGridChanged);
    }
}


StarDetailView::~StarDetailView()
{
    // Star outlives this view (project-owned); detach so the lambda
    // doesn't fire into a dangling `this`.
    if (_star) _star->setSummaryChangedCallback(nullptr);
}


void StarDetailView::setupUi()
{
    QString title = _star->getAlias().isEmpty()
        ? QString("Star Detail - %1").arg(_star->getSourceId())
        : QString("Star Detail - %1").arg(_star->getAlias());
    setWindowTitle(title);
    setAttribute(Qt::WA_DeleteOnClose);
    resize(1400, 900);

    // The window must be freely resizable, so nothing inside may pin a floor
    // on it: the layout keeps its hands off the window minimum, the panels
    // shrink to nothing (see buildGrid) and the sidebar scrolls.
    setMinimumSize(0, 0);

    auto* outer = new QHBoxLayout(this);
    outer->setSizeConstraint(QLayout::SetNoConstraint);
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
    _populateQueue.clear();

    // The linker holds pointers into the panels' plots, so it has to go first -
    // and before the panels are deleted, so it can hand their margins back.
    delete _axisLinker;
    _axisLinker = nullptr;
    delete _axisLinkButton;
    _axisLinkButton      = nullptr;
    _axisLinkHandleIndex = -1;
    _axisLinkRowA = _axisLinkRowB = nullptr;
    _axisLinkColA = _axisLinkColB = -1;

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
    _rootVSplit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

    DetailPanel::Context ctx { _star, _dbm, _controller, _projectId };

    // Where the RV / light-curve panel landed, so a vertically adjacent pair
    // can be offered the x-axis link below.
    GridSlot rvSlot, lcSlot;

    int rowIndex = -1;
    for (const auto& row : grid) {
        ++rowIndex;
        auto* hSplit = new QSplitter(Qt::Horizontal, _rootVSplit);
        hSplit->setOpaqueResize(false);
        hSplit->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);

        bool anyInRow = false;
        for (auto which : row) {
            // Build each panel deferred: it shows a loading shimmer immediately
            // and its heavy populate() is driven later, one panel per event-loop
            // turn, so the whole window appears instantly.
            DetailPanel* panel =
                DetailPanelFactory::create(which, ctx, hSplit, /*deferPopulate=*/true);
            if (panel) {
                // Ignored policy: the panel keeps filling the splitter cell but
                // contributes no minimum size, so the window can shrink past it.
                panel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
                hSplit->addWidget(panel);
                _panels.append(panel);
                _populateQueue.append(panel);
                anyInRow = true;
                if (!rvSlot.panel && qobject_cast<RVPanel*>(panel))
                    rvSlot = { panel, hSplit, hSplit->count() - 1, rowIndex };
                if (!lcSlot.panel && qobject_cast<LCPanel*>(panel))
                    lcSlot = { panel, hSplit, hSplit->count() - 1, rowIndex };
            } else {
                // placeholder for "None" cells so row proportions are preserved
                auto* empty = new QWidget(hSplit);
                empty->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
                hSplit->addWidget(empty);
            }
        }
        // Equal stretch per column
        for (int i = 0; i < hSplit->count(); ++i)
            hSplit->setStretchFactor(i, 1);

        connect(hSplit, &QSplitter::splitterMoved, this, [this, hSplit] {
            alignLinkedColumns(hSplit);
            positionAxisLinkButton();
        });

        _rootVSplit->addWidget(hSplit);
        if (!anyInRow) hSplit->setMaximumHeight(1);  // collapse empty rows visually
    }
    connect(_rootVSplit, &QSplitter::splitterMoved, this,
            [this] { positionAxisLinkButton(); });
    for (int i = 0; i < _rootVSplit->count(); ++i)
        _rootVSplit->setStretchFactor(i, 1);

    // ── Cross-panel wiring: highlight the RV point of the shown spectrum ──
    // When a Spectra Panel and an RV Panel coexist in the grid, changing the
    // displayed spectrum emphasises its matching RV point in the RV plot(s).
    {
        SpectraPanel* spectraPanel = nullptr;
        RVPanel*      rvPanel      = nullptr;
        for (auto* p : _panels) {
            if (!spectraPanel) spectraPanel = qobject_cast<SpectraPanel*>(p);
            if (!rvPanel)      rvPanel      = qobject_cast<RVPanel*>(p);
        }
        if (spectraPanel && rvPanel) {
            connect(spectraPanel, &SpectraPanel::selectionChanged,
                    rvPanel,      &RVPanel::highlightSpectrum);
            // Both panels populate asynchronously; apply the initial highlight
            // once the spectra panel has loaded the spectrum it shows first.
            connect(spectraPanel, &DetailPanel::populated, rvPanel,
                    [spectraPanel, rvPanel] {
                        rvPanel->highlightSpectrum(spectraPanel->currentSpectrumId(),
                                                   spectraPanel->currentFitId());
                    });
        }
    }

    setupAxisLink(rvSlot, lcSlot);

    _gridHost->layout()->addWidget(_rootVSplit);

    // Start filling the panels in, one per event-loop turn, behind their
    // shimmers. The window is already laid out, so it paints immediately.
    QTimer::singleShot(0, this, [this] { populateNextPanel(); });

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
        positionAxisLinkButton();
    });
}

void StarDetailView::populateNextPanel()
{
    while (!_populateQueue.isEmpty() && _populateQueue.first().isNull())
        _populateQueue.removeFirst();   // skip panels destroyed by a rebuild
    if (_populateQueue.isEmpty()) return;

    DetailPanel* panel = _populateQueue.takeFirst();
    panel->populateNow();

    // Hand control back to the event loop so this panel paints before the next
    // one blocks; keeps the window responsive while everything fills in.
    if (!_populateQueue.isEmpty())
        QTimer::singleShot(0, this, [this] { populateNextPanel(); });
}

void StarDetailView::scheduleThemeRefresh() {
    if (_themeRefreshPending)
        return;
    _themeRefreshPending = true;
    QTimer::singleShot(0, this, [this] {
        _themeRefreshPending = false;
        refreshAllThemes();
    });
}

void StarDetailView::onSettingsGridChanged() {
    buildGrid();
    scheduleThemeRefresh();
}

void StarDetailView::refreshAllThemes()
{
    for (auto* p : _panels) if (p) p->refreshTheme();
    refreshAxisLinkButton();
}

QWidget* StarDetailView::createButtonSidebar()
{
    QWidget* sidebar = new QWidget;
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


    _viewAdjustRVButton = makeButton("View / Adjust RV", "View and adjust radial velocity data");
    connect(_viewAdjustRVButton, &QPushButton::clicked, this, &StarDetailView::onViewAdjustRV);

    _viewFitSpectraButton = makeButton("View / Fit Spectra", "View and fit spectra");
    connect(_viewFitSpectraButton, &QPushButton::clicked, this, &StarDetailView::onViewFitSpectra);

    _fetchLCButton = makeButton("Fetch / Fit LC", "Fetch and fit light curves");
    connect(_fetchLCButton, &QPushButton::clicked, this, &StarDetailView::onFetchLightcurves);

    _viewFitSEDButton = makeButton("View / Fit SED", "View and fit SED");
    connect(_viewFitSEDButton, &QPushButton::clicked, this, &StarDetailView::onViewFitSED);

    layout->addSpacing(8);

    _observabilityButton = makeButton("Observability", "Plan observations of this star");
    connect(_observabilityButton, &QPushButton::clicked,
            this, &StarDetailView::onShowObservability);

    _cmdButton = makeButton("Show CMD", "Show colour–magnitude diagram");
    connect(_cmdButton, &QPushButton::clicked, this, &StarDetailView::onShowCMD);

    _calcOrbitButton = makeButton("Galactic Orbit", "Calculate galactic orbit");
    connect(_calcOrbitButton, &QPushButton::clicked, this, &StarDetailView::onCalculateOrbit);

    layout->addSpacing(8);

    _shareButton =
        makeButton("Share Star", "Export this star to a shareable .astra file");
    connect(_shareButton, &QPushButton::clicked, this,
            &StarDetailView::onShareStar);

    layout->addStretch();

    // Scrolling the button stack keeps it usable when the window is shorter
    // than the buttons need, instead of forcing a minimum height on it.
    auto* scroll = new QScrollArea;
    scroll->setWidget(sidebar);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFixedWidth(180);
    scroll->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Ignored);
    return scroll;
}

namespace {

/// Left and right edge of column `col`, in `row`'s own coordinates.
QPair<int, int> columnSpan(QSplitter* row, int col)
{
    if (!row || col < 0 || col >= row->count()) return { 0, -1 };
    const QList<int> sizes = row->sizes();
    if (col >= sizes.size()) return { 0, -1 };
    int left = row->handleWidth() * col;
    for (int i = 0; i < col; ++i) left += sizes[i];
    return { left, left + sizes[col] };
}

/// Resize `row` so column `col` spans [left, right]. The space is taken from
/// (or handed back to) the other columns in proportion to their current
/// widths. QSplitter still enforces its children's own minimum sizes, so the
/// result can fall short of the request - the axis-margin alignment covers
/// whatever difference is left.
void setColumnSpan(QSplitter* row, int col, int left, int right)
{
    if (!row || col < 0 || col >= row->count()) return;
    QList<int> sizes = row->sizes();
    const int n = sizes.size();
    if (n <= 1 || col >= n) return;

    int content = 0;
    for (int s : sizes) content += s;

    // Never squeeze a neighbouring column out of existence.
    constexpr int kMinColumn = 24;

    int before = left - row->handleWidth() * col;
    const int loBefore = kMinColumn * col;
    const int hiBefore = content - kMinColumn * (n - col);
    if (hiBefore < loBefore) return;
    before = std::clamp(before, loBefore, hiBefore);

    int self = right - left;
    const int loSelf = kMinColumn;
    const int hiSelf = content - before - kMinColumn * (n - 1 - col);
    if (hiSelf < loSelf) return;
    self = std::clamp(self, loSelf, hiSelf);

    const int after = content - before - self;

    auto spread = [&sizes](int from, int to, int total) {
        const int count = to - from;
        if (count <= 0) return;
        int current = 0;
        for (int i = from; i < to; ++i) current += sizes[i];
        int assigned = 0;
        for (int i = from; i < to - 1; ++i) {
            sizes[i] = (current > 0) ? total * sizes[i] / current : total / count;
            assigned += sizes[i];
        }
        sizes[to - 1] = total - assigned;
    };
    spread(0, col, before);
    sizes[col] = self;
    spread(col + 1, n, after);

    row->setSizes(sizes);   // does not re-emit splitterMoved
}

} // namespace

void StarDetailView::setupAxisLink(const GridSlot& rv, const GridSlot& lc)
{
    // Only worth offering when the two panels are stacked: side by side, or
    // with another row between them, there is no shared x axis to read.
    if (!rv.panel || !lc.panel || std::abs(rv.rowIndex - lc.rowIndex) != 1)
        return;

    const bool rvOnTop  = rv.rowIndex < lc.rowIndex;
    DetailPanel* top    = rvOnTop ? rv.panel : lc.panel;
    DetailPanel* bottom = rvOnTop ? lc.panel : rv.panel;

    _axisLinkRowA = rv.row; _axisLinkColA = rv.col;
    _axisLinkRowB = lc.row; _axisLinkColB = lc.col;

    // Handle i of a splitter sits above widget i, so the divider between the
    // two rows is the lower row's handle.
    _axisLinkHandleIndex = std::max(rv.rowIndex, lc.rowIndex);
    _axisLinker = new PlotAxisLinker(top, bottom, _gridHost, this);

    _axisLinkButton = new QToolButton(_gridHost);
    _axisLinkButton->setCheckable(true);
    _axisLinkButton->setFocusPolicy(Qt::NoFocus);
    _axisLinkButton->setCursor(Qt::PointingHandCursor);
    _axisLinkButton->setFixedSize(22, 22);
    _axisLinkButton->hide();

    connect(_axisLinkButton, &QToolButton::toggled, this, [this](bool on) {
        if (_axisLinker) _axisLinker->setLinked(on);
        // Columns first: it moves the panels, and the axis margins are then
        // computed against where they ended up.
        if (on) alignLinkedColumns(nullptr);
        refreshAxisLinkButton();
        positionAxisLinkButton();
    });
    connect(_axisLinker, &PlotAxisLinker::availabilityChanged, this,
            [this] { positionAxisLinkButton(); });

    // Row heights (and so the divider's y) change with the window, and the
    // panels' widths change with the horizontal splitters.
    _gridHost->installEventFilter(this);

    refreshAxisLinkButton();
    positionAxisLinkButton();
}

void StarDetailView::alignLinkedColumns(QSplitter* source)
{
    if (!_axisLinker || !_axisLinker->isLinked()) return;
    if (!_axisLinkRowA || !_axisLinkRowB) return;
    if (source && source != _axisLinkRowA && source != _axisLinkRowB) return;
    if (_aligningColumns) return;

    const QPair<int, int> a = columnSpan(_axisLinkRowA, _axisLinkColA);
    const QPair<int, int> b = columnSpan(_axisLinkRowB, _axisLinkColB);
    if (a.second <= a.first || b.second <= b.first) return;

    int left  = 0;
    int right = 0;
    if (source == _axisLinkRowA)      { left = a.first; right = a.second; }
    else if (source == _axisLinkRowB) { left = b.first; right = b.second; }
    else {
        // No row to follow: take the widest span either row has on each side,
        // so switching the link on never makes a panel narrower than it was.
        left  = std::min(a.first,  b.first);
        right = std::max(a.second, b.second);
    }

    _aligningColumns = true;
    if (source != _axisLinkRowA)
        setColumnSpan(_axisLinkRowA, _axisLinkColA, left, right);
    if (source != _axisLinkRowB)
        setColumnSpan(_axisLinkRowB, _axisLinkColB, left, right);
    _aligningColumns = false;

    _axisLinker->scheduleRealign();
    positionAxisLinkButton();
}

void StarDetailView::refreshAxisLinkButton()
{
    if (!_axisLinkButton) return;
    const bool on = _axisLinkButton->isChecked();

    // A QSS background-color never reaches the QPalette, so palette(window) and
    // friends in a stylesheet resolve to Qt's defaults rather than to ASTRA's
    // theme - which is how this button ended up a black disc. Build the colours
    // from what ThemeManager actually published instead, the same way the plot
    // chrome does, and re-run this from refreshAllThemes() on a theme switch.
    const QColor surface = PanelUtils::themeSurface();
    const QColor fg      = PanelUtils::themeFg();
    const QColor border  = PanelUtils::mix(surface, fg, on ? 0.55 : 0.28);
    const QColor bg      = on ? PanelUtils::mix(surface, fg, 0.10) : surface;
    const QColor hover   = PanelUtils::mix(surface, fg, 0.18);
    const QColor pressed = PanelUtils::mix(surface, fg, 0.28);

    _axisLinkButton->setStyleSheet(
        QString("QToolButton { border: 1px solid %1; border-radius: 11px;"
                " background: %2; padding: 0px; }"
                "QToolButton:hover { background: %3; }"
                "QToolButton:pressed { background: %4; }")
            .arg(border.name(), bg.name(), hover.name(), pressed.name()));

    UiIcons::apply(_axisLinkButton,
                   on ? UiIcons::Role::LinkOn : UiIcons::Role::LinkOff, 13);
    _axisLinkButton->setToolTip(
        on ? "Radial velocity and light curve share one x axis.\n"
             "Click to unlink."
           : "Line the radial velocity and light-curve x axes up with each "
             "other,\nand keep them showing the same range.");
}

void StarDetailView::positionAxisLinkButton()
{
    if (!_axisLinkButton || !_axisLinker || !_rootVSplit) return;

    QSplitterHandle* handle = _rootVSplit->handle(_axisLinkHandleIndex);
    if (!handle || !handle->isVisible() || !_axisLinker->isAvailable()) {
        _axisLinkButton->hide();
        return;
    }

    // Centre it over the strip the two panels actually share; if they barely
    // overlap there is nothing meaningful to align, so stay out of the way.
    const QPair<int, int> span = _axisLinker->overlapInReference();
    if (span.second - span.first < 80) {
        _axisLinkButton->hide();
        return;
    }

    const int y = handle->mapTo(_gridHost, QPoint(0, handle->height() / 2)).y();
    _axisLinkButton->move((span.first + span.second) / 2
                              - _axisLinkButton->width() / 2,
                          y - _axisLinkButton->height() / 2);
    _axisLinkButton->show();
    _axisLinkButton->raise();
}

bool StarDetailView::eventFilter(QObject* watched, QEvent* e)
{
    if (watched == _gridHost && (e->type() == QEvent::Resize
                                 || e->type() == QEvent::Show)) {
        // After the splitters have relaid themselves out, not during. A resize
        // hands each row its share of the new width independently, so the two
        // rows' column dividers have to be brought back together.
        QTimer::singleShot(0, this, [this] {
            alignLinkedColumns(nullptr);
            positionAxisLinkButton();
        });
    }
    return QWidget::eventFilter(watched, e);
}

bool StarDetailView::event(QEvent *e) {
    if (e->type() == QEvent::ApplicationPaletteChange ||
        e->type() == QEvent::StyleChange) {
        scheduleThemeRefresh(); 
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
    auto* dialog = new RVInspectorDialog(_star, _dbm, _controller, _projectId, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    connect(dialog, &QDialog::finished, this, [this](int) {
        for (auto* p : _panels) if (p) p->refresh();
    });

    dialog->show();
}

void StarDetailView::onViewFitSpectra()
{
    auto* dialog = new SpectraFitDialog(_star, _dbm, _projectId, this, _controller);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    connect(dialog, &SpectraFitDialog::starParametersChanged, this, [this]() {
        for (auto* p : _panels) if (p) p->refresh();
    });
    // Fires when spectra or their fits change (new fit run, add/remove,
    // flags, instrument re-detection) so the grid panels stay in sync.
    connect(dialog, &SpectraFitDialog::spectraUpdated, this, [this]() {
        for (auto* p : _panels) if (p) p->refresh();
    });

    dialog->show();
}

void StarDetailView::onFetchLightcurves()
{
    auto* dialog = new LightcurveFetchDialog(_star, _dbm, _controller, _projectId, this);
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
    std::vector<std::shared_ptr<Star>> projectStars;
    if (_controller) {
        if (auto proj = _controller->getCurrentProject())
            projectStars = proj->getAllStars();
    }
    // Re-read the table samples now, so the dialog offers what is currently
    // highlighted / filtered rather than the state at window-open time.
    std::vector<std::shared_ptr<Star>> filteredStars, selectedStars;
    if (_filteredProvider)
        filteredStars = _filteredProvider();
    if (_selectedProvider)
        selectedStars = _selectedProvider();

    auto* dialog = new GalacticOrbitDialog(_star, _dbm, _projectId,
                                           std::move(projectStars),
                                           std::move(filteredStars),
                                           std::move(selectedStars), this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    connect(dialog, &GalacticOrbitDialog::kinematicsSaved, this, [this]() {
        for (auto* p : _panels) if (p) p->refresh();
    });

    dialog->show();
}

void StarDetailView::onShareStar() {
    if (!_star)
        return;
    StarShare::exportStarsInteractive(this, _controller, {_star});
}

void StarDetailView::onShowInSimbad()
{
    if (!_star) return;
    QString url = QString("https://simbad.cds.unistra.fr/simbad/sim-id?Ident=Gaia+DR3+%1&submit=submit+id")
                      .arg(_star->getSourceId());
    QDesktopServices::openUrl(QUrl(url));
}

void StarDetailView::onShowObservability()
{
    auto* dialog = new ObservabilityDialog(_star, _dbm, this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->show();
}