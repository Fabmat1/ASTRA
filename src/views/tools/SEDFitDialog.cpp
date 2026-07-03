#include "SEDFitDialog.h"
#include "db/DatabaseManager.h"
#include "db/PhotometryRepository.h"
#include "dialogs/SettingsDialog.h"
#include "models/Photometry.h"
#include "models/Spectrum.h"
#include "models/Star.h"
#include "plotting/qcustomplot.h"
#include "utils/AppSettings.h"
#include "utils/ExtractSED.h"
#include "utils/IsisEnvironment.h"
#include "utils/Logger.h"
#include "utils/SystematicErrors.h"
#include "views/widgets/GridSelectorWidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFuture>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QEventLoop>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QStyle>
#include <QUrlQuery>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>

// ═══════════════════════════════════════════════════════════════════
// Helper: format an asymmetric value as HTML
// ═══════════════════════════════════════════════════════════════════

namespace {

enum PhotomCol {
    PC_Include = 0,
    PC_System,
    PC_Band,
    PC_Lambda,
    PC_Mag,
    PC_MagErr,
    PC_Residual,
    PC_Catalog,
    PC_COUNT
};

enum ParamCol { PP_Name = 0, PP_Value, PP_Freeze, PP_Min, PP_Max, PP_COUNT };

// Returns true if `path` is reachable within `timeoutMs`.
// The (potentially blocking) probe runs in a worker thread so an
// unreachable network path can never freeze the GUI thread for more
// than `timeoutMs`.
bool pathReachable(const QString &path, int timeoutMs = 1500) {
    QFuture<bool> probe = QtConcurrent::run([path]() -> bool {
        QStorageInfo info(path);
        if (info.isValid() && info.isReady())
            return true;
        // Fallback for raw UNC paths that QStorageInfo doesn't resolve.
        return QFileInfo::exists(path);
    });

    QElapsedTimer timer;
    timer.start();
    while (!probe.isFinished() && timer.elapsed() < timeoutMs)
        QThread::msleep(20);

    if (!probe.isFinished())
        return false; // timed out -> treat as unreachable
    return probe.result();
}

// Filters a list of grid base paths down to those that respond quickly.
QStringList reachableGridPaths(const QStringList &paths) {
    QStringList ok;
    for (const QString &p : paths) {
        if (p.trimmed().isEmpty())
            continue;
        if (pathReachable(p)) {
            ok << p;
        } else {
            LOG_WARNING(
                "SED",
                QString("Skipping unreachable grid base path: %1").arg(p));
        }
    }
    return ok;
}

std::shared_ptr<SpectralFit>
findBestSpectralFit(const std::shared_ptr<Star> &star) {
    if (!star)
        return nullptr;
    for (auto &spec : star->getSpectra()) {
        if (!spec)
            continue;
        if (auto bf = spec->getBestFit())
            return bf;
    }
    return nullptr;
}

} // namespace

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
    setWindowFlags(Qt::Window);

    setupUi();
    loadExistingFits();
    initDefaultFitParams();

    LOG_INFO("Tools", QString("SED Fit dialog opened for %1")
                          .arg(_star->getSourceId()));

    QTimer::singleShot(0, this, [this] {
        applyPlotTheme(_sedPlot);
        applyPlotTheme(_residualPlot);
        _sedPlot->replot();
        _residualPlot->replot();
    });
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
    setWindowTitle(QString("SED Analysis - %1").arg(title));
    resize(1500, 850);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(6, 6, 6, 6);
    root->setSpacing(4);

    root->addWidget(createFitSelectorBar());

    auto* mainSplit = new QSplitter(Qt::Horizontal);

    // Left side: plots + params above, photometry below
    auto* leftWidget = new QWidget;
    auto* leftLayout = new QVBoxLayout(leftWidget);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(4);

    auto* plotParamSplit = new QSplitter(Qt::Horizontal);
    plotParamSplit->addWidget(createPlotArea());
    plotParamSplit->addWidget(createParameterPanel());
    plotParamSplit->setStretchFactor(0, 3);
    plotParamSplit->setStretchFactor(1, 1);
    leftLayout->addWidget(plotParamSplit, 1);
    leftLayout->addWidget(createPhotometrySection());

    mainSplit->addWidget(leftWidget);

    mainSplit->addWidget(createNewFitPanel());
    mainSplit->setStretchFactor(0, 5);
    mainSplit->setStretchFactor(1, 2);
    mainSplit->setSizes({940, 560});
    root->addWidget(mainSplit, 1);
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

    _sedPlot->yAxis->setLabel("λ³ Fλ  (10⁻⁴ erg s⁻¹ cm⁻² Å²)");
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
        {"Include", "System", "Band", "λ (Å)", "Mag",
         "±Mag", "Resid. (σ)", "Catalog"});
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
    _newFitScroll = new QScrollArea;
    _newFitScroll->setWidgetResizable(true);
    _newFitScroll->setMinimumWidth(200); 
    _newFitScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _newFitScroll->setFrameShape(QFrame::NoFrame);

    auto* scrollContent = new QWidget;
    auto* nfLay = new QVBoxLayout(scrollContent);
    nfLay->setContentsMargins(8, 4, 8, 4);

    auto *headerLabel = new QLabel("<b>New Fit Configuration</b>");
    headerLabel->setStyleSheet("font-size: 12pt; padding: 4px;");
    nfLay->addWidget(headerLabel);

    // ── Grid - Component 1 ─────────────────────────────────
    AppSettings       settings;

    _gridSelector1 = new GridSelectorWidget;
    _gridSelector1->setTitle("Model Grid - Component 1");
    _gridSelector1->setBasePaths(settings.gridBasePaths());
    _gridSelector1->setShowConfigureButton(true);
    nfLay->addWidget(_gridSelector1);

    // ── Component 2 toggle + selector ──────────────────────
    _enableComp2Cb = new QCheckBox("Enable second component grid");
    nfLay->addWidget(_enableComp2Cb);

    _gridSelector2 = new GridSelectorWidget;
    _gridSelector2->setTitle("Model Grid - Component 2");
    _gridSelector2->setBasePaths(settings.gridBasePaths());
    _gridSelector2->setShowConfigureButton(true);
    _gridSelector2->setVisible(false);
    nfLay->addWidget(_gridSelector2);

    connect(_enableComp2Cb, &QCheckBox::toggled,
            _gridSelector2, &QWidget::setVisible);
    connect(_enableComp2Cb, &QCheckBox::toggled,
            this,           &SEDFitDialog::onComp2Toggled);

    auto reconfigurePaths = [this] {
        AppSettings    s;
        SettingsDialog dlg(&s, this);
        if (dlg.exec() == QDialog::Accepted) {
            AppSettings fresh;
            _gridSelector1->setBasePaths(fresh.gridBasePaths());
            _gridSelector2->setBasePaths(fresh.gridBasePaths());
        }
    };
    connect(_gridSelector1, &GridSelectorWidget::configurePathsRequested,
            this, reconfigurePaths);
    connect(_gridSelector2, &GridSelectorWidget::configurePathsRequested,
            this, reconfigurePaths);

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

    // Small icon button: query Gaia DR3 and apply the Lindegren (2021)
    // parallax zero-point correction + El-Badry (2021) error inflation.
    _distCorrectBtn = new QToolButton;
    _distCorrectBtn->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    _distCorrectBtn->setAutoRaise(true);
    _distCorrectBtn->setEnabled(false);
    _distCorrectBtn->setToolTip(
        tr("Query Gaia DR3 and apply the parallax zero-point correction\n"
           "(Lindegren et al. 2021) and uncertainty inflation\n"
           "(El-Badry et al. 2021) to the fixed distance."));
    dLay->addWidget(_distCorrectBtn);
    dLay->addStretch();

    connect(_fixDistCb, &QCheckBox::toggled, this, [this](bool on) {
        _distSpin->setEnabled(on);
        _distErrSpin->setEnabled(on);
        _distCorrectBtn->setEnabled(on);
    });
    connect(_distCorrectBtn, &QToolButton::clicked,
            this, &SEDFitDialog::applyGaiaDistanceCorrection);

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
    _paramTableWidget->setMinimumHeight(150);
    _paramTableWidget->setMaximumHeight(350);
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

    // ── Options ──────────────────────────────────────────────
    auto *optGroup = new QGroupBox("Options");
    auto *oLay     = new QGridLayout(optGroup);
    oLay->setHorizontalSpacing(8);
    oLay->setVerticalSpacing(4);
    int orow = 0;

    oLay->addWidget(new QLabel("Confidence:"), orow, 0);
    _confLevelCombo = new QComboBox;
    _confLevelCombo->addItem("None", -1);
    _confLevelCombo->addItem("68%", 0);
    _confLevelCombo->addItem("90%", 1);
    _confLevelCombo->addItem("99%", 2);
    _confLevelCombo->setCurrentIndex(1);
    oLay->addWidget(_confLevelCombo, orow, 1);

    oLay->addWidget(new QLabel("MC trials:"), orow, 2);
    _nmcSpin = new QSpinBox;
    _nmcSpin->setRange(1000, 50000000);
    _nmcSpin->setValue(2000000);
    _nmcSpin->setSingleStep(100000);
    _nmcSpin->setGroupSeparatorShown(true);
    oLay->addWidget(_nmcSpin, orow++, 3);

    oLay->addWidget(new QLabel("Outlier reject (σ):"), orow, 0);
    _rejectionSpin = new QDoubleSpinBox;
    _rejectionSpin->setRange(0.0, 1000.0);
    _rejectionSpin->setDecimals(1);
    _rejectionSpin->setSingleStep(1.0);
    _rejectionSpin->setValue(5.0);
    _rejectionSpin->setToolTip(
        "σ threshold passed to photometric_fitting(remove_outliers=...).\n"
        "Lower = rejects discrepant points more aggressively; "
        "higher = more lenient. 5 is the upstream default.");
    oLay->addWidget(_rejectionSpin, orow++, 1);

    _writeModelCb = new QCheckBox("Write model");
    _writeModelCb->setChecked(true);
    oLay->addWidget(_writeModelCb, orow, 0, 1, 2);

    _saveMCCb = new QCheckBox("Save MC");
    _saveMCCb->setChecked(false);
    oLay->addWidget(_saveMCCb, orow, 2);

    _applyZPOCb = new QCheckBox("Apply ZPO");
    _applyZPOCb->setChecked(true);
    _applyZPOCb->setToolTip(
        "Apply empirical corrections to photometric zero-point offsets");
    oLay->addWidget(_applyZPOCb, orow++, 3);

    _useSavedPhotCb = new QCheckBox("Use saved photometry");
    _useSavedPhotCb->setChecked(true);
    _useSavedPhotCb->setToolTip(
        "When enabled, the star's saved photometry points are written to\n"
        "photometry.dat and used for the fit. When disabled, no photometry.dat\n"
        "is written and ISIS re-queries the photometry from the archives.");
    oLay->addWidget(_useSavedPhotCb, orow++, 0, 1, 2);

    oLay->setColumnStretch(1, 1);
    oLay->setColumnStretch(3, 1);
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

    nfLay->addStretch();
    _newFitScroll->setWidget(scrollContent);

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

    return _newFitScroll;
}

void SEDFitDialog::writePhotometryDat(const QString& filepath)
{
    // Single source of truth: the star's canonical SED photometry points.
    ensureCanonicalPhotometryPoints();
    std::vector<SEDPhotometryPoint> points = canonicalPhotometryPoints();

    if (points.empty()) {
        auto phot = _star->getPhotometry();
        if (phot) {
            for (const auto& pp : phot->getPhotometricPoints()) {
                SEDPhotometryPoint sp;
                sp.system       = pp.instrument;
                sp.passband     = pp.filter;
                sp.magnitude    = pp.magnitude;
                sp.magnitudeErr = pp.magnitudeError;
                sp.type         = "magnitude";
                sp.flag         = 0;
                points.push_back(sp);
            }
        }
    }

    bool hasMags = false;
    for (const auto& p : points)
        if (p.magnitude != 0.0) { hasMags = true; break; }
    if (!hasMags) return;

    QFile f(filepath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream out(&f);

    // ── Header: coordinates ──────────────────────────────────
    if (Star::isSet(_star->getRa()) && Star::isSet(_star->getDec())) {
        out << QString("# RA = %1 DEC = %2\n")
               .arg(_star->getRa(), 0, 'f', 10)
               .arg(_star->getDec(), 0, 'f', 10);
    }

    // ── Header: reddening from any available fit ─────────────
    double sfd = 0, sfdErr = 0, sf = 0, sfErr = 0;

    // Prefer best fit, fall back to current, then any fit
    for (const auto& fit : _fits) {
        if (!fit->isBestFit) continue;
        sfd = fit->ebvSFD;  sfdErr = fit->ebvSFDError;
        sf  = fit->ebvSF;   sfErr  = fit->ebvSFError;
        break;
    }
    if (sfd == 0 && sf == 0 &&
        _currentFitIndex >= 0 && _currentFitIndex < static_cast<int>(_fits.size()))
    {
        auto& cur = _fits[_currentFitIndex];
        sfd = cur->ebvSFD;  sfdErr = cur->ebvSFDError;
        sf  = cur->ebvSF;   sfErr  = cur->ebvSFError;
    }
    if (sfd == 0 && sf == 0) {
        for (const auto& fit : _fits) {
            if (fit->ebvSFD > 0 || fit->ebvSF > 0) {
                sfd = fit->ebvSFD;  sfdErr = fit->ebvSFDError;
                sf  = fit->ebvSF;   sfErr  = fit->ebvSFError;
                break;
            }
        }
    }

    if (sfd > 0)
        out << QString("# meanSFD = %1 stdSFD = %2\n")
               .arg(sfd, 0, 'f', 4).arg(sfdErr, 0, 'f', 4);
    if (sf > 0)
        out << QString("# meanSandF = %1 stdSandF = %2\n")
               .arg(sf, 0, 'f', 4).arg(sfErr, 0, 'f', 4);

    // ── Column header ────────────────────────────────────────
    out << "flag    system     passband            magnitude"
           "           uncertainty        type"
           "      angu_dist_arcsec     VizieR_catalog\n";

    // ── Data rows ────────────────────────────────────────────
    for (const auto& p : points) {
        if (p.magnitude == 0.0 && p.magnitudeErr == 0.0) continue;
        QString type = p.type.isEmpty() ? "magnitude" : p.type;

        out << QString::asprintf("%4d     %-10s%16s%20s%20s   %-13s%21s         %s\n",
                p.flag,
                qPrintable(p.system),
                qPrintable(p.passband),
                qPrintable(QString::number(p.magnitude, 'g', 10)),
                qPrintable(QString::number(p.magnitudeErr, 'g', 10)),
                qPrintable(type),
                qPrintable(QString::number(p.angularDist, 'g', 16)),
                qPrintable(p.vizierCatalog));
    }
}

// ═══════════════════════════════════════════════════════════════════
// Gaia parallax corrections for the manually fixed distance
// ═══════════════════════════════════════════════════════════════════
//
// Reproduces the group's `query_astrometry` convention:
//   * parallax zero-point: Lindegren et al. (2021) recipe, ported from the
//     official `gaiadr3_zeropoint` package (coefficient tables z5/z6 200720).
//   * parallax-error inflation: El-Badry, Rix & Heintz (2021), Eq. (16).

namespace {

// Lindegren et al. (2021) parallax zero-point Z [mas]. corrected = plx - Z.
// `colour` is nu_eff_used_in_astrometry (5p) or pseudocolour (6p).
// `solved` is astrometric_params_solved (31 -> 5p, 95 -> 6p).
// Returns NaN if the solution type has no zero-point recipe.
double lindegrenParallaxZpo(double gMag, double colour, double sinBeta, int solved)
{
    // G-magnitude interpolation nodes (shared by z5 and z6).
    static const double gNodes[13] = {
        6.0, 10.8, 11.2, 11.8, 12.2, 12.9, 13.1, 15.9, 16.1, 17.5, 19.0, 20.0, 21.0};

    // z5: 8 basis terms; q[node][term].
    static const int    j5[8] = {0, 0, 0, 1, 1, 2, 3, 4};
    static const int    k5[8] = {0, 1, 2, 0, 1, 0, 0, 0};
    static const double q5[13][8] = {
        {-26.98,  -9.62,  27.40,  -25.1,   -0.0, -1257,    0.0,    0.0},
        {-27.23,  -3.07,  23.04,   35.3,   15.7, -1257,    0.0,    0.0},
        {-30.33,  -9.23,   9.08,  -88.4,  -11.8, -1257,    0.0,    0.0},
        {-33.54, -10.08,  13.28, -126.7,   11.6, -1257,    0.0,    0.0},
        {-13.65,  -0.07,   9.35, -111.4,   40.6, -1257,    0.0,    0.0},
        {-19.53,  -1.64,  15.86,  -66.8,   20.6, -1257,    0.0,    0.0},
        {-37.99,   2.63,  16.14,   -5.7,   14.0, -1257,  107.9,  104.3},
        {-38.33,   5.61,  15.42,    0.0,   18.7, -1189,  243.8,  155.2},
        {-31.05,   2.83,   8.59,    0.0,   15.5, -1404,  105.5,  170.7},
        {-29.18,  -0.09,   2.41,    0.0,   24.5, -1165,  189.7,  325.0},
        {-18.40,   5.98,  -6.46,    0.0,    5.5,     0,    0.0,  276.6},
        {-12.65,  -4.57,  -7.46,    0.0,   97.9,     0,    0.0,    0.0},
        {-18.22, -15.24, -18.54,    0.0,  128.2,     0,    0.0,    0.0},
    };

    // z6: 7 basis terms.
    static const int    j6[7] = {0, 0, 0, 1, 1, 1, 2};
    static const int    k6[7] = {0, 1, 2, 0, 1, 2, 0};
    static const double q6[13][7] = {
        {-27.85,  -7.78,  27.47,  -32.1,   14.4,    9.5,   -67},
        {-28.91,  -3.57,  22.92,    7.7,   12.6,    1.6,  -572},
        {-26.72,  -8.74,   9.36,  -30.3,    5.6,   17.2, -1104},
        {-29.04,  -9.69,  13.63,  -49.4,   36.3,   17.7, -1129},
        {-12.39,  -2.16,  10.23,  -92.6,   19.8,   27.6,  -365},
        {-18.99,  -1.93,  15.90,  -57.2,   -8.0,   19.9,  -554},
        {-38.29,   2.59,  16.20,  -10.5,    1.4,    0.4,  -960},
        {-36.83,   4.20,  15.76,   22.3,   11.1,   10.0, -1367},
        {-28.37,   1.99,   9.28,   50.4,   17.2,   13.7, -1351},
        {-24.68,  -1.37,   3.52,   86.8,   19.8,   21.3, -1380},
        {-15.32,   4.01,  -6.03,   29.2,   14.1,    0.4,  -563},
        {-13.73, -10.92,  -8.30,  -74.4,  196.4,  -42.0,   536},
        {-29.53, -20.34, -18.74,  -39.5,  326.8, -262.3,  1598},
    };

    int m;
    const int*    jj;
    const int*    kk;
    const double* q;     // flat pointer into the chosen table
    int stride;
    if (solved == 31)      { m = 8; jj = j5; kk = k5; q = &q5[0][0]; stride = 8; }
    else if (solved == 95) { m = 7; jj = j6; kk = k6; q = &q6[0][0]; stride = 7; }
    else return std::numeric_limits<double>::quiet_NaN();   // 2p: no recipe

    // Colour basis functions (clamped, mirrors zpt.py).
    const double c[5] = {
        1.0,
        std::max(-0.24, std::min(0.24, colour - 1.48)),
        std::pow(std::min(0.24, std::max(0.0, 1.48 - colour)), 3.0),
        std::min(0.0, colour - 1.24),
        std::max(0.0, colour - 1.72),
    };
    const double b[3] = {1.0, sinBeta, sinBeta * sinBeta - 1.0 / 3.0};

    // Locate the G bin and the linear interpolation weight h.
    constexpr int n = 13;
    int dig = 0;
    while (dig < n && gMag >= gNodes[dig]) ++dig;
    int ig = std::max(0, std::min(n - 2, dig - 1));
    double h = (gNodes[ig + 1] - gNodes[ig]) > 0
                   ? (gMag - gNodes[ig]) / (gNodes[ig + 1] - gNodes[ig])
                   : 0.0;
    h = std::max(0.0, std::min(1.0, h));

    double zpt = 0.0;     // micro-arcseconds
    for (int i = 0; i < m; ++i) {
        double qi = (1.0 - h) * q[ig * stride + i] + h * q[(ig + 1) * stride + i];
        zpt += qi * c[jj[i]] * b[kk[i]];
    }
    return zpt * 0.001;   // -> mas
}

// El-Badry, Rix & Heintz (2021), Eq. (16): parallax-error inflation factor.
double elBadryErrorInflation(double gMag)
{
    return 0.21 * std::exp(-std::pow((gMag - 12.65) / 0.9, 2.0))
           + 1.141 + 0.0040 * gMag - 0.00062 * gMag * gMag;
}

} // namespace

void SEDFitDialog::applyGaiaDistanceCorrection()
{
    // Build the source selector: prefer the Gaia source_id, fall back to a
    // small cone search on the stored coordinates.
    const QString sourceId = _star->getSourceId().trimmed();
    bool sourceIdNumeric = false;
    sourceId.toLongLong(&sourceIdNumeric);

    QString whereClause;
    if (sourceIdNumeric) {
        whereClause = "source_id = " + sourceId;
    } else if (Star::isSet(_star->getRa()) && Star::isSet(_star->getDec())) {
        whereClause = QString("1 = CONTAINS(POINT('ICRS', ra, dec), "
                              "CIRCLE('ICRS', %1, %2, 0.01))")
                          .arg(_star->getRa(), 0, 'f', 10)
                          .arg(_star->getDec(), 0, 'f', 10);
    } else {
        QMessageBox::warning(this, tr("Gaia query"),
            tr("This star has no Gaia source_id or coordinates to query."));
        return;
    }

    const QString adql =
        "SELECT TOP 1 phot_g_mean_mag, parallax, parallax_error, "
        "nu_eff_used_in_astrometry, pseudocolour, astrometric_params_solved, "
        "ra, dec FROM gaiadr3.gaia_source WHERE " + whereClause;

    LOG_DEBUG("SED", QString("Gaia ADQL: %1").arg(adql));

    QNetworkRequest req(QUrl("https://gea.esac.esa.int/tap-server/tap/sync"));
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  "application/x-www-form-urlencoded");
    req.setRawHeader("User-Agent", "ASTRA/1.0");

    QUrlQuery post;
    post.addQueryItem("REQUEST", "doQuery");
    post.addQueryItem("LANG", "ADQL");
    post.addQueryItem("FORMAT", "csv");
    post.addQueryItem("QUERY", adql);

    QNetworkAccessManager nam;
    _distCorrectBtn->setEnabled(false);
    QApplication::setOverrideCursor(Qt::WaitCursor);

    QNetworkReply* reply =
        nam.post(req, post.toString(QUrl::FullyEncoded).toUtf8());
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(30000);
    loop.exec();

    QApplication::restoreOverrideCursor();
    _distCorrectBtn->setEnabled(_fixDistCb->isChecked());

    if (!timeout.isActive()) {
        reply->abort();
        reply->deleteLater();
        QMessageBox::warning(this, tr("Gaia query"),
                             tr("The Gaia archive query timed out."));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        const QString msg = reply->errorString();
        reply->deleteLater();
        QMessageBox::warning(this, tr("Gaia query"),
            tr("Gaia archive query failed:\n%1").arg(msg));
        return;
    }

    const QString body = QString::fromUtf8(reply->readAll());
    reply->deleteLater();

    const QStringList lines = body.split('\n', Qt::SkipEmptyParts);
    if (lines.size() < 2) {
        QMessageBox::warning(this, tr("Gaia query"),
            tr("No matching Gaia DR3 source was found."));
        return;
    }

    const QStringList headers = lines[0].split(',');
    QMap<QString, int> idx;
    for (int i = 0; i < headers.size(); ++i)
        idx[headers[i].trimmed().toLower().remove('"')] = i;
    const QStringList values = lines[1].split(',');

    auto getD = [&](const QString& col) -> double {
        int i = idx.value(col.toLower(), -1);
        if (i < 0 || i >= values.size())
            return std::numeric_limits<double>::quiet_NaN();
        QString s = values[i].trimmed().remove('"');
        if (s.isEmpty()) return std::numeric_limits<double>::quiet_NaN();
        bool ok; double v = s.toDouble(&ok);
        return ok ? v : std::numeric_limits<double>::quiet_NaN();
    };

    const double gMag    = getD("phot_g_mean_mag");
    const double plx     = getD("parallax");            // mas
    const double plxErr  = getD("parallax_error");      // mas
    const double nuEff   = getD("nu_eff_used_in_astrometry");
    const double pscol   = getD("pseudocolour");
    const double ra      = getD("ra");
    const double dec     = getD("dec");
    const int    solved  = static_cast<int>(getD("astrometric_params_solved"));

    if (std::isnan(plx) || plx <= 0.0) {
        QMessageBox::warning(this, tr("Gaia query"),
            tr("Gaia returned no usable (positive) parallax for this source."));
        return;
    }

    // Ecliptic latitude from ICRS coordinates (J2000 mean obliquity).
    constexpr double kDeg2Rad = M_PI / 180.0;
    constexpr double kEps     = 23.439279 * kDeg2Rad;
    double sinBeta = std::sin(dec * kDeg2Rad) * std::cos(kEps)
                   - std::cos(dec * kDeg2Rad) * std::sin(kEps) * std::sin(ra * kDeg2Rad);

    const double colour = (solved == 95) ? pscol : nuEff;
    double zpo = lindegrenParallaxZpo(gMag, colour, sinBeta, solved);

    double plxCorr = plx;
    QString zpoNote;
    if (!std::isnan(zpo)) {
        plxCorr = plx - zpo;   // corrected = catalogue - Z
        zpoNote = tr("Zero-point (Lindegren 2021): %1 mas").arg(zpo, 0, 'f', 4);
    } else {
        zpoNote = tr("Zero-point: not available for this solution type "
                     "(astrometric_params_solved=%1); using raw parallax.")
                      .arg(solved);
    }
    if (plxCorr <= 0.0) {
        QMessageBox::warning(this, tr("Gaia query"),
            tr("The zero-point-corrected parallax is non-positive; "
               "cannot derive a distance."));
        return;
    }

    double plxErrCorr = plxErr;
    if (!std::isnan(plxErr) && !std::isnan(gMag))
        plxErrCorr = plxErr * elBadryErrorInflation(gMag);

    const double dKpc    = 1.0 / plxCorr;                       // mas -> kpc
    const double dErrKpc = std::isnan(plxErrCorr) ? 0.0
                                                  : plxErrCorr / (plxCorr * plxCorr);

    _fixDistCb->setChecked(true);
    _distSpin->setValue(dKpc);
    _distErrSpin->setValue(dErrKpc);

    QMessageBox::information(this, tr("Distance corrected"),
        tr("Applied Gaia DR3 parallax corrections.\n\n"
           "Raw parallax: %1 ± %2 mas\n"
           "%3\n"
           "Error inflation (El-Badry 2021): ×%4\n\n"
           "Distance: %5 ± %6 kpc")
            .arg(plx, 0, 'f', 4)
            .arg(std::isnan(plxErr) ? 0.0 : plxErr, 0, 'f', 4)
            .arg(zpoNote)
            .arg(std::isnan(gMag) ? 1.0 : elBadryErrorInflation(gMag), 0, 'f', 3)
            .arg(dKpc, 0, 'f', 4)
            .arg(dErrKpc, 0, 'f', 4));

    LOG_INFO("SED", QString("Gaia distance correction for %1: plx %2 -> %3 mas, "
                            "d = %4 ± %5 kpc")
                        .arg(_star->getSourceId())
                        .arg(plx).arg(plxCorr).arg(dKpc).arg(dErrKpc));
}

void SEDFitDialog::populateParamsFromFit() {
    if (_currentFitIndex < 0 ||
        _currentFitIndex >= static_cast<int>(_fits.size()))
        return;

    auto &model = _fits[_currentFitIndex];
    bool  multi = model->numComponents > 1;

    _enableComp2Cb->blockSignals(true);
    _enableComp2Cb->setChecked(multi);
    _enableComp2Cb->blockSignals(false);
    if (_gridSelector2)
        _gridSelector2->setVisible(multi);

    _paramTableWidget->setRowCount(0);

    auto addRow = [this](const QString &name, double value, bool frozen,
                         double min, double max, bool hasRange) {
        int row = _paramTableWidget->rowCount();
        _paramTableWidget->insertRow(row);
        _paramTableWidget->setItem(row, PP_Name, new QTableWidgetItem(name));
        _paramTableWidget->setItem(
            row, PP_Value,
            new QTableWidgetItem(QString::number(value, 'g', 8)));

        auto *fz = new QTableWidgetItem;
        fz->setFlags(fz->flags() | Qt::ItemIsUserCheckable);
        fz->setCheckState(frozen ? Qt::Checked : Qt::Unchecked);
        fz->setText("");
        _paramTableWidget->setItem(row, PP_Freeze, fz);

        _paramTableWidget->setItem(
            row, PP_Min,
            new QTableWidgetItem(hasRange ? QString::number(min, 'g', 8)
                                          : QString()));
        _paramTableWidget->setItem(
            row, PP_Max,
            new QTableWidgetItem(hasRange ? QString::number(max, 'g', 8)
                                          : QString()));
    };

    for (int ci = 0; ci < static_cast<int>(model->components.size()); ++ci) {
        const auto &c      = model->components[ci];
        QString     prefix = multi ? QString("c%1_").arg(ci + 1) : "c*_";

        double teff = c.teff > 0 ? c.teff : 25000;
        double logg = c.logg > 0 ? c.logg : 5.5;
        double he   = c.heAbundance;
        double z    = c.metallicity;

        // For the first (or single) component prefer the Star's spectroscopic
        // values/errors so that re-fitting is anchored to the spectroscopic
        // prior rather than the previous fit's posterior.
        double eTeff = (c.teffErrUp + c.teffErrDown) * 0.5;
        double eLogg = (c.loggErrUp + c.loggErrDown) * 0.5;
        double eHe   = (c.heAbundanceErrUp + c.heAbundanceErrDown) * 0.5;

        if (ci == 0) {
            if (Star::isSet(_star->getTeff())) {
                teff = _star->getTeff();
                eTeff =
                    Star::isSet(_star->getETeff()) ? _star->getETeff() : 0.0;
            }
            if (Star::isSet(_star->getLogg())) {
                logg = _star->getLogg();
                eLogg =
                    Star::isSet(_star->getELogg()) ? _star->getELogg() : 0.0;
            }
            if (Star::isSet(_star->getHe())) {
                he  = _star->getHe();
                eHe = Star::isSet(_star->getEHe()) ? _star->getEHe() : 0.0;
            }
        }

        bool heRich = (he > -1.0);

        double totTeff = teffError(teff, eTeff, heRich);
        double totLogg = loggError(teff, logg, eLogg, heRich);
        double totHe   = heError(teff, he, eHe, heRich);

        bool frozenTeff = (c.teffStatus != SEDParamStatus::Fitted);
        bool frozenLogg = (c.loggStatus != SEDParamStatus::Fitted);
        bool frozenHe   = (c.heAbundanceStatus != SEDParamStatus::Fitted);
        bool frozenZ    = (c.metallicityStatus != SEDParamStatus::Fitted);
        bool frozenXi   = (c.microturbulenceStatus != SEDParamStatus::Fitted);

        addRow(prefix + "xi", c.microturbulence, frozenXi, 0, 0, false);
        addRow(prefix + "z", z, frozenZ, 0, 0, false);
        addRow(prefix + "HE", he, frozenHe, std::max(he - totHe, -5.0),
               std::min(he + totHe, 0.0), !frozenHe);
        addRow(prefix + "logg", logg, frozenLogg, std::max(logg - totLogg, 0.0),
               std::min(logg + totLogg, 9.5), !frozenLogg);
        addRow(prefix + "teff", teff, frozenTeff,
               std::max(teff - totTeff, 3000.0), teff + totTeff, !frozenTeff);
    }

    double r55 = model->r55 > 0 ? model->r55 : 3.02;
    addRow("R_55", r55, true, 2.5, 6.0, true);
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
// Default fit parameter table
// ═══════════════════════════════════════════════════════════════════

void SEDFitDialog::initDefaultFitParams() {
    _populatingParams = true;

    // ── Lowest priority: reasonable defaults ───────────────────────────
    double teff = 25000.0, logg = 5.5, he = -2.5, z = 0.0;
    double eTeff = 0.0, eLogg = 0.0, eHe = 0.0;
    bool   teffFromValue = false, loggFromValue = false, heFromValue = false;

    // ── Third priority: midpoint of the selected grid's boundaries ─────
    if (_gridSelector1) {
        if (auto g = _gridSelector1->selectedGrid()) {
            if (g->teffMax > g->teffMin)
                teff = 0.5 * (g->teffMin + g->teffMax);
            if (g->loggMax > g->loggMin)
                logg = 0.5 * (g->loggMin + g->loggMax);
            if (g->heMax > g->heMin)
                he = 0.5 * (g->heMin + g->heMax);
        }
    }

    // ── Second priority: Star spectroscopic fields (manual input) ──────
    // Treat a stored 0 as "not set" (some stars carry 0 instead of NaN);
    // otherwise we'd freeze the fit on a bogus zero value + range.
    if (Star::isSet(_star->getTeff()) && _star->getTeff() > 0.0) {
        teff  = _star->getTeff();
        eTeff = Star::isSet(_star->getETeff()) ? _star->getETeff() : 0.0;
        teffFromValue = true;
    }
    if (Star::isSet(_star->getLogg()) && _star->getLogg() > 0.0) {
        logg  = _star->getLogg();
        eLogg = Star::isSet(_star->getELogg()) ? _star->getELogg() : 0.0;
        loggFromValue = true;
    }
    if (Star::isSet(_star->getHe()) && _star->getHe() != 0.0) {
        he          = _star->getHe();
        eHe         = Star::isSet(_star->getEHe()) ? _star->getEHe() : 0.0;
        heFromValue = true;
    }

    // ── Highest priority: best-fit spectrum ────────────────────────────
    if (auto sf = findBestSpectralFit(_star)) {
        if (sf->teff > 0) {
            teff          = sf->teff;
            eTeff         = sf->teffError;
            teffFromValue = true;
        }
        if (sf->logg > 0) {
            logg          = sf->logg;
            eLogg         = sf->loggError;
            loggFromValue = true;
        }
        if (Star::isSet(sf->he) && sf->he != 0.0) {
            he          = sf->he;
            eHe         = sf->heError;
            heFromValue = true;
        }
        if (sf->metallicity != 0.0)
            z = sf->metallicity;
    }

    bool heRich = (he > -1.0);

    // ── Resolve range + freeze state per parameter ─────────────────────
    double teffMin = 0, teffMax = 0;
    bool   teffRange = false, teffFrozen = false;
    double loggMin = 0, loggMax = 0;
    bool   loggRange = false, loggFrozen = false;
    double heMin = 0, heMax = 0;
    bool   heRange = false, heFrozen = false;

    // When we have a real fitted/spectroscopic value, freeze it within its
    // uncertainty range. Otherwise (only grid midpoints or plain defaults)
    // leave Teff/logg/He free with no min/max so the fit can explore them.
    if (teffFromValue) {
        double tot = teffError(teff, eTeff, heRich);
        teffMin    = std::max(teff - tot, 3000.0);
        teffMax    = teff + tot;
        teffRange  = true;
        teffFrozen = true;
    }

    if (loggFromValue) {
        double tot = loggError(teff, logg, eLogg, heRich);
        loggMin    = std::max(logg - tot, 0.0);
        loggMax    = std::min(logg + tot, 9.5);
        loggRange  = true;
        loggFrozen = true;
    }

    if (heFromValue) {
        double tot = heError(teff, he, eHe, heRich);
        heMin      = std::max(he - tot, -5.0);
        heMax      = std::min(he + tot, 0.0);
        heRange    = true;
        heFrozen   = true;
    }

    _fitParams = {
        {"c*_xi", 0.0, true, 0, 0, false},
        {"c*_z", z, true, 0, 0, false},
        {"c*_HE", he, heFrozen, heMin, heMax, heRange},
        {"c*_logg", logg, loggFrozen, loggMin, loggMax, loggRange},
        {"c*_teff", teff, teffFrozen, teffMin, teffMax, teffRange},
        {"R_55", 3.02, true, 2.5, 6.0, true},
    };

    _paramTableWidget->setRowCount(static_cast<int>(_fitParams.size()));
    for (int i = 0; i < static_cast<int>(_fitParams.size()); ++i) {
        const auto &p = _fitParams[i];

        _paramTableWidget->setItem(i, PP_Name, new QTableWidgetItem(p.name));
        _paramTableWidget->setItem(
            i, PP_Value,
            new QTableWidgetItem(QString::number(p.value, 'g', 8)));

        auto *fzItem = new QTableWidgetItem;
        fzItem->setFlags(fzItem->flags() | Qt::ItemIsUserCheckable);
        fzItem->setCheckState(p.frozen ? Qt::Checked : Qt::Unchecked);
        fzItem->setText("");
        _paramTableWidget->setItem(i, PP_Freeze, fzItem);

        _paramTableWidget->setItem(
            i, PP_Min,
            new QTableWidgetItem(p.hasRange ? QString::number(p.min, 'g', 8)
                                            : QString()));
        _paramTableWidget->setItem(
            i, PP_Max,
            new QTableWidgetItem(p.hasRange ? QString::number(p.max, 'g', 8)
                                            : QString()));
    }

    if (!_paramSignalsConnected) {
        _paramSignalsConnected = true;

        connect(_paramTableWidget, &QTableWidget::cellChanged, this,
                [this](int, int) {
                    if (!_populatingParams)
                        _paramsUserModified = true;
                });

        connect(_gridSelector1, &GridSelectorWidget::selectionChanged, this,
                [this] {
                    if (!_paramsUserModified && !_enableComp2Cb->isChecked())
                        initDefaultFitParams();
                });
    }

    _paramsUserModified = false;
    _populatingParams   = false;
}

void SEDFitDialog::onAddParameter()
{
    _paramsUserModified = true;
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
    _paramsUserModified = true;
    auto sel = _paramTableWidget->selectedItems();
    if (sel.isEmpty()) return;
    QSet<int> rows;
    for (auto* it : sel) rows.insert(it->row());
    auto sorted = rows.values();
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int r : sorted)
        _paramTableWidget->removeRow(r);
}

void SEDFitDialog::onComp2Toggled(bool enabled)
{
    _paramsUserModified = true;
    if (enabled) {
        // Rename c*_ → c1_
        for (int r = 0; r < _paramTableWidget->rowCount(); ++r) {
            auto* item = _paramTableWidget->item(r, PP_Name);
            if (!item) continue;
            QString name = item->text();
            if (name.startsWith("c*_"))
                item->setText("c1_" + name.mid(3));
        }

        // Add generic c2 parameters
        struct C2Param {
            QString name; double value; bool frozen;
            double min; double max; bool hasRange;
        };
        std::vector<C2Param> c2 = {
            {"c2_xi",   0.0,   true,   0,      0,      false},
            {"c2_z",    0.0,   true,   0,      0,      false},
            {"c2_HE",  -1.0,   true,  -5.0,    0.0,    true},
            {"c2_logg", 4.5,   true,   3.0,    5.5,    true},
            {"c2_teff", 6000,  true,   3500,   10000,  true},
        };

        for (const auto& p : c2) {
            int row = _paramTableWidget->rowCount();
            _paramTableWidget->insertRow(row);

            _paramTableWidget->setItem(row, PP_Name,
                new QTableWidgetItem(p.name));
            _paramTableWidget->setItem(row, PP_Value,
                new QTableWidgetItem(QString::number(p.value, 'g', 8)));

            auto* fz = new QTableWidgetItem;
            fz->setFlags(fz->flags() | Qt::ItemIsUserCheckable);
            fz->setCheckState(p.frozen ? Qt::Checked : Qt::Unchecked);
            fz->setText("");
            _paramTableWidget->setItem(row, PP_Freeze, fz);

            _paramTableWidget->setItem(row, PP_Min,
                new QTableWidgetItem(
                    p.hasRange ? QString::number(p.min, 'g', 8) : QString()));
            _paramTableWidget->setItem(row, PP_Max,
                new QTableWidgetItem(
                    p.hasRange ? QString::number(p.max, 'g', 8) : QString()));
        }
    } else {
        // Remove c2_ rows, rename c1_ → c*_
        for (int r = _paramTableWidget->rowCount() - 1; r >= 0; --r) {
            auto* item = _paramTableWidget->item(r, PP_Name);
            if (!item) continue;
            QString name = item->text();
            if (name.startsWith("c2_")) {
                _paramTableWidget->removeRow(r);
            } else if (name.startsWith("c1_")) {
                item->setText("c*_" + name.mid(3));
            }
        }
    }
}

// ═══════════════════════════════════════════════════════════════════
// Data loading
// ═══════════════════════════════════════════════════════════════════

void SEDFitDialog::loadExistingFits()
{
    // Ensure photometry is loaded from the database if not already in memory
    auto phot = _star->getPhotometry();
    if (!phot && _dbm) {
        phot = _dbm->loadPhotometry(_star->getId());
        if (phot)
            _star->setPhotometry(phot);
    }

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

    if (_currentFitIndex >= 0) {
        _fitCombo->setCurrentIndex(_currentFitIndex);
        onFitSelected(_currentFitIndex);
    } else {
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
            label += m->creationDate.toString("yyyy-MM-dd");

            for (int ci = 0; ci < static_cast<int>(m->components.size()); ++ci) {
                auto& c = m->components[ci];
                if (m->components.size() > 1)
                    label += QString("  C%1:").arg(ci + 1);

                if (c.teff > 0)
                    label += QString(" %1K").arg(c.teff, 0, 'f', 0);
                if (c.logg > 0)
                    label += QString(" g=%1").arg(c.logg, 0, 'f', 2);
                if (c.heAbundance != 0)
                    label += QString(" He=%1").arg(c.heAbundance, 0, 'f', 1);

                if (m->components.size() == 1) {
                    if (c.radius.isValid())
                        label += QString(" R=%1R☉").arg(c.radius.value, 0, 'f', 2);
                    if (c.mass.isValid())
                        label += QString(" M=%1M☉").arg(c.mass.value, 0, 'f', 2);
                    if (c.luminosity.isValid())
                        label += QString(" L=%1L☉").arg(c.luminosity.value, 0, 'f', 0);
                }
            }

            if (m->excessNoise > 0)
                label += QString("  δexc=%1").arg(m->excessNoise, 0, 'f', 3);

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

void SEDFitDialog::onFitSelected(int index) {
    if (index < 0 || index >= static_cast<int>(_fits.size())) {
        _currentFitIndex = -1;
    } else {
        _currentFitIndex = index;
    }

    // Overlay the canonical include/exclude flags so the plot and table agree
    // with the single source of truth.
    if (_currentFitIndex >= 0 && _currentFitIndex < static_cast<int>(_fits.size())) {
        ensureCanonicalPhotometryPoints();
        applyCanonicalFlagsToFit(_fits[_currentFitIndex]);
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
    onFitSelected(_currentFitIndex);

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

    auto doomed = _fits[_currentFitIndex];
    bool wasBest = doomed->isBestFit;
    QString doomedId = doomed->getId();

    // Remove from the local working vector
    _fits.erase(_fits.begin() + _currentFitIndex);

    // Remove from the Photometry container on the star
    auto phot = _star->getPhotometry();
    if (phot)
        phot->removeSEDModel(doomedId);

    // Delete from the database and remove the data file
    if (_dbm)
        _dbm->deleteSEDModel(doomedId);

    // If we deleted the best fit, promote the first remaining fit
    if (wasBest && !_fits.empty()) {
        _fits[0]->isBestFit = true;
        applyBestFitToStar(_fits[0]);

        if (_dbm) {
            _dbm->saveSEDModelForStar(_star->getId(), _fits[0]);
            _dbm->saveStar(_projectId, _star);
        }
    }

    _currentFitIndex = _fits.empty() ? -1 : 0;
    updateFitSelector();
    if (_currentFitIndex >= 0) {
        _fitCombo->setCurrentIndex(_currentFitIndex);
        onFitSelected(_currentFitIndex);
    } else {
        onFitSelected(-1);
    }

    emit fitDataChanged();
    LOG_INFO("SED", QString("Deleted SED fit %1 for %2")
                        .arg(doomedId, _star->getSourceId()));
}

// ═══════════════════════════════════════════════════════════════════
// SED Plot
// ═══════════════════════════════════════════════════════════════════

QColor SEDFitDialog::systemColor(int index) const
{
    static const QColor dark[] = {
        {"#3498DB"}, {"#E74C3C"}, {"#2ECC71"}, {"#F39C12"},
        {"#9B59B6"}, {"#1ABC9C"}, {"#E67E22"}, {"#85C1E9"},
        {"#F7DC6F"}, {"#82E0AA"}, {"#F1948A"}, {"#BB8FCE"},
    };
    static const QColor light[] = {
        {"#2980B9"}, {"#C0392B"}, {"#27AE60"}, {"#D4AC0D"},
        {"#8E44AD"}, {"#16A085"}, {"#D35400"}, {"#2C3E50"},
        {"#B7950B"}, {"#1E8449"}, {"#CB4335"}, {"#6C3483"},
    };
    constexpr int n = 12;
    return isDarkTheme() ? dark[index % n] : light[index % n];
}

void SEDFitDialog::updatePlot(bool preserveRange)
{
    QCPRange savedX, savedY;
    if (preserveRange) {
        savedX = _sedPlot->xAxis->range();
        savedY = _sedPlot->yAxis->range();
    }

    _sedPlot->clearPlottables();
    _sedPlot->clearItems();

    if (_currentFitIndex < 0 || _currentFitIndex >= static_cast<int>(_fits.size())) {
        _sedPlot->xAxis->setRange(500, 60000);
        _sedPlot->yAxis->setRange(0, 10);
        _sedPlot->replot();
        return;
    }

    auto& model = _fits[_currentFitIndex];
    bool hasCurve = !model->modelWavelengths.empty();

    const double yScale = 1e4;
    auto fL3 = [yScale](double f, double l) { return f * l * l * l * yScale; };

    double xMin = 1e30, xMax = 0, yMin = 1e30, yMax = 0;

    // ── Model SED curve (total) ──────────────────────────────
    if (hasCurve) {
        const int nw = static_cast<int>(model->modelWavelengths.size());
        QVector<double> wl(nw), fl(nw);
        for (int i = 0; i < nw; ++i) {
            wl[i] = model->modelWavelengths[i];
            fl[i] = fL3(model->modelFluxes[i], wl[i]);
        }

        auto* totalGraph = _sedPlot->addGraph();
        totalGraph->setData(wl, fl);
        totalGraph->setPen(QPen(modelCurveColor(), 2));
        totalGraph->setName("Total model");

        for (auto v : wl) { xMin = qMin(xMin, v); xMax = qMax(xMax, v); }
        for (auto v : fl) { if (v > 0) { yMin = qMin(yMin, v); yMax = qMax(yMax, v); } }

        QList<QColor> compColors = { comp1Color(), comp2Color(),
                                     QColor("#8E44AD"), QColor("#16A085") };
        for (int c = 0; c < static_cast<int>(model->componentFluxes.size()); ++c) {
            if (model->componentFluxes[c].empty()) continue;
            if (model->componentFluxes.size() == 1) break;

            QVector<double> cf(nw);
            for (int i = 0; i < nw; ++i)
                cf[i] = fL3(model->componentFluxes[c][i], wl[i]);

            auto* cGraph = _sedPlot->addGraph();
            cGraph->setData(wl, cf);
            QPen pen(compColors[c % compColors.size()], 1.5, Qt::DashLine);
            cGraph->setPen(pen);
            cGraph->setName(QString("Component %1").arg(c + 1));
        }
    }

    // ── Group observed points by system ──────────────────────
    std::map<QString, std::vector<int>> systemGroups;
    for (int i = 0; i < static_cast<int>(model->observedPoints.size()); ++i) {
        const auto& p = model->observedPoints[i];
        if (p.lambda <= 0 || p.flux <= 0) continue;
        systemGroups[p.system].push_back(i);
    }

    int sysIdx = 0;
    for (auto& [system, indices] : systemGroups) {
        QColor color = systemColor(sysIdx++);

        QVector<double> incL, incF, incFErrLo, incFErrHi, incLErrLo, incLErrHi;
        QVector<double> excL, excF;
        struct LabelInfo { double x; double y; QString band; };
        QVector<LabelInfo> incLabels, excLabels;

        for (int idx : indices) {
            const auto& p = model->observedPoints[idx];
            double l3s = p.lambda * p.lambda * p.lambda * yScale;
            double sf  = p.flux * l3s;

            double lMin = p.lambdaMin > 0 ? p.lambdaMin : p.lambda;
            double lMax = p.lambdaMax > 0 ? p.lambdaMax : p.lambda;
            double fMinS = (p.fluxMin > 0 ? p.fluxMin : p.flux) * l3s;
            double fMaxS = (p.fluxMax > 0 ? p.fluxMax : p.flux) * l3s;
            xMin = qMin(xMin, lMin);  xMax = qMax(xMax, lMax);
            yMin = qMin(yMin, fMinS); yMax = qMax(yMax, fMaxS);

            if (p.flag >= 0) {
                incL.append(p.lambda);
                incF.append(sf);
                incFErrLo.append((p.flux - p.fluxMin) * l3s);
                incFErrHi.append((p.fluxMax - p.flux) * l3s);
                incLErrLo.append(p.lambda - lMin);
                incLErrHi.append(lMax - p.lambda);
                incLabels.append({p.lambda, sf, p.passband});
            } else {
                excL.append(p.lambda);
                excF.append(sf);
                excLabels.append({p.lambda, sf, p.passband});
            }
        }

        if (!incL.isEmpty()) {
            auto* g = _sedPlot->addGraph();
            g->setData(incL, incF);
            g->setLineStyle(QCPGraph::lsNone);
            g->setScatterStyle(QCPScatterStyle(
                QCPScatterStyle::ssDisc, color, 7));
            g->setName(system);

            auto* vErr = new QCPErrorBars(_sedPlot->xAxis, _sedPlot->yAxis);
            vErr->removeFromLegend();
            vErr->setDataPlottable(g);
            vErr->setErrorType(QCPErrorBars::etValueError);
            vErr->setPen(QPen(color, 1));
            vErr->setData(incFErrLo, incFErrHi);

            auto* hErr = new QCPErrorBars(_sedPlot->xAxis, _sedPlot->yAxis);
            hErr->removeFromLegend();
            hErr->setDataPlottable(g);
            hErr->setErrorType(QCPErrorBars::etKeyError);
            hErr->setPen(QPen(color, 1));
            hErr->setData(incLErrLo, incLErrHi);

            for (const auto& lb : incLabels) {
                auto* t = new QCPItemText(_sedPlot);
                t->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
                t->position->setType(QCPItemPosition::ptPlotCoords);
                t->position->setCoords(lb.x, lb.y * 1.08);
                t->setText(lb.band);
                t->setFont(QFont(font().family(), 7));
                t->setColor(color);
                t->setPadding(QMargins(0, 0, 0, 0));
                t->setBrush(Qt::NoBrush);
                t->setPen(Qt::NoPen);
            }
        }

        if (!excL.isEmpty()) {
            auto* g = _sedPlot->addGraph();
            g->setData(excL, excF);
            g->setLineStyle(QCPGraph::lsNone);
            QColor dim = color;
            dim.setAlpha(100);
            g->setScatterStyle(QCPScatterStyle(
                QCPScatterStyle::ssCircle, dim, dim, 6));
            g->removeFromLegend();

            for (const auto& lb : excLabels) {
                auto* t = new QCPItemText(_sedPlot);
                t->setPositionAlignment(Qt::AlignBottom | Qt::AlignHCenter);
                t->position->setType(QCPItemPosition::ptPlotCoords);
                t->position->setCoords(lb.x, lb.y * 1.08);
                t->setText(lb.band);
                t->setFont(QFont(font().family(), 7));
                QColor dimText = color;
                dimText.setAlpha(100);
                t->setColor(dimText);
                t->setPadding(QMargins(0, 0, 0, 0));
                t->setBrush(Qt::NoBrush);
                t->setPen(Qt::NoPen);
            }
        }
    }

    // ── Axis range ───────────────────────────────────────────
    if (preserveRange) {
        _sedPlot->xAxis->setRange(savedX);
        _sedPlot->yAxis->setRange(savedY);
    } else if (xMin < xMax && yMin < yMax) {
        double lxMin = std::log10(xMin), lxMax = std::log10(xMax);
        _sedPlot->xAxis->setRange(std::pow(10, lxMin - 0.15),
                                   std::pow(10, lxMax + 0.15));
        double yPad = (yMax - yMin) * 0.15;
        _sedPlot->yAxis->setRange(std::max(0.0, yMin - yPad), yMax + yPad);
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
    _residualPlot->clearItems();

    if (_currentFitIndex < 0 || _currentFitIndex >= static_cast<int>(_fits.size())) {
        _residualPlot->replot();
        return;
    }

    auto& model = _fits[_currentFitIndex];
    double xLo = _sedPlot->xAxis->range().lower;
    double xHi = _sedPlot->xAxis->range().upper;

    // ── Reference bands: ±1σ and ±3σ ────────────────────────
    bool dark = isDarkTheme();
    QColor band1 = dark ? QColor(255, 255, 255, 15) : QColor(0, 0, 0, 12);
    QColor band3 = dark ? QColor(255, 255, 255, 8)  : QColor(0, 0, 0, 6);

    auto addBand = [&](double lo, double hi, const QColor& col) {
        auto* upper = _residualPlot->addGraph();
        auto* lower = _residualPlot->addGraph();
        upper->setData({xLo, xHi}, {hi, hi});
        lower->setData({xLo, xHi}, {lo, lo});
        upper->setPen(Qt::NoPen);
        lower->setPen(Qt::NoPen);
        upper->setBrush(Qt::NoBrush);
        lower->setBrush(QBrush(col));
        lower->setChannelFillGraph(upper);
        upper->removeFromLegend();
        lower->removeFromLegend();
    };

    addBand(-3.0, 3.0, band3);
    addBand(-1.0, 1.0, band1);

    // ── Zero line ────────────────────────────────────────────
    auto* zeroLine = _residualPlot->addGraph();
    zeroLine->setData({xLo, xHi}, {0.0, 0.0});
    zeroLine->setPen(QPen(Qt::gray, 1, Qt::DashLine));
    zeroLine->removeFromLegend();

    // ── Group by system ──────────────────────────────────────
    std::map<QString, std::vector<int>> systemGroups;
    for (int i = 0; i < static_cast<int>(model->observedPoints.size()); ++i) {
        const auto& p = model->observedPoints[i];
        if (p.lambda <= 0) continue;
        systemGroups[p.system].push_back(i);
    }

    double maxR = 4.0;
    int sysIdx = 0;

    for (auto& [system, indices] : systemGroups) {
        QColor color = systemColor(sysIdx++);

        QVector<double> incL, incR, incErrLo, incErrHi;
        QVector<double> excL, excR;

        for (int idx : indices) {
            const auto& p = model->observedPoints[idx];
            double res = (p.diffErr > 0) ? p.diff / p.diffErr : p.diff;

            if (p.flag >= 0) {
                incL.append(p.lambda);
                incR.append(res);
                incErrLo.append(1.0);
                incErrHi.append(1.0);
                maxR = qMax(maxR, std::abs(res) + 1.5);
            } else {
                excL.append(p.lambda);
                excR.append(res);
                maxR = qMax(maxR, std::abs(res) + 1.0);
            }
        }

        if (!incL.isEmpty()) {
            auto* g = _residualPlot->addGraph();
            g->setData(incL, incR);
            g->setLineStyle(QCPGraph::lsNone);
            g->setScatterStyle(QCPScatterStyle(
                QCPScatterStyle::ssDisc, color, 5));
            g->removeFromLegend();

            auto* vErr = new QCPErrorBars(_residualPlot->xAxis, _residualPlot->yAxis);
            vErr->removeFromLegend();
            vErr->setDataPlottable(g);
            vErr->setErrorType(QCPErrorBars::etValueError);
            vErr->setPen(QPen(color, 1));
            vErr->setData(incErrLo, incErrHi);
        }

        if (!excL.isEmpty()) {
            auto* g = _residualPlot->addGraph();
            g->setData(excL, excR);
            g->setLineStyle(QCPGraph::lsNone);
            QColor dim = color;
            dim.setAlpha(100);
            g->setScatterStyle(QCPScatterStyle(
                QCPScatterStyle::ssCircle, dim, dim, 5));
            g->removeFromLegend();
        }
    }

    _residualPlot->xAxis->setRange(_sedPlot->xAxis->range());
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

// ── Canonical (per-star) SED photometry points ──────────────────────

std::vector<SEDPhotometryPoint>& SEDFitDialog::canonicalPhotometryPoints()
{
    auto phot = _star->getPhotometry();
    if (!phot) {
        phot = std::make_shared<Photometry>();
        _star->setPhotometry(phot);
    }
    return phot->mutableSedPhotometryPoints();
}

void SEDFitDialog::ensureCanonicalPhotometryPoints()
{
    auto& canon = canonicalPhotometryPoints();
    if (!canon.empty()) return;

    // Seed from existing fits (backward compatibility for stars whose points
    // only live on individual SED models). Prefer the best fit, otherwise the
    // fit carrying the most observed points.
    const SEDModel* source = nullptr;
    for (const auto& f : _fits) {
        if (f && f->isBestFit && !f->observedPoints.empty()) { source = f.get(); break; }
    }
    if (!source) {
        for (const auto& f : _fits) {
            if (f && (!source || f->observedPoints.size() > source->observedPoints.size()))
                source = f.get();
        }
    }
    if (!source || source->observedPoints.empty()) return;

    canon = source->observedPoints;
    persistCanonicalPhotometryPoints();
}

void SEDFitDialog::persistCanonicalPhotometryPoints()
{
    if (!_dbm) return;
    auto phot = _star->getPhotometry();
    if (phot)
        _dbm->saveSedPhotometryPointsForStar(_star->getId(), phot);
}

void SEDFitDialog::applyCanonicalFlagsToFit(const std::shared_ptr<SEDModel>& model)
{
    if (!model) return;
    const auto& canon = canonicalPhotometryPoints();
    if (canon.empty()) return;

    auto key = [](const SEDPhotometryPoint& p) {
        return (p.system.trimmed() + '|' + p.passband.trimmed()).toLower();
    };
    auto findCanon = [&](const QString& k) -> const SEDPhotometryPoint* {
        for (const auto& c : canon)
            if (key(c) == k) return &c;
        return nullptr;
    };

    // 1. Sync the user's include/exclude choice onto the points the fit emitted.
    //    Excluded bands are dropped from the fit (or echoed without SED flux),
    //    so fall back to the canonical SED data for those, otherwise the point
    //    can't be plotted at all.
    QSet<QString> present;
    for (auto& p : model->observedPoints) {
        const QString k = key(p);
        present.insert(k);
        const auto* c = findCanon(k);
        if (!c) continue;
        p.flag = c->flag;
        if (!(p.lambda > 0.0 && p.flux > 0.0) &&
            c->lambda > 0.0 && c->flux > 0.0) {
            p.lambda    = c->lambda;    p.lambdaMin = c->lambdaMin;
            p.lambdaMax = c->lambdaMax; p.flux      = c->flux;
            p.fluxMin   = c->fluxMin;   p.fluxMax   = c->fluxMax;
        }
    }

    // 2. Canonical points the fit omitted entirely: add them back (using their
    //    last-known SED data) so excluded points still appear on the plot and
    //    re-checking them in the table brings them back.
    for (const auto& c : canon) {
        if (present.contains(key(c))) continue;
        if (c.lambda > 0.0 && c.flux > 0.0)
            model->observedPoints.push_back(c);
    }
}

void SEDFitDialog::updatePhotometryTable()
{
    _updatingPhotTable = true;
    _photTable->setRowCount(0);

    ensureCanonicalPhotometryPoints();
    const auto& pts = canonicalPhotometryPoints();
    _photTable->setRowCount(static_cast<int>(pts.size()));

    // Residuals are fit-specific: look them up from the currently selected
    // fit's observed points, matched by system+passband.
    QMap<QString, QString> residualByKey;
    if (_currentFitIndex >= 0 && _currentFitIndex < static_cast<int>(_fits.size())) {
        for (const auto& p : _fits[_currentFitIndex]->observedPoints) {
            if (p.diffErr > 0) {
                const QString k = (p.system.trimmed() + '|' + p.passband.trimmed()).toLower();
                residualByKey[k] = QString::number(p.diff / p.diffErr, 'f', 2);
            }
        }
    }

    for (int i = 0; i < static_cast<int>(pts.size()); ++i) {
        const auto& p = pts[i];
        bool included = (p.flag >= 0);
        QColor dimColor = excludedPointColor();

        // Include checkbox
        auto* cbItem = new QTableWidgetItem;
        cbItem->setFlags(cbItem->flags() | Qt::ItemIsUserCheckable);
        cbItem->setCheckState(included ? Qt::Checked : Qt::Unchecked);
        cbItem->setText("");
        _photTable->setItem(i, PC_Include, cbItem);

        auto setReadOnly = [&](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setFlags(item->flags() & ~Qt::ItemIsEditable);
            if (!included) item->setForeground(dimColor);
            _photTable->setItem(i, col, item);
        };

        auto setEditable = [&](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            if (!included) item->setForeground(dimColor);
            _photTable->setItem(i, col, item);
        };

        setReadOnly(PC_System,   p.system);
        setReadOnly(PC_Band,     p.passband);
        setReadOnly(PC_Lambda,   QString::number(p.lambda, 'f', 1));

        setEditable(PC_Mag,
            p.magnitude != 0 ? QString::number(p.magnitude, 'f', 4) : "-");
        setEditable(PC_MagErr,
            p.magnitudeErr != 0 ? QString::number(p.magnitudeErr, 'f', 4) : "-");

        const QString k = (p.system.trimmed() + '|' + p.passband.trimmed()).toLower();
        setReadOnly(PC_Residual, residualByKey.value(k, "-"));
        setReadOnly(PC_Catalog, p.vizierCatalog);
    }

    _updatingPhotTable = false;
}

void SEDFitDialog::onPhotometryFlagToggled(int row, int column)
{
    if (_updatingPhotTable) return;

    auto& pts = canonicalPhotometryPoints();
    if (row < 0 || row >= static_cast<int>(pts.size())) return;

    bool changed = false;

    if (column == PC_Include) {
        auto* item = _photTable->item(row, PC_Include);
        bool included = (item->checkState() == Qt::Checked);
        pts[row].flag = included ? 0 : -1;

        QColor col = included ? QColor() : excludedPointColor();
        for (int c = 1; c < PC_COUNT; ++c) {
            auto* cell = _photTable->item(row, c);
            if (cell) cell->setForeground(included ? QColor() : col);
        }
        changed = true;

    } else if (column == PC_Mag) {
        bool ok;
        double val = _photTable->item(row, PC_Mag)->text().toDouble(&ok);
        if (ok) {
            pts[row].magnitude = val;
            changed = true;
        }

    } else if (column == PC_MagErr) {
        bool ok;
        double val = _photTable->item(row, PC_MagErr)->text().toDouble(&ok);
        if (ok) {
            pts[row].magnitudeErr = val;
            changed = true;
        }
    }

    if (!changed) return;

    persistCanonicalPhotometryPoints();

    if (column == PC_Include) {
        // Reflect the change on the plotted fit and re-render.
        if (_currentFitIndex >= 0 && _currentFitIndex < static_cast<int>(_fits.size()))
            applyCanonicalFlagsToFit(_fits[_currentFitIndex]);
        updatePlot(true);
    }
}

// ═══════════════════════════════════════════════════════════════════
// ISIS integration
// ═══════════════════════════════════════════════════════════════════

bool SEDFitDialog::isIsisAvailable() const {
    return !findIsisBinary().isEmpty();
}

QString SEDFitDialog::findIsisBinary() const {
    return IsisEnvironment::resolveBinary();
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
    QStringList gridDirs;
    QSet<QString> extraBases;
    
    auto collect = [&](GridSelectorWidget* sel) {
        QString rel  = sel->selectedRelativePath();
        QString base = sel->selectedBasePath();
        if (rel.isEmpty()) return;
        gridDirs << "\"" + rel + "\"";
        if (!base.isEmpty()) extraBases.insert(base);
    };
    
    collect(_gridSelector1);
    if (_enableComp2Cb->isChecked()) collect(_gridSelector2);
    
    s << "variable griddirectories, bpaths;\n";
    if (gridDirs.isEmpty()) {
        s << "griddirectories = [\"sdB/processed/\"];\n";
    } else {
        s << "griddirectories = [" << gridDirs.join(", ") << "];\n";
    }
    
    // Merge extra bases from selected grids with configured base paths
    QStringList quotedPaths;
    quotedPaths << "\"./\"";
    for (const auto& bp : extraBases) {
        QString t = bp;
        if (!t.endsWith('/')) t += '/';
        quotedPaths << "\"" + t + "\"";
    }
    AppSettings sGrid;
    for (const auto& p : sGrid.gridBasePaths()) {
        QString t = p.trimmed();
        if (t.isEmpty()) continue;
        if (!t.endsWith('/')) t += '/';
        QString q = "\"" + t + "\"";
        if (!quotedPaths.contains(q)) quotedPaths << q;
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
    s << "variable apply_ZPO_corr = " << (_applyZPOCb->isChecked() ? 1 : 0)
      << ";\n";
    s << "variable remove_outliers = " << _rejectionSpin->value()
      << ";\n";
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
        QMessageBox::warning(
            this, "ISIS Not Found",
            "Cannot run fit: ISIS binary not found. "
            "Configure the path in Settings or install ISIS to your PATH.");
        return;
    }

    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        QMessageBox::warning(this, "Error",
                             "Failed to create temporary working directory.");
        return;
    }
    tmpDir.setAutoRemove(false);
    _workDir = tmpDir.path();

    // Only feed ISIS the saved photometry when the toggle is on; otherwise
    // leave photometry.dat absent so ISIS re-queries the archives.
    const bool useSaved = !_useSavedPhotCb || _useSavedPhotCb->isChecked();
    if (useSaved)
        writePhotometryDat(_workDir + "/photometry.dat");

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

    _runFitBtn->setEnabled(false);
    _isisProgress->setVisible(true);
    _isisOutput->setVisible(true);
    _isisOutput->clear();
    _isisOutput->append("Working directory: " + _workDir);
    if (QFile::exists(_workDir + "/photometry.dat"))
        _isisOutput->append("✓ Wrote photometry.dat with existing photometric data");
    else
        _isisOutput->append("No photometry.dat written - ISIS will query for data");
    _isisOutput->append("Starting ISIS...\n");

    const QString isisBinary = findIsisBinary();

    _isisProcess = new QProcess(this);
    _isisProcess->setWorkingDirectory(_workDir);
    _isisProcess->setProcessEnvironment(IsisEnvironment::environmentFor(isisBinary));

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

    _isisProcess->start(isisBinary, {"photometry.sl"});
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

    // Merge the freshly fitted/queried photometry into the canonical set
    // (single source of truth): add points that were missing, refresh those
    // that already exist (keeping the user's include/exclude choice).
    if (phot->mergeSedPhotometryPoints(newModel->observedPoints))
        persistCanonicalPhotometryPoints();
    // Reflect the canonical include/exclude flags on the new fit for display.
    applyCanonicalFlagsToFit(newModel);

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
    onFitSelected(newIdx);

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

    // Errors follow the storage merge rule: near-symmetric up/down collapse
    // to a single symmetric error, genuinely asymmetric ones keep both sides.
    if (!model->components.empty()) {
        auto& c1 = model->components[0];
        if (c1.radius.isValid()) {
            const auto e = AsymErr::toStorage(c1.radius.errUp, c1.radius.errDown);
            _star->setSedRadius1(c1.radius.value);
            _star->setSedERadius1(e.sym);
            _star->setSedERadius1Up(e.up);
            _star->setSedERadius1Down(e.down);
        }
        if (c1.mass.isValid()) {
            const auto e = AsymErr::toStorage(c1.mass.errUp, c1.mass.errDown);
            _star->setSedMass1(c1.mass.value);
            _star->setSedEMass1(e.sym);
            _star->setSedEMass1Up(e.up);
            _star->setSedEMass1Down(e.down);
        }
        if (c1.luminosity.isValid()) {
            const auto e = AsymErr::toStorage(c1.luminosity.errUp,
                                              c1.luminosity.errDown);
            _star->setSedLum1(c1.luminosity.value);
            _star->setSedELum1(e.sym);
            _star->setSedELum1Up(e.up);
            _star->setSedELum1Down(e.down);
        }
    }

    if (model->components.size() > 1) {
        auto& c2 = model->components[1];
        if (c2.radius.isValid()) {
            const auto e = AsymErr::toStorage(c2.radius.errUp, c2.radius.errDown);
            _star->setSedRadius2(c2.radius.value);
            _star->setSedERadius2(e.sym);
            _star->setSedERadius2Up(e.up);
            _star->setSedERadius2Down(e.down);
        }
        if (c2.mass.isValid()) {
            const auto e = AsymErr::toStorage(c2.mass.errUp, c2.mass.errDown);
            _star->setSedMass2(c2.mass.value);
            _star->setSedEMass2(e.sym);
            _star->setSedEMass2Up(e.up);
            _star->setSedEMass2Down(e.down);
        }
        if (c2.luminosity.isValid()) {
            const auto e = AsymErr::toStorage(c2.luminosity.errUp,
                                              c2.luminosity.errDown);
            _star->setSedLum2(c2.luminosity.value);
            _star->setSedELum2(e.sym);
            _star->setSedELum2Up(e.up);
            _star->setSedELum2Down(e.down);
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

    QColor bgColor      = dark ? QColor(42, 42, 42)   : QColor(255, 255, 255);
    QColor textColor    = dark ? QColor(210, 210, 210) : QColor(30, 30, 30);
    QColor gridColor    = dark ? QColor(80, 80, 80)    : QColor(200, 200, 200);
    QColor subGridColor = dark ? QColor(55, 55, 55)    : QColor(225, 225, 225);

    for (QWidget* w = plot->parentWidget(); w; w = w->parentWidget()) {
        QColor c = w->palette().color(QPalette::Window);
        bool consistent = dark ? (c.lightnessF() < 0.45)
                               : (c.lightnessF() > 0.55);
        if (consistent && c.alpha() == 255) {
            bgColor = c;
            break;
        }
    }

    plot->setStyleSheet("");
    plot->setBackground(QBrush(bgColor));
    plot->axisRect()->setBackground(QBrush(bgColor));

    for (auto* axis : {plot->xAxis, plot->xAxis2, plot->yAxis, plot->yAxis2}) {
        axis->setBasePen(QPen(textColor, 1));
        axis->setTickPen(QPen(textColor, 1));
        axis->setSubTickPen(QPen(gridColor, 1));
        axis->setLabelColor(textColor);
        axis->setTickLabelColor(textColor);
        axis->grid()->setPen(QPen(gridColor, 0.5, Qt::DotLine));
        axis->grid()->setSubGridPen(QPen(subGridColor, 0.3, Qt::DotLine));
        axis->grid()->setZeroLinePen(QPen(gridColor, 0.8));
        axis->grid()->setSubGridVisible(false);
    }

    if (plot->legend) {
        plot->legend->setBorderPen(QPen(gridColor));
        plot->legend->setBrush(QBrush(bgColor));
        plot->legend->setTextColor(textColor);
    }
}