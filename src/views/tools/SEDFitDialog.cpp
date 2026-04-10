#include "SEDFitDialog.h"
#include "models/Star.h"
#include "models/Photometry.h"
#include "db/DatabaseManager.h"
#include "db/PhotometryRepository.h"
#include "utils/ExtractSED.h"
#include "utils/Logger.h"
#include "plotting/qcustomplot.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QSplitter>
#include <QScrollArea>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QGroupBox>
#include <QLineEdit>
#include <QTextEdit>
#include <QProgressBar>
#include <QProcess>
#include <QDir>
#include <QTemporaryDir>
#include <QTextStream>
#include <QFileDialog>
#include <QMessageBox>
#include <QStandardPaths>
#include <QApplication>
#include <QTimer>
#include <QUuid>

#include <cmath>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════
// Grid preset catalogue (mirrors photometry.sl commented-out options)
// ═══════════════════════════════════════════════════════════════════

static const std::vector<GridPreset>& sGridPresets()
{
    static const std::vector<GridPreset> p = {
        // sdB
        {"sdB", "sdB standard",      "sdB/processed/",             15000,55000, 4.6,6.6, -5.05,-0.041, -1.0,1.0},
        {"sdB", "sdB extended",       "sdB/processed_sdB24/",       15000,55000, 4.6,7.0, -5.05,-0.041, -1.0,1.0},
        {"sdB", "ELM / BHB",         "sdB/processed_ELM_BHB/",     9000,20000,  2.8,7.0, -5.05,-0.300, -1.0,1.0},
        {"sdB", "BLAPS",             "sdB/processed_blaps/",        15000,31000, 3.6,7.0, -4.05,-0.300, -2.0,1.0},
        {"sdB", "Hot sdO",           "sdB/processed_hot_sdO/",      51000,75000, 5.2,6.6, -5.05,-0.041, -1.0,1.0},
        {"sdB", "He-sdO",            "sdB/processed_He_sdO/",       25000,55000, 5.0,6.6, -1.05,-0.001, -1.0,1.0},
        {"sdB", "He-sdO Z=0 xl",    "sdB/processed_He_sdO_Z0.00_xl/",25000,55000,4.0,6.6,-1.05,-0.001, 0.0,0.0},
        {"sdB", "Very hot sdO",      "sdB/processed_vhot_sdO/",     75000,99000, 5.6,7.0, -5.05,-0.041, -1.0,0.0},
        {"sdB", "Super-hot sdO",     "sdB/processed_shot_sdO/",     75000,115000,5.8,7.0, -5.05,-0.041, -1.0,0.0},
        // B stars
        {"B stars", "Late B (III–V)","B_V_III/processed_late/",     10000,19000, 3.0,4.6, -1.25,-0.85, -0.5,0.5},
        {"B stars", "Mid B (III–V)", "B_V_III/processed_mid/",      18000,25000, 3.0,4.6, -1.25,-0.85, -0.5,0.5},
        {"B stars", "Early B (III–V)","B_V_III/processed_early/",   25000,33000, 3.4,4.6, -1.25,-0.85, -0.5,0.5},
        // sdO (2020)
        {"sdO (2020)", "Standard",   "sdOstar2020_SED/processed/",  26250,57500, 4.25,6.75,-1.75,4.00, 0.0,0.0},
        {"sdO (2020)", "Hot",        "sdOstar2020_SED/processed_hot/",26250,65000,4.50,6.75,-1.50,4.00, 0.0,0.0},
        {"sdO (2020)", "Hot He-sdO", "sdOstar2020_SED/processed_hot_HesdO/",26250,72500,4.625,6.75,-1.00,4.00,0.0,0.0},
        {"sdO (2020)", "Low-He sdO", "sdOstar2020_SED/processed_lHe-sdO/",26250,55000,4.00,6.75,-1.50,4.00,0.0,0.0},
        {"sdO (2020)", "MS",         "sdOstar2020_SED/processed_MS/",26250,45000,3.625,6.75,-1.50,4.00,0.0,0.0},
        {"sdO (2020)", "Cool",       "sdOstar2020_SED/processed_cool/",26250,47500,3.75,6.75,-1.50,4.00,0.0,0.0},
        {"sdO (2020)", "EHe",        "sdOstar2020_SED/processed_EHe/",26250,35000,3.25,6.75,-1.50,4.00,0.0,0.0},
        {"sdO (2020)", "EHe cool",   "sdOstar2020_SED/processed_EHe_cool/",26250,31250,3.00,6.75,-0.75,4.00,0.0,0.0},
        // Steven
        {"Steven", "Grid 5 (hot)",  "steven/grid5/",               38000,55000, 4.6,6.6, -5.00,-0.25, -2.0,0.5},
        {"Steven", "Grid 4",        "steven/grid4/",               22000,40000, 4.0,6.6, -5.00,-0.25, -2.0,0.5},
        {"Steven", "Grid 3",        "steven/grid3/",               15000,26000, 3.0,6.4, -5.00,-0.25, -2.0,0.5},
        {"Steven", "Grid 2",        "steven/grid2/",               11000,17000, 2.8,6.0, -5.00,-0.25, -2.0,0.5},
        {"Steven", "Grid 1 (cool)", "steven/grid1/",                8000,12500, 2.4,4.4, -5.00,-0.50, -2.0,0.5},
        // Cool stars
        {"Cool stars", "Synthe",     "synthe/processed/",            3800,12500, 1.4,5.6, -1.0,-1.0, -2.5,0.5},
        {"Cool stars", "Synthe high logg","synthe/processed_vhighlogg/",4600,14000,1.6,7.0,-1.0,-1.0,-2.0,0.5},
        {"Cool stars", "Synthe low logg", "synthe/processed_lowlogg/",3400,6200,0.0,4.0,-1.0,-1.0,-2.0,0.5},
        {"Cool stars", "Synthe α+0.3","synthe_alpha+0.3/processed/", 4000,8000, 2.0,5.2, -1.0,-1.0, -2.0,0.5},
        {"Cool stars", "Synthe α+0.4","synthe_alpha+0.4/processed/", 4000,8000, 2.0,5.2, -1.0,-1.0, -2.0,0.5},
        {"Cool stars", "Phoenix",    "Phoenix_late_type_stars_photometry_v2.0/processed/",2300,15000,2.0,5.0,-1.05,-1.05,-2.0,0.0},
        // White dwarfs
        {"WD", "DAO (Nicole)",       "WD/Nicole/DAO/processed/",    40000,180000,6.0,9.0, -5.0,0.0, 0.0,0.0},
        {"WD", "DO (Nicole)",        "WD/Nicole/DO/processed/",     40000,180000,6.0,9.0, 99.0,99.0, 0.0,0.0},
        {"WD", "DA (Nicole)",        "WD/Nicole/DA/processed/",     20000,180000,6.0,9.0,-99.0,-99.0,0.0,0.0},
        {"WD", "OH (Nicole)",        "WD/Nicole/OH/processed/",     40000,140000,5.5,6.5,-1.553,-0.423,0.0,0.0},
        {"WD", "DA NLTE (Tremblay)", "WD/Tremblay/processed_DA1DNLTE/",2000,140000,6.5,9.5,-99,-99,0.0,0.0},
        {"WD", "DA",                 "WD/DA/processed",              6000,100000,5.5,9.5,-100,-100,0.0,0.0},
        {"WD", "DB",                 "WD/DB/processed",             10000,40000, 7.0,9.0, 0,0, 0.0,0.0},
    };
    return p;
}

const std::vector<GridPreset>& SEDFitDialog::gridPresets() { return sGridPresets(); }

// ═══════════════════════════════════════════════════════════════════
// Helper: format an asymmetric value as HTML
// ═══════════════════════════════════════════════════════════════════

namespace {

enum PhotomCol { PC_Include=0, PC_System, PC_Band, PC_Lambda, PC_Flux,
                 PC_FluxErr, PC_Residual, PC_Catalog, PC_COUNT };

enum ParamCol  { PP_Name=0, PP_Value, PP_Freeze, PP_Min, PP_Max, PP_COUNT };

} // anon

// ═══════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════

SEDFitDialog::SEDFitDialog(std::shared_ptr<Star> star,
                           DatabaseManager* dbm,
                           const QString& projectId,
                           QWidget* parent)
    : QDialog(parent)
    , _star(std::move(star))
    , _dbm(dbm)
    , _projectId(projectId)
{
    setupUi();
    discoverGrids();
    loadExistingFits();
    initDefaultFitParams();

    LOG_INFO("Tools", QString("SED Fit dialog opened for %1")
                          .arg(_star->getSourceId()));
}

SEDFitDialog::~SEDFitDialog()
{
    if (_isisProcess && _isisProcess->state() != QProcess::NotRunning) {
        _isisProcess->kill();
        _isisProcess->waitForFinished(2000);
    }
}

// ═══════════════════════════════════════════════════════════════════
// UI setup
// ═══════════════════════════════════════════════════════════════════

void SEDFitDialog::setupUi()
{
    QString title = _star->getAlias().isEmpty()
                        ? _star->getSourceId()
                        : _star->getAlias();
    setWindowTitle(QString("SED Analysis — %1").arg(title));
    resize(1250, 850);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    root->addWidget(createFitSelectorBar());

    auto* mainSplit = new QSplitter(Qt::Horizontal);
    mainSplit->addWidget(createPlotArea());
    mainSplit->addWidget(createParameterPanel());
    mainSplit->setStretchFactor(0, 3);
    mainSplit->setStretchFactor(1, 1);
    root->addWidget(mainSplit, 1);

    root->addWidget(createPhotometrySection());
    root->addWidget(createNewFitPanel());
}

// ── Fit selector bar ─────────────────────────────────────────────

QWidget* SEDFitDialog::createFitSelectorBar()
{
    auto* w = new QWidget;
    auto* lay = new QHBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);

    lay->addWidget(new QLabel("Fit:"));

    _fitCombo = new QComboBox;
    _fitCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    lay->addWidget(_fitCombo, 1);

    _setBestFitBtn = new QPushButton("★ Set as Best Fit");
    _setBestFitBtn->setEnabled(false);
    lay->addWidget(_setBestFitBtn);

    _deleteFitBtn = new QPushButton("Delete Fit");
    _deleteFitBtn->setEnabled(false);
    lay->addWidget(_deleteFitBtn);

    connect(_fitCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SEDFitDialog::onFitSelected);
    connect(_setBestFitBtn, &QPushButton::clicked,
            this, &SEDFitDialog::onSetBestFit);
    connect(_deleteFitBtn, &QPushButton::clicked,
            this, &SEDFitDialog::onDeleteFit);

    return w;
}

// ── Plot area (SED + residuals, stacked vertically) ──────────────

QWidget* SEDFitDialog::createPlotArea()
{
    auto* split = new QSplitter(Qt::Vertical);

    // ── SED plot ─────────────────────────────────────────────
    _sedPlot = new QCustomPlot;
    _sedPlot->setMinimumHeight(250);
    applyPlotTheme(_sedPlot);

    QSharedPointer<QCPAxisTickerLog> xLogTicker(new QCPAxisTickerLog);
    _sedPlot->xAxis->setTicker(xLogTicker);
    _sedPlot->xAxis->setScaleType(QCPAxis::stLogarithmic);
    _sedPlot->xAxis->setLabel("Wavelength (Å)");

    QSharedPointer<QCPAxisTickerLog> yLogTicker(new QCPAxisTickerLog);
    _sedPlot->yAxis->setTicker(yLogTicker);
    _sedPlot->yAxis->setScaleType(QCPAxis::stLogarithmic);
    _sedPlot->yAxis->setLabel("Flux (erg s⁻¹ cm⁻² Å⁻¹)");

    _sedPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);
    _sedPlot->legend->setVisible(true);
    _sedPlot->legend->setFont(QFont(font().family(), 8));
    _sedPlot->axisRect()->insetLayout()->setInsetAlignment(
        0, Qt::AlignTop | Qt::AlignRight);

    split->addWidget(_sedPlot);

    // ── Residual plot ────────────────────────────────────────
    _residualPlot = new QCustomPlot;
    _residualPlot->setMinimumHeight(80);
    applyPlotTheme(_residualPlot);

    QSharedPointer<QCPAxisTickerLog> rxLog(new QCPAxisTickerLog);
    _residualPlot->xAxis->setTicker(rxLog);
    _residualPlot->xAxis->setScaleType(QCPAxis::stLogarithmic);
    _residualPlot->xAxis->setLabel("Wavelength (Å)");
    _residualPlot->yAxis->setLabel("Residual (σ)");

    _residualPlot->setInteractions(QCP::iRangeDrag | QCP::iRangeZoom);

    split->addWidget(_residualPlot);
    split->setStretchFactor(0, 4);
    split->setStretchFactor(1, 1);

    // Sync x-axis between plots
    connect(_sedPlot->xAxis, QOverload<const QCPRange&>::of(&QCPAxis::rangeChanged),
            this, [this](const QCPRange& r) {
                _residualPlot->xAxis->setRange(r);
                _residualPlot->replot();
            });

    return split;
}

// ── Parameter panel (right side, scrollable HTML) ────────────────

QWidget* SEDFitDialog::createParameterPanel()
{
    _paramScroll = new QScrollArea;
    _paramScroll->setWidgetResizable(true);
    _paramScroll->setMinimumWidth(260);
    _paramScroll->setMaximumWidth(380);

    _paramLabel = new QLabel;
    _paramLabel->setWordWrap(true);
    _paramLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    _paramLabel->setTextFormat(Qt::RichText);
    _paramLabel->setMargin(8);
    _paramLabel->setText("<i style='color:gray;'>No fit selected</i>");

    _paramScroll->setWidget(_paramLabel);
    return _paramScroll;
}

// ── Photometry table (collapsible) ───────────────────────────────

QWidget* SEDFitDialog::createPhotometrySection()
{
    auto* container = new QWidget;
    auto* vlay = new QVBoxLayout(container);
    vlay->setContentsMargins(0, 0, 0, 0);
    vlay->setSpacing(0);

    _photToggleBtn = new QPushButton("▾ Photometry Points");
    _photToggleBtn->setFlat(true);
    _photToggleBtn->setStyleSheet("text-align: left; font-weight: bold; padding: 4px;");
    vlay->addWidget(_photToggleBtn);

    _photContent = new QWidget;
    auto* pcLay = new QVBoxLayout(_photContent);
    pcLay->setContentsMargins(0, 0, 0, 0);

    _photTable = new QTableWidget(0, PC_COUNT);
    _photTable->setHorizontalHeaderLabels(
        {"Include", "System", "Band", "λ (Å)", "Flux",
         "±Flux", "Resid. (σ)", "Catalog"});
    _photTable->horizontalHeader()->setStretchLastSection(true);
    _photTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _photTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _photTable->setAlternatingRowColors(true);
    _photTable->setMaximumHeight(200);
    _photTable->verticalHeader()->setDefaultSectionSize(22);
    pcLay->addWidget(_photTable);

    vlay->addWidget(_photContent);

    connect(_photToggleBtn, &QPushButton::clicked, this, [this] {
        bool vis = !_photContent->isVisible();
        _photContent->setVisible(vis);
        _photToggleBtn->setText(vis ? "▾ Photometry Points"
                                    : "▸ Photometry Points");
    });

    connect(_photTable, &QTableWidget::cellChanged,
            this, &SEDFitDialog::onPhotometryFlagToggled);

    return container;
}

// ── New Fit configuration panel (collapsible) ────────────────────

QWidget* SEDFitDialog::createNewFitPanel()
{
    auto* container = new QWidget;
    auto* vlay = new QVBoxLayout(container);
    vlay->setContentsMargins(0, 0, 0, 0);
    vlay->setSpacing(0);

    _newFitToggleBtn = new QPushButton("▸ New Fit Configuration");
    _newFitToggleBtn->setFlat(true);
    _newFitToggleBtn->setStyleSheet(
        "text-align: left; font-weight: bold; padding: 4px;");
    vlay->addWidget(_newFitToggleBtn);

    _newFitScroll = new QScrollArea;
    _newFitScroll->setWidgetResizable(true);
    _newFitScroll->setMaximumHeight(500);
    _newFitScroll->setVisible(false);
    _newFitScroll->setFrameShape(QFrame::NoFrame);

    auto* scrollContent = new QWidget;
    auto* nfLay = new QVBoxLayout(scrollContent);
    nfLay->setContentsMargins(8, 4, 8, 4);

    // ── ISIS binary ──────────────────────────────────────────
    auto* isisGroup = new QGroupBox("ISIS");
    auto* iLay = new QHBoxLayout(isisGroup);

    _isisStatusLabel = new QLabel;
    iLay->addWidget(_isisStatusLabel, 1);

    iLay->addWidget(new QLabel("Binary:"));
    _isisPathEdit = new QLineEdit;
    _isisPathEdit->setPlaceholderText("Leave empty for system PATH lookup");
    iLay->addWidget(_isisPathEdit, 1);

    auto* isisBrowse = new QPushButton("…");
    isisBrowse->setMaximumWidth(30);
    iLay->addWidget(isisBrowse);

    nfLay->addWidget(isisGroup);

    connect(isisBrowse, &QPushButton::clicked, this, [this] {
        QString f = QFileDialog::getOpenFileName(
            this, "Select ISIS binary", QDir::homePath());
        if (!f.isEmpty()) {
            _isisPathEdit->setText(f);
            updateIsisStatus();
        }
    });
    connect(_isisPathEdit, &QLineEdit::editingFinished,
            this, &SEDFitDialog::updateIsisStatus);
    updateIsisStatus();

    // ── Grid search paths ────────────────────────────────────
    auto* pathGroup = new QGroupBox("Grid Search Paths");
    auto* pLay = new QHBoxLayout(pathGroup);

    _gridPathsEdit = new QLineEdit;
    _gridPathsEdit->setPlaceholderText(
        "Semicolon-separated base directories containing model grids");
    _gridPathsEdit->setText(
        QDir::homePath() + "/ISIS_models/;" +
        QDir::homePath() + "/isis/synthetic_spectra/grids/;" +
        "/data/stellar/modelgrids/");
    pLay->addWidget(_gridPathsEdit, 1);

    auto* pathBrowse = new QPushButton("Add…");
    pathBrowse->setMaximumWidth(50);
    pLay->addWidget(pathBrowse);

    nfLay->addWidget(pathGroup);

    connect(pathBrowse, &QPushButton::clicked, this, [this] {
        QString dir = QFileDialog::getExistingDirectory(
            this, "Select Grid Base Path");
        if (!dir.isEmpty()) {
            QString cur = _gridPathsEdit->text().trimmed();
            if (!cur.isEmpty() && !cur.endsWith(';')) cur += ";";
            _gridPathsEdit->setText(cur + dir);
            onSearchPathsChanged();
        }
    });
    connect(_gridPathsEdit, &QLineEdit::editingFinished,
            this, &SEDFitDialog::onSearchPathsChanged);

    // ── Grid selection ───────────────────────────────────────
    auto* gridGroup = new QGroupBox("Model Grid — Component 1");
    auto* gLay = new QGridLayout(gridGroup);
    gLay->setColumnStretch(1, 1);

    gLay->addWidget(new QLabel("Category:"), 0, 0);
    _gridCatCombo = new QComboBox;
    gLay->addWidget(_gridCatCombo, 0, 1, 1, 2);

    gLay->addWidget(new QLabel("Grid:"), 1, 0);
    _gridCombo = new QComboBox;
    _gridCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _gridCombo->setMinimumContentsLength(40);
    gLay->addWidget(_gridCombo, 1, 1, 1, 2);

    gLay->addWidget(new QLabel("Override:"), 2, 0);
    _gridOverrideEdit = new QLineEdit;
    _gridOverrideEdit->setPlaceholderText(
        "Custom grid relative path (leave empty to use combo)");
    gLay->addWidget(_gridOverrideEdit, 2, 1, 1, 2);

    nfLay->addWidget(gridGroup);

    // ── Component 2 grid ─────────────────────────────────────
    _enableComp2Cb = new QCheckBox("Enable second component grid");
    nfLay->addWidget(_enableComp2Cb);

    auto* grid2Group = new QGroupBox("Model Grid — Component 2");
    grid2Group->setVisible(false);
    auto* g2Lay = new QGridLayout(grid2Group);
    g2Lay->setColumnStretch(1, 1);

    g2Lay->addWidget(new QLabel("Category:"), 0, 0);
    _grid2CatCombo = new QComboBox;
    g2Lay->addWidget(_grid2CatCombo, 0, 1, 1, 2);

    g2Lay->addWidget(new QLabel("Grid:"), 1, 0);
    _grid2Combo = new QComboBox;
    _grid2Combo->setSizeAdjustPolicy(
        QComboBox::AdjustToMinimumContentsLengthWithIcon);
    _grid2Combo->setMinimumContentsLength(40);
    g2Lay->addWidget(_grid2Combo, 1, 1, 1, 2);

    g2Lay->addWidget(new QLabel("Override:"), 2, 0);
    _grid2OverrideEdit = new QLineEdit;
    _grid2OverrideEdit->setPlaceholderText(
        "Custom grid relative path (leave empty to use combo)");
    g2Lay->addWidget(_grid2OverrideEdit, 2, 1, 1, 2);

    nfLay->addWidget(grid2Group);

    connect(_gridCatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SEDFitDialog::onGridCategoryChanged);
    connect(_grid2CatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &SEDFitDialog::onGrid2CategoryChanged);
    connect(_enableComp2Cb, &QCheckBox::toggled,
            grid2Group, &QWidget::setVisible);

    // ── Distance ─────────────────────────────────────────────
    auto* distGroup = new QGroupBox("Distance");
    auto* dLay = new QHBoxLayout(distGroup);

    _fixDistCb = new QCheckBox("Fix distance:");
    dLay->addWidget(_fixDistCb);

    _distSpin = new QDoubleSpinBox;
    _distSpin->setRange(0.0, 1e6);
    _distSpin->setDecimals(4);
    _distSpin->setSuffix(" kpc");
    _distSpin->setEnabled(false);
    dLay->addWidget(_distSpin);
    dLay->addWidget(new QLabel("±"));
    _distErrSpin = new QDoubleSpinBox;
    _distErrSpin->setRange(0.0, 1e6);
    _distErrSpin->setDecimals(4);
    _distErrSpin->setSuffix(" kpc");
    _distErrSpin->setEnabled(false);
    dLay->addWidget(_distErrSpin);
    dLay->addStretch();

    connect(_fixDistCb, &QCheckBox::toggled, this, [this](bool on) {
        _distSpin->setEnabled(on);
        _distErrSpin->setEnabled(on);
    });

    if (Star::isSet(_star->getPlx()) && _star->getPlx() > 0) {
        double d_kpc = 1.0 / _star->getPlx();
        _distSpin->setValue(d_kpc);
        if (Star::isSet(_star->getEPlx()))
            _distErrSpin->setValue(d_kpc * d_kpc * _star->getEPlx());
    }
    nfLay->addWidget(distGroup);

    // ── Fit Parameters table ─────────────────────────────────
    auto* parGroup = new QGroupBox("Fit Parameters (par / par_full)");
    auto* pLayout = new QVBoxLayout(parGroup);

    _paramTableWidget = new QTableWidget(0, PP_COUNT);
    _paramTableWidget->setHorizontalHeaderLabels(
        {"Name", "Value", "Freeze", "Min", "Max"});
    _paramTableWidget->horizontalHeader()->setSectionResizeMode(
        PP_Name, QHeaderView::Stretch);
    _paramTableWidget->horizontalHeader()->setSectionResizeMode(
        PP_Value, QHeaderView::ResizeToContents);
    _paramTableWidget->horizontalHeader()->setSectionResizeMode(
        PP_Freeze, QHeaderView::ResizeToContents);
    _paramTableWidget->setMaximumHeight(180);
    _paramTableWidget->verticalHeader()->setDefaultSectionSize(24);
    pLayout->addWidget(_paramTableWidget);

    auto* parBtnLay = new QHBoxLayout;
    _addParamBtn = new QPushButton("+ Add");
    _removeParamBtn = new QPushButton("− Remove");
    parBtnLay->addWidget(_addParamBtn);
    parBtnLay->addWidget(_removeParamBtn);
    parBtnLay->addStretch();
    pLayout->addLayout(parBtnLay);
    nfLay->addWidget(parGroup);

    connect(_addParamBtn, &QPushButton::clicked,
            this, &SEDFitDialog::onAddParameter);
    connect(_removeParamBtn, &QPushButton::clicked,
            this, &SEDFitDialog::onRemoveParameter);

    // ── Options row ──────────────────────────────────────────
    auto* optGroup = new QGroupBox("Options");
    auto* oLay = new QHBoxLayout(optGroup);

    oLay->addWidget(new QLabel("Confidence:"));
    _confLevelCombo = new QComboBox;
    _confLevelCombo->addItem("None", -1);
    _confLevelCombo->addItem("68%",   0);
    _confLevelCombo->addItem("90%",   1);
    _confLevelCombo->addItem("99%",   2);
    _confLevelCombo->setCurrentIndex(1);
    oLay->addWidget(_confLevelCombo);

    oLay->addWidget(new QLabel("MC trials:"));
    _nmcSpin = new QSpinBox;
    _nmcSpin->setRange(1000, 50000000);
    _nmcSpin->setValue(2000000);
    _nmcSpin->setSingleStep(100000);
    oLay->addWidget(_nmcSpin);

    _writeModelCb = new QCheckBox("Write model");
    _writeModelCb->setChecked(true);
    oLay->addWidget(_writeModelCb);
    _saveMCCb = new QCheckBox("Save MC");
    _saveMCCb->setChecked(false);
    oLay->addWidget(_saveMCCb);
    _applyZPOCb = new QCheckBox("Apply ZPO");
    _applyZPOCb->setChecked(true);
    _applyZPOCb->setToolTip(
        "Apply empirical corrections to photometric zero-point offsets");
    oLay->addWidget(_applyZPOCb);
    oLay->addStretch();
    nfLay->addWidget(optGroup);

    // ── Advanced options ─────────────────────────────────────
    nfLay->addWidget(createAdvancedOptions());

    // ── Run / preview ────────────────────────────────────────
    auto* runLay = new QHBoxLayout;
    _runFitBtn = new QPushButton("▶ Run Fit");
    _runFitBtn->setEnabled(isIsisAvailable());
    _runFitBtn->setStyleSheet("font-weight: bold; padding: 6px 20px;");
    runLay->addWidget(_runFitBtn);

    _previewBtn = new QPushButton("Preview Script…");
    runLay->addWidget(_previewBtn);
    runLay->addStretch();

    _isisProgress = new QProgressBar;
    _isisProgress->setVisible(false);
    _isisProgress->setRange(0, 0);
    runLay->addWidget(_isisProgress);
    nfLay->addLayout(runLay);

    _isisOutput = new QTextEdit;
    _isisOutput->setReadOnly(true);
    _isisOutput->setMaximumHeight(150);
    _isisOutput->setVisible(false);
    _isisOutput->setFont(QFont("monospace", 8));
    nfLay->addWidget(_isisOutput);

    _newFitScroll->setWidget(scrollContent);
    vlay->addWidget(_newFitScroll);

    connect(_newFitToggleBtn, &QPushButton::clicked, this, [this] {
        bool vis = !_newFitScroll->isVisible();
        _newFitScroll->setVisible(vis);
        _newFitToggleBtn->setText(
            vis ? "▾ New Fit Configuration"
                : "▸ New Fit Configuration");
    });

    connect(_runFitBtn, &QPushButton::clicked, this, &SEDFitDialog::onRunFit);
    connect(_previewBtn, &QPushButton::clicked, this, [this] {
        QDialog dlg(this);
        dlg.setWindowTitle("Script Preview");
        dlg.resize(700, 500);
        auto* l = new QVBoxLayout(&dlg);
        auto* te = new QTextEdit;
        te->setReadOnly(true);
        te->setFont(QFont("monospace", 9));
        te->setPlainText(generateScript());
        l->addWidget(te);
        auto* cb = new QPushButton("Close");
        connect(cb, &QPushButton::clicked, &dlg, &QDialog::accept);
        l->addWidget(cb);
        dlg.exec();
    });

    return container;
}

void SEDFitDialog::updateIsisStatus()
{
    QString bin = findIsisBinary();
    if (!bin.isEmpty()) {
        _isisStatusLabel->setText("✓ ISIS: " + bin);
        _isisStatusLabel->setStyleSheet("color: green;");
    } else {
        _isisStatusLabel->setText(
            "✗ ISIS not found — specify path or install to PATH");
        _isisStatusLabel->setStyleSheet("color: #cc4444; font-style: italic;");
    }
    if (_runFitBtn)
        _runFitBtn->setEnabled(!bin.isEmpty());
}

void SEDFitDialog::discoverGrids()
{
    _discoveredGrids.clear();

    QStringList bases =
        _gridPathsEdit->text().split(';', Qt::SkipEmptyParts);

    QSet<QString> seen;
    const auto& presets = gridPresets();

    for (const QString& raw : bases) {
        QString base = raw.trimmed();
        if (base.isEmpty()) continue;
        QDir baseDir(base);
        if (!baseDir.exists()) continue;
        QString baseCan = baseDir.canonicalPath();

        std::function<void(const QString&, int)> scan =
            [&](const QString& dir, int depth)
        {
            if (depth > 5) return;
            QDir d(dir);

            if (d.exists("grid.fits")) {
                QString canon = QDir(dir).canonicalPath();
                if (seen.contains(canon)) return;
                seen.insert(canon);

                DiscoveredGrid dg;
                dg.fullPath     = canon;
                dg.basePath     = baseCan;
                dg.relativePath = baseDir.relativeFilePath(canon);
                if (!dg.relativePath.endsWith('/'))
                    dg.relativePath += '/';

                // Match against known presets by suffix
                QString normFull = canon;
                for (int pi = 0; pi < static_cast<int>(presets.size()); ++pi) {
                    QString suffix = presets[pi].path;
                    while (suffix.endsWith('/')) suffix.chop(1);
                    if (normFull.endsWith(suffix)) {
                        dg.presetIndex = pi;
                        dg.category    = presets[pi].category;
                        dg.displayName = presets[pi].name;
                        dg.teffMin     = presets[pi].teffMin;
                        dg.teffMax     = presets[pi].teffMax;
                        dg.loggMin     = presets[pi].loggMin;
                        dg.loggMax     = presets[pi].loggMax;
                        dg.heMin       = presets[pi].heMin;
                        dg.heMax       = presets[pi].heMax;
                        dg.zMin        = presets[pi].zMin;
                        dg.zMax        = presets[pi].zMax;
                        break;
                    }
                }

                if (dg.presetIndex < 0) {
                    dg.category    = "Discovered";
                    dg.displayName = dg.relativePath;
                }

                _discoveredGrids.push_back(std::move(dg));
            }

            for (const auto& sub :
                 d.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
            {
                if (sub.startsWith('.')) continue;
                scan(dir + "/" + sub, depth + 1);
            }
        };

        scan(baseCan, 0);
    }

    populateGridCombos();
}


// ── Advanced options sub-panel ───────────────────────────────────

QWidget* SEDFitDialog::createAdvancedOptions()
{
    auto* w = new QWidget;
    auto* topLay = new QVBoxLayout(w);
    topLay->setContentsMargins(0, 0, 0, 0);
    topLay->setSpacing(0);

    _advToggleBtn = new QPushButton("▸ Advanced Options");
    _advToggleBtn->setFlat(true);
    _advToggleBtn->setStyleSheet("text-align: left; padding: 2px;");
    topLay->addWidget(_advToggleBtn);

    _advContent = new QWidget;
    _advContent->setVisible(false);
    auto* aLay = new QGridLayout(_advContent);
    aLay->setContentsMargins(8, 2, 8, 2);
    int r = 0;

    // Stilism
    _stilDistSimpleCb = new QCheckBox("Stilism distance simple");
    _stilDistSimpleCb->setChecked(true);
    aLay->addWidget(_stilDistSimpleCb, r, 0);
    _stilEbmvSimpleCb = new QCheckBox("Stilism E(B-V) simple");
    _stilEbmvSimpleCb->setChecked(true);
    aLay->addWidget(_stilEbmvSimpleCb, r, 1);
    _stilEbmvRerunCb = new QCheckBox("Stilism E(B-V) rerun");
    _stilEbmvRerunCb->setChecked(true);
    aLay->addWidget(_stilEbmvRerunCb, r++, 2);

    // Canonical mass
    aLay->addWidget(new QLabel("Canonical mass:"), r, 0);
    _massCanSpin = new QDoubleSpinBox;
    _massCanSpin->setRange(0, 100);
    _massCanSpin->setDecimals(3);
    _massCanSpin->setValue(0);
    _massCanSpin->setToolTip("If > 0, compute spectroscopic distances from this mass");
    aLay->addWidget(_massCanSpin, r, 1);
    _deltaMassCanSpin = new QDoubleSpinBox;
    _deltaMassCanSpin->setRange(0, 50);
    _deltaMassCanSpin->setDecimals(3);
    _deltaMassCanSpin->setValue(0.05);
    _deltaMassCanSpin->setPrefix("± ");
    aLay->addWidget(_deltaMassCanSpin, r++, 2);

    // HB / logg options
    _deriveLoggCb = new QCheckBox("Derive logg from (IA)HB");
    aLay->addWidget(_deriveLoggCb, r, 0);
    _hbDistanceCb = new QCheckBox("HB distance");
    aLay->addWidget(_hbDistanceCb, r++, 1);

    // Component 2 logg
    _deriveLoggC2Cb = new QCheckBox("Derive c2_logg from MS");
    aLay->addWidget(_deriveLoggC2Cb, r, 0);
    aLay->addWidget(new QLabel("Z_c2:"), r, 1);
    _zC2Spin = new QDoubleSpinBox;
    _zC2Spin->setRange(-5, 5);
    _zC2Spin->setDecimals(2);
    _zC2Spin->setValue(-0.9);
    aLay->addWidget(_zC2Spin, r++, 2);

    // Surface ratio
    _deriveSRCb = new QCheckBox("Derive surface ratio");
    aLay->addWidget(_deriveSRCb, r, 0);
    aLay->addWidget(new QLabel("sdOB R:"), r, 1);
    _sdOBRadSpin = new QDoubleSpinBox;
    _sdOBRadSpin->setRange(0, 100);
    _sdOBRadSpin->setDecimals(2);
    _sdOBRadSpin->setValue(0.2);
    _sdOBRadSpin->setSuffix(" R☉");
    aLay->addWidget(_sdOBRadSpin, r++, 2);

    // R1 for eclipsing binaries
    aLay->addWidget(new QLabel("R₁ (eclipsing):"), r, 0);
    _r1Spin = new QDoubleSpinBox;
    _r1Spin->setRange(0, 1000);
    _r1Spin->setDecimals(3);
    _r1Spin->setValue(0);
    _r1Spin->setSuffix(" R☉");
    aLay->addWidget(_r1Spin, r, 1);
    _r1ErrSpin = new QDoubleSpinBox;
    _r1ErrSpin->setRange(0, 100);
    _r1ErrSpin->setDecimals(3);
    _r1ErrSpin->setValue(0.01);
    _r1ErrSpin->setPrefix("± ");
    _r1ErrSpin->setSuffix(" R☉");
    aLay->addWidget(_r1ErrSpin, r++, 2);

    topLay->addWidget(_advContent);

    connect(_advToggleBtn, &QPushButton::clicked, this, [this] {
        bool vis = !_advContent->isVisible();
        _advContent->setVisible(vis);
        _advToggleBtn->setText(vis ? "▾ Advanced Options"
                                   : "▸ Advanced Options");
    });

    return w;
}

// ═══════════════════════════════════════════════════════════════════
// Grid combo population
// ═══════════════════════════════════════════════════════════════════

void SEDFitDialog::populateGridCombos()
{
    // Collect categories that have at least one discovered grid
    QStringList cats;
    for (const auto& dg : _discoveredGrids)
        if (!cats.contains(dg.category)) cats << dg.category;

    // Also include all preset categories even if absent
    // (they'll just show 0 grids, making it clear nothing was found)
    for (const auto& p : gridPresets())
        if (!cats.contains(p.category)) cats << p.category;

    auto fillCat = [&](QComboBox* catCombo, QComboBox* gridCombo) {
        QString prevCat = catCombo->currentText();
        catCombo->blockSignals(true);
        catCombo->clear();

        for (const auto& c : cats) {
            int n = 0;
            for (const auto& dg : _discoveredGrids)
                if (dg.category == c) ++n;
            catCombo->addItem(
                n > 0 ? QString("%1  (%2 found)").arg(c).arg(n)
                       : QString("%1  (none found)").arg(c),
                c);
        }

        int idx = catCombo->findData(prevCat);
        if (idx >= 0) catCombo->setCurrentIndex(idx);
        catCombo->blockSignals(false);

        // Populate grid combo for current category
        QString cat = catCombo->currentData().toString();
        gridCombo->blockSignals(true);
        gridCombo->clear();
        for (int i = 0; i < static_cast<int>(_discoveredGrids.size()); ++i) {
            const auto& dg = _discoveredGrids[i];
            if (dg.category != cat) continue;
            QString label;
            if (dg.presetIndex >= 0) {
                label = QString("%1  (%2–%3 kK, logg %4–%5)")
                            .arg(dg.displayName)
                            .arg(dg.teffMin / 1000.0, 0, 'f', 0)
                            .arg(dg.teffMax / 1000.0, 0, 'f', 0)
                            .arg(dg.loggMin, 0, 'f', 1)
                            .arg(dg.loggMax, 0, 'f', 1);
            } else {
                label = dg.displayName;
            }
            gridCombo->addItem(label, i);
        }
        gridCombo->blockSignals(false);
    };

    fillCat(_gridCatCombo, _gridCombo);
    fillCat(_grid2CatCombo, _grid2Combo);
}

void SEDFitDialog::onSearchPathsChanged()
{
    discoverGrids();
}

void SEDFitDialog::onGridCategoryChanged(int)
{
    QString cat = _gridCatCombo->currentData().toString();
    _gridCombo->blockSignals(true);
    _gridCombo->clear();
    for (int i = 0; i < static_cast<int>(_discoveredGrids.size()); ++i) {
        const auto& dg = _discoveredGrids[i];
        if (dg.category != cat) continue;
        QString label;
        if (dg.presetIndex >= 0) {
            label = QString("%1  (%2–%3 kK, logg %4–%5)")
                        .arg(dg.displayName)
                        .arg(dg.teffMin / 1000.0, 0, 'f', 0)
                        .arg(dg.teffMax / 1000.0, 0, 'f', 0)
                        .arg(dg.loggMin, 0, 'f', 1)
                        .arg(dg.loggMax, 0, 'f', 1);
        } else {
            label = dg.displayName;
        }
        _gridCombo->addItem(label, i);
    }
    _gridCombo->blockSignals(false);
}

void SEDFitDialog::onGrid2CategoryChanged(int)
{
    QString cat = _grid2CatCombo->currentData().toString();
    _grid2Combo->blockSignals(true);
    _grid2Combo->clear();
    for (int i = 0; i < static_cast<int>(_discoveredGrids.size()); ++i) {
        const auto& dg = _discoveredGrids[i];
        if (dg.category != cat) continue;
        QString label;
        if (dg.presetIndex >= 0) {
            label = QString("%1  (%2–%3 kK, logg %4–%5)")
                        .arg(dg.displayName)
                        .arg(dg.teffMin / 1000.0, 0, 'f', 0)
                        .arg(dg.teffMax / 1000.0, 0, 'f', 0)
                        .arg(dg.loggMin, 0, 'f', 1)
                        .arg(dg.loggMax, 0, 'f', 1);
        } else {
            label = dg.displayName;
        }
        _grid2Combo->addItem(label, i);
    }
    _grid2Combo->blockSignals(false);
}

// ═══════════════════════════════════════════════════════════════════
// Default fit parameter table
// ═══════════════════════════════════════════════════════════════════

void SEDFitDialog::initDefaultFitParams()
{
    double teff = 25000, logg = 5.5, he = -3.0, z = 0.0;

    // Pre-fill from best SED fit (component 1 only)
    for (const auto& f : _fits) {
        if (f->isBestFit && !f->components.empty()) {
            auto& c1 = f->components[0];
            if (c1.teff > 0) teff = c1.teff;
            if (c1.logg > 0) logg = c1.logg;
            if (c1.heAbundance != 0) he = c1.heAbundance;
            if (c1.metallicity != 0) z = c1.metallicity;
            break;
        }
    }
    // Fall back to star spectroscopic values (also c1)
    if (Star::isSet(_star->getTeff())) teff = _star->getTeff();
    if (Star::isSet(_star->getLogg())) logg = _star->getLogg();
    if (Star::isSet(_star->getHe()))   he   = _star->getHe();

    // All frozen by default — user unfreezes what they want to fit
    _fitParams = {
        {"c*_xi",   0.0,  true,  0,    0,    false},
        {"c*_z",    z,    true,  -2.5, 1.0,  true},
        {"c*_HE",   he,   true,  he - 2.0, std::min(he + 2.0, 0.0), true},
        {"c*_logg", logg, true,  std::max(logg - 1.5, 0.0), logg + 1.5, true},
        {"c*_teff", teff, true,  teff * 0.7, teff * 1.3, true},
        {"R_55",    3.02, true,  2.5, 6.0, true},
    };

    _paramTableWidget->setRowCount(static_cast<int>(_fitParams.size()));
    for (int i = 0; i < static_cast<int>(_fitParams.size()); ++i) {
        const auto& p = _fitParams[i];

        _paramTableWidget->setItem(i, PP_Name,
            new QTableWidgetItem(p.name));
        _paramTableWidget->setItem(i, PP_Value,
            new QTableWidgetItem(QString::number(p.value, 'g', 8)));

        auto* fzItem = new QTableWidgetItem;
        fzItem->setFlags(fzItem->flags() | Qt::ItemIsUserCheckable);
        fzItem->setCheckState(p.frozen ? Qt::Checked : Qt::Unchecked);
        fzItem->setText("");
        _paramTableWidget->setItem(i, PP_Freeze, fzItem);

        _paramTableWidget->setItem(i, PP_Min,
            new QTableWidgetItem(
                p.hasRange ? QString::number(p.min, 'g', 8) : QString()));
        _paramTableWidget->setItem(i, PP_Max,
            new QTableWidgetItem(
                p.hasRange ? QString::number(p.max, 'g', 8) : QString()));
    }
}

void SEDFitDialog::onAddParameter()
{
    int row = _paramTableWidget->rowCount();
    _paramTableWidget->insertRow(row);

    _paramTableWidget->setItem(row, PP_Name,  new QTableWidgetItem("c1_param"));
    _paramTableWidget->setItem(row, PP_Value, new QTableWidgetItem("0"));

    auto* fz = new QTableWidgetItem;
    fz->setFlags(fz->flags() | Qt::ItemIsUserCheckable);
    fz->setCheckState(Qt::Checked);
    fz->setText("");
    _paramTableWidget->setItem(row, PP_Freeze, fz);

    _paramTableWidget->setItem(row, PP_Min, new QTableWidgetItem());
    _paramTableWidget->setItem(row, PP_Max, new QTableWidgetItem());
    _paramTableWidget->editItem(_paramTableWidget->item(row, PP_Name));
}

void SEDFitDialog::onRemoveParameter()
{
    auto sel = _paramTableWidget->selectedItems();
    if (sel.isEmpty()) return;
    QSet<int> rows;
    for (auto* it : sel) rows.insert(it->row());
    auto sorted = rows.values();
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int r : sorted)
        _paramTableWidget->removeRow(r);
}

// ═══════════════════════════════════════════════════════════════════
// Data loading
// ═══════════════════════════════════════════════════════════════════

void SEDFitDialog::loadExistingFits()
{
    auto phot = _star->getPhotometry();
    if (!phot) {
        updateFitSelector();
        return;
    }

    _fits = phot->getSEDModels();

    for (auto& model : _fits) {
        if (model->modelWavelengths.empty() &&
            !model->getModelDataFile().isEmpty()) {
            model->loadDataFromFile(model->getModelDataFile());
        }
    }

    updateFitSelector();

    _currentFitIndex = -1;
    for (int i = 0; i < static_cast<int>(_fits.size()); ++i) {
        if (_fits[i]->isBestFit) {
            _currentFitIndex = i;
            break;
        }
    }
    if (_currentFitIndex < 0 && !_fits.empty())
        _currentFitIndex = 0;

    if (_currentFitIndex >= 0)
        _fitCombo->setCurrentIndex(_currentFitIndex);
    else {
        updatePlot();
        updateParameterDisplay();
        updatePhotometryTable();
    }
}

void SEDFitDialog::updateFitSelector()
{
    _fitCombo->blockSignals(true);
    _fitCombo->clear();

    if (_fits.empty()) {
        _fitCombo->addItem("(no fits available)");
        _fitCombo->setEnabled(false);
        _setBestFitBtn->setEnabled(false);
        _deleteFitBtn->setEnabled(false);
    } else {
        _fitCombo->setEnabled(true);
        for (int i = 0; i < static_cast<int>(_fits.size()); ++i) {
            auto& m = _fits[i];
            QString label;
            if (m->isBestFit) label += "★ ";

            // Date
            label += m->creationDate.toString("yyyy-MM-dd");

            // Key parameters from component 1
            if (!m->components.empty()) {
                auto& c = m->components[0];
                if (c.teff > 0)
                    label += QString("  Teff=%1K").arg(c.teff, 0, 'f', 0);
                if (c.logg > 0)
                    label += QString("  logg=%1").arg(c.logg, 0, 'f', 2);
            }

            if (m->chi2Reduced > 0)
                label += QString("  χ²=%1").arg(m->chi2Reduced, 0, 'f', 3);

            label += QString("  (%1 comp)").arg(m->numComponents);

            _fitCombo->addItem(label);
        }
        _setBestFitBtn->setEnabled(true);
        _deleteFitBtn->setEnabled(true);
    }
    _fitCombo->blockSignals(false);
}

// ═══════════════════════════════════════════════════════════════════
// Fit selection
// ═══════════════════════════════════════════════════════════════════

void SEDFitDialog::onFitSelected(int index)
{
    if (index < 0 || index >= static_cast<int>(_fits.size())) {
        _currentFitIndex = -1;
    } else {
        _currentFitIndex = index;
    }

    updatePlot();
    updateResidualPlot();
    updateParameterDisplay();
    updatePhotometryTable();
}

void SEDFitDialog::onSetBestFit()
{
    if (_currentFitIndex < 0 || _currentFitIndex >= static_cast<int>(_fits.size()))
        return;

    // Update in-memory flags
    for (auto& m : _fits)
        m->isBestFit = false;
    _fits[_currentFitIndex]->isBestFit = true;

    // Update the photometry container
    auto phot = _star->getPhotometry();
    if (phot) {
        auto models = phot->getSEDModels();
        for (auto& m : models)
            m->isBestFit = false;
        for (auto& m : models) {
            if (m->getId() == _fits[_currentFitIndex]->getId())
                m->isBestFit = true;
        }
    }

    // Apply to star summary fields
    applyBestFitToStar(_fits[_currentFitIndex]);

    // Save to database if available
    if (_dbm) {
        for (auto& m : _fits) {
            _dbm->saveSEDModelForStar(_star->getId(), m);
        }
        _dbm->saveStar(_projectId, _star);
    }

    updateFitSelector();
    _fitCombo->setCurrentIndex(_currentFitIndex);

    emit fitDataChanged();
    LOG_INFO("SED", QString("Set fit %1 as best for %2")
                        .arg(_fits[_currentFitIndex]->getId())
                        .arg(_star->getSourceId()));
}

void SEDFitDialog::onDeleteFit()
{
    if (_currentFitIndex < 0 || _currentFitIndex >= static_cast<int>(_fits.size()))
        return;

    auto answer = QMessageBox::question(this, "Delete SED Fit",
                                        "Delete the selected SED fit?",
                                        QMessageBox::Yes | QMessageBox::No);
    if (answer != QMessageBox::Yes) return;

    bool wasBest = _fits[_currentFitIndex]->isBestFit;
    _fits.erase(_fits.begin() + _currentFitIndex);

    // If we deleted the best fit, mark the first remaining as best
    if (wasBest && !_fits.empty()) {
        _fits[0]->isBestFit = true;
        applyBestFitToStar(_fits[0]);
    }

    _currentFitIndex = _fits.empty() ? -1 : 0;
    updateFitSelector();
    if (_currentFitIndex >= 0)
        _fitCombo->setCurrentIndex(_currentFitIndex);
    else
        onFitSelected(-1);

    emit fitDataChanged();
}

// ═══════════════════════════════════════════════════════════════════
// SED Plot
// ═══════════════════════════════════════════════════════════════════

void SEDFitDialog::updatePlot()
{
    _sedPlot->clearPlottables();

    if (_currentFitIndex < 0 || _currentFitIndex >= static_cast<int>(_fits.size())) {
        _sedPlot->xAxis->setRange(500, 60000);
        _sedPlot->yAxis->setRange(1e-18, 1e-13);
        _sedPlot->replot();
        return;
    }

    auto& model = _fits[_currentFitIndex];
    bool hasCurve = !model->modelWavelengths.empty();

    // Track data range for auto-scaling
    double xMin = 1e30, xMax = 0, yMin = 1e30, yMax = 0;

    // ── Model SED curve (total) ──────────────────────────────
    if (hasCurve) {
        QVector<double> wl(model->modelWavelengths.begin(),
                           model->modelWavelengths.end());
        QVector<double> fl(model->modelFluxes.begin(),
                           model->modelFluxes.end());

        auto* totalGraph = _sedPlot->addGraph();
        totalGraph->setData(wl, fl);
        totalGraph->setPen(QPen(modelCurveColor(), 2));
        totalGraph->setName("Total model");

        for (auto v : wl) { xMin = qMin(xMin, v); xMax = qMax(xMax, v); }
        for (auto v : fl) { if (v > 0) { yMin = qMin(yMin, v); yMax = qMax(yMax, v); } }

        // ── Component curves ─────────────────────────────────
        QList<QColor> compColors = { comp1Color(), comp2Color(),
                                     QColor("#8E44AD"), QColor("#16A085") };
        for (int c = 0; c < static_cast<int>(model->componentFluxes.size()); ++c) {
            if (model->componentFluxes[c].empty()) continue;
            // Skip if identical to total (single component)
            if (model->componentFluxes.size() == 1) break;

            QVector<double> cf(model->componentFluxes[c].begin(),
                               model->componentFluxes[c].end());
            auto* cGraph = _sedPlot->addGraph();
            cGraph->setData(wl, cf);
            QPen pen(compColors[c % compColors.size()], 1.5, Qt::DashLine);
            cGraph->setPen(pen);
            cGraph->setName(QString("Component %1").arg(c + 1));
        }
    }

    // ── Observed photometry points ───────────────────────────
    QVector<double> incL, incF, incFErrLo, incFErrHi, incLErrLo, incLErrHi;
    QVector<double> excL, excF;

    for (const auto& p : model->observedPoints) {
        if (p.lambda <= 0 || p.flux <= 0) continue;

        if (p.flag >= 0) {
            incL.append(p.lambda);
            incF.append(p.flux);
            incFErrLo.append(p.flux - p.fluxMin);
            incFErrHi.append(p.fluxMax - p.flux);
            incLErrLo.append(p.lambda - p.lambdaMin);
            incLErrHi.append(p.lambdaMax - p.lambda);
        } else {
            excL.append(p.lambda);
            excF.append(p.flux);
        }

        xMin = qMin(xMin, p.lambdaMin > 0 ? p.lambdaMin : p.lambda);
        xMax = qMax(xMax, p.lambdaMax > 0 ? p.lambdaMax : p.lambda);
        yMin = qMin(yMin, p.fluxMin > 0 ? p.fluxMin : p.flux);
        yMax = qMax(yMax, p.fluxMax > 0 ? p.fluxMax : p.flux);
    }

    // Included points with error bars
    if (!incL.isEmpty()) {
        auto* incGraph = _sedPlot->addGraph();
        incGraph->setData(incL, incF);
        incGraph->setLineStyle(QCPGraph::lsNone);
        incGraph->setScatterStyle(QCPScatterStyle(
            QCPScatterStyle::ssDisc, includedPointColor(), 7));
        incGraph->setName("Included");

        auto* vErr = new QCPErrorBars(_sedPlot->xAxis, _sedPlot->yAxis);
        vErr->removeFromLegend();
        vErr->setDataPlottable(incGraph);
        vErr->setErrorType(QCPErrorBars::etValueError);
        vErr->setPen(QPen(includedPointColor(), 1));
        vErr->setData(incFErrLo, incFErrHi);

        auto* hErr = new QCPErrorBars(_sedPlot->xAxis, _sedPlot->yAxis);
        hErr->removeFromLegend();
        hErr->setDataPlottable(incGraph);
        hErr->setErrorType(QCPErrorBars::etKeyError);
        hErr->setPen(QPen(includedPointColor(), 1));
        hErr->setData(incLErrLo, incLErrHi);
    }

    // Excluded points (no error bars)
    if (!excL.isEmpty()) {
        auto* excGraph = _sedPlot->addGraph();
        excGraph->setData(excL, excF);
        excGraph->setLineStyle(QCPGraph::lsNone);
        excGraph->setScatterStyle(QCPScatterStyle(
            QCPScatterStyle::ssCircle, excludedPointColor(), excludedPointColor(), 6));
        excGraph->setName("Excluded");
    }

    // ── Auto-range ───────────────────────────────────────────
    if (xMin < xMax && yMin < yMax) {
        double xPad = 0.15, yPad = 0.3;
        double lxMin = std::log10(xMin), lxMax = std::log10(xMax);
        double lyMin = std::log10(yMin), lyMax = std::log10(yMax);
        _sedPlot->xAxis->setRange(std::pow(10, lxMin - xPad),
                                   std::pow(10, lxMax + xPad));
        _sedPlot->yAxis->setRange(std::pow(10, lyMin - yPad),
                                   std::pow(10, lyMax + yPad));
    }

    _sedPlot->replot();
    updateResidualPlot();
}

// ═══════════════════════════════════════════════════════════════════
// Residual plot
// ═══════════════════════════════════════════════════════════════════

void SEDFitDialog::updateResidualPlot()
{
    _residualPlot->clearPlottables();

    if (_currentFitIndex < 0 || _currentFitIndex >= static_cast<int>(_fits.size())) {
        _residualPlot->replot();
        return;
    }

    auto& model = _fits[_currentFitIndex];

    // Add zero line
    auto* zeroLine = _residualPlot->addGraph();
    QVector<double> zx = {_sedPlot->xAxis->range().lower,
                          _sedPlot->xAxis->range().upper};
    QVector<double> zy = {0, 0};
    zeroLine->setData(zx, zy);
    zeroLine->setPen(QPen(Qt::gray, 1, Qt::DashLine));
    zeroLine->removeFromLegend();

    // Separate included/excluded residuals
    QVector<double> incL, incR, incE;
    QVector<double> excL, excR;

    for (const auto& p : model->observedPoints) {
        if (p.lambda <= 0) continue;
        double res = (p.diffErr > 0) ? p.diff / p.diffErr : p.diff;

        if (p.flag >= 0) {
            incL.append(p.lambda);
            incR.append(res);
            incE.append((p.diffErr > 0) ? 1.0 : 0.0);
        } else {
            excL.append(p.lambda);
            excR.append(res);
        }
    }

    if (!incL.isEmpty()) {
        auto* g = _residualPlot->addGraph();
        g->setData(incL, incR);
        g->setLineStyle(QCPGraph::lsNone);
        g->setScatterStyle(QCPScatterStyle(
            QCPScatterStyle::ssDisc, includedPointColor(), 5));
        g->removeFromLegend();
    }

    if (!excL.isEmpty()) {
        auto* g = _residualPlot->addGraph();
        g->setData(excL, excR);
        g->setLineStyle(QCPGraph::lsNone);
        g->setScatterStyle(QCPScatterStyle(
            QCPScatterStyle::ssCircle, excludedPointColor(), excludedPointColor(), 5));
        g->removeFromLegend();
    }

    // Sync x range, auto-range y
    _residualPlot->xAxis->setRange(_sedPlot->xAxis->range());
    double maxR = 4.0;
    for (auto v : incR) maxR = qMax(maxR, std::abs(v) + 1.0);
    for (auto v : excR) maxR = qMax(maxR, std::abs(v) + 1.0);
    _residualPlot->yAxis->setRange(-maxR, maxR);

    _residualPlot->replot();
}

// ═══════════════════════════════════════════════════════════════════
// Parameter display (HTML)
// ═══════════════════════════════════════════════════════════════════

QString SEDFitDialog::formatAsymVal(double val, double up, double down,
                                    int prec, const QString& unit) const
{
    if (up == 0.0 && down == 0.0)
        return QString("<b>%1</b>%2").arg(val, 0, 'f', prec).arg(unit.isEmpty() ? "" : " " + unit);
    if (qAbs(up - down) < 1e-10 * (qAbs(up) + 1e-30))
        return QString("<b>%1</b> ± %2%3")
            .arg(val, 0, 'f', prec).arg(up, 0, 'f', prec)
            .arg(unit.isEmpty() ? "" : " " + unit);
    return QString("<b>%1</b> <sup>+%2</sup><sub>−%3</sub>%4")
        .arg(val, 0, 'f', prec).arg(up, 0, 'f', prec).arg(down, 0, 'f', prec)
        .arg(unit.isEmpty() ? "" : " " + unit);
}

QString SEDFitDialog::formatParamRow(const QString& label,
                                     const QString& value) const
{
    return QString("<tr><td style='color:gray; padding-right:12px;'>%1</td>"
                   "<td>%2</td></tr>").arg(label, value);
}

QString SEDFitDialog::statusTag(int status) const
{
    switch (status) {
    case 2:  return " <i style='color:gray;'>(fixed)</i>";
    case 1:  return " <i style='color:gray;'>(prescribed)</i>";
    default: return "";
    }
}

void SEDFitDialog::updateParameterDisplay()
{
    if (_currentFitIndex < 0 || _currentFitIndex >= static_cast<int>(_fits.size())) {
        _paramLabel->setText("<i style='color:gray;'>No fit selected</i>");
        return;
    }

    auto& m = _fits[_currentFitIndex];
    QColor accent = isDarkTheme() ? QColor("#7EC8E3") : QColor("#2980B9");
    QColor c1col  = comp1Color();
    QColor c2col  = comp2Color();
    QColor distcol = isDarkTheme() ? QColor("#A3D977") : QColor("#27AE60");

    QString html;
    QTextStream s(&html);

    // ── Global ───────────────────────────────────────────────
    s << "<h3 style='color:" << accent.name() << ";margin-bottom:4px;'>Global</h3>";
    s << "<table cellpadding='2'>";

    if (m->e4455 > 0 || m->e4455Error > 0)
        s << formatParamRow("E(44−55)",
               formatAsymVal(m->e4455, m->e4455Error, m->e4455Error, 3));
    if (m->r55 > 0)
        s << formatParamRow("R(55)", QString("<b>%1</b>").arg(m->r55, 0, 'f', 2));
    if (m->logTheta != 0)
        s << formatParamRow("log θ",
               formatAsymVal(m->logTheta, m->logThetaError, m->logThetaError, 3));
    if (m->chi2Reduced > 0)
        s << formatParamRow("χ²<sub>red</sub>",
               QString("<b>%1</b>").arg(m->chi2Reduced, 0, 'f', 3));
    if (m->excessNoise > 0)
        s << formatParamRow("δ<sub>excess</sub>",
               QString("<b>%1</b>").arg(m->excessNoise, 0, 'f', 3));
    if (m->ebvSFD > 0)
        s << formatParamRow("E(B−V)<sub>SFD</sub>",
               formatAsymVal(m->ebvSFD, m->ebvSFDError, m->ebvSFDError, 3));
    if (m->ebvSF > 0)
        s << formatParamRow("E(B−V)<sub>S&amp;F</sub>",
               formatAsymVal(m->ebvSF, m->ebvSFError, m->ebvSFError, 3));
    s << "</table>";

    // ── Components ───────────────────────────────────────────
    QList<QColor> compColors = { c1col, c2col };
    for (int ci = 0; ci < static_cast<int>(m->components.size()); ++ci) {
        auto& c = m->components[ci];
        QColor cc = compColors[ci % compColors.size()];

        s << "<h3 style='color:" << cc.name() << ";margin-bottom:4px;'>"
          << "Component " << (ci + 1) << "</h3>";
        s << "<table cellpadding='2'>";

        if (c.teff > 0)
            s << formatParamRow("T<sub>eff</sub>",
                   formatAsymVal(c.teff, c.teffErrUp, c.teffErrDown, 0, "K")
                   + statusTag(static_cast<int>(c.teffStatus)));
        if (c.logg > 0)
            s << formatParamRow("log g",
                   formatAsymVal(c.logg, c.loggErrUp, c.loggErrDown, 2)
                   + statusTag(static_cast<int>(c.loggStatus)));
        if (c.heAbundance != 0)
            s << formatParamRow("log n(He)",
                   formatAsymVal(c.heAbundance, c.heAbundanceErrUp,
                                 c.heAbundanceErrDown, 2)
                   + statusTag(static_cast<int>(c.heAbundanceStatus)));
        if (c.metallicity != 0 || c.metallicityStatus == SEDParamStatus::Fixed)
            s << formatParamRow("[Z]",
                   QString("<b>%1</b>").arg(c.metallicity, 0, 'f', 2)
                   + statusTag(static_cast<int>(c.metallicityStatus)));
        if (c.surfaceRatio > 0 && c.surfaceRatio != 1.0)
            s << formatParamRow("Surf. ratio",
                   formatAsymVal(c.surfaceRatio, c.surfaceRatioErrUp,
                                 c.surfaceRatioErrDown, 3));

        // Derived quantities
        if (c.radius.isValid())
            s << formatParamRow("R",
                   formatAsymVal(c.radius.value, c.radius.errUp,
                                 c.radius.errDown, 3, "R☉"));
        if (c.mass.isValid())
            s << formatParamRow("M",
                   formatAsymVal(c.mass.value, c.mass.errUp,
                                 c.mass.errDown, 3, "M☉"));
        if (c.luminosity.isValid())
            s << formatParamRow("L",
                   formatAsymVal(c.luminosity.value, c.luminosity.errUp,
                                 c.luminosity.errDown, 1, "L☉"));
        if (c.vGrav.isValid())
            s << formatParamRow("v<sub>grav</sub>",
                   formatAsymVal(c.vGrav.value, c.vGrav.errUp,
                                 c.vGrav.errDown, 1, "km/s"));
        s << "</table>";
    }

    // ── Distance ─────────────────────────────────────────────
    s << "<h3 style='color:" << distcol.name() << ";margin-bottom:4px;'>"
      << "Distance</h3>";
    s << "<table cellpadding='2'>";
    if (m->parallax > 0)
        s << formatParamRow("π",
               formatAsymVal(m->parallax, m->parallaxError, m->parallaxError, 3, "mas"));
    if (m->parallaxRuwe > 0)
        s << formatParamRow("RUWE",
               QString("<b>%1</b>").arg(m->parallaxRuwe, 0, 'f', 1));
    if (m->distanceMode > 0)
        s << formatParamRow("d (mode)",
               formatAsymVal(m->distanceMode, m->distanceModeError,
                             m->distanceModeError, 3, "kpc"));
    if (m->distanceMedian > 0)
        s << formatParamRow("d (median)",
               formatAsymVal(m->distanceMedian, m->distanceMedianError,
                             m->distanceMedianError, 3, "kpc"));
    s << "</table>";

    // ── Metadata ─────────────────────────────────────────────
    s << "<hr><p style='color:gray; font-size:9pt;'>"
      << "Created: " << m->creationDate.toString("yyyy-MM-dd hh:mm")
      << "<br>Components: " << m->numComponents
      << "<br>ID: " << m->getId().left(8) << "…</p>";

    _paramLabel->setText(html);
}

// ═══════════════════════════════════════════════════════════════════
// Photometry table
// ═══════════════════════════════════════════════════════════════════

void SEDFitDialog::updatePhotometryTable()
{
    _updatingPhotTable = true;
    _photTable->setRowCount(0);

    if (_currentFitIndex < 0 || _currentFitIndex >= static_cast<int>(_fits.size())) {
        _updatingPhotTable = false;
        return;
    }

    auto& model = _fits[_currentFitIndex];
    const auto& pts = model->observedPoints;
    _photTable->setRowCount(static_cast<int>(pts.size()));

    QColor incColor = includedPointColor();
    QColor excColor = excludedPointColor();

    for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
        const auto& p = pts[i];
        bool included = (p.flag >= 0);
        QColor rowColor = included ? QColor() : excColor;

        // Include checkbox
        auto* cbItem = new QTableWidgetItem;
        cbItem->setFlags(cbItem->flags() | Qt::ItemIsUserCheckable);
        cbItem->setCheckState(included ? Qt::Checked : Qt::Unchecked);
        cbItem->setText("");
        _photTable->setItem(i, PC_Include, cbItem);

        auto setCell = [&](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            if (!included)
                item->setForeground(excColor);
            _photTable->setItem(i, col, item);
        };

        setCell(PC_System,   p.system);
        setCell(PC_Band,     p.passband);
        setCell(PC_Lambda,   QString::number(p.lambda, 'f', 1));
        setCell(PC_Flux,     QString::number(p.flux, 'e', 4));
        setCell(PC_FluxErr,  QString::number((p.fluxMax - p.fluxMin) * 0.5, 'e', 3));
        setCell(PC_Residual, (p.diffErr > 0)
                                 ? QString::number(p.diff / p.diffErr, 'f', 2)
                                 : "—");
        setCell(PC_Catalog,  p.vizierCatalog);
    }

    _updatingPhotTable = false;
}

void SEDFitDialog::onPhotometryFlagToggled(int row, int column)
{
    if (_updatingPhotTable || column != PC_Include) return;
    if (_currentFitIndex < 0 || _currentFitIndex >= static_cast<int>(_fits.size()))
        return;

    auto& pts = _fits[_currentFitIndex]->observedPoints;
    if (row < 0 || row >= static_cast<int>(pts.size())) return;

    auto* item = _photTable->item(row, PC_Include);
    bool included = (item->checkState() == Qt::Checked);
    pts[row].flag = included ? 0 : -1;

    // Update row styling
    QColor col = included ? QColor() : excludedPointColor();
    for (int c = 1; c < PC_COUNT; ++c) {
        auto* cell = _photTable->item(row, c);
        if (cell) cell->setForeground(included ? QColor() : col);
    }

    // Refresh plots
    updatePlot();
}

// ═══════════════════════════════════════════════════════════════════
// ISIS integration
// ═══════════════════════════════════════════════════════════════════

bool SEDFitDialog::isIsisAvailable() const
{
    return !findIsisBinary().isEmpty();
}

QString SEDFitDialog::findIsisBinary() const
{
    // Check custom path first
    if (_isisPathEdit) {
        QString custom = _isisPathEdit->text().trimmed();
        if (!custom.isEmpty() && QFileInfo(custom).isExecutable())
            return custom;
    }
    // Fall back to system PATH
    return QStandardPaths::findExecutable("isis");
}

QString SEDFitDialog::starIdentifierForScript() const
{
    // Prefer Gaia DR3 source_id
    QString sid = _star->getSourceId();
    if (!sid.isEmpty() && sid.contains("Gaia", Qt::CaseInsensitive))
        return sid;
    if (!sid.isEmpty()) {
        bool ok;
        sid.toLongLong(&ok);
        if (ok) return "Gaia DR3 " + sid;
    }
    // Fall back to alias
    if (!_star->getAlias().isEmpty())
        return _star->getAlias();
    if (!_star->getJName().isEmpty())
        return _star->getJName();
    return sid;
}

QString SEDFitDialog::generateScript() const
{
    QString script;
    QTextStream s(&script);

    s << "require(\"stellar_isisscripts.sl\");\n";
    s << "variable tscript_start = _ftime;\n\n";

    QString starId = starIdentifierForScript();
    s << "variable basename = \"\";\n";
    s << "variable star = \"" << starId << "\";\n";
    s << "variable nargs = length(__argv);\n";
    s << "if(nargs==2){\n";
    s << "  star = __argv[1];\n";
    s << "  basename = strreplace(star, \" \", \"_\") + \"_\";\n";
    s << "  if(_slang_guess_type(star)==Integer_Type)"
         " star = \"Gaia DR3 \" + star;\n";
    s << "}\n";
    s << "star = strreplace(strtrim(star), \"_\", \" \");\n\n";

    if (Star::isSet(_star->getRa()) && Star::isSet(_star->getDec())) {
        s << "variable coordinates = struct{ra=" << _star->getRa()
          << ", dec=" << _star->getDec() << "};\n";
    } else {
        s << "variable coordinates = struct{ra=NULL, dec=NULL};\n";
    }

    if (_fixDistCb->isChecked()) {
        s << "variable fix_distance = " << _distSpin->value() << ";\n";
        s << "variable fix_distance_err = " << _distErrSpin->value() << ";\n";
    } else {
        s << "variable fix_distance = NULL;\n";
        s << "variable fix_distance_err = NULL;\n";
    }

    // ── Read parameter table into par / par_full ─────────────
    QStringList parN, parV, parF;
    QStringList pfN, pfV, pfF, pfMin, pfMax;

    for (int i = 0; i < _paramTableWidget->rowCount(); ++i) {
        QString name = _paramTableWidget->item(i, PP_Name)->text().trimmed();
        QString val  = _paramTableWidget->item(i, PP_Value)->text().trimmed();
        bool frozen  = _paramTableWidget->item(i, PP_Freeze)->checkState()
                       == Qt::Checked;
        QString mn   = _paramTableWidget->item(i, PP_Min)->text().trimmed();
        QString mx   = _paramTableWidget->item(i, PP_Max)->text().trimmed();
        if (name.isEmpty()) continue;

        if (!mn.isEmpty() && !mx.isEmpty()) {
            pfN   << "\"" + name + "\"";
            pfV   << val;
            pfF   << (frozen ? "1" : "0");
            pfMin << mn;
            pfMax << mx;
        } else {
            parN << "\"" + name + "\"";
            parV << val;
            parF << (frozen ? "1" : "0");
        }
    }

    if (!parN.isEmpty()) {
        s << "variable par = struct{name = [" << parN.join(", ") << "],\n"
          << "                      value = [" << parV.join(", ") << "],\n"
          << "                      freeze = [" << parF.join(", ") << "]};\n";
    } else {
        s << "variable par = NULL;\n";
    }
    if (!pfN.isEmpty()) {
        s << "variable par_full = struct{"
             "name = [" << pfN.join(", ") << "],\n"
          << "                           "
             "value = [" << pfV.join(", ") << "],\n"
          << "                           "
             "freeze = [" << pfF.join(", ") << "],\n"
          << "                           "
             "min = [" << pfMin.join(", ") << "],\n"
          << "                           "
             "max = [" << pfMax.join(", ") << "]};\n";
    } else {
        s << "variable par_full = NULL;\n";
    }

    // ── Grid directories ─────────────────────────────────────
    // Resolve selected grids to relative paths
    auto resolveGrid = [&](QComboBox* combo, QLineEdit* override)
        -> QPair<QString, QString>   // (relativePath, basePath)
    {
        QString ov = override->text().trimmed();
        if (!ov.isEmpty())
            return {ov, {}};

        int idx = combo->currentData().toInt();
        if (idx >= 0 && idx < static_cast<int>(_discoveredGrids.size())) {
            return {_discoveredGrids[idx].relativePath,
                    _discoveredGrids[idx].basePath};
        }
        return {};
    };

    QStringList gridDirs;
    QSet<QString> extraBases;

    auto g1 = resolveGrid(_gridCombo, _gridOverrideEdit);
    if (!g1.first.isEmpty()) {
        gridDirs << "\"" + g1.first + "\"";
        if (!g1.second.isEmpty()) extraBases.insert(g1.second);
    }

    if (_enableComp2Cb->isChecked()) {
        auto g2 = resolveGrid(_grid2Combo, _grid2OverrideEdit);
        if (!g2.first.isEmpty()) {
            gridDirs << "\"" + g2.first + "\"";
            if (!g2.second.isEmpty()) extraBases.insert(g2.second);
        }
    }

    s << "variable griddirectories, bpaths;\n";
    if (gridDirs.isEmpty()) {
        s << "griddirectories = [\"sdB/processed/\"];\n";
    } else {
        s << "griddirectories = [" << gridDirs.join(", ") << "];\n";
    }

    // Collect all search paths
    QStringList quotedPaths;
    quotedPaths << "\"./\"";
    for (const auto& bp : extraBases)
        quotedPaths << "\"" + bp + "/\"";
    for (const auto& p :
         _gridPathsEdit->text().split(';', Qt::SkipEmptyParts))
    {
        QString t = p.trimmed();
        if (!t.isEmpty()) {
            if (!t.endsWith('/')) t += '/';
            QString q = "\"" + t + "\"";
            if (!quotedPaths.contains(q))
                quotedPaths << q;
        }
    }
    s << "bpaths = [" << quotedPaths.join(",\n          ") << "];\n";
    s << "griddirectories = search_grid_fit_photometry("
         "bpaths, griddirectories, \"grid.fits\");\n\n";

    // ── Options ──────────────────────────────────────────────
    s << "variable conf_level = "
      << _confLevelCombo->currentData().toInt() << ";\n";
    s << "variable write_model = "
      << (_writeModelCb->isChecked() ? 1 : 0) << ";\n";
    s << "variable save_MC = "
      << (_saveMCCb->isChecked() ? 1 : 0) << ";\n";
    s << "variable apply_ZPO_corr = "
      << (_applyZPOCb->isChecked() ? 1 : 0) << ";\n";
    s << "variable nMC = nint(" << _nmcSpin->value() << ");\n";

    s << "variable stilism_distance_simple = "
      << (_stilDistSimpleCb->isChecked() ? 1 : 0) << ";\n";
    s << "variable stilism_ebmv_simple = "
      << (_stilEbmvSimpleCb->isChecked() ? 1 : 0) << ";\n";
    s << "variable stilism_ebmv_rerun = "
      << (_stilEbmvRerunCb->isChecked() ? 1 : 0) << ";\n";

    s << "variable mass_can = " << _massCanSpin->value() << ";\n";
    s << "variable delta_mass_can = "
      << _deltaMassCanSpin->value() << ";\n";

    s << "variable derive_logg = "
      << (_deriveLoggCb->isChecked() ? 1 : 0) << ";\n";
    s << "variable hb_distance = "
      << (_hbDistanceCb->isChecked() ? 1 : 0) << ";\n";
    s << "if(hb_distance) derive_logg = 1;\n";

    s << "variable derive_logg_c2 = "
      << (_deriveLoggC2Cb->isChecked() ? 1 : 0) << ";\n";
    s << "variable z_c2 = " << _zC2Spin->value() << ";\n";
    s << "variable derive_sr = "
      << (_deriveSRCb->isChecked() ? 1 : 0) << ";\n";
    s << "variable sdOB_radius = " << _sdOBRadSpin->value() << ";\n";
    s << "variable R1 = " << _r1Spin->value() << ";\n";
    s << "variable R1_err = " << _r1ErrSpin->value() << ";\n\n";

    // ── Script body ──────────────────────────────────────────
    QFile bodyFile(":/scripts/photometry_body.sl");
    if (bodyFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        s << bodyFile.readAll();
    } else {
        s << "% ERROR: Script body template not found.\n";
        s << "% Place the photometry.sl body at"
             " resources/scripts/photometry_body.sl\n";
        LOG_WARNING("SED", "photometry_body.sl resource not found");
    }

    return script;
}

// ── Run fit ──────────────────────────────────────────────────────

void SEDFitDialog::onRunFit()
{
    if (!isIsisAvailable()) {
        QMessageBox::warning(this, "ISIS Not Found",
                             "Cannot run fit: ISIS is not installed or not in PATH.");
        return;
    }

    // Create temp working directory
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        QMessageBox::warning(this, "Error",
                             "Failed to create temporary working directory.");
        return;
    }
    tmpDir.setAutoRemove(false);
    _workDir = tmpDir.path();

    // Write script
    QString scriptPath = _workDir + "/photometry.sl";
    {
        QFile f(scriptPath);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(this, "Error",
                                 "Failed to write script file.");
            return;
        }
        QTextStream out(&f);
        out << generateScript();
    }

    // UI state
    _runFitBtn->setEnabled(false);
    _isisProgress->setVisible(true);
    _isisOutput->setVisible(true);
    _isisOutput->clear();
    _isisOutput->append("Working directory: " + _workDir);
    _isisOutput->append("Starting ISIS...\n");

    // Launch isis
    _isisProcess = new QProcess(this);
    _isisProcess->setWorkingDirectory(_workDir);

    connect(_isisProcess, &QProcess::readyReadStandardOutput, this, [this] {
        _isisOutput->append(
            QString::fromLocal8Bit(_isisProcess->readAllStandardOutput()));
    });
    connect(_isisProcess, &QProcess::readyReadStandardError, this, [this] {
        _isisOutput->append(
            QString::fromLocal8Bit(_isisProcess->readAllStandardError()));
    });
    connect(_isisProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &SEDFitDialog::onIsisFinished);

    _isisProcess->start(findIsisBinary(), {"photometry.sl"});
}

void SEDFitDialog::onIsisFinished(int exitCode, QProcess::ExitStatus status)
{
    _isisProgress->setVisible(false);
    _runFitBtn->setEnabled(true);

    if (status != QProcess::NormalExit || exitCode != 0) {
        _isisOutput->append(QString("\n⚠ ISIS exited with code %1").arg(exitCode));
        return;
    }

    _isisOutput->append("\n✓ ISIS finished successfully. Importing results...");
    importFitResults(_workDir);

    _isisProcess->deleteLater();
    _isisProcess = nullptr;
}

// ── Import fit results from working directory ────────────────────

void SEDFitDialog::importFitResults(const QString& workDir)
{
    auto result = ExtractSED::extractFromDirectory(workDir);
    if (!result.success) {
        QMessageBox::warning(this, "Import Failed",
                             "Could not parse ISIS results:\n" + result.errorMessage);
        return;
    }

    auto newModel = result.model;
    newModel->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

    // Ensure photometry container exists
    auto phot = _star->getPhotometry();
    if (!phot) {
        phot = std::make_shared<Photometry>();
        _star->setPhotometry(phot);
    }

    // If no existing best fit, mark new one as best
    if (!phot->getBestSEDModel())
        newModel->isBestFit = true;

    phot->addSEDModel(newModel);

    // Save to database
    if (_dbm) {
        _dbm->saveSEDModelForStar(_star->getId(), newModel);
        if (newModel->isBestFit) {
            applyBestFitToStar(newModel);
            _dbm->saveStar(_projectId, _star);
        }
    }

    // Reload fits into dialog
    _fits.push_back(newModel);
    updateFitSelector();

    int newIdx = static_cast<int>(_fits.size()) - 1;
    _fitCombo->setCurrentIndex(newIdx);

    emit fitDataChanged();

    _isisOutput->append("✓ New SED model imported successfully.");
    LOG_INFO("SED", QString("Imported new SED fit for %1 from %2")
                        .arg(_star->getSourceId(), workDir));
}

// ═══════════════════════════════════════════════════════════════════
// Star summary update from best fit
// ═══════════════════════════════════════════════════════════════════

void SEDFitDialog::applyBestFitToStar(std::shared_ptr<SEDModel> model)
{
    if (!model) return;

    if (!model->components.empty()) {
        auto& c1 = model->components[0];
        if (c1.radius.isValid()) {
            _star->setSedRadius1(c1.radius.value);
            _star->setSedERadius1(c1.radius.symmetricError());
        }
        if (c1.mass.isValid()) {
            _star->setSedMass1(c1.mass.value);
            _star->setSedEMass1(c1.mass.symmetricError());
        }
        if (c1.luminosity.isValid()) {
            _star->setSedLum1(c1.luminosity.value);
            _star->setSedELum1(c1.luminosity.symmetricError());
        }
    }

    if (model->components.size() > 1) {
        auto& c2 = model->components[1];
        if (c2.radius.isValid()) {
            _star->setSedRadius2(c2.radius.value);
            _star->setSedERadius2(c2.radius.symmetricError());
        }
        if (c2.mass.isValid()) {
            _star->setSedMass2(c2.mass.value);
            _star->setSedEMass2(c2.mass.symmetricError());
        }
        if (c2.luminosity.isValid()) {
            _star->setSedLum2(c2.luminosity.value);
            _star->setSedELum2(c2.luminosity.symmetricError());
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Theme helpers
// ═══════════════════════════════════════════════════════════════════

bool SEDFitDialog::isDarkTheme() const
{
    return qApp->property("isDarkTheme").toBool();
}

QColor SEDFitDialog::modelCurveColor() const
{
    return isDarkTheme() ? QColor("#5DADE2") : QColor("#2C3E50");
}

QColor SEDFitDialog::comp1Color() const
{
    return isDarkTheme() ? QColor("#F0A030") : QColor("#E67E22");
}

QColor SEDFitDialog::comp2Color() const
{
    return isDarkTheme() ? QColor("#58D68D") : QColor("#27AE60");
}

QColor SEDFitDialog::includedPointColor() const
{
    return isDarkTheme() ? QColor("#3498DB") : QColor("#2980B9");
}

QColor SEDFitDialog::excludedPointColor() const
{
    return isDarkTheme() ? QColor("#7F8C8D") : QColor("#95A5A6");
}

void SEDFitDialog::applyPlotTheme(QCustomPlot* plot)
{
    bool dark = isDarkTheme();
    QColor bg    = dark ? QColor("#1E1E2E") : QColor("#FFFFFF");
    QColor fg    = dark ? QColor("#CDD6F4") : QColor("#2C3E50");
    QColor grid  = dark ? QColor("#45475A") : QColor("#ECF0F1");
    QColor sub   = dark ? QColor("#313244") : QColor("#F8F9FA");

    plot->setBackground(bg);
    plot->axisRect()->setBackground(sub);

    for (auto* axis : {plot->xAxis, plot->xAxis2, plot->yAxis, plot->yAxis2}) {
        axis->setBasePen(QPen(fg));
        axis->setTickPen(QPen(fg));
        axis->setSubTickPen(QPen(fg));
        axis->setTickLabelColor(fg);
        axis->setLabelColor(fg);
        axis->grid()->setPen(QPen(grid, 0, Qt::DotLine));
    }

    if (plot->legend) {
        plot->legend->setBrush(QBrush(bg));
        plot->legend->setTextColor(fg);
        plot->legend->setBorderPen(QPen(grid));
    }
}