#include "ProjectPlotDialog.h"
#include "models/Star.h"
#include "models/ColumnPreset.h"
#include "views/panels/PanelUtils.h"
#include "plotting/qcustomplot.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QRadioButton>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QListWidget>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QScrollArea>
#include <QMenu>
#include <QWidgetAction>
#include <QDialogButtonBox>
#include <QDoubleValidator>
#include <QColorDialog>
#include <QFontDialog>
#include <QEvent>
#include <QFileDialog>
#include <QMessageBox>
#include <QSvgGenerator>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QSignalBlocker>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <map>
#include <unordered_map>

namespace {

// Return the numeric value of a star-field variant, rejecting strings,
// booleans and NaN (Star::getFieldValue() encodes "not set" as a string).
bool numericValue(const QVariant& v, double& out)
{
    switch (v.typeId()) {
    case QMetaType::Double:
    case QMetaType::Float:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        out = v.toDouble();
        return std::isfinite(out);
    default:
        return false;
    }
}

// Hammer (equal-area "Aitoff-style") projection. l, b in radians,
// l in [-pi, pi]. Output x in [-2*sqrt(2), 2*sqrt(2)], y in [-sqrt(2), sqrt(2)].
void hammerProject(double l, double b, double& x, double& y)
{
    const double denom = std::sqrt(1.0 + std::cos(b) * std::cos(l / 2.0));
    x = 2.0 * std::sqrt(2.0) * std::cos(b) * std::sin(l / 2.0) / denom;
    y = std::sqrt(2.0) * std::sin(b) / denom;
}

// Map RA (deg) to projection longitude (rad): RA 0h at the centre,
// RA increasing to the left (east-left, astronomical convention).
double raToL(double raDeg)
{
    double l = -raDeg;
    while (l < -180.0) l += 360.0;
    while (l >  180.0) l -= 360.0;
    return l * M_PI / 180.0;
}

const struct { const char* name; QCPScatterStyle::ScatterShape shape; } kMarkers[] = {
    { "Disc",     QCPScatterStyle::ssDisc     },
    { "Circle",   QCPScatterStyle::ssCircle   },
    { "Cross",    QCPScatterStyle::ssCross    },
    { "Plus",     QCPScatterStyle::ssPlus     },
    { "Square",   QCPScatterStyle::ssSquare   },
    { "Diamond",  QCPScatterStyle::ssDiamond  },
    { "Triangle", QCPScatterStyle::ssTriangle },
    { "Star",     QCPScatterStyle::ssStar     },
};

// Okabe-Ito colourblind-safe palette for data series
const struct { const char* name; const char* hex; } kSeriesPalette[] = {
    { "Orange",         "#E69F00" },
    { "Sky blue",       "#56B4E9" },
    { "Bluish green",   "#009E73" },
    { "Yellow",         "#F0E442" },
    { "Blue",           "#0072B2" },
    { "Vermillion",     "#D55E00" },
    { "Reddish purple", "#CC79A7" },
    { "Black",          "#000000" },
    { "Grey",           "#999999" },
    { "White",          "#FFFFFF" },
};

// Matplotlib's viridis: perceptually uniform and colourblind-safe, for
// encoding a third field in the marker colour.
QCPColorGradient viridisGradient()
{
    QCPColorGradient g;
    g.clearColorStops();
    g.setColorInterpolation(QCPColorGradient::ciRGB);
    const struct { double pos; const char* hex; } stops[] = {
        { 0.000, "#440154" }, { 0.125, "#472d7b" }, { 0.250, "#3b528b" },
        { 0.375, "#2c728e" }, { 0.500, "#21918c" }, { 0.625, "#28ae80" },
        { 0.750, "#5ec962" }, { 0.875, "#addc30" }, { 1.000, "#fde725" },
    };
    for (const auto& s : stops)
        g.setColorStopAt(s.pos, QColor(s.hex));
    return g;
}

// Neutral backgrounds only: white, black and shades of grey
const struct { const char* name; const char* hex; } kBackgroundPalette[] = {
    { "White",      "#FFFFFF" },
    { "Light grey", "#E4E4E4" },
    { "Grey",       "#C0C0C0" },
    { "Mid grey",   "#909090" },
    { "Dark grey",  "#585858" },
    { "Charcoal",   "#2A2A2A" },
    { "Black",      "#000000" },
};

// Running mean/median of ys over a centred window of `window` points,
// computed on data sorted by x. Outputs one smoothed point per input point.
void windowedTrend(const QVector<double>& xs, const QVector<double>& ys,
                   int window, bool median,
                   QVector<double>& outX, QVector<double>& outY)
{
    const int n = xs.size();
    if (n == 0)
        return;

    std::vector<int> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return xs[a] < xs[b]; });

    const int half = window / 2;
    outX.reserve(n);
    outY.reserve(n);
    std::vector<double> buf;
    for (int i = 0; i < n; ++i) {
        const int lo = std::max(0, i - half);
        const int hi = std::min(n - 1, i + half);
        buf.clear();
        for (int j = lo; j <= hi; ++j)
            buf.push_back(ys[order[j]]);
        double v;
        if (median) {
            const size_t mid = buf.size() / 2;
            std::nth_element(buf.begin(), buf.begin() + mid, buf.end());
            v = buf[mid];
            if (buf.size() % 2 == 0) {
                const double lower = *std::max_element(buf.begin(), buf.begin() + mid);
                v = 0.5 * (v + lower);
            }
        } else {
            v = std::accumulate(buf.begin(), buf.end(), 0.0) / double(buf.size());
        }
        outX.push_back(xs[order[i]]);
        outY.push_back(v);
    }
}

// Detect values that an unnaturally large number of points share exactly
// (failed-fit sentinels like 0, grid boundaries, ...). A value qualifies when
// more than `threshold` points sit exactly on it. Axes whose values are all
// integers are counting axes where repeats are expected → no filtering.
std::vector<double> stackedValues(const std::vector<double>& vals, int threshold)
{
    std::vector<double> finite;
    finite.reserve(vals.size());
    for (double v : vals)
        if (!std::isnan(v))
            finite.push_back(v);
    if (finite.empty())
        return {};

    bool allInt = true;
    for (double v : finite) {
        if (v != std::round(v)) {
            allInt = false;
            break;
        }
    }
    if (allInt)
        return {};

    std::unordered_map<double, int> counts;
    for (double v : finite)
        ++counts[v];

    std::vector<double> stacked;
    for (const auto& [value, count] : counts)
        if (count > threshold)
            stacked.push_back(value);
    std::sort(stacked.begin(), stacked.end());
    return stacked;
}

// Parse a user-entered list of tick positions ("0, 0.5, 1" / "10 20 30")
QVector<double> parseTickList(const QString& text)
{
    QVector<double> out;
    static const QRegularExpression sep("[,;\\s]+");
    for (const QString& part : text.split(sep, Qt::SkipEmptyParts)) {
        bool ok = false;
        const double v = part.toDouble(&ok);
        if (ok)
            out.push_back(v);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool isStackedValue(const std::vector<double>& stacked, double v)
{
    return std::binary_search(stacked.begin(), stacked.end(), v);
}

QString formatValueList(const std::vector<double>& vals, int maxShown = 4)
{
    QStringList parts;
    for (int i = 0; i < int(vals.size()) && i < maxShown; ++i)
        parts << QString::number(vals[i], 'g', 6);
    if (int(vals.size()) > maxShown)
        parts << "…";
    return parts.join(", ");
}

// Classic expander: a full-width "> Name" header that becomes "v Name" when
// expanded; clicking anywhere on the header toggles the content.
QWidget* collapsibleGroup(const QString& title, QWidget* content,
                          bool expanded = false)
{
    auto* box = new QWidget;
    auto* l = new QVBoxLayout(box);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(2);

    auto* header = new QToolButton(box);
    header->setText(title);
    header->setCheckable(true);
    header->setChecked(expanded);
    header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setAutoRaise(true);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    QFont f = header->font();
    f.setBold(true);
    header->setFont(f);

    content->setContentsMargins(12, 0, 0, 0);
    l->addWidget(header);
    l->addWidget(content);
    content->setVisible(expanded);
    QObject::connect(header, &QToolButton::toggled, content,
                     [header, content](bool on) {
                         content->setVisible(on);
                         header->setArrowType(on ? Qt::DownArrow
                                                 : Qt::RightArrow);
                     });
    return box;
}

} // namespace

ProjectPlotDialog::ProjectPlotDialog(std::vector<std::shared_ptr<Star>> allStars,
                                     std::vector<std::shared_ptr<Star>> filteredStars,
                                     std::vector<std::shared_ptr<Star>> selectedStars,
                                     QWidget* parent)
    : QDialog(parent)
    , _allStars(std::move(allStars))
    , _filteredStars(std::move(filteredStars))
    , _selectedStars(std::move(selectedStars))
{
    setWindowTitle(tr("Create Plot"));
    setWindowFlags(windowFlags() | Qt::WindowMinMaxButtonsHint);
    setSizeGripEnabled(true);
    resize(1100, 720);

    _seriesColor = QColor(kSeriesPalette[4].hex);  // Okabe-Ito blue
    _bgColor = QColor(0, 0, 0, 0);                 // transparent by default

    detectNumericFields();
    setupUi();
    onPlotTypeChanged(_typeCombo->currentIndex());
    updatePlot();
}

// ─────────────────────────────────────────────────────────────────────────────
// Field discovery
// ─────────────────────────────────────────────────────────────────────────────

void ProjectPlotDialog::detectNumericFields()
{
    const auto& cols = ColumnPresetManager::instance().allColumns();
    for (const auto& c : cols) {
        if (c.isBoolFlag)
            continue;
        bool numeric = false;
        double d;
        for (const auto& s : _allStars) {
            if (s && numericValue(s->getFieldValue(c.key), d)) {
                numeric = true;
                break;
            }
        }
        // Empty project: fall back to everything that isn't a known string field
        // so the dialog is still usable.
        if (_allStars.empty()) {
            static const QSet<QString> stringFields =
                { "alias", "source_id", "tic", "jname", "spec_class" };
            numeric = !stringFields.contains(c.key);
        }
        if (numeric)
            _numericFields << c.key;
    }
}

QString ProjectPlotDialog::fieldLabel(const QString& key) const
{
    return ColumnPresetManager::instance().displayName(key);
}

void ProjectPlotDialog::populateFieldCombo(QComboBox* combo, const QString& defaultKey,
                                           bool noneOption)
{
    auto* model = new QStandardItemModel(combo);
    const auto& mgr = ColumnPresetManager::instance();

    if (noneOption) {
        auto* none = new QStandardItem(tr("(None)"));
        none->setData(QString(), Qt::UserRole);
        model->appendRow(none);
    }

    QString lastCategory;
    int defaultRow = -1;
    for (const QString& key : _numericFields) {
        const ColumnDef* def = mgr.columnDef(key);
        if (!def)
            continue;
        if (def->category != lastCategory) {
            lastCategory = def->category;
            auto* header = new QStandardItem(def->category);
            header->setFlags(Qt::NoItemFlags);
            QFont f = combo->font();
            f.setBold(true);
            header->setFont(f);
            model->appendRow(header);
        }
        auto* item = new QStandardItem("  " + def->displayName);
        item->setData(key, Qt::UserRole);
        model->appendRow(item);
        if (key == defaultKey)
            defaultRow = model->rowCount() - 1;
    }
    combo->setModel(model);
    if (defaultRow >= 0) {
        combo->setCurrentIndex(defaultRow);
    } else {
        // First selectable item
        for (int i = 0; i < model->rowCount(); ++i) {
            if (model->item(i)->flags() & Qt::ItemIsSelectable) {
                combo->setCurrentIndex(i);
                break;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// UI
// ─────────────────────────────────────────────────────────────────────────────

void ProjectPlotDialog::setupUi()
{
    auto* mainLayout = new QHBoxLayout(this);

    mainLayout->addWidget(buildControlPanel());

    _plot = new QCustomPlot(this);
    _plot->setMinimumSize(500, 400);
    _plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _plot->setAutoAddPlottableToLegend(false);   // legend entries added explicitly
    PanelUtils::stylePlot(_plot);
    mainLayout->addWidget(_plot, 1);
    _marginGroup = new QCPMarginGroup(_plot);   // aligns the colour bar

    _axisFont = _plot->xAxis->labelFont();
    updateFontButton();

    // Overlaid on the plot; appears once the user zooms/pans away from the
    // home view and restores it on click.
    _resetViewBtn = new QPushButton(tr("Reset view"), _plot);
    _resetViewBtn->setCursor(Qt::ArrowCursor);
    _resetViewBtn->adjustSize();
    _resetViewBtn->hide();
    connect(_resetViewBtn, &QPushButton::clicked,
            this, &ProjectPlotDialog::updatePlot);
    _plot->installEventFilter(this);
    positionResetButton();

    auto onRangeChanged = [this] {
        if (!_updatingPlot && _typeCombo->currentIndex() != SkyMap)
            _resetViewBtn->show();
    };
    connect(_plot->xAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
            this, onRangeChanged);
    connect(_plot->yAxis, qOverload<const QCPRange&>(&QCPAxis::rangeChanged),
            this, onRangeChanged);
}

QWidget* ProjectPlotDialog::buildControlPanel()
{
    auto* panel = new QWidget(this);
    panel->setFixedWidth(310);
    auto* panelLayout = new QVBoxLayout(panel);
    panelLayout->setContentsMargins(0, 0, 0, 0);

    // All settings live in a scroll area so that expanding the collapsible
    // groups scrolls instead of resizing the window.
    auto* scroll = new QScrollArea(panel);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* scrollContent = new QWidget(scroll);
    auto* layout = new QVBoxLayout(scrollContent);
    layout->setContentsMargins(0, 0, 0, 0);

    // ── Data ────────────────────────────────────────────────────────────────
    auto* dataGroup = new QGroupBox(tr("Data"), panel);
    auto* dataForm = new QFormLayout(dataGroup);

    _sourceCombo = new QComboBox(dataGroup);
    _sourceCombo->addItem(tr("All stars (%1)").arg(_allStars.size()), int(0));
    if (!_filteredStars.empty())
        _sourceCombo->addItem(tr("Filtered stars (%1)").arg(_filteredStars.size()), int(1));
    if (!_selectedStars.empty())
        _sourceCombo->addItem(tr("Selected stars (%1)").arg(_selectedStars.size()), int(2));
    dataForm->addRow(tr("Source:"), _sourceCombo);

    _typeCombo = new QComboBox(dataGroup);
    _typeCombo->addItems({ tr("Scatter plot"), tr("Histogram"), tr("Sky map") });
    dataForm->addRow(tr("Plot type:"), _typeCombo);

    layout->addWidget(dataGroup);

    // ── Series ──────────────────────────────────────────────────────────────
    // The source/field/marker/color controls edit the selected series.
    auto* seriesGroup = new QGroupBox(tr("Series"), panel);
    auto* seriesLayout = new QVBoxLayout(seriesGroup);
    _seriesList = new QListWidget(seriesGroup);
    _seriesList->setFixedHeight(72);
    _seriesList->setToolTip(tr("Each series is one dataset in the plot.\n"
                               "Double-click a series to rename it (legend label)."));
    seriesLayout->addWidget(_seriesList);
    auto* seriesBtns = new QHBoxLayout();
    auto* addSeriesBtn = new QPushButton(tr("Add"), seriesGroup);
    auto* removeSeriesBtn = new QPushButton(tr("Remove"), seriesGroup);
    seriesBtns->addWidget(addSeriesBtn);
    seriesBtns->addWidget(removeSeriesBtn);
    seriesBtns->addStretch();
    seriesLayout->addLayout(seriesBtns);
    layout->addWidget(seriesGroup);

    // ── Type-specific options ───────────────────────────────────────────────
    _optionsStack = new QStackedWidget(panel);

    // Scatter page
    auto* scatterPage = new QGroupBox(tr("Axes"), panel);
    auto* scatterForm = new QFormLayout(scatterPage);
    _xFieldCombo = new QComboBox(scatterPage);
    _yFieldCombo = new QComboBox(scatterPage);
    populateFieldCombo(_xFieldCombo, "teff");
    populateFieldCombo(_yFieldCombo, "logg");
    // Swap button sits between the row labels and the dropdowns, vertically
    // centred across both axis rows. Plain ↑↓ arrows render everywhere,
    // unlike the fancier combined-arrow glyphs.
    auto* swapBtn = new QToolButton(scatterPage);
    swapBtn->setText(QStringLiteral("↑↓"));
    swapBtn->setAutoRaise(true);
    swapBtn->setToolTip(tr("Swap X and Y axes"));
    connect(swapBtn, &QToolButton::clicked, this, &ProjectPlotDialog::onSwapAxes);

    auto* axesGrid = new QGridLayout();
    axesGrid->setContentsMargins(0, 0, 0, 0);
    axesGrid->addWidget(new QLabel(tr("X axis:"), scatterPage), 0, 0);
    axesGrid->addWidget(new QLabel(tr("Y axis:"), scatterPage), 1, 0);
    axesGrid->addWidget(swapBtn, 0, 1, 2, 1, Qt::AlignVCenter);
    axesGrid->addWidget(_xFieldCombo, 0, 2);
    axesGrid->addWidget(_yFieldCombo, 1, 2);
    axesGrid->setColumnStretch(2, 1);
    scatterForm->addRow(axesGrid);

    auto* xOpts = new QHBoxLayout();
    _logXCheck = new QCheckBox(tr("log"), scatterPage);
    _invXCheck = new QCheckBox(tr("invert"), scatterPage);
    xOpts->addWidget(_logXCheck, 1);
    xOpts->addWidget(_invXCheck, 1);
    scatterForm->addRow(tr("X options:"), xOpts);

    auto* yOpts = new QHBoxLayout();
    _logYCheck = new QCheckBox(tr("log"), scatterPage);
    _invYCheck = new QCheckBox(tr("invert"), scatterPage);
    yOpts->addWidget(_logYCheck, 1);
    yOpts->addWidget(_invYCheck, 1);
    scatterForm->addRow(tr("Y options:"), yOpts);

    _colorByCombo = new QComboBox(scatterPage);
    populateFieldCombo(_colorByCombo, QString(), true);
    _colorByCombo->setToolTip(tr("Encode a third field in the marker colour "
                                 "(adds a colour bar)"));
    scatterForm->addRow(tr("Color by:"), _colorByCombo);

    _sizeByCombo = new QComboBox(scatterPage);
    populateFieldCombo(_sizeByCombo, QString(), true);
    _sizeByCombo->setToolTip(tr("Encode a third field in the marker size"));
    scatterForm->addRow(tr("Size by:"), _sizeByCombo);

    _trendCombo = new QComboBox(scatterPage);
    _trendCombo->addItems({ tr("None"), tr("Running mean"), tr("Running median") });
    scatterForm->addRow(tr("Trend curve:"), _trendCombo);

    _trendWindowSpin = new QSpinBox(scatterPage);
    _trendWindowSpin->setRange(3, 501);
    _trendWindowSpin->setSingleStep(2);
    _trendWindowSpin->setValue(21);
    _trendWindowSpin->setSuffix(tr(" pts"));
    _trendWindowSpin->setEnabled(false);
    scatterForm->addRow(tr("Window:"), _trendWindowSpin);

    _optionsStack->addWidget(scatterPage);

    // Histogram page
    auto* histPage = new QGroupBox(tr("Histogram"), panel);
    auto* histForm = new QFormLayout(histPage);
    _histFieldCombo = new QComboBox(histPage);
    populateFieldCombo(_histFieldCombo, "teff");
    _binsSpin = new QSpinBox(histPage);
    _binsSpin->setRange(2, 500);
    _binsSpin->setValue(30);
    histForm->addRow(tr("Field:"), _histFieldCombo);
    histForm->addRow(tr("Bins:"), _binsSpin);

    auto* histXOpts = new QHBoxLayout();
    _histLogXCheck = new QCheckBox(tr("log"), histPage);
    _histInvXCheck = new QCheckBox(tr("invert"), histPage);
    histXOpts->addWidget(_histLogXCheck, 1);
    histXOpts->addWidget(_histInvXCheck, 1);
    histForm->addRow(tr("X options:"), histXOpts);

    _histLogYCheck = new QCheckBox(tr("log"), histPage);
    histForm->addRow(tr("Y options:"), _histLogYCheck);

    _histLogBinsCheck = new QCheckBox(tr("Log-spaced bins"), histPage);
    _histLogBinsCheck->setEnabled(false);
    histForm->addRow(QString(), _histLogBinsCheck);

    _optionsStack->addWidget(histPage);

    // Sky page
    auto* skyPage = new QGroupBox(tr("Sky map"), panel);
    auto* skyLayout = new QVBoxLayout(skyPage);
    _skyGridCheck = new QCheckBox(tr("Show coordinate grid"), skyPage);
    _skyGridCheck->setChecked(true);
    skyLayout->addWidget(_skyGridCheck);
    auto* skyInfo = new QLabel(tr("Equal-area Hammer projection of RA/Dec, "
                                  "RA = 0h centred, east to the left."), skyPage);
    skyInfo->setWordWrap(true);
    skyLayout->addWidget(skyInfo);
    skyLayout->addStretch();
    _optionsStack->addWidget(skyPage);

    layout->addWidget(_optionsStack);

    // ── Overlays (collapsible) ──────────────────────────────────────────────
    auto* overlaysContent = new QWidget(panel);
    auto* overlaysLayout = new QVBoxLayout(overlaysContent);
    overlaysLayout->setContentsMargins(0, 0, 0, 0);
    _overlayList = new QListWidget(overlaysContent);
    _overlayList->setFixedHeight(72);
    _overlayList->setToolTip(tr("Lines (isochrones, tracks, ...) and shaded "
                                "region boundaries loaded from file.\n"
                                "Double-click an overlay to edit it."));
    overlaysLayout->addWidget(_overlayList);
    auto* overlayBtns = new QHBoxLayout();
    auto* addOverlayBtn = new QPushButton(tr("Add"), overlaysContent);
    auto* addOverlayMenu = new QMenu(addOverlayBtn);
    addOverlayMenu->addAction(tr("Line from file…"), this,
        [this] { addOverlayOfType(OverlayConfig::Line); });
    addOverlayMenu->addAction(tr("Region boundary from file…"), this,
        [this] { addOverlayOfType(OverlayConfig::Region); });
    addOverlayBtn->setMenu(addOverlayMenu);
    auto* editOverlayBtn = new QPushButton(tr("Edit"), overlaysContent);
    auto* removeOverlayBtn = new QPushButton(tr("Remove"), overlaysContent);
    overlayBtns->addWidget(addOverlayBtn);
    overlayBtns->addWidget(editOverlayBtn);
    overlayBtns->addWidget(removeOverlayBtn);
    overlayBtns->addStretch();
    overlaysLayout->addLayout(overlayBtns);
    connect(editOverlayBtn, &QPushButton::clicked,
            this, &ProjectPlotDialog::onEditOverlay);
    connect(removeOverlayBtn, &QPushButton::clicked,
            this, &ProjectPlotDialog::onRemoveOverlay);
    connect(_overlayList, &QListWidget::itemDoubleClicked,
            this, &ProjectPlotDialog::onEditOverlay);

    _overlaysGroup = collapsibleGroup(tr("Overlays && annotations"), overlaysContent);
    layout->addWidget(_overlaysGroup);

    // ── Axis limits & filtering (collapsible) ───────────────────────────────
    auto* limitsContent = new QWidget(panel);
    auto* limitsForm = new QFormLayout(limitsContent);
    limitsForm->setContentsMargins(0, 0, 0, 0);
    auto makeLimitEdit = [this](QWidget* parent) {
        auto* e = new QLineEdit(parent);
        e->setPlaceholderText(tr("auto"));
        auto* val = new QDoubleValidator(e);
        val->setNotation(QDoubleValidator::ScientificNotation);
        e->setValidator(val);
        connect(e, &QLineEdit::editingFinished,
                this, &ProjectPlotDialog::updatePlot);
        return e;
    };
    _xMinEdit = makeLimitEdit(limitsContent);
    _xMaxEdit = makeLimitEdit(limitsContent);
    _yMinEdit = makeLimitEdit(limitsContent);
    _yMaxEdit = makeLimitEdit(limitsContent);

    auto* xLimRow = new QHBoxLayout();
    xLimRow->addWidget(_xMinEdit, 1);
    xLimRow->addWidget(new QLabel("–", limitsContent));
    xLimRow->addWidget(_xMaxEdit, 1);
    limitsForm->addRow(tr("X range:"), xLimRow);

    auto* yLimRow = new QHBoxLayout();
    yLimRow->addWidget(_yMinEdit, 1);
    yLimRow->addWidget(new QLabel("–", limitsContent));
    yLimRow->addWidget(_yMaxEdit, 1);
    limitsForm->addRow(tr("Y range:"), yLimRow);

    _hideStackedCheck = new QCheckBox(tr("Hide stacked values"), limitsContent);
    _hideStackedCheck->setChecked(true);
    _hideStackedCheck->setToolTip(
        tr("Hide values that more than N stars share exactly\n"
           "(failed-fit markers like 0, grid boundaries, ...).\n"
           "Uncheck to plot all values."));
    _stackedNSpin = new QSpinBox(limitsContent);
    _stackedNSpin->setRange(1, 100000);
    _stackedNSpin->setValue(5);
    _stackedNSpin->setPrefix(tr("> "));
    _stackedNSpin->setSuffix(tr(" pts"));
    _stackedNSpin->setToolTip(_hideStackedCheck->toolTip());
    connect(_hideStackedCheck, &QCheckBox::toggled,
            _stackedNSpin, &QSpinBox::setEnabled);
    // Separate rows: side by side, the checkbox text plus the spin box's
    // widest value exceed the panel width.
    limitsForm->addRow(QString(), _hideStackedCheck);
    limitsForm->addRow(tr("Threshold:"), _stackedNSpin);

    _limitsGroup = collapsibleGroup(tr("Axis limits && filtering"), limitsContent);
    layout->addWidget(_limitsGroup);

    // ── Style (collapsible) ─────────────────────────────────────────────────
    auto* styleContent = new QWidget(panel);
    _styleForm = new QFormLayout(styleContent);
    _styleForm->setContentsMargins(0, 0, 0, 0);

    _markerCombo = new QComboBox(styleContent);
    for (const auto& m : kMarkers)
        _markerCombo->addItem(tr(m.name));
    _styleForm->addRow(tr("Marker:"), _markerCombo);

    _markerSizeSpin = new QSpinBox(styleContent);
    _markerSizeSpin->setRange(1, 30);
    _markerSizeSpin->setValue(6);
    _styleForm->addRow(tr("Size:"), _markerSizeSpin);

    _allPointsRadio = new QRadioButton(tr("Draw all points"), styleContent);
    _cullRadio = new QRadioButton(tr("Cull dense points"), styleContent);
    _allPointsRadio->setChecked(true);
    _cullRadio->setToolTip(
        tr("Faster for very large datasets: skips points that overlap\n"
           "at the current zoom level. Exported plots always contain\n"
           "every point regardless of this setting."));
    _allPointsRadio->setToolTip(_cullRadio->toolTip());
    auto* renderCol = new QVBoxLayout();
    renderCol->setSpacing(2);
    renderCol->addWidget(_allPointsRadio);
    renderCol->addWidget(_cullRadio);
    _styleForm->addRow(tr("Rendering:"), renderCol);

    _colorBtn = new QPushButton(styleContent);
    _colorBtn->setFixedSize(60, 22);
    _styleForm->addRow(tr("Color:"), _colorBtn);

    _bgColorBtn = new QPushButton(styleContent);
    _bgColorBtn->setFixedSize(60, 22);
    _styleForm->addRow(tr("Background:"), _bgColorBtn);
    updateColorSwatch();

    _axisWidthSpin = new QDoubleSpinBox(styleContent);
    _axisWidthSpin->setRange(0.5, 5.0);
    _axisWidthSpin->setSingleStep(0.25);
    _axisWidthSpin->setValue(1.0);
    _axisWidthSpin->setSuffix(" px");
    _styleForm->addRow(tr("Axis width:"), _axisWidthSpin);

    _minorTicksCheck = new QCheckBox(tr("Minor ticks"), styleContent);
    _minorTicksCheck->setChecked(true);
    _styleForm->addRow(QString(), _minorTicksCheck);

    _gridCheck = new QCheckBox(tr("Show grid"), styleContent);
    _gridCheck->setChecked(true);
    _styleForm->addRow(QString(), _gridCheck);

    _xTicksEdit = new QLineEdit(styleContent);
    _xTicksEdit->setPlaceholderText(tr("auto"));
    _xTicksEdit->setToolTip(tr("Comma-separated major tick positions, e.g. 0, 0.5, 1"));
    _styleForm->addRow(tr("X ticks:"), _xTicksEdit);

    _yTicksEdit = new QLineEdit(styleContent);
    _yTicksEdit->setPlaceholderText(tr("auto"));
    _yTicksEdit->setToolTip(_xTicksEdit->toolTip());
    _styleForm->addRow(tr("Y ticks:"), _yTicksEdit);

    _xLabelEdit = new QLineEdit(styleContent);
    _xLabelEdit->setPlaceholderText(tr("auto"));
    _xLabelEdit->setToolTip(tr("Custom axis label; leave empty to use the field name"));
    _styleForm->addRow(tr("X label:"), _xLabelEdit);

    _yLabelEdit = new QLineEdit(styleContent);
    _yLabelEdit->setPlaceholderText(tr("auto"));
    _yLabelEdit->setToolTip(_xLabelEdit->toolTip());
    _styleForm->addRow(tr("Y label:"), _yLabelEdit);

    _fontBtn = new QPushButton(styleContent);
    _fontBtn->setToolTip(tr("Font for the axis labels and tick labels"));
    // Long family names must not widen the fixed-width panel
    _fontBtn->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    _styleForm->addRow(tr("Label font:"), _fontBtn);

    _titleEdit = new QLineEdit(styleContent);
    _titleEdit->setPlaceholderText(tr("(no title)"));
    _styleForm->addRow(tr("Title:"), _titleEdit);

    layout->addWidget(collapsibleGroup(tr("Style"), styleContent));
    layout->addStretch();

    scroll->setWidget(scrollContent);
    panelLayout->addWidget(scroll, 1);

    // ── Status + actions (fixed below the scroll area) ──────────────────────
    _statusLabel = new QLabel(panel);
    _statusLabel->setWordWrap(true);
    panelLayout->addWidget(_statusLabel);

    auto* btnRow = new QHBoxLayout();
    auto* exportBtn = new QPushButton(tr("Export…"), panel);
    auto* closeBtn = new QPushButton(tr("Close"), panel);
    btnRow->addWidget(exportBtn);
    btnRow->addStretch();
    btnRow->addWidget(closeBtn);
    panelLayout->addLayout(btnRow);

    // ── Wiring ──────────────────────────────────────────────────────────────
    connect(_typeCombo, &QComboBox::currentIndexChanged,
            this, &ProjectPlotDialog::onPlotTypeChanged);

    for (QComboBox* cb : { _sourceCombo, _typeCombo, _xFieldCombo,
                           _yFieldCombo, _histFieldCombo, _markerCombo,
                           _trendCombo, _colorByCombo, _sizeByCombo })
        connect(cb, &QComboBox::currentIndexChanged,
                this, &ProjectPlotDialog::updatePlot);
    for (QCheckBox* ck : { _logXCheck, _logYCheck, _invXCheck, _invYCheck,
                           _histLogXCheck, _histLogYCheck, _histInvXCheck,
                           _histLogBinsCheck, _skyGridCheck, _hideStackedCheck,
                           _minorTicksCheck, _gridCheck })
        connect(ck, &QCheckBox::toggled, this, &ProjectPlotDialog::updatePlot);
    for (QSpinBox* sp : { _binsSpin, _markerSizeSpin, _trendWindowSpin,
                          _stackedNSpin })
        connect(sp, &QSpinBox::valueChanged, this, &ProjectPlotDialog::updatePlot);
    connect(_axisWidthSpin, &QDoubleSpinBox::valueChanged,
            this, &ProjectPlotDialog::updatePlot);
    for (QLineEdit* le : { _xTicksEdit, _yTicksEdit, _xLabelEdit, _yLabelEdit })
        connect(le, &QLineEdit::editingFinished,
                this, &ProjectPlotDialog::updatePlot);
    connect(_fontBtn, &QPushButton::clicked, this, &ProjectPlotDialog::onPickFont);
    connect(_cullRadio, &QRadioButton::toggled,
            this, &ProjectPlotDialog::updatePlot);

    connect(_trendCombo, &QComboBox::currentIndexChanged, this,
            [this](int i) { _trendWindowSpin->setEnabled(i != 0); });
    connect(_histLogXCheck, &QCheckBox::toggled,
            _histLogBinsCheck, &QCheckBox::setEnabled);

    connect(_titleEdit, &QLineEdit::textChanged,
            this, &ProjectPlotDialog::updatePlot);
    connect(_colorBtn, &QPushButton::clicked, this, &ProjectPlotDialog::onPickColor);
    connect(_bgColorBtn, &QPushButton::clicked, this, &ProjectPlotDialog::onPickBgColor);
    connect(exportBtn, &QPushButton::clicked, this, &ProjectPlotDialog::onExport);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::close);

    connect(addSeriesBtn, &QPushButton::clicked,
            this, &ProjectPlotDialog::onAddSeries);
    connect(removeSeriesBtn, &QPushButton::clicked,
            this, &ProjectPlotDialog::onRemoveSeries);
    connect(_seriesList, &QListWidget::currentRowChanged,
            this, &ProjectPlotDialog::onSeriesSelectionChanged);
    connect(_seriesList, &QListWidget::itemChanged,
            this, &ProjectPlotDialog::onSeriesRenamed);

    // Initial series from the controls' default state
    SeriesConfig first;
    first.label = tr("Series 1");
    first.sourceId = _sourceCombo->currentData().toInt();
    first.xKey = _xFieldCombo->currentData(Qt::UserRole).toString();
    first.yKey = _yFieldCombo->currentData(Qt::UserRole).toString();
    first.histKey = _histFieldCombo->currentData(Qt::UserRole).toString();
    first.markerIndex = _markerCombo->currentIndex();
    first.color = _seriesColor;
    _series.push_back(first);
    auto* firstItem = new QListWidgetItem(first.label);
    firstItem->setFlags(firstItem->flags() | Qt::ItemIsEditable);
    _seriesList->addItem(firstItem);
    _seriesList->setCurrentRow(0);

    return panel;
}

void ProjectPlotDialog::onAddSeries()
{
    syncUiToSeries();
    SeriesConfig cfg = _series[_seriesList->currentRow()];
    cfg.label = tr("Series %1").arg(_series.size() + 1);
    constexpr int paletteCount = int(sizeof(kSeriesPalette) / sizeof(kSeriesPalette[0]));
    cfg.color = QColor(kSeriesPalette[int(_series.size()) % paletteCount].hex);
    _series.push_back(cfg);

    auto* item = new QListWidgetItem(cfg.label);
    item->setFlags(item->flags() | Qt::ItemIsEditable);
    _seriesList->addItem(item);
    _seriesList->setCurrentRow(_seriesList->count() - 1);
    updatePlot();
}

void ProjectPlotDialog::onRemoveSeries()
{
    const int row = _seriesList->currentRow();
    if (_series.size() <= 1 || row < 0)
        return;
    _series.erase(_series.begin() + row);
    delete _seriesList->takeItem(row);   // selection change reloads the UI
    updatePlot();
}

void ProjectPlotDialog::onSeriesSelectionChanged(int row)
{
    if (row >= 0 && row < int(_series.size()))
        loadSeriesIntoUi(_series[row]);
}

void ProjectPlotDialog::onSeriesRenamed(QListWidgetItem* item)
{
    const int row = _seriesList->row(item);
    if (row < 0 || row >= int(_series.size()))
        return;
    _series[row].label = item->text();
    updatePlot();
}

void ProjectPlotDialog::syncUiToSeries()
{
    const int row = _seriesList ? _seriesList->currentRow() : -1;
    if (row < 0 || row >= int(_series.size()))
        return;
    SeriesConfig& s = _series[row];
    s.sourceId    = _sourceCombo->currentData().toInt();
    s.xKey        = _xFieldCombo->currentData(Qt::UserRole).toString();
    s.yKey        = _yFieldCombo->currentData(Qt::UserRole).toString();
    s.histKey     = _histFieldCombo->currentData(Qt::UserRole).toString();
    s.markerIndex = _markerCombo->currentIndex();
    s.color       = _seriesColor;
}

void ProjectPlotDialog::loadSeriesIntoUi(const SeriesConfig& s)
{
    const QSignalBlocker b1(_sourceCombo), b2(_xFieldCombo), b3(_yFieldCombo);
    const QSignalBlocker b4(_histFieldCombo), b5(_markerCombo);
    const int srcIdx = _sourceCombo->findData(s.sourceId);
    if (srcIdx >= 0)
        _sourceCombo->setCurrentIndex(srcIdx);
    setComboKey(_xFieldCombo, s.xKey);
    setComboKey(_yFieldCombo, s.yKey);
    setComboKey(_histFieldCombo, s.histKey);
    if (s.markerIndex >= 0 && s.markerIndex < _markerCombo->count())
        _markerCombo->setCurrentIndex(s.markerIndex);
    _seriesColor = s.color;
    updateColorSwatch();
}

void ProjectPlotDialog::setComboKey(QComboBox* combo, const QString& key)
{
    for (int i = 0; i < combo->count(); ++i) {
        if (combo->itemData(i, Qt::UserRole).toString() == key
            && (combo->model()->flags(combo->model()->index(i, 0))
                & Qt::ItemIsSelectable)) {
            combo->setCurrentIndex(i);
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Overlays
// ─────────────────────────────────────────────────────────────────────────────

void ProjectPlotDialog::addOverlayOfType(int type)
{
    OverlayConfig ov;
    ov.type = type;
    ov.color = QColor(kSeriesPalette[5].hex);   // vermillion

    const QString path = QFileDialog::getOpenFileName(
        this,
        type == OverlayConfig::Region ? tr("Load Region Boundary")
                                      : tr("Load Line / Track / Isochrone"),
        QDir::homePath(),
        tr("Data files (*.dat *.txt *.csv *.iso);;All files (*)"));
    if (path.isEmpty())
        return;
    ov.filePath = path;
    ov.label = QFileInfo(path).completeBaseName();

    if (!editOverlayDialog(ov))
        return;
    _overlays.push_back(std::move(ov));
    _overlayList->addItem(overlayDisplayText(_overlays.back()));
    updatePlot();
}

void ProjectPlotDialog::onEditOverlay()
{
    const int row = _overlayList->currentRow();
    if (row < 0 || row >= int(_overlays.size()))
        return;
    if (!editOverlayDialog(_overlays[row]))
        return;
    _overlayList->item(row)->setText(overlayDisplayText(_overlays[row]));
    updatePlot();
}

void ProjectPlotDialog::onRemoveOverlay()
{
    const int row = _overlayList->currentRow();
    if (row < 0 || row >= int(_overlays.size()))
        return;
    _overlays.erase(_overlays.begin() + row);
    delete _overlayList->takeItem(row);
    updatePlot();
}

QString ProjectPlotDialog::overlayDisplayText(const OverlayConfig& ov) const
{
    const QString kind = ov.type == OverlayConfig::Region ? tr("Region")
                                                          : tr("Line");
    const QString name = ov.label.isEmpty()
                             ? QFileInfo(ov.filePath).fileName() : ov.label;
    return QString("%1: %2").arg(kind, name);
}

bool ProjectPlotDialog::loadTrackFile(OverlayConfig& ov, QString* err)
{
    ov.tx.clear();
    ov.ty.clear();
    QFile f(ov.filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = tr("Cannot open %1").arg(ov.filePath);
        return false;
    }
    QTextStream ts(&f);
    static const QRegularExpression sep("[,;\\s\\t]+");
    const int ix = ov.colX - 1;
    const int iy = ov.colY - 1;
    while (!ts.atEnd()) {
        const QString line = ts.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith("//")
            || line.startsWith('!'))
            continue;
        const QStringList parts = line.split(sep, Qt::SkipEmptyParts);
        if (parts.size() <= std::max(ix, iy))
            continue;
        bool okx = false, oky = false;
        const double x = parts[ix].toDouble(&okx);
        const double y = parts[iy].toDouble(&oky);
        if (okx && oky) {
            ov.tx.push_back(x);
            ov.ty.push_back(y);
        }
    }
    if (ov.tx.isEmpty()) {
        if (err)
            *err = tr("No usable numeric data in columns %1/%2 of %3")
                       .arg(ov.colX).arg(ov.colY).arg(QFileInfo(ov.filePath).fileName());
        return false;
    }
    return true;
}

bool ProjectPlotDialog::editOverlayDialog(OverlayConfig& ov)
{
    QDialog dlg(this);
    dlg.setWindowTitle(ov.type == OverlayConfig::Region ? tr("Region Overlay")
                                                        : tr("Line Overlay"));
    auto* form = new QFormLayout(&dlg);

    auto* labelEdit = new QLineEdit(ov.label, &dlg);
    labelEdit->setPlaceholderText(tr("legend label"));
    form->addRow(tr("Label:"), labelEdit);

    auto* fileRow = new QHBoxLayout();
    auto* fileLabel = new QLabel(QFileInfo(ov.filePath).fileName(), &dlg);
    fileLabel->setToolTip(ov.filePath);
    auto* chooseBtn = new QPushButton(tr("Choose…"), &dlg);
    fileRow->addWidget(fileLabel, 1);
    fileRow->addWidget(chooseBtn);
    form->addRow(tr("File:"), fileRow);
    connect(chooseBtn, &QPushButton::clicked, &dlg, [&ov, fileLabel, this] {
        const QString path = QFileDialog::getOpenFileName(
            this,
            ov.type == OverlayConfig::Region ? tr("Load Region Boundary")
                                             : tr("Load Line / Track / Isochrone"),
            QFileInfo(ov.filePath).absolutePath(),
            tr("Data files (*.dat *.txt *.csv *.iso);;All files (*)"));
        if (!path.isEmpty()) {
            ov.filePath = path;
            fileLabel->setText(QFileInfo(path).fileName());
            fileLabel->setToolTip(path);
        }
    });

    auto* colXSpin = new QSpinBox(&dlg);
    colXSpin->setRange(1, 99);
    colXSpin->setValue(ov.colX);
    auto* colYSpin = new QSpinBox(&dlg);
    colYSpin->setRange(1, 99);
    colYSpin->setValue(ov.colY);
    form->addRow(tr("X column:"), colXSpin);
    form->addRow(tr("Y column:"), colYSpin);

    auto* colorBtn = new QPushButton(&dlg);
    colorBtn->setFixedSize(60, 22);
    QColor cur = ov.color;
    auto updateSwatch = [&cur, colorBtn] {
        colorBtn->setStyleSheet(
            QString("background-color: %1; border: 1px solid gray;").arg(cur.name()));
    };
    updateSwatch();
    connect(colorBtn, &QPushButton::clicked, &dlg, [&] {
        const QColor chosen = pickColor(colorBtn, cur);
        if (chosen.isValid()) {
            cur = chosen;
            updateSwatch();
        }
    });
    form->addRow(tr("Color:"), colorBtn);

    auto* widthSpin = new QDoubleSpinBox(&dlg);
    widthSpin->setRange(0.5, 10.0);
    widthSpin->setSingleStep(0.5);
    widthSpin->setValue(ov.width);
    widthSpin->setSuffix(" px");
    form->addRow(tr("Width:"), widthSpin);

    auto* styleCombo = new QComboBox(&dlg);
    styleCombo->addItems({ tr("Solid"), tr("Dashed"), tr("Dotted") });
    styleCombo->setCurrentIndex(std::clamp(ov.penStyle, 0, 2));
    form->addRow(tr("Style:"), styleCombo);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    form->addRow(buttons);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    ov.label    = labelEdit->text().trimmed();
    ov.color    = cur;
    ov.width    = widthSpin->value();
    ov.penStyle = styleCombo->currentIndex();
    ov.colX     = colXSpin->value();
    ov.colY     = colYSpin->value();
    QString err;
    if (!loadTrackFile(ov, &err))
        QMessageBox::warning(this, tr("Overlay Load Failed"), err);
    return true;
}

void ProjectPlotDialog::drawOverlays()
{
    if (_overlays.empty() || _typeCombo->currentIndex() == SkyMap)
        return;

    auto penStyleOf = [](int s) {
        switch (s) {
        case 1:  return Qt::DashLine;
        case 2:  return Qt::DotLine;
        default: return Qt::SolidLine;
        }
    };

    bool legendAdds = false;
    for (auto& ov : _overlays) {
        if (ov.tx.isEmpty()) {
            QString err;
            if (!loadTrackFile(ov, &err))
                continue;
        }

        QVector<double> xs = ov.tx;
        QVector<double> ys = ov.ty;
        // Close the polygon outline of a region boundary
        if (ov.type == OverlayConfig::Region
            && (xs.first() != xs.last() || ys.first() != ys.last())) {
            xs.push_back(xs.first());
            ys.push_back(ys.first());
        }

        auto* curve = new QCPCurve(_plot->xAxis, _plot->yAxis);
        QVector<double> t(xs.size());
        std::iota(t.begin(), t.end(), 0.0);
        curve->setData(t, xs, ys);
        curve->setPen(QPen(ov.color, ov.width, penStyleOf(ov.penStyle)));
        curve->setLineStyle(QCPCurve::lsLine);
        curve->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssNone));
        if (ov.type == OverlayConfig::Region) {
            QColor fill = ov.color;
            fill.setAlpha(60);
            curve->setBrush(fill);
        }
        curve->setName(ov.label.isEmpty() ? QFileInfo(ov.filePath).fileName()
                                          : ov.label);
        curve->addToLegend();
        legendAdds = true;
    }
    if (legendAdds)
        _plot->legend->setVisible(true);
}

void ProjectPlotDialog::onSwapAxes()
{
    {
        const QSignalBlocker b1(_xFieldCombo), b2(_yFieldCombo);
        const QSignalBlocker b3(_logXCheck), b4(_logYCheck);
        const QSignalBlocker b5(_invXCheck), b6(_invYCheck);

        const int xi = _xFieldCombo->currentIndex();
        _xFieldCombo->setCurrentIndex(_yFieldCombo->currentIndex());
        _yFieldCombo->setCurrentIndex(xi);

        const bool log = _logXCheck->isChecked();
        _logXCheck->setChecked(_logYCheck->isChecked());
        _logYCheck->setChecked(log);
        const bool inv = _invXCheck->isChecked();
        _invXCheck->setChecked(_invYCheck->isChecked());
        _invYCheck->setChecked(inv);

        // Limits and custom ticks follow their data to the other axis
        QString t = _xMinEdit->text();
        _xMinEdit->setText(_yMinEdit->text());
        _yMinEdit->setText(t);
        t = _xMaxEdit->text();
        _xMaxEdit->setText(_yMaxEdit->text());
        _yMaxEdit->setText(t);
        t = _xTicksEdit->text();
        _xTicksEdit->setText(_yTicksEdit->text());
        _yTicksEdit->setText(t);
        t = _xLabelEdit->text();
        _xLabelEdit->setText(_yLabelEdit->text());
        _yLabelEdit->setText(t);
    }
    updatePlot();
}

void ProjectPlotDialog::onPlotTypeChanged(int index)
{
    _optionsStack->setCurrentIndex(index);
    // Markers are meaningless for histograms; limits don't apply to the
    // fixed-projection sky map.
    const bool markers = (index != Histogram);
    _styleForm->setRowVisible(_markerCombo, markers);
    _styleForm->setRowVisible(_markerSizeSpin, markers);
    _limitsGroup->setEnabled(index != SkyMap);
    _overlaysGroup->setEnabled(index != SkyMap);   // projection coords

    // Cartesian axis styling doesn't apply to the sky map (axes are hidden,
    // the graticule has its own toggle).
    const bool cartesian = (index != SkyMap);
    for (QWidget* w : std::initializer_list<QWidget*>{
             _axisWidthSpin, _minorTicksCheck, _gridCheck,
             _xTicksEdit, _yTicksEdit, _xLabelEdit, _yLabelEdit })
        _styleForm->setRowVisible(w, cartesian);
}

// ─────────────────────────────────────────────────────────────────────────────
// Colors
// ─────────────────────────────────────────────────────────────────────────────

void ProjectPlotDialog::updateColorSwatch()
{
    _colorBtn->setStyleSheet(QString("background-color: %1; border: 1px solid gray;")
                                 .arg(_seriesColor.name()));
    if (_bgColor.isValid() && _bgColor.alpha() == 0) {
        _bgColorBtn->setStyleSheet("border: 1px solid gray;");
        _bgColorBtn->setText(tr("none"));
    } else {
        const QColor bg = _bgColor.isValid() ? _bgColor : PanelUtils::themeBg();
        _bgColorBtn->setStyleSheet(
            QString("background-color: %1; border: 1px solid gray;").arg(bg.name()));
        _bgColorBtn->setText(_bgColor.isValid() ? QString() : tr("theme"));
    }
}

QColor ProjectPlotDialog::pickColor(QPushButton* anchor, const QColor& current,
                                    bool background, bool* themeDefault)
{
    QMenu menu(this);
    QColor result;
    bool customRequested = false;

    if (themeDefault) {
        *themeDefault = false;
        QAction* def = menu.addAction(tr("Theme default"));
        connect(def, &QAction::triggered, this,
                [themeDefault] { *themeDefault = true; });
    }
    if (background) {
        QAction* transp = menu.addAction(tr("Transparent"));
        connect(transp, &QAction::triggered, this,
                [&result] { result = QColor(0, 0, 0, 0); });
    }
    if (themeDefault || background)
        menu.addSeparator();

    auto* grid = new QWidget(&menu);
    auto* gl = new QGridLayout(grid);
    gl->setContentsMargins(8, 4, 8, 4);
    gl->setSpacing(4);
    auto addSwatch = [&](const char* name, const char* hex, int idx) {
        auto* swatch = new QPushButton(grid);
        swatch->setFixedSize(26, 26);
        swatch->setToolTip(tr(name));
        swatch->setStyleSheet(QString("background-color: %1; border: 1px solid gray;")
                                  .arg(hex));
        const QColor c(hex);
        connect(swatch, &QPushButton::clicked, &menu, [&result, c, &menu] {
            result = c;
            menu.close();
        });
        gl->addWidget(swatch, idx / 5, idx % 5);
    };
    if (background) {
        int i = 0;
        for (const auto& entry : kBackgroundPalette)
            addSwatch(entry.name, entry.hex, i++);
    } else {
        int i = 0;
        for (const auto& entry : kSeriesPalette)
            addSwatch(entry.name, entry.hex, i++);
    }
    auto* wa = new QWidgetAction(&menu);
    wa->setDefaultWidget(grid);
    menu.addAction(wa);

    menu.addSeparator();
    QAction* customAct = menu.addAction(tr("Custom color…"));
    connect(customAct, &QAction::triggered, this,
            [&customRequested] { customRequested = true; });

    menu.exec(anchor->mapToGlobal(QPoint(0, anchor->height())));

    if (customRequested)
        return QColorDialog::getColor(current, this, tr("Select Color"));
    return result;
}

void ProjectPlotDialog::onPickColor()
{
    QColor c = pickColor(_colorBtn, _seriesColor);
    if (!c.isValid())
        return;
    _seriesColor = c;
    updateColorSwatch();
    updatePlot();
}

void ProjectPlotDialog::onPickBgColor()
{
    bool themeDefault = false;
    QColor c = pickColor(_bgColorBtn,
                         _bgColor.isValid() ? _bgColor : PanelUtils::themeBg(),
                         true, &themeDefault);
    if (themeDefault)
        _bgColor = QColor();   // back to theme
    else if (c.isValid())
        _bgColor = c;          // alpha 0 → transparent
    else
        return;
    updateColorSwatch();
    updatePlot();
}

void ProjectPlotDialog::onPickFont()
{
    bool ok = false;
    const QFont f = QFontDialog::getFont(&ok, _axisFont, this,
                                         tr("Axis Label Font"));
    if (!ok)
        return;
    _axisFont = f;
    updateFontButton();
    updatePlot();
}

void ProjectPlotDialog::updateFontButton()
{
    if (_fontBtn)
        _fontBtn->setText(QString("%1, %2 pt")
                              .arg(_axisFont.family())
                              .arg(_axisFont.pointSize()));
}

QColor ProjectPlotDialog::effectiveFg() const
{
    // Transparent backgrounds keep theme-appropriate foregrounds
    if (_bgColor.isValid() && _bgColor.alpha() > 0)
        return _bgColor.lightness() < 128 ? QColor(210, 210, 210)
                                          : QColor(30, 30, 30);
    return PanelUtils::isDarkTheme() ? QColor(210, 210, 210) : QColor(30, 30, 30);
}

QColor ProjectPlotDialog::effectiveGrid() const
{
    if (_bgColor.isValid() && _bgColor.alpha() > 0)
        return _bgColor.lightness() < 128 ? QColor(80, 80, 80)
                                          : QColor(200, 200, 200);
    return PanelUtils::isDarkTheme() ? QColor(80, 80, 80) : QColor(200, 200, 200);
}

void ProjectPlotDialog::applyCustomBackground()
{
    if (!_bgColor.isValid())
        return;   // stylePlot() already applied the theme

    if (_bgColor.alpha() == 0) {
        // Transparent: no background fill (exports keep alpha); foreground
        // colours stay theme-appropriate for the on-screen preview.
        _plot->setBackground(QBrush(Qt::transparent));
        _plot->axisRect()->setBackground(QBrush(Qt::NoBrush));
        return;
    }

    const bool dark = _bgColor.lightness() < 128;
    const QColor textColor    = effectiveFg();
    const QColor gridColor    = effectiveGrid();
    const QColor subGridColor = dark ? QColor(55, 55, 55) : QColor(225, 225, 225);

    _plot->setBackground(QBrush(_bgColor));
    _plot->axisRect()->setBackground(QBrush(_bgColor));
    for (auto* axis : { _plot->xAxis, _plot->xAxis2, _plot->yAxis, _plot->yAxis2 }) {
        axis->setBasePen(QPen(textColor, 1));
        axis->setTickPen(QPen(textColor, 1));
        axis->setSubTickPen(QPen(gridColor, 1));
        axis->setLabelColor(textColor);
        axis->setTickLabelColor(textColor);
        axis->grid()->setPen(QPen(gridColor, 0.5, Qt::DotLine));
        axis->grid()->setSubGridPen(QPen(subGridColor, 0.3, Qt::DotLine));
        axis->grid()->setZeroLinePen(QPen(gridColor, 0.8));
    }
    _plot->legend->setBorderPen(QPen(gridColor));
    _plot->legend->setBrush(QBrush(_bgColor));
    _plot->legend->setTextColor(textColor);
}

void ProjectPlotDialog::setStatus(const QString& text)
{
    _statusLabel->setText(text);
}

// ─────────────────────────────────────────────────────────────────────────────
// Data access
// ─────────────────────────────────────────────────────────────────────────────

const std::vector<std::shared_ptr<Star>>& ProjectPlotDialog::starsForSource(int sourceId) const
{
    switch (sourceId) {
    case 1:  return _filteredStars;
    case 2:  return _selectedStars;
    default: return _allStars;
    }
}

std::vector<double>
ProjectPlotDialog::extractField(const std::vector<std::shared_ptr<Star>>& stars,
                                const QString& key) const
{
    std::vector<double> out;
    out.reserve(stars.size());
    for (const auto& s : stars) {
        double d = std::numeric_limits<double>::quiet_NaN();
        if (s)
            numericValue(s->getFieldValue(key), d);
        out.push_back(d);
    }
    return out;
}

bool ProjectPlotDialog::limitValue(const QLineEdit* edit, double& out)
{
    bool ok = false;
    const double v = edit->text().toDouble(&ok);
    if (ok)
        out = v;
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// Plotting
// ─────────────────────────────────────────────────────────────────────────────

void ProjectPlotDialog::resetPlotState()
{
    _plot->clearPlottables();
    _plot->clearItems();

    if (_colorScale) {
        _plot->plotLayout()->remove(_colorScale);
        _plot->plotLayout()->simplify();
        _colorScale = nullptr;
    }

    for (QCPAxis* ax : { _plot->xAxis, _plot->yAxis }) {
        ax->setVisible(true);
        ax->setRangeReversed(false);
        ax->setScaleType(QCPAxis::stLinear);
        ax->setTicker(QSharedPointer<QCPAxisTicker>(new QCPAxisTicker));
        ax->setLabel(QString());
        ax->grid()->setVisible(true);
    }
    PanelUtils::stylePlot(_plot);
    applyCustomBackground();
    applyAxisStyling();

    // Legend defaults; plot functions enable it when several series exist
    _plot->legend->setVisible(false);
    _plot->legend->clearItems();
    _plot->legend->setTextColor(effectiveFg());
    _plot->legend->setBorderPen(QPen(effectiveGrid()));
    QColor legendBg = (_bgColor.isValid() && _bgColor.alpha() > 0)
                          ? _bgColor : PanelUtils::themeBg();
    legendBg.setAlpha(190);
    _plot->legend->setBrush(QBrush(legendBg));
    QFont legendFont = _axisFont;
    if (legendFont.pointSizeF() > 0.0)
        legendFont.setPointSizeF(legendFont.pointSizeF() * 0.9);
    _plot->legend->setFont(legendFont);
}

// Axis line/tick thickness and minor-tick visibility; minor ticks always use
// the same colour as the major ticks (stylePlot/applyCustomBackground give
// them the grid colour, which reads as a different, washed-out hue).
void ProjectPlotDialog::applyAxisStyling()
{
    const double w = _axisWidthSpin ? _axisWidthSpin->value() : 1.0;
    const bool minor = !_minorTicksCheck || _minorTicksCheck->isChecked();
    for (auto* axis : { _plot->xAxis, _plot->xAxis2, _plot->yAxis, _plot->yAxis2 }) {
        QPen base = axis->basePen();
        base.setWidthF(w);
        axis->setBasePen(base);
        QPen tick = axis->tickPen();
        tick.setWidthF(w);
        axis->setTickPen(tick);
        QPen sub = axis->subTickPen();
        sub.setColor(tick.color());
        sub.setWidthF(w);
        axis->setSubTickPen(sub);
        axis->setSubTicks(minor);
        axis->setLabelFont(_axisFont);
        axis->setTickLabelFont(_axisFont);
    }
    const bool grid = !_gridCheck || _gridCheck->isChecked();
    _plot->xAxis->grid()->setVisible(grid);
    _plot->yAxis->grid()->setVisible(grid);
}

// User-specified major tick positions override the automatic ticker
void ProjectPlotDialog::applyCustomTickers()
{
    auto apply = [](QCPAxis* ax, const QString& text) {
        const QVector<double> ticks = parseTickList(text);
        if (ticks.isEmpty())
            return;
        auto ticker = QSharedPointer<QCPAxisTickerText>(new QCPAxisTickerText);
        for (double v : ticks)
            ticker->addTick(v, QString::number(v));
        ax->setTicker(ticker);
    };
    apply(_plot->xAxis, _xTicksEdit->text());
    apply(_plot->yAxis, _yTicksEdit->text());
}

void ProjectPlotDialog::updatePlot()
{
    _updatingPlot = true;
    syncUiToSeries();

    // Third-field encodings only make sense for a single series
    const bool multi = _series.size() > 1;
    if (_colorByCombo) {
        _colorByCombo->setEnabled(!multi);
        _sizeByCombo->setEnabled(!multi);
    }

    resetPlotState();
    updateTitleElement();

    switch (_typeCombo->currentIndex()) {
    case Histogram: plotHistogram(); break;
    case SkyMap:    plotSky();       break;
    case Scatter:
    default:        plotScatter();   break;
    }
    drawOverlays();

    _plot->replot();
    _updatingPlot = false;
    if (_resetViewBtn)
        _resetViewBtn->hide();
}

void ProjectPlotDialog::applyAxisLimits()
{
    auto apply = [](QCPAxis* ax, const QLineEdit* loEdit, const QLineEdit* hiEdit) {
        QCPRange r = ax->range();
        double v;
        if (limitValue(loEdit, v)) r.lower = v;
        if (limitValue(hiEdit, v)) r.upper = v;
        if (r.lower > r.upper) std::swap(r.lower, r.upper);
        // Log axes must stay within one sign domain; follow the sign of the
        // auto-scaled range, i.e. of the plotted data.
        if (ax->scaleType() == QCPAxis::stLogarithmic) {
            if (ax->range().upper < 0.0) {   // negative-data axis
                if (r.upper >= 0.0)
                    r.upper = r.lower < 0.0 ? r.lower * 1e-4 : -1e-4;
            } else if (r.lower <= 0.0) {
                r.lower = r.upper > 1.0 ? r.upper * 1e-4 : 1e-4;
            }
        }
        ax->setRange(r);
    };
    apply(_plot->xAxis, _xMinEdit, _xMaxEdit);
    apply(_plot->yAxis, _yMinEdit, _yMaxEdit);
}

void ProjectPlotDialog::plotScatter()
{
    _plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    const bool logX  = _logXCheck->isChecked();
    const bool logY  = _logYCheck->isChecked();
    const bool cull  = _cullRadio->isChecked();
    const bool multi = _series.size() > 1;
    const int trendMode = _trendCombo->currentIndex();

    // Collect raw values per series first: the log sign domain has to be
    // decided on the combined data of all series.
    struct SeriesData {
        const SeriesConfig* cfg;
        std::vector<double> xs, ys;
        size_t starCount = 0;
    };
    std::vector<SeriesData> data;
    for (const auto& s : _series) {
        if (s.xKey.isEmpty() || s.yKey.isEmpty())
            continue;
        const auto& stars = starsForSource(s.sourceId);
        SeriesData d;
        d.cfg = &s;
        d.xs = extractField(stars, s.xKey);
        d.ys = extractField(stars, s.yKey);
        d.starCount = stars.size();
        data.push_back(std::move(d));
    }
    if (data.empty()) {
        setStatus(tr("Select X and Y fields."));
        return;
    }

    // Log axes follow the dominant sign of the data, so purely negative
    // quantities (He abundance, log P, ...) can be log-scaled too; values of
    // the other sign and zeros are skipped.
    double xSign = 1.0, ySign = 1.0;
    if (logX || logY) {
        int posX = 0, negX = 0, posY = 0, negY = 0;
        for (const auto& d : data) {
            for (double v : d.xs) {
                if (std::isnan(v)) continue;
                if (v > 0.0)      ++posX;
                else if (v < 0.0) ++negX;
            }
            for (double v : d.ys) {
                if (std::isnan(v)) continue;
                if (v > 0.0)      ++posY;
                else if (v < 0.0) ++negY;
            }
        }
        if (logX) xSign = negX > posX ? -1.0 : 1.0;
        if (logY) ySign = negY > posY ? -1.0 : 1.0;
    }

    // Optional third-field encodings (single series only)
    const QString cKey = multi ? QString()
        : _colorByCombo->currentData(Qt::UserRole).toString();
    const QString sKey = multi ? QString()
        : _sizeByCombo->currentData(Qt::UserRole).toString();
    const bool colorBy = !cKey.isEmpty();
    const bool sizeBy  = !sKey.isEmpty();

    const double baseSize = _markerSizeSpin->value();
    int plotted = 0;
    size_t starTotal = 0;
    int hiddenStacked = 0;
    int missingEncode = 0;
    std::vector<double> stackedAll;

    for (auto& d : data) {
        starTotal += d.starCount;

        std::vector<double> stackedX, stackedY;
        if (_hideStackedCheck->isChecked()) {
            stackedX = stackedValues(d.xs, _stackedNSpin->value());
            stackedY = stackedValues(d.ys, _stackedNSpin->value());
            stackedAll.insert(stackedAll.end(), stackedX.begin(), stackedX.end());
            stackedAll.insert(stackedAll.end(), stackedY.begin(), stackedY.end());
        }

        std::vector<double> cs, ss;
        if (colorBy) cs = extractField(starsForSource(d.cfg->sourceId), cKey);
        if (sizeBy)  ss = extractField(starsForSource(d.cfg->sourceId), sKey);

        QVector<double> px, py, pc, ps;
        px.reserve(int(d.xs.size()));
        py.reserve(int(d.xs.size()));
        for (size_t i = 0; i < d.xs.size(); ++i) {
            if (std::isnan(d.xs[i]) || std::isnan(d.ys[i]))
                continue;
            if ((logX && d.xs[i] * xSign <= 0.0) || (logY && d.ys[i] * ySign <= 0.0))
                continue;
            if (isStackedValue(stackedX, d.xs[i]) || isStackedValue(stackedY, d.ys[i])) {
                ++hiddenStacked;
                continue;
            }
            if ((colorBy && std::isnan(cs[i])) || (sizeBy && std::isnan(ss[i]))) {
                ++missingEncode;
                continue;
            }
            px.push_back(d.xs[i]);
            py.push_back(d.ys[i]);
            if (colorBy) pc.push_back(cs[i]);
            if (sizeBy)  ps.push_back(ss[i]);
        }
        plotted += px.size();

        const QCPScatterStyle::ScatterShape shape = kMarkers[d.cfg->markerIndex].shape;

        if (!colorBy && !sizeBy) {
            auto* graph = _plot->addGraph();
            graph->setLineStyle(QCPGraph::lsNone);
            graph->setScatterStyle(QCPScatterStyle(shape, d.cfg->color, baseSize));
            graph->setAdaptiveSampling(cull);
            graph->setData(px, py);
            graph->setName(d.cfg->label);
            if (multi)
                graph->addToLegend();
        } else {
            // Encoded points are split into colour x size buckets and drawn as
            // one graph per bucket (QCPGraph has a single scatter style).
            double cMin = 0.0, cMax = 1.0, sMin = 0.0, sMax = 1.0;
            if (colorBy && !pc.isEmpty()) {
                const auto mm = std::minmax_element(pc.begin(), pc.end());
                cMin = *mm.first;
                cMax = *mm.second;
            }
            if (sizeBy && !ps.isEmpty()) {
                const auto mm = std::minmax_element(ps.begin(), ps.end());
                sMin = *mm.first;
                sMax = *mm.second;
            }

            const int nC = (colorBy && cMax > cMin) ? 24 : 1;
            const int nS = (sizeBy && sMax > sMin) ? 6 : 1;
            QCPColorGradient grad = viridisGradient();   // color() is non-const
            const double loSize = std::max(1.0, baseSize * 0.5);
            const double hiSize = baseSize * 2.0;
            auto bucketOf = [](double v, double lo, double hi, int n) {
                if (n <= 1 || hi <= lo)
                    return 0;
                return std::clamp(int((v - lo) / (hi - lo) * n), 0, n - 1);
            };

            std::map<int, std::pair<QVector<double>, QVector<double>>> buckets;
            for (int i = 0; i < px.size(); ++i) {
                const int bc = colorBy ? bucketOf(pc[i], cMin, cMax, nC) : 0;
                const int bs = sizeBy ? bucketOf(ps[i], sMin, sMax, nS) : 0;
                auto& b = buckets[bc * nS + bs];
                b.first.push_back(px[i]);
                b.second.push_back(py[i]);
            }
            for (auto& [idx, bucket] : buckets) {
                const int bc = idx / nS;
                const int bs = idx % nS;
                const QColor col = colorBy
                    ? QColor::fromRgb(grad.color((bc + 0.5) / nC, QCPRange(0.0, 1.0)))
                    : d.cfg->color;
                const double size = sizeBy
                    ? loSize + (bs + 0.5) / nS * (hiSize - loSize)
                    : baseSize;
                auto* graph = _plot->addGraph();
                graph->setLineStyle(QCPGraph::lsNone);
                graph->setScatterStyle(QCPScatterStyle(shape, col, size));
                graph->setAdaptiveSampling(cull);
                graph->setData(bucket.first, bucket.second);
            }

            if (colorBy) {
                _colorScale = new QCPColorScale(_plot);
                _colorScale->setType(QCPAxis::atRight);
                _colorScale->setGradient(grad);
                _colorScale->setDataRange(
                    QCPRange(cMin, cMax > cMin ? cMax : cMin + 1.0));
                const QColor fg = effectiveFg();
                QCPAxis* cAxis = _colorScale->axis();
                cAxis->setLabel(fieldLabel(cKey));
                cAxis->setLabelColor(fg);
                cAxis->setTickLabelColor(fg);
                cAxis->setBasePen(QPen(fg, 1));
                cAxis->setTickPen(QPen(fg, 1));
                cAxis->setSubTickPen(QPen(fg, 1));
                cAxis->setLabelFont(_axisFont);
                cAxis->setTickLabelFont(_axisFont);
                const int row = _titleElement ? 1 : 0;
                _plot->plotLayout()->addElement(row, 1, _colorScale);
                _colorScale->setMarginGroup(QCP::msBottom | QCP::msTop, _marginGroup);
                _plot->axisRect()->setMarginGroup(QCP::msBottom | QCP::msTop,
                                                  _marginGroup);
            }
        }

        // Windowed trend curve per series
        if (trendMode != 0 && px.size() >= 3) {
            QVector<double> tx, ty;
            windowedTrend(px, py, _trendWindowSpin->value(),
                          trendMode == 2 /* median */, tx, ty);
            auto* trend = _plot->addGraph();
            trend->setData(tx, ty, true);
            trend->setLineStyle(QCPGraph::lsLine);
            trend->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssNone));
            trend->setPen(QPen(multi ? d.cfg->color.darker(140)
                                     : PanelUtils::fitCurveColor(), 2.0));
        }
    }

    _plot->legend->setVisible(multi);

    if (logX) {
        _plot->xAxis->setScaleType(QCPAxis::stLogarithmic);
        _plot->xAxis->setTicker(QSharedPointer<QCPAxisTickerLog>(new QCPAxisTickerLog));
        // Seed the sign domain: rescaleAxes() only considers data on the
        // side of zero the current range is on.
        if (xSign < 0.0)
            _plot->xAxis->setRange(-10.0, -1.0);
    }
    if (logY) {
        _plot->yAxis->setScaleType(QCPAxis::stLogarithmic);
        _plot->yAxis->setTicker(QSharedPointer<QCPAxisTickerLog>(new QCPAxisTickerLog));
        if (ySign < 0.0)
            _plot->yAxis->setRange(-10.0, -1.0);
    }
    _plot->xAxis->setRangeReversed(_invXCheck->isChecked());
    _plot->yAxis->setRangeReversed(_invYCheck->isChecked());
    const QString xLab = _xLabelEdit->text().trimmed();
    const QString yLab = _yLabelEdit->text().trimmed();
    _plot->xAxis->setLabel(xLab.isEmpty() ? fieldLabel(data.front().cfg->xKey) : xLab);
    _plot->yAxis->setLabel(yLab.isEmpty() ? fieldLabel(data.front().cfg->yKey) : yLab);
    applyCustomTickers();

    if (plotted > 0) {
        _plot->rescaleAxes();
        _plot->xAxis->scaleRange(1.1);
        _plot->yAxis->scaleRange(1.1);
    }
    applyAxisLimits();

    QString status = tr("Plotted %1 of %2 stars.").arg(plotted).arg(starTotal);
    if (hiddenStacked > 0) {
        std::sort(stackedAll.begin(), stackedAll.end());
        stackedAll.erase(std::unique(stackedAll.begin(), stackedAll.end()),
                         stackedAll.end());
        status += tr(" %1 hidden at stacked values (%2).")
                      .arg(hiddenStacked).arg(formatValueList(stackedAll));
    }
    if (missingEncode > 0)
        status += tr(" %1 without color/size field skipped.").arg(missingEncode);
    setStatus(status);
}

void ProjectPlotDialog::plotHistogram()
{
    _plot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    const bool logX    = _histLogXCheck->isChecked();
    const bool logBins = logX && _histLogBinsCheck->isChecked();
    const bool multi   = _series.size() > 1;

    // Raw values per series; the log sign domain is decided on the combined
    // data (purely negative quantities like He abundance can be log-scaled).
    struct SeriesData {
        const SeriesConfig* cfg;
        std::vector<double> values;
        std::vector<double> stacked;
    };
    std::vector<SeriesData> data;
    int posCnt = 0, negCnt = 0;
    for (const auto& s : _series) {
        if (s.histKey.isEmpty())
            continue;
        SeriesData d;
        d.cfg = &s;
        d.values = extractField(starsForSource(s.sourceId), s.histKey);
        for (double v : d.values) {
            if (std::isnan(v)) continue;
            if (v > 0.0)      ++posCnt;
            else if (v < 0.0) ++negCnt;
        }
        data.push_back(std::move(d));
    }
    if (data.empty()) {
        setStatus(tr("Select a field."));
        return;
    }
    const bool negLog = logX && negCnt > posCnt;

    // Manual X limits restrict the binning range
    double limLo = 0.0, limHi = 0.0;
    bool hasLo = limitValue(_xMinEdit, limLo);
    bool hasHi = limitValue(_xMaxEdit, limHi);
    if (hasLo && hasHi && limLo > limHi)
        std::swap(limLo, limHi);

    int hiddenStacked = 0;
    std::vector<double> stackedAll;
    size_t totalValues = 0;
    for (auto& d : data) {
        if (_hideStackedCheck->isChecked())
            d.stacked = stackedValues(d.values, _stackedNSpin->value());
        stackedAll.insert(stackedAll.end(), d.stacked.begin(), d.stacked.end());
        std::vector<double> filtered;
        filtered.reserve(d.values.size());
        for (double v : d.values) {
            if (std::isnan(v) || (logX && (negLog ? v >= 0.0 : v <= 0.0)))
                continue;
            if (isStackedValue(d.stacked, v)) {
                ++hiddenStacked;
                continue;
            }
            if ((hasLo && v < limLo) || (hasHi && v > limHi))
                continue;
            filtered.push_back(v);
        }
        d.values = std::move(filtered);
        totalValues += d.values.size();
    }
    if (totalValues == 0) {
        setStatus(tr("No values for this field in the current selection/range."));
        return;
    }

    // Negative log axes are binned in magnitude space (all positive) and the
    // resulting edges/counts mirrored back below.
    if (negLog) {
        for (auto& d : data)
            for (double& v : d.values)
                v = -v;
        const bool hadLo = hasLo;
        const double oldLo = limLo;
        hasLo = hasHi;
        limLo = -limHi;
        hasHi = hadLo;
        limHi = -oldLo;
    }

    // Common bin edges across all series so the histograms are comparable
    double dataMin = std::numeric_limits<double>::max();
    double dataMax = std::numeric_limits<double>::lowest();
    for (const auto& d : data) {
        for (double v : d.values) {
            dataMin = std::min(dataMin, v);
            dataMax = std::max(dataMax, v);
        }
    }
    double lo = hasLo ? limLo : dataMin;
    double hi = hasHi ? limHi : dataMax;
    if (logBins && lo <= 0.0)
        lo = dataMin;   // values guaranteed positive after filtering above
    const int nBins = _binsSpin->value();

    // Bin edges: linear or logarithmic spacing
    QVector<double> edges(nBins + 1);
    if (hi <= lo) {
        // All values identical: a single bar of nominal width
        const double w = (std::abs(lo) > 0.0) ? std::abs(lo) * 0.1 : 1.0;
        lo -= w / 2.0;
        hi = lo + w;
    }
    if (logBins) {
        const double llo = std::log10(lo), lhi = std::log10(hi);
        for (int i = 0; i <= nBins; ++i)
            edges[i] = std::pow(10.0, llo + (lhi - llo) * i / nBins);
    } else {
        for (int i = 0; i <= nBins; ++i)
            edges[i] = lo + (hi - lo) * i / nBins;
    }

    double maxCount = 0.0;
    std::vector<QVector<double>> allCounts;
    for (const auto& d : data) {
        QVector<double> counts(nBins, 0.0);
        for (double v : d.values) {
            int bin;
            if (logBins)
                bin = int((std::log10(v) - std::log10(lo))
                          / (std::log10(hi) - std::log10(lo)) * nBins);
            else
                bin = int((v - lo) / (hi - lo) * nBins);
            counts[std::clamp(bin, 0, nBins - 1)] += 1.0;
        }
        maxCount = std::max(maxCount,
                            *std::max_element(counts.begin(), counts.end()));
        allCounts.push_back(std::move(counts));
    }

    // Mirror magnitude-space binning back onto the negative axis
    if (negLog) {
        for (double& e : edges)
            e = -e;
        std::reverse(edges.begin(), edges.end());
        for (auto& counts : allCounts)
            std::reverse(counts.begin(), counts.end());
    }

    // Step-filled outlines (exact for variable-width bins, unlike QCPBars)
    for (size_t di = 0; di < data.size(); ++di) {
        const QVector<double>& counts = allCounts[di];
        const SeriesConfig* cfg = data[di].cfg;
        QVector<double> sx, sy;
        sx.reserve(nBins + 3);
        sy.reserve(nBins + 3);
        sx.push_back(edges[0]);
        sy.push_back(0.0);
        for (int i = 0; i < nBins; ++i) {
            sx.push_back(edges[i]);
            sy.push_back(counts[i]);
        }
        sx.push_back(edges[nBins]);
        sy.push_back(counts[nBins - 1]);
        sx.push_back(edges[nBins]);
        sy.push_back(0.0);

        auto* graph = _plot->addGraph();
        graph->setData(sx, sy, true);
        graph->setLineStyle(QCPGraph::lsStepLeft);
        graph->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssNone));
        QColor fill = cfg->color;
        fill.setAlpha(multi ? 110 : 170);   // more transparent when overlaid
        graph->setBrush(fill);
        graph->setPen(QPen(cfg->color.darker(130), 1.2));
        graph->setName(cfg->label);
        if (multi)
            graph->addToLegend();
    }

    _plot->legend->setVisible(multi);

    if (logX) {
        _plot->xAxis->setScaleType(QCPAxis::stLogarithmic);
        _plot->xAxis->setTicker(QSharedPointer<QCPAxisTickerLog>(new QCPAxisTickerLog));
    }
    _plot->xAxis->setRangeReversed(_histInvXCheck->isChecked());
    const QString xLab = _xLabelEdit->text().trimmed();
    const QString yLab = _yLabelEdit->text().trimmed();
    _plot->xAxis->setLabel(xLab.isEmpty() ? fieldLabel(data.front().cfg->histKey)
                                          : xLab);
    _plot->yAxis->setLabel(yLab.isEmpty() ? tr("Count") : yLab);
    applyCustomTickers();

    _plot->xAxis->setRange(edges.first(), edges.last());
    if (!logX)
        _plot->xAxis->scaleRange(1.05);
    if (_histLogYCheck->isChecked()) {
        _plot->yAxis->setScaleType(QCPAxis::stLogarithmic);
        _plot->yAxis->setTicker(
            QSharedPointer<QCPAxisTickerLog>(new QCPAxisTickerLog));
        // Lower bound below 1 so single-count bins remain visible
        _plot->yAxis->setRange(0.7, std::max(maxCount * 1.5, 1.5));
    } else {
        _plot->yAxis->setRange(0.0, maxCount * 1.05);
    }
    applyAxisLimits();

    QString status = tr("Histogram of %1 values.").arg(totalValues);
    if (hiddenStacked > 0) {
        std::sort(stackedAll.begin(), stackedAll.end());
        stackedAll.erase(std::unique(stackedAll.begin(), stackedAll.end()),
                         stackedAll.end());
        status += tr(" %1 hidden at stacked values (%2).")
                      .arg(hiddenStacked).arg(formatValueList(stackedAll));
    }
    setStatus(status);
}

void ProjectPlotDialog::plotSky()
{
    _plot->setInteractions(QCP::Interactions());

    const QColor gridColor = effectiveGrid();
    const QColor textColor = effectiveFg();
    const QPen gridPen(gridColor, 0.8, Qt::DotLine);
    const QPen borderPen(gridColor, 1.2, Qt::SolidLine);

    // Hide cartesian axes; the graticule carries the coordinate information.
    for (QCPAxis* ax : { _plot->xAxis, _plot->yAxis }) {
        ax->setVisible(false);
        ax->grid()->setVisible(false);
    }

    auto addCurve = [&](const QVector<double>& xs, const QVector<double>& ys,
                        const QPen& pen) {
        auto* curve = new QCPCurve(_plot->xAxis, _plot->yAxis);
        QVector<double> t(xs.size());
        std::iota(t.begin(), t.end(), 0.0);
        curve->setData(t, xs, ys);
        curve->setPen(pen);
        curve->setLineStyle(QCPCurve::lsLine);
        curve->setScatterStyle(QCPScatterStyle(QCPScatterStyle::ssNone));
    };

    if (_skyGridCheck->isChecked()) {
        // Meridians (constant RA), drawn from pole to pole
        for (int ra = 0; ra < 360; ra += 30) {
            QVector<double> xs, ys;
            for (int i = 0; i <= 90; ++i) {
                const double b = (-90.0 + 2.0 * i) * M_PI / 180.0;
                double x, y;
                hammerProject(raToL(ra), b, x, y);
                xs.push_back(x);
                ys.push_back(y);
            }
            addCurve(xs, ys, gridPen);
        }
        // Parallels (constant Dec)
        for (int dec = -60; dec <= 60; dec += 30) {
            QVector<double> xs, ys;
            for (int i = 0; i <= 180; ++i) {
                const double l = (-180.0 + 2.0 * i) * M_PI / 180.0;
                double x, y;
                hammerProject(l, dec * M_PI / 180.0, x, y);
                xs.push_back(x);
                ys.push_back(y);
            }
            addCurve(xs, ys, dec == 0 ? QPen(gridColor, 1.0) : gridPen);
        }

        // RA labels along the equator, Dec labels along the central meridian
        QFont labelFont = _axisFont;
        if (labelFont.pointSizeF() > 0.0)
            labelFont.setPointSizeF(labelFont.pointSizeF() * 0.85);
        auto addLabel = [&](double x, double y, const QString& text) {
            auto* item = new QCPItemText(_plot);
            item->position->setCoords(x, y);
            item->setText(text);
            item->setFont(labelFont);
            item->setColor(textColor);
        };
        for (int ra = 60; ra < 360; ra += 60) {
            double x, y;
            hammerProject(raToL(ra), 0.0, x, y);
            addLabel(x, y - 0.09, QString("%1h").arg(ra / 15));
        }
        for (int dec = -60; dec <= 60; dec += 30) {
            if (dec == 0)
                continue;
            double x, y;
            hammerProject(0.0, dec * M_PI / 180.0, x, y);
            addLabel(x + 0.18, y, QString("%1°").arg(dec));
        }
    }

    // Projection boundary
    {
        QVector<double> xs, ys;
        for (int i = 0; i <= 180; ++i) {
            const double b = (-90.0 + 2.0 * i) * M_PI / 180.0;
            double x, y;
            hammerProject(M_PI, b, x, y);
            xs.push_back(x);
            ys.push_back(y);
        }
        for (int i = 180; i >= 0; --i) {
            const double b = (-90.0 + 2.0 * i) * M_PI / 180.0;
            double x, y;
            hammerProject(-M_PI, b, x, y);
            xs.push_back(x);
            ys.push_back(y);
        }
        addCurve(xs, ys, borderPen);
    }

    // Star positions, one graph per series
    const bool multi = _series.size() > 1;
    const bool cull = _cullRadio->isChecked();
    int plotted = 0;
    size_t starTotal = 0;
    for (const auto& s : _series) {
        const auto& stars = starsForSource(s.sourceId);
        const auto ras  = extractField(stars, "ra");
        const auto decs = extractField(stars, "dec");
        starTotal += stars.size();

        QVector<double> px, py;
        for (size_t i = 0; i < stars.size(); ++i) {
            if (std::isnan(ras[i]) || std::isnan(decs[i]))
                continue;
            double x, y;
            hammerProject(raToL(ras[i]), decs[i] * M_PI / 180.0, x, y);
            px.push_back(x);
            py.push_back(y);
            ++plotted;
        }
        auto* graph = _plot->addGraph();
        graph->setLineStyle(QCPGraph::lsNone);
        graph->setScatterStyle(QCPScatterStyle(
            kMarkers[s.markerIndex].shape, s.color, _markerSizeSpin->value()));
        graph->setAdaptiveSampling(cull);
        graph->setData(px, py);
        graph->setName(s.label);
        if (multi)
            graph->addToLegend();
    }
    _plot->legend->setVisible(multi);

    _plot->xAxis->setRange(-3.05, 3.05);
    _plot->yAxis->setRange(-1.65, 1.65);
    applySkyAspect();

    setStatus(tr("Plotted %1 of %2 stars (%3 without coordinates skipped).")
                  .arg(plotted).arg(starTotal).arg(int(starTotal) - plotted));
}

void ProjectPlotDialog::applySkyAspect()
{
    _plot->yAxis->setScaleRatio(_plot->xAxis, 1.0);
}

void ProjectPlotDialog::resizeEvent(QResizeEvent* event)
{
    QDialog::resizeEvent(event);
    if (_plot && _typeCombo && _typeCombo->currentIndex() == SkyMap) {
        applySkyAspect();
        _plot->replot();
    }
}

void ProjectPlotDialog::positionResetButton()
{
    if (_resetViewBtn && _plot)
        _resetViewBtn->move(_plot->width() - _resetViewBtn->width() - 12, 12);
}

bool ProjectPlotDialog::eventFilter(QObject* obj, QEvent* event)
{
    if (obj == _plot && event->type() == QEvent::Resize)
        positionResetButton();
    return QDialog::eventFilter(obj, event);
}

void ProjectPlotDialog::updateTitleElement()
{
    const QString title = _titleEdit ? _titleEdit->text().trimmed() : QString();
    if (title.isEmpty()) {
        if (_titleElement) {
            _plot->plotLayout()->remove(_titleElement);
            _plot->plotLayout()->simplify();
            _titleElement = nullptr;
        }
        return;
    }
    if (!_titleElement) {
        _plot->plotLayout()->insertRow(0);
        QFont f = font();
        f.setPointSizeF(f.pointSizeF() * 1.3);
        f.setBold(true);
        _titleElement = new QCPTextElement(_plot, title, f);
        _plot->plotLayout()->addElement(0, 0, _titleElement);
    }
    _titleElement->setText(title);
    _titleElement->setTextColor(effectiveFg());
}

// ─────────────────────────────────────────────────────────────────────────────
// Export
// ─────────────────────────────────────────────────────────────────────────────

// Suggested file name: the plot title if set, otherwise derived from the
// plotted fields ("logg_vs_teff", "teff_hist", "sky_map").
QString ProjectPlotDialog::defaultExportBaseName() const
{
    QString base = _titleEdit->text().trimmed();
    if (base.isEmpty()) {
        switch (_typeCombo->currentIndex()) {
        case Histogram:
            base = _histFieldCombo->currentData(Qt::UserRole).toString() + "_hist";
            break;
        case SkyMap:
            base = QStringLiteral("sky_map");
            break;
        case Scatter:
        default:
            base = _yFieldCombo->currentData(Qt::UserRole).toString()
                   + "_vs_"
                   + _xFieldCombo->currentData(Qt::UserRole).toString();
            break;
        }
    }
    static const QRegularExpression invalid("[^A-Za-z0-9_\\-]+");
    base.replace(invalid, "_");
    static const QRegularExpression edges("^_+|_+$");
    base.remove(edges);
    return base.isEmpty() ? QStringLiteral("plot") : base;
}

void ProjectPlotDialog::onExport()
{
    QString selectedFilter;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Export Plot"),
        QDir::homePath() + "/" + defaultExportBaseName() + ".pdf",
        tr("PDF (*.pdf);;SVG (*.svg);;PNG (*.png)"), &selectedFilter);
    if (path.isEmpty())
        return;

    QString suffix = QFileInfo(path).suffix().toLower();
    if (suffix != "pdf" && suffix != "svg" && suffix != "png") {
        if (selectedFilter.contains("svg"))      suffix = "svg";
        else if (selectedFilter.contains("png")) suffix = "png";
        else                                     suffix = "pdf";
        path += "." + suffix;
    }

    // ── Output size / resolution options ────────────────────────────────────
    QDialog sizeDlg(this);
    sizeDlg.setWindowTitle(tr("Export Options"));
    auto* form = new QFormLayout(&sizeDlg);

    // Standard output sizes; A&A figure widths are 88 mm / 180 mm
    struct SizePreset { const char* name; bool inches; double w, h; int dpi; };
    static const SizePreset kSizePresets[] = {
        { "A&A single column (88 mm)",  true,   3.46,  2.60, 300 },
        { "A&A double column (180 mm)", true,   7.09,  5.31, 300 },
        { "A4 landscape",               true,  11.69,  8.27, 300 },
        { "A4 portrait",                true,   8.27, 11.69, 300 },
        { "16:9 screen (Full HD)",      false, 1920,  1080,   96 },
        { "16:9 screen (4K)",           false, 3840,  2160,   96 },
        { "4:3 screen",                 false, 1600,  1200,   96 },
    };
    auto* presetCombo = new QComboBox(&sizeDlg);
    presetCombo->addItem(tr("Custom"));
    for (const auto& p : kSizePresets)
        presetCombo->addItem(tr(p.name));
    form->addRow(tr("Preset:"), presetCombo);

    auto* inchesRadio = new QRadioButton(tr("inches"), &sizeDlg);
    auto* pixelsRadio = new QRadioButton(tr("pixels"), &sizeDlg);
    inchesRadio->setChecked(true);
    auto* unitRow = new QHBoxLayout();
    unitRow->addWidget(inchesRadio);
    unitRow->addWidget(pixelsRadio);
    form->addRow(tr("Units:"), unitRow);

    auto* widthSpin = new QDoubleSpinBox(&sizeDlg);
    auto* heightSpin = new QDoubleSpinBox(&sizeDlg);
    form->addRow(tr("Width:"), widthSpin);
    form->addRow(tr("Height:"), heightSpin);

    auto* dpiSpin = new QSpinBox(&sizeDlg);
    dpiSpin->setRange(50, 1200);
    dpiSpin->setValue(300);
    dpiSpin->setSuffix(" dpi");
    dpiSpin->setToolTip(tr("Pixel density: sets the raster resolution for PNG\n"
                           "and the inch ↔ pixel conversion."));
    form->addRow(tr("Resolution:"), dpiSpin);

    // Default: 7 inches wide, height preserving the on-screen aspect ratio
    const double aspect = double(_plot->height()) / double(_plot->width());
    auto setupUnit = [&](bool inches) {
        const double oldW = widthSpin->value();
        const double oldH = heightSpin->value();
        const double dpi = double(dpiSpin->value());
        for (auto* sp : { widthSpin, heightSpin }) {
            sp->blockSignals(true);
            sp->setDecimals(inches ? 2 : 0);
            sp->setRange(inches ? 1.0 : 100.0, inches ? 200.0 : 20000.0);
            sp->setSuffix(inches ? tr(" in") : tr(" px"));
            sp->blockSignals(false);
        }
        if (oldW > 0.0) {  // convert existing values between units
            widthSpin->setValue(inches ? oldW / dpi : oldW * dpi);
            heightSpin->setValue(inches ? oldH / dpi : oldH * dpi);
        }
    };
    setupUnit(true);
    widthSpin->setValue(7.0);
    heightSpin->setValue(std::round(7.0 * aspect * 100.0) / 100.0);
    connect(inchesRadio, &QRadioButton::toggled, &sizeDlg,
            [&](bool inches) { setupUnit(inches); });

    bool applyingPreset = false;
    connect(presetCombo, &QComboBox::currentIndexChanged, &sizeDlg, [&](int idx) {
        if (idx <= 0)
            return;
        const SizePreset& p = kSizePresets[idx - 1];
        applyingPreset = true;
        dpiSpin->setValue(p.dpi);
        (p.inches ? inchesRadio : pixelsRadio)->setChecked(true);
        widthSpin->setValue(p.w);
        heightSpin->setValue(p.h);
        applyingPreset = false;
    });
    // Manual edits revert the preset selection to "Custom"
    auto toCustom = [&] {
        if (!applyingPreset) {
            const QSignalBlocker b(presetCombo);
            presetCombo->setCurrentIndex(0);
        }
    };
    connect(widthSpin, &QDoubleSpinBox::valueChanged, &sizeDlg, toCustom);
    connect(heightSpin, &QDoubleSpinBox::valueChanged, &sizeDlg, toCustom);
    connect(dpiSpin, &QSpinBox::valueChanged, &sizeDlg, toCustom);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &sizeDlg);
    connect(buttons, &QDialogButtonBox::accepted, &sizeDlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &sizeDlg, &QDialog::reject);
    form->addRow(buttons);

    if (sizeDlg.exec() != QDialog::Accepted)
        return;

    const bool inches = inchesRadio->isChecked();
    const int dpi = dpiSpin->value();
    const double wIn = inches ? widthSpin->value() : widthSpin->value() / dpi;
    const double hIn = inches ? heightSpin->value() : heightSpin->value() / dpi;
    const int wPx = int(std::lround(inches ? wIn * dpi : widthSpin->value()));
    const int hPx = int(std::lround(inches ? hIn * dpi : heightSpin->value()));

    // Saved plots must contain every point: disable any point culling for
    // the render and restore it afterwards.
    std::vector<QCPGraph*> culled;
    for (int i = 0; i < _plot->graphCount(); ++i) {
        QCPGraph* g = _plot->graph(i);
        if (g->adaptiveSampling()) {
            g->setAdaptiveSampling(false);
            culled.push_back(g);
        }
    }

    bool ok = false;
    if (suffix == "pdf") {
        // savePdf() page size is in points (1 pt = 1/72 in)
        ok = _plot->savePdf(path, int(std::lround(wIn * 72.0)),
                            int(std::lround(hIn * 72.0)));
    } else if (suffix == "png") {
        // Render at 96-dpi logical size, scaled so output is wPx × hPx with
        // proportionally scaled fonts/lines; embed the dpi in the metadata.
        const int baseW = int(std::lround(wIn * 96.0));
        const int baseH = int(std::lround(hIn * 96.0));
        ok = _plot->savePng(path, baseW, baseH, double(wPx) / double(baseW),
                            -1, dpi);
    } else { // svg
        const int w = int(std::lround(wIn * 96.0));
        const int h = int(std::lround(hIn * 96.0));
        QSvgGenerator generator;
        generator.setFileName(path);
        generator.setResolution(96);
        generator.setSize(QSize(w, h));
        generator.setViewBox(QRect(0, 0, w, h));
        generator.setTitle(_titleEdit->text());
        QCPPainter painter;
        if (painter.begin(&generator)) {
            _plot->toPainter(&painter, w, h);
            painter.end();
            ok = true;
        }
    }

    for (QCPGraph* g : culled)
        g->setAdaptiveSampling(true);

    if (ok)
        setStatus(tr("Exported to %1").arg(path));
    else
        QMessageBox::warning(this, tr("Export Failed"),
                             tr("Could not write %1").arg(path));
}
