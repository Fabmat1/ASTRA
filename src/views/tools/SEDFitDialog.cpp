#include "SEDFitDialog.h"
#include "db/DatabaseManager.h"
#include "db/PhotometryRepository.h"
#include "dialogs/SettingsDialog.h"
#include "models/Photometry.h"
#include "models/Star.h"
#include "plotting/qcustomplot.h"
#include "utils/AppSettings.h"
#include "utils/ExtractSED.h"
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
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStorageInfo>
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
    setWindowTitle(QString("SED Analysis — %1").arg(title));
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

    // Right side: new fit configuration
    mainSplit->addWidget(createNewFitPanel());
    mainSplit->setStretchFactor(0, 3);
    mainSplit->setStretchFactor(1, 1);

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
    _newFitScroll->setMinimumWidth(340);
    _newFitScroll->setFrameShape(QFrame::NoFrame);

    auto* scrollContent = new QWidget;
    auto* nfLay = new QVBoxLayout(scrollContent);
    nfLay->setContentsMargins(8, 4, 8, 4);

    auto *headerLabel = new QLabel("<b>New Fit Configuration</b>");
    headerLabel->setStyleSheet("font-size: 12pt; padding: 4px;");
    nfLay->addWidget(headerLabel);

    // ── Grid — Component 1 ─────────────────────────────────
    AppSettings       settings;

    _gridSelector1 = new GridSelectorWidget;
    _gridSelector1->setTitle("Model Grid — Component 1");
    _gridSelector1->setBasePaths(settings.gridBasePaths());
    _gridSelector1->setShowConfigureButton(true);
    nfLay->addWidget(_gridSelector1);

    // ── Component 2 toggle + selector ──────────────────────
    _enableComp2Cb = new QCheckBox("Enable second component grid");
    nfLay->addWidget(_enableComp2Cb);

    _gridSelector2 = new GridSelectorWidget;
    _gridSelector2->setTitle("Model Grid — Component 2");
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
    std::vector<SEDPhotometryPoint> points;

    if (_currentFitIndex >= 0 && _currentFitIndex < static_cast<int>(_fits.size()))
        points = _fits[_currentFitIndex]->observedPoints;

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
    double teff = 25000, logg = 5.5, he = -3.0, z = 0.0;
    double eTeff = 0, eLogg = 0, eHe = 0;
    bool   hasSpectralValues = false;

    for (const auto &f : _fits) {
        if (f->isBestFit && !f->components.empty()) {
            auto &c1 = f->components[0];
            if (c1.teff > 0) {
                teff              = c1.teff;
                eTeff             = (c1.teffErrUp + c1.teffErrDown) * 0.5;
                hasSpectralValues = true;
            }
            if (c1.logg > 0) {
                logg              = c1.logg;
                eLogg             = (c1.loggErrUp + c1.loggErrDown) * 0.5;
                hasSpectralValues = true;
            }
            if (c1.heAbundance != 0) {
                he  = c1.heAbundance;
                eHe = (c1.heAbundanceErrUp + c1.heAbundanceErrDown) * 0.5;
                hasSpectralValues = true;
            }
            if (c1.metallicity != 0)
                z = c1.metallicity;
            break;
        }
    }

    if (Star::isSet(_star->getTeff())) {
        teff = _star->getTeff();
        if (Star::isSet(_star->getETeff()))
            eTeff = _star->getETeff();
        hasSpectralValues = true;
    }
    if (Star::isSet(_star->getLogg())) {
        logg = _star->getLogg();
        if (Star::isSet(_star->getELogg()))
            eLogg = _star->getELogg();
        hasSpectralValues = true;
    }
    if (Star::isSet(_star->getHe())) {
        he = _star->getHe();
        if (Star::isSet(_star->getEHe()))
            eHe = _star->getEHe();
        hasSpectralValues = true;
    }

    double teffMin = 0, teffMax = 0;
    double loggMin = 0, loggMax = 0;
    double heMin = 0, heMax = 0;
    bool   hasRanges = false;

    if (hasSpectralValues) {
        bool heRich = (he > -1.0);

        double totTeff = teffError(teff, eTeff, heRich);
        double totLogg = loggError(teff, logg, eLogg, heRich);
        double totHe   = heError(teff, he, eHe, heRich);

        teffMin   = std::max(teff - totTeff, 3000.0);
        teffMax   = teff + totTeff;
        loggMin   = std::max(logg - totLogg, 0.0);
        loggMax   = std::min(logg + totLogg, 9.5);
        heMin     = std::max(he - totHe, -5.0);
        heMax     = std::min(he + totHe, 0.0);
        hasRanges = true;
    }

    _fitParams = {
        {"c*_xi", 0.0, true, 0, 0, false},
        {"c*_z", z, true, 0, 0, false},
        {"c*_HE", he, true, heMin, heMax, hasRanges},
        {"c*_logg", logg, true, loggMin, loggMax, hasRanges},
        {"c*_teff", teff, true, teffMin, teffMax, hasRanges},
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

void SEDFitDialog::onComp2Toggled(bool enabled)
{
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
    populateParamsFromFit();
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
            p.magnitude != 0 ? QString::number(p.magnitude, 'f', 4) : "—");
        setEditable(PC_MagErr,
            p.magnitudeErr != 0 ? QString::number(p.magnitudeErr, 'f', 4) : "—");

        setReadOnly(PC_Residual,
            (p.diffErr > 0) ? QString::number(p.diff / p.diffErr, 'f', 2)
                            : "—");
        setReadOnly(PC_Catalog, p.vizierCatalog);
    }

    _updatingPhotTable = false;
}

void SEDFitDialog::onPhotometryFlagToggled(int row, int column)
{
    if (_updatingPhotTable) return;
    if (_currentFitIndex < 0 || _currentFitIndex >= static_cast<int>(_fits.size()))
        return;

    auto& model = _fits[_currentFitIndex];
    auto& pts = model->observedPoints;
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

    if (_dbm) {
        _dbm->saveSEDModelForStar(_star->getId(), model);
    } else if (!model->getModelDataFile().isEmpty()) {
        model->saveDataToFile(model->getModelDataFile());
    }

    if (column == PC_Include)
        updatePlot(true);
}

// ═══════════════════════════════════════════════════════════════════
// ISIS integration
// ═══════════════════════════════════════════════════════════════════

bool SEDFitDialog::isIsisAvailable() const {
    return !findIsisBinary().isEmpty();
}

QString SEDFitDialog::findIsisBinary() const {
    AppSettings s;
    QString     custom = s.isisBinaryPath().trimmed();
    if (!custom.isEmpty() && QFileInfo(custom).isExecutable())
        return custom;
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
        _isisOutput->append("No photometry.dat written — ISIS will query for data");
    _isisOutput->append("Starting ISIS...\n");

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