#include "LightcurveFetchDialog.h"
#include "controllers/ApplicationController.h"
#include "db/DatabaseManager.h"
#include "dialogs/ImportLightcurve.h"
#include "dialogs/LCFitDialog.h"
#include "dialogs/LightcurveCredentialPrompts.h"
#include "models/Photometry.h"
#include "models/Star.h"
#include "utils/AppPaths.h"
#include "utils/AppSettings.h"
#include "utils/FilterWavelength.h"
#include "utils/LCFitPhysics.h"
#include "utils/LcqueryEnvironment.h"
#include "utils/Logger.h"
#include "utils/TessSectors.h"
#include "views/tools/LcquerySetupDialog.h"
#include "views/panels/DetailPanel.h"
#include "views/panels/LCPanel.h"
#include "views/panels/PeriodogramPanel.h"
#include "views/widgets/PreciseDoubleSpinBox.h"
#include "utils/UiIcons.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextCursor>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

namespace {

struct PreviewEntry {
    QString filename;     // file written by lightcurvequery
    QString title;        // big title shown above image
    QString description;  // smaller subtitle line
};

static const QList<PreviewEntry>& previewEntries()
{
    static const QList<PreviewEntry> entries = {
        { "tess_preview.png", "TESS FFI cutout",
          "TESScut FFI median image" },
        { "ztf_preview.png",  "ZTF reference",
          "ZTF reference image cutout" },
        { "dss_preview.png",  "DSS2 Red",
          "DSS2 Red BW image" },
        { "ps1_preview.png",  "Pan-STARRS y/i/g",
          "Pan-STARRS composite" },
    };
    return entries;
}

constexpr double kClickSnapRelWindow = 0.01;

// Convert a single line of ANSI-coloured text to a safe HTML span.
QString ansiToHtml(const QString& line)
{
    static const QRegularExpression re(QStringLiteral("\x1b\\[([0-9;]*)m"));

    auto colorFor = [](int code) -> QString {
        switch (code) {
            case 30: case 90: return "#7f7f7f";
            case 31:          return "#cd3131";
            case 91:          return "#f14c4c";
            case 32:          return "#0dbc79";
            case 92:          return "#23d18b";
            case 33:          return "#c19c00";
            case 93:          return "#f5f543";
            case 34:          return "#2472c8";
            case 94:          return "#3b8eea";
            case 35:          return "#bc3fbc";
            case 95:          return "#d670d6";
            case 36:          return "#11a8cd";
            case 96:          return "#29b8db";
            case 37:          return "#e5e5e5";
            case 97:          return "#ffffff";
        }
        return {};
    };

    QString html;
    bool   bold = false;
    QString color;
    bool   spanOpen = false;

    auto closeSpan = [&]() {
        if (spanOpen) { html += "</span>"; spanOpen = false; }
    };
    auto openSpan = [&]() {
        QString style;
        if (bold)             style += "font-weight:bold;";
        if (!color.isEmpty()) style += "color:" + color + ";";
        if (!style.isEmpty()) { html += "<span style=\"" + style + "\">"; spanOpen = true; }
    };
    auto apply = [&](const QStringList& codes) {
        closeSpan();
        for (const QString& c : codes) {
            const int n = c.toInt();
            if (n == 0)        { bold = false; color.clear(); }
            else if (n == 1)   { bold = true; }
            else if (n == 22)  { bold = false; }
            else if (n == 39)  { color.clear(); }
            else { QString cc = colorFor(n); if (!cc.isEmpty()) color = cc; }
        }
        openSpan();
    };

    int pos = 0;
    auto it = re.globalMatch(line);
    while (it.hasNext()) {
        auto m = it.next();
        if (m.capturedStart() > pos)
            html += line.mid(pos, m.capturedStart() - pos).toHtmlEscaped();
        QStringList codes = m.captured(1).split(';', Qt::SkipEmptyParts);
        if (codes.isEmpty()) codes << QStringLiteral("0");
        apply(codes);
        pos = m.capturedEnd();
    }
    if (pos < line.size())
        html += line.mid(pos).toHtmlEscaped();
    closeSpan();
    return html;
}

} // anon

LightcurveFetchDialog::LightcurveFetchDialog(std::shared_ptr<Star>  star,
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

// ── Top-level layout ───────────────────────────────────────────────

void LightcurveFetchDialog::setupUi()
{
    setWindowTitle(QString("Light Curves - %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(1400, 850);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    _tabs = new QTabWidget;
    _tabs->addTab(buildViewerTab(), "Viewer");
    _periodogramTabIdx = _tabs->addTab(buildPeriodogramTab(), "Periodogram");
    _previewsTabIdx    = _tabs->addTab(buildPreviewsTab(), "Previews");
    _tabs->addTab(buildFetchTab(), "Fetch");

    // The Fit tab builds a second full LCPanel over the same (potentially
    // million-point) lightcurves; defer it until the user first opens it so the
    // dialog becomes interactive immediately. A lightweight placeholder holds
    // its slot in the tab bar until then.
    _fitTabPage = new QWidget;
    auto* fitPlaceholderLay = new QVBoxLayout(_fitTabPage);
    fitPlaceholderLay->setContentsMargins(0, 0, 0, 0);
    _fitTabIdx = _tabs->addTab(_fitTabPage, "Fit");

    layout->addWidget(_tabs, 1);

    connect(_tabs, &QTabWidget::currentChanged, this, [this](int idx){
        if (idx == _fitTabIdx)           ensureFitTabBuilt();
        if (idx == _periodogramTabIdx)   onPeriodogramTabActivated();
        else if (idx == _previewsTabIdx) refreshPreviewsTab();
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);

    loadPersistedPeaks();
}

QWidget* LightcurveFetchDialog::buildViewerTab()
{
    auto* page = new QWidget;
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(4);

    auto* split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);

    DetailPanel::Context ctx;
    ctx.star       = _star;
    ctx.dbm        = _dbm;
    ctx.controller = _controller;
    ctx.projectId  = _projectId;
    _lcPanel = new LCPanel(ctx);
    split->addWidget(_lcPanel);

    // ── Right side pane: lightcurve management + meta info ──────────────
    auto* side = new QWidget;
    auto* sideLay = new QVBoxLayout(side);
    sideLay->setContentsMargins(6, 4, 6, 4);
    sideLay->setSpacing(6);

    auto* manageBox = new QGroupBox(tr("Manage"));
    auto* manageLay = new QVBoxLayout(manageBox);

    auto* srcRow = new QHBoxLayout;
    srcRow->addWidget(new QLabel(tr("Source:")));
    _viewerSourceCombo = new QComboBox;
    _viewerSourceCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    _viewerSourceCombo->setMinimumWidth(100);
    srcRow->addWidget(_viewerSourceCombo, 1);
    manageLay->addLayout(srcRow);

    _recomputeBjdBtn = new QPushButton(tr("Recompute BJD…"));
    _recomputeBjdBtn->setToolTip(tr(
        "Re-set the original time scale for this lightcurve and force "
        "BJD values to be recomputed from the native timestamps. "
        "Useful for fixing all-zero BJDs left over from a faulty import."));

    _deleteLcBtn = new QPushButton(tr("Delete…"));
    _deleteLcBtn->setToolTip(tr("Delete this lightcurve from the star."));

    manageLay->addWidget(_recomputeBjdBtn);
    manageLay->addWidget(_deleteLcBtn);
    sideLay->addWidget(manageBox);

    // ── Folding: what is on screen, and one click to adopt it ───────────
    auto* foldBox = new QGroupBox(tr("Folding"));
    auto* foldLay = new QVBoxLayout(foldBox);

    _viewerFoldLabel = new QLabel;
    _viewerFoldLabel->setWordWrap(true);
    _viewerFoldLabel->setTextFormat(Qt::RichText);
    foldLay->addWidget(_viewerFoldLabel);

    _viewerSetBestBtn = new QPushButton(tr("Set as Best Period"));
    _viewerSetBestBtn->setToolTip(tr(
        "Store the period the plot is currently folded to as the star's "
        "best photometric period."));
    _viewerSetBestBtn->setEnabled(false);
    foldLay->addWidget(_viewerSetBestBtn);

    _viewerBestLabel = new QLabel;
    _viewerBestLabel->setStyleSheet("color: gray;");
    _viewerBestLabel->setWordWrap(true);
    foldLay->addWidget(_viewerBestLabel);

    sideLay->addWidget(foldBox);

    // Meta info sections (one per lightcurve), scrollable
    auto* metaHost = new QWidget;
    _viewerMetaLayout = new QVBoxLayout(metaHost);
    _viewerMetaLayout->setContentsMargins(0, 0, 0, 0);
    _viewerMetaLayout->setSpacing(6);
    _viewerMetaLayout->addStretch();

    auto* metaScroll = new QScrollArea;
    metaScroll->setWidgetResizable(true);
    metaScroll->setFrameShape(QFrame::NoFrame);
    metaScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    metaScroll->setWidget(metaHost);
    sideLay->addWidget(metaScroll, 1);

    split->addWidget(side);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 0);
    split->setSizes({1080, 320});
    root->addWidget(split, 1);

    connect(_deleteLcBtn,     &QPushButton::clicked,
            this, &LightcurveFetchDialog::onDeleteLightcurveClicked);
    connect(_recomputeBjdBtn, &QPushButton::clicked,
            this, &LightcurveFetchDialog::onRecomputeBjdClicked);
    connect(_viewerSetBestBtn, &QPushButton::clicked,
            this, &LightcurveFetchDialog::onViewerSetAsBestClicked);
    connect(_lcPanel, &LCPanel::foldStateChanged,
            this, &LightcurveFetchDialog::onViewerFoldStateChanged);

    refreshViewerSourceCombo();
    onViewerFoldStateChanged(_lcPanel->foldPeriod(), _lcPanel->foldT0(),
                             _lcPanel->isFolded());
    return page;
}

void LightcurveFetchDialog::onViewerFoldStateChanged(double period, double t0,
                                                     bool folded)
{
    Q_UNUSED(t0);
    if (!_viewerFoldLabel) return;

    if (period > 0.0) {
        _viewerFoldLabel->setText(
            folded ? tr("Folded to <b>P = %1 d</b>").arg(period, 0, 'g', 8)
                   : tr("Not folded - fold period would be "
                        "<b>P = %1 d</b>").arg(period, 0, 'g', 8));
    } else {
        _viewerFoldLabel->setText(tr("No fold period available."));
    }
    if (_viewerSetBestBtn) _viewerSetBestBtn->setEnabled(period > 0.0);

    if (!_viewerBestLabel) return;
    if (_star && Star::isSet(_star->getPhotPeriod())) {
        _viewerBestLabel->setText(
            tr("Current best-fit P = %1 ± %2 d")
                .arg(_star->getPhotPeriod(), 0, 'g', 6)
                .arg(Star::isSet(_star->getPhotEPeriod())
                         ? _star->getPhotEPeriod() : 0.0, 0, 'g', 2));
    } else {
        _viewerBestLabel->setText(tr("No best-fit period stored yet."));
    }
}

void LightcurveFetchDialog::onViewerSetAsBestClicked()
{
    if (!_lcPanel) return;
    const double p = _lcPanel->foldPeriod();
    if (p <= 0.0) return;
    // The fold period usually comes from a marked peak; reuse that peak's
    // uncertainty when it does, rather than dropping it.
    applyBestPeriod(p, peakErrorFor(p));
}

QWidget* LightcurveFetchDialog::buildFetchTab()
{
    auto* page = new QWidget;
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(8);

    auto* hdr = new QLabel;
    hdr->setWordWrap(true);
    hdr->setText(tr("Fetch public light curves for "
                    "<b>Gaia DR3 %1</b> via the bundled "
                    "<i>lightcurvequery</i> Python tool.")
                 .arg(_star->getSourceId().toHtmlEscaped()));
    root->addWidget(hdr);

    auto* srcBox = new QGroupBox(tr("Sources"));
    auto* srcLay = new QHBoxLayout(srcBox);
    _fetchTess  = new QCheckBox("TESS");      _fetchTess->setChecked(true);
    _fetchZtf   = new QCheckBox("ZTF");       _fetchZtf->setChecked(true);
    _fetchAtlas = new QCheckBox("ATLAS");     _fetchAtlas->setChecked(true);
    _fetchGaia  = new QCheckBox("Gaia");      _fetchGaia->setChecked(true);
    _fetchBg    = new QCheckBox("BlackGEM");  _fetchBg->setChecked(false);
    _fetchBg->setToolTip(tr("Requires BLACKGEM_QUERYSCRIPT_LOCATION env var "
                            "to be set in your environment."));
    for (auto* cb : { _fetchTess, _fetchZtf, _fetchAtlas, _fetchGaia, _fetchBg })
        srcLay->addWidget(cb);
    srcLay->addStretch();
    root->addWidget(srcBox);

    auto* optBox = new QGroupBox(tr("Options"));
    auto* optLay = new QFormLayout(optBox);

    _trimTess = new QDoubleSpinBox;
    _trimTess->setRange(0.0, 0.5); _trimTess->setSingleStep(0.01);
    _trimTess->setDecimals(3);     _trimTess->setValue(0.0);
    _trimTess->setSuffix(tr(" frac"));
    _trimTess->setToolTip(tr("Trim this fraction from beginning and end of every TESS sector"));
    optLay->addRow(tr("Trim TESS:"), _trimTess);

    _ztfInner = new QDoubleSpinBox;
    _ztfInner->setRange(0.5, 60.0); _ztfInner->setValue(5.0);
    _ztfInner->setSuffix(QStringLiteral(" \""));
    optLay->addRow(tr("ZTF inner radius:"), _ztfInner);

    _ztfOuter = new QDoubleSpinBox;
    _ztfOuter->setRange(1.0, 120.0); _ztfOuter->setValue(20.0);
    _ztfOuter->setSuffix(QStringLiteral(" \""));
    optLay->addRow(tr("ZTF outer radius:"), _ztfOuter);

    _reattemptAll = new QCheckBox(tr("Reattempt everything (clear previous results)"));
    _reattemptAll->setToolTip(tr(
        "Delete the cached lightcurvequery output files for the selected "
        "sources before fetching, and fully replace any existing in-memory "
        "lightcurves for those sources with the fresh results."));
    optLay->addRow(QString(), _reattemptAll);

    root->addWidget(optBox);

    auto* btnRow = new QHBoxLayout;
    _fetchBtn = new QPushButton(tr("Fetch"));
    _fetchBtn->setDefault(true);
    _cancelFetch = new QPushButton(tr("Cancel"));
    _cancelFetch->setEnabled(false);
    _importCsvBtn = new QPushButton(tr("Import from CSV…"));
    _importCsvBtn->setToolTip(tr("Import a lightcurve from a CSV file for "
                                 "any instrument with a photometric mode."));
    _setupEnvBtn = new QPushButton(tr("Set up environment…"));
    _setupEnvBtn->setToolTip(tr("Automatically unpack the bundled lightcurvequery "
                                "scripts and build a Python environment with all "
                                "required packages."));
    _setupEnvBtn->setVisible(false);
    _fetchBusy = new QProgressBar;
    _fetchBusy->setRange(0, 0);
    _fetchBusy->setVisible(false);
    _fetchBusy->setMaximumHeight(18);
    _fetchStatus = new QLabel;
    _fetchStatus->setStyleSheet("color: gray;");
    btnRow->addWidget(_fetchBtn);
    btnRow->addWidget(_cancelFetch);
    btnRow->addWidget(_importCsvBtn);
    btnRow->addWidget(_setupEnvBtn);
    btnRow->addSpacing(8);
    btnRow->addWidget(_fetchBusy, 1);
    btnRow->addWidget(_fetchStatus, 2);
    root->addLayout(btnRow);

    connect(_fetchBtn,     &QPushButton::clicked, this, &LightcurveFetchDialog::onFetchClicked);
    connect(_cancelFetch,  &QPushButton::clicked, this, &LightcurveFetchDialog::onFetchCancelClicked);
    connect(_importCsvBtn, &QPushButton::clicked, this, &LightcurveFetchDialog::onImportCsvClicked);
    connect(_setupEnvBtn,  &QPushButton::clicked, this, &LightcurveFetchDialog::onSetupEnvClicked);

    _fetchLog = new AnsiTerminalWidget;
    root->addWidget(_fetchLog, 1);

    _fetchService = _controller ? _controller->lightcurveFetchService() : nullptr;

    connect(_fetchService, &LightcurveFetchService::sessionStarted,
            this, &LightcurveFetchDialog::onFetchSessionStarted);
    connect(_fetchService, &LightcurveFetchService::sessionOutput,
            this, &LightcurveFetchDialog::onFetchSessionOutput);
    connect(_fetchService, &LightcurveFetchService::sessionFinished,
            this, &LightcurveFetchDialog::onFetchSessionFinished);

    // A batch fetch (started from the Analysis menu) may import lightcurves
    // for this star while the dialog is open - refresh the views then too.
    connect(_fetchService, &LightcurveFetchService::starLightcurvesUpdated,
            this, [this](const QString& starId) {
        if (!_star || starId != _star->getId()) return;
        if (_lcPanel)          _lcPanel->refresh();
        if (_fitLcPanel)       _fitLcPanel->refresh();
        if (_periodogramPanel) pushSeriesIntoPanel();
        refreshViewerSourceCombo();
        refreshPreviewsTab();
    });

    connect(_fetchService, &LightcurveFetchService::availabilityChecked,
            this, [this](bool ok, const QString& msg) {
        const bool sessionActive = !_fetchSessionId.isEmpty() &&
            _fetchService->isSessionActive(_fetchSessionId);
        if (ok) {
            if (!sessionActive) {
                _fetchStatus->setStyleSheet("color: gray;");
                _fetchStatus->setText(tr("Ready."));
                _fetchBtn->setEnabled(true);
            }
            if (_setupEnvBtn) _setupEnvBtn->setVisible(false);
        } else {
            _fetchStatus->setStyleSheet("color: #c46060;");
            _fetchStatus->setText(tr("⚠ %1").arg(msg.section('\n', 0, 0)));
            _fetchBtn->setEnabled(false);
            _fetchLog->feed("[availability] " + msg + '\n');
            // Offer one-click setup when a bundled copy is available; otherwise
            // fall back to the manual configuration hint.
            const bool canBootstrap = LcqueryEnvironment::bundleAvailable();
            if (_setupEnvBtn) _setupEnvBtn->setVisible(canBootstrap);
            if (canBootstrap)
                _fetchLog->feed(tr("→ Click \"Set up environment…\" to install it "
                                   "automatically, or configure paths in "
                                   "Settings → Lightcurve Fetching.\n"));
            else
                _fetchLog->feed(tr("→ Open Settings → Lightcurve Fetching to configure.\n"));
        }
    });

    _fetchStatus->setStyleSheet("color: gray;");
    _fetchStatus->setText(tr("Checking Python setup…"));
    _fetchBtn->setEnabled(false);
    _fetchService->checkAvailabilityAsync();

    attachToExistingSession();

    return page;
}

void LightcurveFetchDialog::setFetchRunningUi(bool running)
{
    _fetchBtn->setEnabled(!running);
    _cancelFetch->setEnabled(running);
    _fetchBusy->setVisible(running);
    if (running) {
        _fetchStatus->setStyleSheet("color: #dca84d;");
        _fetchStatus->setText(tr("Running…"));
    }
}

void LightcurveFetchDialog::attachToExistingSession()
{
    if (!_fetchService || !_star) return;
    const QString id = _fetchService->sessionForStar(_star->getId());
    if (id.isEmpty()) return;

    _fetchSessionId = id;

    // Replay the buffered terminal output of the (possibly still running)
    // session so closing and reopening the dialog loses nothing.
    _fetchLog->clearTerminal();
    const QByteArray buf = _fetchService->sessionBuffer(id);
    if (!buf.isEmpty()) _fetchLog->feed(buf);

    const auto info = _fetchService->sessionInfo(id);
    if (info.state == LightcurveFetchService::State::Running) {
        setFetchRunningUi(true);
    } else if (info.state == LightcurveFetchService::State::Queued) {
        setFetchRunningUi(true);
        _fetchStatus->setText(tr("Queued…"));
    } else if (!info.summary.isEmpty()) {
        _fetchStatus->setStyleSheet(info.ok ? "color: #7dbd5e;" : "color: gray;");
        _fetchStatus->setText(info.summary);
    }
}

void LightcurveFetchDialog::ensureFitTabBuilt()
{
    if (_fitTabBuilt) return;
    _fitTabBuilt = true;
    QWidget* content = buildFitTab();
    _fitTabPage->layout()->addWidget(content);
}

QWidget *LightcurveFetchDialog::buildFitTab() {
    auto *page = new QWidget;
    auto *root = new QHBoxLayout(page);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(6);

    DetailPanel::Context ctx;
    ctx.star       = _star;
    ctx.dbm        = _dbm;
    ctx.controller = _controller;
    ctx.projectId  = _projectId;
    // Build the panel in deferred mode: it shows a single-card loading shimmer
    // immediately and runs the heavy lightcurve load/plot build on the next
    // event-loop turn, so the Fit tab becomes interactive instantly.
    _fitLcPanel    = new LCPanel(ctx, nullptr, /*deferPopulate=*/true);
    root->addWidget(_fitLcPanel, 1);

    auto *sidebar = new QWidget;
    sidebar->setMinimumWidth(280);
    sidebar->setMaximumWidth(380);
    auto *sv = new QVBoxLayout(sidebar);
    sv->setContentsMargins(4, 4, 4, 4);
    sv->setSpacing(8);

    // ── Data selection ──────────────────────────────────────────────────
    auto *dBox      = new QGroupBox(tr("Data to fit"));
    auto *dLay      = new QFormLayout(dBox);
    _fitSourceCombo = new QComboBox;
    _fitFilterCombo = new QComboBox;
    _fitSourceCombo->setToolTip(tr("Lightcurve source (TESS, ZTF, ...)"));
    _fitFilterCombo->setToolTip(
        tr("Filter / passband. Each filter is fit independently because the "
           "lightcurve amplitude varies with wavelength."));
    dLay->addRow(tr("Source:"), _fitSourceCombo);
    dLay->addRow(tr("Filter:"), _fitFilterCombo);
    connect(_fitSourceCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { onFitSourceChanged(); });
    connect(_fitFilterCombo,
            QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int) { onFitFilterChanged(); });
    sv->addWidget(dBox);

    // ── Period list (unchanged) ─────────────────────────────────────────
    auto *pBox  = new QGroupBox(tr("Period"));
    auto *pLay  = new QVBoxLayout(pBox);
    auto *pInfo = new QLabel(tr("Peaks collected in the Periodogram tab:"));
    pInfo->setStyleSheet("color: gray;");
    pLay->addWidget(pInfo);
    _fitPeriodList = new QListWidget;
    _fitPeriodList->setAlternatingRowColors(true);
    _fitPeriodList->setSelectionMode(QAbstractItemView::SingleSelection);
    connect(_fitPeriodList, &QListWidget::currentRowChanged, this,
            [this](int) { onFitPeriodSelectionChanged(); });
    pLay->addWidget(_fitPeriodList, 1);
    sv->addWidget(pBox, 1);

    auto *bBox   = new QGroupBox(tr("Fit binning"));
    auto *bLay   = new QFormLayout(bBox);
    _fitBinsSpin = new QSpinBox;
    _fitBinsSpin->setRange(5, 5000);
    _fitBinsSpin->setValue(100);
    connect(_fitBinsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this,
            &LightcurveFetchDialog::onFitBinsChanged);
    bLay->addRow(tr("N bins:"), _fitBinsSpin);

    _fitCombinerCombo = new QComboBox;
    _fitCombinerCombo->addItem(
        LCBinning::combinerLabel(LCBinning::Combiner::WeightedMean),
        int(LCBinning::Combiner::WeightedMean));
    _fitCombinerCombo->addItem(
        LCBinning::combinerLabel(LCBinning::Combiner::MedianScatter),
        int(LCBinning::Combiner::MedianScatter));
    _fitCombinerCombo->setToolTip(
        tr("How the samples in a bin become one point.<br><br>"
           "<b>Weighted mean</b> propagates the catalogue errors, which is "
           "only honest when those errors are. Surveys that quote them too "
           "small (ATLAS is a repeat offender) hand the fit error bars that "
           "are too small by the same factor, and a single wild sample with a "
           "tiny quoted error takes over the mean of its bin.<br><br>"
           "<b>Median · error from scatter</b> ignores the quoted errors and "
           "measures what the samples actually do: the bin value is their "
           "median, the error their observed scatter. Needs a decent number "
           "of samples per bin."));
    connect(_fitCombinerCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int) { onFitBinsChanged(); });
    bLay->addRow(tr("Combine by:"), _fitCombinerCombo);
    sv->addWidget(bBox);

    _fitInfoLabel = new QLabel;
    _fitInfoLabel->setWordWrap(true);
    _fitInfoLabel->setStyleSheet("color: gray;");
    sv->addWidget(_fitInfoLabel);

    auto *fitsBox = new QGroupBox(tr("Existing fits"));
    auto *fitsLay = new QVBoxLayout(fitsBox);
    fitsLay->setContentsMargins(4, 4, 4, 4);
    fitsLay->setSpacing(4);

    _existingFitsTree = new QTreeWidget;
    _existingFitsTree->setHeaderLabels({tr("Fit"), tr("P [d]"), tr("χ²")});
    _existingFitsTree->setSelectionMode(QAbstractItemView::SingleSelection);
    _existingFitsTree->setAlternatingRowColors(true);
    _existingFitsTree->setRootIsDecorated(true);
    _existingFitsTree->header()->setStretchLastSection(false);
    _existingFitsTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    _existingFitsTree->header()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    _existingFitsTree->header()->setSectionResizeMode(
        2, QHeaderView::ResizeToContents);
    _existingFitsTree->setToolTip(
        tr("All LC fits stored for this star, grouped by source/filter. "
           "Double-click a fit to fold the Viewer on its period & T₀. "
           "The current best fit per (source, filter) is highlighted."));
    connect(_existingFitsTree, &QTreeWidget::itemDoubleClicked, this,
            [this](QTreeWidgetItem *, int) { onPlotExistingFitClicked(); });
    fitsLay->addWidget(_existingFitsTree, 1);

    auto *fitBtnRow = new QHBoxLayout;
    _plotFitBtn     = new QPushButton(tr("Plot in Viewer"));
    _setBestFitBtn  = new QPushButton(tr("Set as Best"));
    _deleteFitBtn   = new QPushButton(tr("Delete…"));
    connect(_plotFitBtn, &QPushButton::clicked, this,
            &LightcurveFetchDialog::onPlotExistingFitClicked);
    connect(_setBestFitBtn, &QPushButton::clicked, this,
            &LightcurveFetchDialog::onSetSelectedAsBestClicked);
    connect(_deleteFitBtn, &QPushButton::clicked, this,
            &LightcurveFetchDialog::onDeleteSelectedFitClicked);
    fitBtnRow->addWidget(_plotFitBtn);
    fitBtnRow->addWidget(_setBestFitBtn);
    fitBtnRow->addWidget(_deleteFitBtn);
    fitsLay->addLayout(fitBtnRow);

    sv->addWidget(fitsBox, 1);

    // ── Parameters of the selected existing fit ─────────────────────────
    auto *detBox = new QGroupBox(tr("Selected fit parameters"));
    auto *detLay = new QVBoxLayout(detBox);
    detLay->setContentsMargins(4, 4, 4, 4);
    _fitDetailsLabel = new QLabel(tr("Select a fit to see its parameters."));
    _fitDetailsLabel->setTextFormat(Qt::RichText);
    _fitDetailsLabel->setWordWrap(true);
    _fitDetailsLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    _fitDetailsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    auto *detScroll = new QScrollArea;
    detScroll->setWidget(_fitDetailsLabel);
    detScroll->setWidgetResizable(true);
    detScroll->setFrameShape(QFrame::NoFrame);
    detScroll->setFixedHeight(200);
    detLay->addWidget(detScroll);
    sv->addWidget(detBox);

    _fitRunBtn = new QPushButton;
    _fitRunBtn->setEnabled(false);
    connect(_fitRunBtn, &QPushButton::clicked, this,
            &LightcurveFetchDialog::onFitRunClicked);
    sv->addWidget(_fitRunBtn);

    connect(_existingFitsTree, &QTreeWidget::itemSelectionChanged, this,
            [this] {
                const bool any = selectedExistingFit() != nullptr;
                if (_plotFitBtn)
                    _plotFitBtn->setEnabled(any);
                if (_setBestFitBtn)
                    _setBestFitBtn->setEnabled(any);
                if (_deleteFitBtn)
                    _deleteFitBtn->setEnabled(any);
                updateSelectedFitDetails();
            });

    root->addWidget(sidebar);

    // These read the star's photometry directly (not the LCPanel's populated
    // series), so they can run now while the panel is still showing its shimmer.
    refreshFitSourceCombo();
    refreshFitPeriodList();
    refreshExistingFitsTree();

    // The folded preview depends on the panel's populated series, so defer it
    // until populate() has finished. populated() fires once, after the heavy
    // load on the next event-loop turn.
    connect(_fitLcPanel, &DetailPanel::populated, this, [this] {
        if (_fitLcPanel && _fitBinsSpin)
            _fitLcPanel->setUniformFoldedBins(_fitBinsSpin->value());
        onFitPeriodSelectionChanged();
    });
    QTimer::singleShot(0, _fitLcPanel, [p = _fitLcPanel] { p->populateNow(); });

    return page;
}

void LightcurveFetchDialog::refreshFitSourceCombo() {
    if (!_fitSourceCombo)
        return;
    const QString  prev = _fitSourceCombo->currentText();
    QSignalBlocker b(_fitSourceCombo);
    _fitSourceCombo->clear();

    auto phot = _star ? _star->getPhotometry() : nullptr;
    if (phot) {
        auto sources = phot->getLightcurveSources();
        std::sort(sources.begin(), sources.end());
        for (const auto &s : sources)
            _fitSourceCombo->addItem(s);
    }
    const int restored = prev.isEmpty() ? -1 : _fitSourceCombo->findText(prev);
    if (restored >= 0) {
        _fitSourceCombo->setCurrentIndex(restored);
    } else if (phot && _fitSourceCombo->count() > 0) {
        // No previous choice to restore: default to the best-sampled source.
        int bestIdx = 0;
        int bestN   = -1;
        for (int i = 0; i < _fitSourceCombo->count(); ++i) {
            const int n =
                int(phot->getLightcurve(_fitSourceCombo->itemText(i)).size());
            if (n > bestN) {
                bestN   = n;
                bestIdx = i;
            }
        }
        _fitSourceCombo->setCurrentIndex(bestIdx);
    }
    refreshFitFilterCombo();
}

void LightcurveFetchDialog::refreshFitFilterCombo() {
    if (!_fitFilterCombo || !_fitSourceCombo)
        return;
    const QString  prev = _fitFilterCombo->currentText();
    QSignalBlocker b(_fitFilterCombo);
    _fitFilterCombo->clear();

    auto          phot = _star ? _star->getPhotometry() : nullptr;
    const QString src  = _fitSourceCombo->currentText();
    QHash<QString, int> counts;
    if (phot && !src.isEmpty()) {
        const auto pts = phot->getLightcurve(src);
        for (const auto &p : pts)
            if (!p.filter.isEmpty())
                ++counts[p.filter];
        QStringList filters = counts.keys();
        std::sort(filters.begin(), filters.end());
        if (filters.isEmpty()) {
            filters << ""; // unfiltered series
            counts[""] = int(pts.size());
        }
        for (const auto &f : filters)
            _fitFilterCombo->addItem(f.isEmpty() ? tr("(unfiltered)") : f, f);
    }
    const int restored = prev.isEmpty() ? -1 : _fitFilterCombo->findText(prev);
    if (restored >= 0) {
        _fitFilterCombo->setCurrentIndex(restored);
    } else if (_fitFilterCombo->count() > 0) {
        // No previous choice to restore: default to the best-sampled filter.
        int bestIdx = 0;
        int bestN   = -1;
        for (int i = 0; i < _fitFilterCombo->count(); ++i) {
            const int n = counts.value(_fitFilterCombo->itemData(i).toString());
            if (n > bestN) {
                bestN   = n;
                bestIdx = i;
            }
        }
        _fitFilterCombo->setCurrentIndex(bestIdx);
    }
}

void LightcurveFetchDialog::onFitSourceChanged() {
    refreshFitFilterCombo();
    onFitFilterChanged();
}

void LightcurveFetchDialog::onFitFilterChanged() {
    // Recompute the folded preview and bin counts for the new selection.
    onFitPeriodSelectionChanged();
}

// ── Periodogram tab (panel + right-side controls) ──────────────────

QWidget* LightcurveFetchDialog::buildPeriodogramTab()
{
    _periodogramPanel = new PeriodogramPanel(_dbm, _star->getId());

    // Sync UI when panel state changes.
    connect(_periodogramPanel, &PeriodogramPanel::seriesChanged,
            this, &LightcurveFetchDialog::refreshSeriesListFromPanel);
    connect(_periodogramPanel, &PeriodogramPanel::computeFinished,
            this, &LightcurveFetchDialog::onPanelComputeFinished);

    // Double-click on a periodogram peak → fold viewer + add to peaks table.
    // Click on a periodogram → fold viewer + add a solution to the peaks table.
    connect(_periodogramPanel, &PeriodogramPanel::periodSelected,
            this, [this](double clickedPeriod) {
        if (clickedPeriod <= 0) return;

        const bool exact = _clickExactRadio && _clickExactRadio->isChecked();
        double foldPeriod = clickedPeriod;

        if (exact) {
            // Mode 2: add the exact period the user clicked on, no snapping.
            PeriodogramPanel::PeriodPeak pk;
            pk.period      = clickedPeriod;
            pk.frequency   = 1.0 / clickedPeriod;
            pk.power       = 0.0;
            pk.periodError = 0.0;
            pk.sourceLabel = "click (exact)";
            addPeak(pk);
        } else {
            // Mode 1: snap to the nearest peak within a small window and add it
            // ± its estimated sigma.
            const Periodogram::Result res = currentPeriodogramResult();
            if (res.isValid()) {
                const auto pk = PeriodogramPanel::estimatePeakAt(
                    res, clickedPeriod, kClickSnapRelWindow);
                addPeak(pk);
                if (pk.period > 0) foldPeriod = pk.period;
            } else {
                // No periodogram computed yet - fall back to the exact click.
                PeriodogramPanel::PeriodPeak pk;
                pk.period      = clickedPeriod;
                pk.frequency   = 1.0 / clickedPeriod;
                pk.sourceLabel = "click";
                addPeak(pk);
            }
        }

        // Fold the viewer immediately for the original UX.
        if (_lcPanel) {
            _lcPanel->setFoldPeriod(foldPeriod);
            _lcPanel->setFolded(true);
        }
        _tabs->setCurrentIndex(0);
    });

    auto* splitter = new QSplitter(Qt::Horizontal);
    splitter->addWidget(_periodogramPanel);
    splitter->addWidget(buildPeriodogramControls());
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({900, 380});

    auto* wrap = new QWidget;
    auto* l = new QVBoxLayout(wrap);
    l->setContentsMargins(0, 0, 0, 0);
    l->addWidget(splitter, 1);
    return wrap;
}

QWidget* LightcurveFetchDialog::buildPeriodogramControls()
{
    auto* host = new QWidget;
    host->setMinimumWidth(340);
    host->setMaximumWidth(460);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* inner  = new QWidget;
    auto* vlay   = new QVBoxLayout(inner);
    vlay->setContentsMargins(6, 6, 6, 6);
    vlay->setSpacing(8);

    // ── Parameters group ──
    auto* paramBox  = new QGroupBox("Periodogram parameters");
    auto* paramForm = new QFormLayout(paramBox);
    paramForm->setLabelAlignment(Qt::AlignRight);
    _pgParamForm = paramForm;

    _backendCombo = new QComboBox;
    _backendCombo->addItem("Lomb-Scargle (GLS)",
                           static_cast<int>(Periodogram::Backend::LombScargle));
    _backendCombo->addItem("FPW (phase-binned)",
                           static_cast<int>(Periodogram::Backend::FPW));
    _backendCombo->setToolTip(
        "Lomb-Scargle fits a sinusoid at every trial frequency - best for smooth,\n"
        "near-sinusoidal variability.\n\n"
        "FPW (Finkbeiner, Prince & Whitebook 2025, arXiv:2502.00243) folds the\n"
        "data into phase bins and scores the Δχ² of a piecewise-constant model.\n"
        "It makes no assumption about the folded waveform, so eclipses and other\n"
        "sharp features are recovered much better.\n\n"
        "Cost is O(N_freq·N_data) with no FFT shortcut: faster than Lomb-Scargle\n"
        "for sparse series (≲2000 points) but noticeably slower for dense ones\n"
        "such as full TESS 2-min cadence.");
    paramForm->addRow("Method:", _backendCombo);

    _fpwBinsSpin = new QSpinBox;
    _fpwBinsSpin->setRange(2, 1000);
    _fpwBinsSpin->setValue(Periodogram::kFPWDefaultBins);
    _fpwBinsSpin->setToolTip(
        "Number of phase bins FPW folds into.\n"
        "≈4-5 for sinusoids, ≈10 for general waveforms, ≈20 for narrow eclipses\n"
        "(a duty cycle down to ~1/M of the period is resolvable).\n\n"
        "More bins resolve finer structure but also demand a finer frequency\n"
        "grid - raise Oversample alongside it.");
    paramForm->addRow("Phase bins:", _fpwBinsSpin);

    connect(_backendCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &LightcurveFetchDialog::onBackendChanged);
    connect(_fpwBinsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &LightcurveFetchDialog::onBackendChanged);

    _minPSpin = new PreciseDoubleSpinBox; // 15-sig-fig, paste-friendly
    _minPSpin->setRange(0.0, 1e9);
    _minPSpin->setSpecialValueText("auto");
    _minPSpin->setSuffix(" d");
    paramForm->addRow("Min P:", _minPSpin);

    _maxPSpin = new PreciseDoubleSpinBox;
    _maxPSpin->setRange(0.0, 1e9);
    _maxPSpin->setSpecialValueText("auto");
    _maxPSpin->setSuffix(" d");
    paramForm->addRow("Max P:", _maxPSpin);

    _nSampSpin = new QSpinBox;
    _nSampSpin->setRange(0, 10000000); _nSampSpin->setSingleStep(1000);
    _nSampSpin->setSpecialValueText("auto");
    paramForm->addRow("N:", _nSampSpin);

    _osSpin = new QDoubleSpinBox;
    _osSpin->setDecimals(1); _osSpin->setRange(0.1, 100.0);
    _osSpin->setValue(20.0);
    paramForm->addRow("Oversample:", _osSpin);

    auto* paramBtnRow = new QHBoxLayout;
    _optimalBtn = new QToolButton;
    _optimalBtn->setText("Optimal");
    _optimalBtn->setToolTip("Auto-fill empty fields based on current selection");
    connect(_optimalBtn, &QToolButton::clicked, this, &LightcurveFetchDialog::onOptimalClicked);
    paramBtnRow->addWidget(_optimalBtn);
    paramBtnRow->addStretch();
    _computeBtn = new QPushButton("Compute");
    _computeBtn->setDefault(true);
    connect(_computeBtn, &QPushButton::clicked, this, &LightcurveFetchDialog::onComputeClicked);
    paramBtnRow->addWidget(_computeBtn);
    paramForm->addRow(paramBtnRow);

    onBackendChanged();   // hides the FPW rows for the default GLS backend

    vlay->addWidget(paramBox);

    // ── Series group ──
    auto* seriesBox = new QGroupBox("Series");
    auto* sLay      = new QVBoxLayout(seriesBox);
    sLay->setContentsMargins(6, 6, 6, 6);
    sLay->setSpacing(4);

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel("Min pts:"));
    _minPtsSpin = new QSpinBox;
    _minPtsSpin->setRange(0, 1000000);
    _minPtsSpin->setValue(50);
    _minPtsSpin->setMaximumWidth(90);
    connect(_minPtsSpin, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &LightcurveFetchDialog::onMinPtsChanged);
    topRow->addWidget(_minPtsSpin);
    topRow->addStretch();
    auto* allBtn  = new QToolButton; allBtn->setText("All");
    auto* noneBtn = new QToolButton; noneBtn->setText("None");
    connect(allBtn,  &QToolButton::clicked, this, &LightcurveFetchDialog::onAllClicked);
    connect(noneBtn, &QToolButton::clicked, this, &LightcurveFetchDialog::onNoneClicked);
    topRow->addWidget(allBtn);
    topRow->addWidget(noneBtn);
    sLay->addLayout(topRow);

    _seriesList = new QListWidget;
    _seriesList->setAlternatingRowColors(true);
    _seriesList->setMinimumHeight(110);
    connect(_seriesList, &QListWidget::itemChanged,
            this, &LightcurveFetchDialog::onSeriesItemChanged);
    sLay->addWidget(_seriesList);

    vlay->addWidget(seriesBox);

    // ── Peaks group ──
    auto *peakBox = new QGroupBox("Period detection");
    auto *pLay    = new QVBoxLayout(peakBox);
    pLay->setContentsMargins(6, 6, 6, 6);
    pLay->setSpacing(4);

    // ── Click-to-add behaviour toggle ─────────────────────────────────
    auto *clickModeRow = new QHBoxLayout;
    clickModeRow->addWidget(new QLabel("On click:"));
    _clickNearestRadio = new QRadioButton("Nearest peak");
    _clickExactRadio   = new QRadioButton("Exact period");
    _clickNearestRadio->setChecked(true);
    _clickNearestRadio->setToolTip(
        "Clicking in the periodogram snaps to the nearest peak (within a small "
        "window) and adds it ± its estimated sigma.");
    _clickExactRadio->setToolTip(
        "Clicking in the periodogram adds the exact period at the cursor as a "
        "solution, without snapping.");
    auto *clickModeGroup = new QButtonGroup(this);
    clickModeGroup->addButton(_clickNearestRadio);
    clickModeGroup->addButton(_clickExactRadio);
    clickModeRow->addWidget(_clickNearestRadio);
    clickModeRow->addWidget(_clickExactRadio);
    clickModeRow->addStretch();
    pLay->addLayout(clickModeRow);

    auto *peakTop = new QHBoxLayout;
    peakTop->addWidget(new QLabel("From:"));
    _peakSourceCombo = new QComboBox;
    _peakSourceCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    peakTop->addWidget(_peakSourceCombo, 1);
    peakTop->addWidget(new QLabel("N:"));
    _peakCountSpin = new QSpinBox;
    _peakCountSpin->setRange(1, 50);
    _peakCountSpin->setValue(5);
    _peakCountSpin->setMaximumWidth(60);
    peakTop->addWidget(_peakCountSpin);
    pLay->addLayout(peakTop);

    auto* peakBtns = new QHBoxLayout;
    _detectBtn       = new QPushButton("Detect peaks");
    _addManualBtn    = new QPushButton("Add…");
    _doublePeriodBtn = new QPushButton("×2");
    _doublePeriodBtn->setToolTip(tr("Add double the selected period as a new "
                                    "peak (e.g. for ellipsoidal variables whose "
                                    "LC peak sits at P/2)."));
    _removeBtn    = new QPushButton("Remove");
    _clearBtn     = new QPushButton("Clear");
    connect(_detectBtn,       &QPushButton::clicked, this, &LightcurveFetchDialog::onDetectPeaksClicked);
    connect(_addManualBtn,    &QPushButton::clicked, this, &LightcurveFetchDialog::onAddManualPeakClicked);
    connect(_doublePeriodBtn, &QPushButton::clicked, this, &LightcurveFetchDialog::onDoublePeriodClicked);
    connect(_removeBtn,       &QPushButton::clicked, this, &LightcurveFetchDialog::onRemovePeakClicked);
    connect(_clearBtn,        &QPushButton::clicked, this, &LightcurveFetchDialog::onClearPeaksClicked);
    peakBtns->addWidget(_detectBtn);
    peakBtns->addWidget(_addManualBtn);
    peakBtns->addWidget(_doublePeriodBtn);
    peakBtns->addWidget(_removeBtn);
    peakBtns->addWidget(_clearBtn);
    pLay->addLayout(peakBtns);

    auto* quickRow = new QHBoxLayout;
    _addRVPeriodBtn   = new QPushButton("Add RV period");
    _addPhotPeriodBtn = new QPushButton("Add phot period");
    _addRVPeriodBtn->setToolTip(
        "Append the star's best RV-fit period to the peaks list.");
    _addPhotPeriodBtn->setToolTip(
        "Append the star's stored photometric best-fit period to the peaks list.");
    connect(_addRVPeriodBtn,   &QPushButton::clicked,
            this, &LightcurveFetchDialog::onAddRVPeriodClicked);
    connect(_addPhotPeriodBtn, &QPushButton::clicked,
            this, &LightcurveFetchDialog::onAddPhotPeriodClicked);
    quickRow->addWidget(_addRVPeriodBtn);
    quickRow->addWidget(_addPhotPeriodBtn);
    quickRow->addStretch();
    pLay->addLayout(quickRow);

    _peaksTable = new QTableWidget(0, 4);
    _peaksTable->setHorizontalHeaderLabels({"Period [d]", "± [d]", "Power", "Source"});
    _peaksTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _peaksTable->horizontalHeader()->setStretchLastSection(true);
    _peaksTable->verticalHeader()->setVisible(false);
    _peaksTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    _peaksTable->setSelectionMode(QAbstractItemView::SingleSelection);
    _peaksTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    _peaksTable->setMinimumHeight(150);
    connect(_peaksTable, &QTableWidget::itemSelectionChanged,
            this, &LightcurveFetchDialog::onPeakSelectionChanged);
    connect(_peaksTable, &QTableWidget::itemDoubleClicked,
            this, &LightcurveFetchDialog::onPeakDoubleClicked);
    pLay->addWidget(_peaksTable);

    auto* actRow = new QHBoxLayout;
    _foldBtn    = new QPushButton("Fold in Viewer");
    _bestFitBtn = new QPushButton("Set as Best Fit");
    _foldBtn->setEnabled(false);
    _bestFitBtn->setEnabled(false);
    connect(_foldBtn,    &QPushButton::clicked, this, &LightcurveFetchDialog::onFoldInViewerClicked);
    connect(_bestFitBtn, &QPushButton::clicked, this, &LightcurveFetchDialog::onSetAsBestFitClicked);
    actRow->addWidget(_foldBtn);
    actRow->addWidget(_bestFitBtn);
    pLay->addLayout(actRow);

    _bestFitLabel = new QLabel;
    _bestFitLabel->setStyleSheet("color: gray;");
    _bestFitLabel->setWordWrap(true);
    if (Star::isSet(_star->getPhotPeriod())) {
        _bestFitLabel->setText(
            QString("Current best-fit P = %1 ± %2 d")
                .arg(_star->getPhotPeriod(), 0, 'g', 6)
                .arg(Star::isSet(_star->getPhotEPeriod()) ? _star->getPhotEPeriod() : 0.0, 0, 'g', 2));
    } else {
        _bestFitLabel->setText("No best-fit period stored yet.");
    }
    pLay->addWidget(_bestFitLabel);

    vlay->addWidget(peakBox, 1);
    bool hasRVBest = false;
    if (auto rv = _star ? _star->getRVCurve() : nullptr) {
        if (auto bf = rv->getBestFit(); bf && bf->getPeriod() > 0)
            hasRVBest = true;
    }
    _addRVPeriodBtn->setEnabled(hasRVBest);
    _addPhotPeriodBtn->setEnabled(_star && Star::isSet(_star->getPhotPeriod()));
    vlay->addStretch();

    scroll->setWidget(inner);

    auto* outerLayout = new QVBoxLayout(host);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->addWidget(scroll);

    return host;
}

// ── Periodogram tab lifecycle ─────────────────────────────────────

void LightcurveFetchDialog::onPeriodogramTabActivated()
{
    if (!_periodogramPanel || !_lcPanel) return;
    pushSeriesIntoPanel();
}

void LightcurveFetchDialog::pushSeriesIntoPanel()
{
    QList<PeriodogramPanel::Series> conv;
    const auto src = _lcPanel->seriesData(false);   // exclude flagged
    conv.reserve(src.size());
    for (const auto& s : src)
        conv.append({s.source, s.filter, s.t, s.y, s.e});
    _periodogramPanel->setSeries(conv);
}

void LightcurveFetchDialog::refreshPeakSourceCombo()
{
    if (!_peakSourceCombo || !_periodogramPanel) return;
    const QString prev = _peakSourceCombo->currentData().toString();
    QSignalBlocker b(_peakSourceCombo);
    _peakSourceCombo->clear();
    for (const auto& d : _periodogramPanel->availableResults())
        _peakSourceCombo->addItem(d.displayName, d.label);
    const int idx = _peakSourceCombo->findData(prev);
    _peakSourceCombo->setCurrentIndex(idx >= 0 ? idx : (_peakSourceCombo->count() > 0 ? 0 : -1));

    const bool any = _peakSourceCombo->count() > 0;
    _detectBtn->setEnabled(any);
}

void LightcurveFetchDialog::refreshSeriesListFromPanel()
{
    if (!_seriesList || !_periodogramPanel) return;
    QSignalBlocker block(_seriesList);
    _seriesList->clear();

    const auto info = _periodogramPanel->seriesInfo();
    for (const auto& si : info) {
        QString label = si.filter.isEmpty()
            ? QString("%1  (%2 pts)").arg(si.source).arg(si.nPoints)
            : QString("%1 · %2  (%3 pts)").arg(si.source, si.filter).arg(si.nPoints);
        if (!si.eligible)
            label += QString("  - skipped (<%1)").arg(_periodogramPanel->minPointsThreshold());

        auto* it = new QListWidgetItem(label);
        it->setData(Qt::UserRole, si.key);
        it->setFlags(si.eligible
            ? (Qt::ItemIsUserCheckable | Qt::ItemIsEnabled | Qt::ItemIsSelectable)
            : (Qt::ItemIsSelectable));
        it->setCheckState(si.enabled ? Qt::Checked : Qt::Unchecked);
        _seriesList->addItem(it);
    }
    refreshPeakSourceCombo();
}

void LightcurveFetchDialog::onSeriesItemChanged(QListWidgetItem* it)
{
    if (!it || !_periodogramPanel) return;
    const QString key = it->data(Qt::UserRole).toString();
    _periodogramPanel->setSeriesEnabled(key, it->checkState() == Qt::Checked);
}

void LightcurveFetchDialog::onMinPtsChanged(int v)
{
    if (_periodogramPanel) _periodogramPanel->setMinPointsThreshold(v);
}

void LightcurveFetchDialog::onAllClicked()
{
    for (int i = 0; i < _seriesList->count(); ++i) {
        auto* it = _seriesList->item(i);
        if (it->flags() & Qt::ItemIsEnabled) it->setCheckState(Qt::Checked);
    }
}

void LightcurveFetchDialog::onNoneClicked()
{
    for (int i = 0; i < _seriesList->count(); ++i)
        _seriesList->item(i)->setCheckState(Qt::Unchecked);
}

void LightcurveFetchDialog::onOptimalClicked()
{
    if (!_periodogramPanel) return;
    _periodogramPanel->setGridParameters(_minPSpin->value(),
                                         _maxPSpin->value(),
                                         _nSampSpin->value(),
                                         _osSpin->value());
    // Only the fields left on "auto" need a data-derived suggestion; if the
    // user pinned both bounds (e.g. a deliberate 0.005-0.01 d search) we go
    // straight to suggesting N for that range.
    const bool needAuto = (_minPSpin->value() <= 0) || (_maxPSpin->value() <= 0);
    double mn = 0, mx = 0;
    if (needAuto && !_periodogramPanel->suggestAutoBounds(mn, mx)) {
        QMessageBox::warning(this, "Optimal",
            "Could not auto-resolve period bounds - check selection / min pts.");
        return;
    }
    if (_minPSpin->value() <= 0) _minPSpin->setValue(mn);
    if (_maxPSpin->value() <= 0) _maxPSpin->setValue(mx);

    if (!(_maxPSpin->value() > _minPSpin->value())) {
        QMessageBox::warning(this, "Optimal",
            QString("Max P (%1 d) must be greater than Min P (%2 d).")
                .arg(_maxPSpin->value(), 0, 'g', 6)
                .arg(_minPSpin->value(), 0, 'g', 6));
        return;
    }
    _periodogramPanel->setGridParameters(_minPSpin->value(),
                                         _maxPSpin->value(),
                                         _nSampSpin->value(),
                                         _osSpin->value());
    const int nf = _periodogramPanel->suggestAutoNSamples();
    if (nf > 0) _nSampSpin->setValue(nf);
}

void LightcurveFetchDialog::onBackendChanged()
{
    if (!_backendCombo) return;
    const auto backend = static_cast<Periodogram::Backend>(
        _backendCombo->currentData().toInt());
    const bool isFpw = (backend == Periodogram::Backend::FPW);

    if (_pgParamForm && _fpwBinsSpin)
        _pgParamForm->setRowVisible(_fpwBinsSpin, isFpw);

    if (_periodogramPanel)
        _periodogramPanel->setBackend(backend,
                                      _fpwBinsSpin ? _fpwBinsSpin->value()
                                                   : Periodogram::kFPWDefaultBins);
}

void LightcurveFetchDialog::onComputeClicked()
{
    if (!_periodogramPanel) return;
    onBackendChanged();
    _periodogramPanel->setGridParameters(_minPSpin->value(),
                                         _maxPSpin->value(),
                                         _nSampSpin->value(),
                                         _osSpin->value());
    _computeBtn->setEnabled(false);
    _periodogramPanel->computeAll(true);
}

void LightcurveFetchDialog::onPanelComputeFinished(bool /*cancelled*/)
{
    _computeBtn->setEnabled(true);
    refreshPeakSourceCombo();
}

// ── Peak detection / management ───────────────────────────────────

Periodogram::Result LightcurveFetchDialog::currentPeriodogramResult() const {
    Periodogram::Result res;
    if (!_periodogramPanel)
        return res;

    const QString label = _peakSourceCombo
                              ? _peakSourceCombo->currentData().toString()
                              : QString();
    if (!label.isEmpty())
        res = _periodogramPanel->resultByLabel(label);

    if (!res.isValid()) {
        const auto descs = _periodogramPanel->availableResults();
        if (!descs.isEmpty())
            res = _periodogramPanel->resultByLabel(descs.front().label);
    }
    return res;
}

void LightcurveFetchDialog::onDetectPeaksClicked()
{
    if (!_periodogramPanel || !_peakSourceCombo) return;
    const QString label = _peakSourceCombo->currentData().toString();
    if (label.isEmpty()) {
        QMessageBox::information(this, "Detect peaks", "No periodograms available - compute first.");
        return;
    }
    const auto peaks = _periodogramPanel->detectPeaks(label, _peakCountSpin->value());
    for (const auto& pk : peaks) addPeak(pk);
}

void LightcurveFetchDialog::onAddManualPeakClicked() {
    if (!_periodogramPanel)
        return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("Add period"));
    auto *v = new QVBoxLayout(&dlg);

    auto *form = new QFormLayout;
    auto *spin = new PreciseDoubleSpinBox;
    spin->setRange(1e-9, 1e9);
    spin->setValue(1.0);
    spin->setSuffix(" d");
    spin->setToolTip(tr("Supports pasting full-precision periods "
                        "(incl. scientific notation)."));
    form->addRow(tr("Period (days):"), spin);
    v->addLayout(form);

    auto *bb =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    v->addWidget(bb);

    if (dlg.exec() != QDialog::Accepted)
        return;

    const double p = spin->value();
    if (p <= 0)
        return;

    // Keep the entered period exact (the whole point of the precise box);
    // only borrow a power/sigma estimate from the current periodogram, if any.
    PeriodogramPanel::PeriodPeak pk;
    pk.period      = p;
    pk.frequency   = 1.0 / p;
    pk.sourceLabel = "manual";

    const Periodogram::Result res = currentPeriodogramResult();
    if (res.isValid()) {
        const auto est =
            PeriodogramPanel::estimatePeakAt(res, p, kClickSnapRelWindow);
        pk.power       = est.power;
        pk.periodError = est.periodError;
    }
    addPeak(pk);
}

void LightcurveFetchDialog::onDoublePeriodClicked()
{
    const int row = _peaksTable ? _peaksTable->currentRow() : -1;
    if (row < 0 || row >= _peaks.size()) {
        QMessageBox::information(this, tr("Double period"),
            tr("Select a period in the list first, then press ×2."));
        return;
    }

    // Copy the selected peak so we keep its provenance, then double the period
    // (frequency halves, the absolute σ_P scales with the period).
    PeriodogramPanel::PeriodPeak pk = _peaks[row];
    const double orig = pk.period;
    pk.period      = orig * 2.0;
    pk.frequency   = pk.period > 0 ? 1.0 / pk.period : 0.0;
    pk.periodError = pk.periodError * 2.0;
    pk.sourceLabel = QString("2×%1")
                         .arg(QString::number(orig, 'g', 6));
    addPeak(pk);
}

void LightcurveFetchDialog::addPeak(const PeriodogramPanel::PeriodPeak& pk)
{
    if (pk.period <= 0) return;
    for (const auto& existing : _peaks)
        if (std::abs(existing.period - pk.period) / pk.period < 0.01) return;
    _peaks.append(pk);
    std::sort(_peaks.begin(), _peaks.end(),
              [](const auto& a, const auto& b){ return a.period < b.period; });
    commitPeaks();
}

void LightcurveFetchDialog::onRemovePeakClicked()
{
    const int row = _peaksTable->currentRow();
    if (row < 0 || row >= _peaks.size()) return;
    _peaks.removeAt(row);
    commitPeaks();
}

void LightcurveFetchDialog::onClearPeaksClicked()
{
    _peaks.clear();
    commitPeaks();
    if (_periodogramPanel) _periodogramPanel->setHighlightedPeriod(0.0);
}

void LightcurveFetchDialog::loadPersistedPeaks()
{
    if (!_dbm || !_star) return;
    const QString json = _dbm->loadStarPhotPeaks(_star->getId());
    _peaks = PeriodogramPanel::peaksFromJson(json);
    rebuildPeaksTable();   // display only - don't re-persist what we just read

    LOG_INFO("Periodogram",
        QString("Loaded %1 persisted peak(s) for star %2")
            .arg(_peaks.size()).arg(_star->getId()));
}

void LightcurveFetchDialog::persistPeaks()
{
    if (!_dbm || !_star) return;
    const QString json = PeriodogramPanel::peaksToJson(_peaks);
    _dbm->saveStarPhotPeaks(_star->getId(), json);
    // Keep the in-memory Star in sync so a later full upsert writes the current
    // peaks rather than the value the star was loaded with (or NULL).
    _star->setPhotPeaksJson(json);
}

void LightcurveFetchDialog::commitPeaks()
{
    rebuildPeaksTable();
    persistPeaks();
}

void LightcurveFetchDialog::rebuildPeaksTable()
{
    _peaksTable->setRowCount(_peaks.size());
    for (int i = 0; i < _peaks.size(); ++i) {
        const auto& pk = _peaks[i];
        auto setItem = [&](int col, const QString& txt){
            auto* it = new QTableWidgetItem(txt);
            it->setFlags(it->flags() & ~Qt::ItemIsEditable);
            _peaksTable->setItem(i, col, it);
        };
        setItem(0, QString::number(pk.period,      'g', 8));
        setItem(1, pk.periodError > 0
                     ? QString::number(pk.periodError, 'g', 3)
                     : QString("-"));
        setItem(2, QString::number(pk.power, 'g', 3));
        setItem(3, pk.sourceLabel);
    }
    const bool any = !_peaks.isEmpty();
    _foldBtn->setEnabled(any);
    _bestFitBtn->setEnabled(any);

    if (_periodogramPanel) _periodogramPanel->setMarkedPeaks(_peaks);
    refreshFitPeriodList();   
}

double LightcurveFetchDialog::currentSelectedPeriod() const
{
    const int row = _peaksTable->currentRow();
    if (row < 0 || row >= _peaks.size()) return 0.0;
    return _peaks[row].period;
}

void LightcurveFetchDialog::onPeakSelectionChanged()
{
    const double p = currentSelectedPeriod();
    if (_periodogramPanel) _periodogramPanel->setHighlightedPeriod(p);
}

void LightcurveFetchDialog::onPeakDoubleClicked()
{
    onFoldInViewerClicked();
}

void LightcurveFetchDialog::onFoldInViewerClicked()
{
    const double p = currentSelectedPeriod();
    if (p <= 0 || !_lcPanel) return;
    _lcPanel->setFoldPeriod(p);
    _lcPanel->setFolded(true);
    _tabs->setCurrentIndex(0);
}

void LightcurveFetchDialog::onSetAsBestFitClicked()
{
    const int row = _peaksTable->currentRow();
    if (row < 0 || row >= _peaks.size() || !_star) return;
    const auto& pk = _peaks[row];
    applyBestPeriod(pk.period, pk.periodError);
}

double LightcurveFetchDialog::peakErrorFor(double period) const
{
    for (const auto& pk : _peaks)
        if (pk.period > 0.0 && std::abs(pk.period - period) <= 1e-9 * period)
            return pk.periodError;
    return 0.0;
}

void LightcurveFetchDialog::applyBestPeriod(double period, double periodError)
{
    if (!_star || period <= 0.0) return;

    _star->setPhotPeriod(period);
    _star->setPhotEPeriod(periodError);
    _star->markSummaryDirty();                       // notify in-app listeners
    if (_dbm) _dbm->updateStar(_projectId, _star);   // persist to DB

    const QString msg = QString("Current best-fit P = %1 ± %2 d  (set just now)")
                            .arg(period,      0, 'g', 6)
                            .arg(periodError, 0, 'g', 2);
    if (_bestFitLabel)    _bestFitLabel->setText(msg);
    if (_viewerBestLabel) _viewerBestLabel->setText(msg);
    if (_addPhotPeriodBtn) _addPhotPeriodBtn->setEnabled(true);

    refreshFitPeriodList();

    LOG_INFO("Periodogram",
        QString("Saved best-fit photometric period for %1: P=%2 ±%3")
            .arg(_star->getId()).arg(period).arg(periodError));
}

// ──────────────────────────────────────────────────────────────────
// Fetch tab logic
// ──────────────────────────────────────────────────────────────────

void LightcurveFetchDialog::onFetchClicked()
{
    if (!_fetchService) return;

    LightcurveFetcher::Options opt;
    if (_fetchTess->isChecked())  opt.sources << "TESS";
    if (_fetchZtf->isChecked())   opt.sources << "ZTF";
    if (_fetchAtlas->isChecked()) opt.sources << "ATLAS";
    if (_fetchGaia->isChecked())  opt.sources << "Gaia";
    if (_fetchBg->isChecked())    opt.sources << "BlackGEM";

    if (opt.sources.isEmpty()) {
        QMessageBox::information(this, tr("Fetch"),
            tr("Select at least one source."));
        return;
    }

    // ZTF needs an IRSA login (~/.ztfquery), ATLAS an API token; prompt for
    // whichever is missing. Declined sources are dropped and unchecked.
    const QStringList declined = LightcurveCredentialPrompts::ensureCredentials(
        this, opt.sources, _controller ? _controller->settings() : nullptr);
    if (declined.contains(QStringLiteral("ZTF")))   _fetchZtf->setChecked(false);
    if (declined.contains(QStringLiteral("ATLAS"))) _fetchAtlas->setChecked(false);
    if (opt.sources.isEmpty()) {
        QMessageBox::information(this, tr("Fetch"),
            tr("No sources left to fetch after skipping %1.")
                .arg(declined.join(", ")));
        return;
    }

    opt.trimTess    = _trimTess->value();
    opt.ztfInnerArc = _ztfInner->value();
    opt.ztfOuterArc = _ztfOuter->value();

    const bool reattempt = _reattemptAll && _reattemptAll->isChecked();

    if (reattempt) {
        const auto ret = QMessageBox::warning(
            this, tr("Reattempt everything"),
            tr("This will delete the cached lightcurvequery output files for "
               "the selected sources (%1) and fully replace any existing "
               "lightcurves for those sources with the new results.\n\n"
               "If the new fetch fails or returns no data, the previously "
               "fetched lightcurves for those sources will be lost.\n\n"
               "Continue?").arg(opt.sources.join(", ")),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No);
        if (ret != QMessageBox::Yes)
            return;
    }

    _fetchSessionId = _fetchService->enqueue(_star, _projectId, opt, reattempt);
    if (_fetchSessionId.isEmpty()) {
        _fetchStatus->setStyleSheet("color: #c46060;");
        _fetchStatus->setText(tr("Could not start fetch (no Gaia ID?)."));
        return;
    }

    // enqueue() may already have started the session and emitted its first
    // output lines before we knew the session id - replay the buffer.
    _fetchLog->clearTerminal();
    const QByteArray buf = _fetchService->sessionBuffer(_fetchSessionId);
    if (!buf.isEmpty()) _fetchLog->feed(buf);

    const auto info = _fetchService->sessionInfo(_fetchSessionId);
    if (info.state == LightcurveFetchService::State::Queued) {
        setFetchRunningUi(true);
        _fetchStatus->setText(tr("Queued…"));
    } else if (info.state == LightcurveFetchService::State::Running) {
        setFetchRunningUi(true);
    } else {
        // Failed instantly (e.g. environment problem).
        setFetchRunningUi(false);
        _fetchStatus->setStyleSheet(info.ok ? "color: gray;" : "color: #c46060;");
        if (!info.summary.isEmpty()) _fetchStatus->setText(info.summary);
    }
}

void LightcurveFetchDialog::onFetchCancelClicked()
{
    if (_fetchService && !_fetchSessionId.isEmpty())
        _fetchService->cancelSession(_fetchSessionId);
}

void LightcurveFetchDialog::onFetchSessionStarted(const QString& id)
{
    if (id != _fetchSessionId) return;
    setFetchRunningUi(true);
}

void LightcurveFetchDialog::onFetchSessionOutput(const QString& id,
                                                 const QByteArray& chunk)
{
    if (id != _fetchSessionId) return;
    _fetchLog->feed(chunk);
}

void LightcurveFetchDialog::onFetchSessionFinished(const QString& id, bool ok,
                                                   const QString& summary)
{
    if (id != _fetchSessionId) return;

    setFetchRunningUi(false);

    if (ok && !summary.isEmpty() && summary != tr("No data was produced.")) {
        _fetchStatus->setStyleSheet("color: #7dbd5e;");
    } else if (ok) {
        _fetchStatus->setStyleSheet("color: gray;");
    } else {
        _fetchStatus->setStyleSheet("color: #c46060;");
    }
    _fetchStatus->setText(summary.isEmpty() ? tr("Done.") : summary);

    // The service already imported any results; refresh the views.
    if (_lcPanel)          _lcPanel->refresh();
    if (_fitLcPanel)       _fitLcPanel->refresh();
    if (_periodogramPanel) pushSeriesIntoPanel();
    refreshViewerSourceCombo();
    refreshPreviewsTab();
}

void LightcurveFetchDialog::onImportCsvClicked()
{
    ImportLightcurveDialog dlg(_star, _dbm, this);
    if (dlg.exec() != QDialog::Accepted) return;
    if (!dlg.wasImported()) return;

    _fetchLog->feed(tr("[csv-import] %1 points imported under source \"%2\"\n")
                    .arg(dlg.importedPoints().size())
                    .arg(dlg.sourceKey()).toUtf8());

    if (_lcPanel)          _lcPanel->refresh();
    if (_fitLcPanel)       _fitLcPanel->refresh();
    if (_periodogramPanel) pushSeriesIntoPanel();
    refreshViewerSourceCombo();

    _fetchStatus->setStyleSheet("color: #7dbd5e;");
    _fetchStatus->setText(tr("Imported %1 CSV points (%2).")
                          .arg(dlg.importedPoints().size())
                          .arg(dlg.sourceKey()));
}

void LightcurveFetchDialog::onSetupEnvClicked()
{
    AppSettings* settings = _controller ? _controller->settings() : nullptr;
    LcquerySetupDialog dlg(settings, this);
    dlg.exec();
    // Whether or not the user finished setup, re-probe so the UI reflects the
    // current state. If settings were updated this also fires via
    // lcquerySettingsChanged, but probing here covers the cancelled case too.
    _fetchStatus->setStyleSheet("color: gray;");
    _fetchStatus->setText(tr("Re-checking…"));
    _fetchBtn->setEnabled(false);
    _fetchService->recheckAvailability();
}

// ── Previews tab ──────────────────────────────────────────────────────────

QString LightcurveFetchDialog::previewDir() const
{
    // lightcurvequery writes outputs relative to its own working dir
    // (lcquery/) into lightcurves/<gaia_id>/. Use the exact same root the
    // LightcurveFetcher was configured with above.
    return QDir(AppPaths::root()).absoluteFilePath(
        QString("lcquery/lightcurves/%1").arg(_star->getSourceId()));
}

QString LightcurveFetchDialog::previewPath(const QString& filename) const
{
    return QDir(previewDir()).absoluteFilePath(filename);
}

double LightcurveFetchDialog::readCrowdsapFile(const QString& path) const
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return std::numeric_limits<double>::quiet_NaN();
    QTextStream s(&f);
    const QString line = s.readLine().trimmed();
    bool ok = false;
    const double v = line.toDouble(&ok);
    return ok ? v : std::numeric_limits<double>::quiet_NaN();
}

static QFrame* wrapPreview(const QString& title, QLabel* lbl)
{
    auto* box = new QFrame;
    box->setFrameStyle(QFrame::StyledPanel | QFrame::Sunken);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(4, 4, 4, 4);
    auto* t = new QLabel("<b>" + title + "</b>");
    t->setAlignment(Qt::AlignCenter);
    v->addWidget(t);
    lbl->setAlignment(Qt::AlignCenter);
    lbl->setMinimumSize(320, 320);
    lbl->setStyleSheet("color: gray;");
    v->addWidget(lbl, 1);
    return box;
}

QWidget* LightcurveFetchDialog::buildPreviewsTab()
{
    auto* page = new QWidget;
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(8, 8, 8, 8);
    root->setSpacing(6);

    _previewTitle = new QLabel;
    _previewTitle->setAlignment(Qt::AlignCenter);
    _previewTitle->setStyleSheet("font-size: 16px; font-weight: bold;");
    root->addWidget(_previewTitle);

    // The TESS CROWDSAP value is folded into this description line (see
    // refreshPreviewsTab) so the layout is identical for every preview and the
    // image area never changes size between frames. RichText lets us colour it.
    _previewDesc = new QLabel;
    _previewDesc->setTextFormat(Qt::RichText);
    _previewDesc->setAlignment(Qt::AlignCenter);
    _previewDesc->setWordWrap(true);
    _previewDesc->setStyleSheet("color: gray;");
    root->addWidget(_previewDesc);

    _previewImage = new QLabel;
    _previewImage->setAlignment(Qt::AlignCenter);
    _previewImage->setMinimumSize(480, 480);
    _previewImage->setStyleSheet("color: gray;");
    // Re-fit the pixmap whenever the label's geometry settles, so every preview
    // (and dialog resize) stays fitted to the final geometry.
    _previewImage->installEventFilter(this);
    root->addWidget(_previewImage, 1);

    auto* nav = new QHBoxLayout;
    _prevPreviewBtn = new QPushButton(tr("Previous"));
    _nextPreviewBtn = new QPushButton(tr("Next"));
    UiIcons::apply(_prevPreviewBtn, UiIcons::Role::NavigatePrev);
    UiIcons::applyTrailing(_nextPreviewBtn, UiIcons::Role::NavigateNext);
    connect(_prevPreviewBtn, &QPushButton::clicked, this, [this]{ stepPreview(-1); });
    connect(_nextPreviewBtn, &QPushButton::clicked, this, [this]{ stepPreview(+1); });
    nav->addStretch();
    nav->addWidget(_prevPreviewBtn);
    nav->addWidget(_nextPreviewBtn);
    nav->addStretch();
    root->addLayout(nav);

    // Left / Right arrow keys, scoped to this tab only.
    auto* leftSC  = new QShortcut(QKeySequence(Qt::Key_Left),  page);
    auto* rightSC = new QShortcut(QKeySequence(Qt::Key_Right), page);
    leftSC ->setContext(Qt::WidgetWithChildrenShortcut);
    rightSC->setContext(Qt::WidgetWithChildrenShortcut);
    connect(leftSC,  &QShortcut::activated, this, [this]{ stepPreview(-1); });
    connect(rightSC, &QShortcut::activated, this, [this]{ stepPreview(+1); });

    _previewIndex = 0;
    refreshPreviewsTab();
    return page;
}

void LightcurveFetchDialog::stepPreview(int delta)
{
    const int n = previewEntries().size();
    if (n <= 0) return;
    _previewIndex = ((_previewIndex + delta) % n + n) % n;
    refreshPreviewsTab();
}


void LightcurveFetchDialog::refreshPreviewsTab()
{
    if (!_previewImage) return;   // tab not built yet

    const auto& entries = previewEntries();
    if (_previewIndex < 0 || _previewIndex >= entries.size()) _previewIndex = 0;
    const auto& e = entries[_previewIndex];

    _previewTitle->setText(QString("%1   (%2 / %3)")
        .arg(e.title).arg(_previewIndex + 1).arg(entries.size()));

    // For TESS, append the CROWDSAP value to the description line rather than
    // using a separate row, so the image area stays the same size across all
    // previews (an extra row would otherwise squeeze the TESS frame).
    const bool isTess = (e.filename == QLatin1String("tess_preview.png"));
    QString desc = e.description;
    if (isTess) {
        const double v = _star->getTessCrowdsap();
        if (Star::isSet(v)) {
            QString interp = "uncontaminated";
            QString color  = "#7dbd5e";
            if      (v < 0.5)  { interp = "heavily contaminated";  color = "#c46060"; }
            else if (v < 0.8)  { interp = "contaminated";          color = "#dca84d"; }
            else if (v < 0.95) { interp = "slightly contaminated"; color = "#dca84d"; }
            desc += QString("&nbsp;&nbsp;&middot;&nbsp;&nbsp;"
                            "<b>CROWDSAP</b> = "
                            "<span style=\"color:%1;font-weight:bold;\">%2</span> "
                            "(%3)")
                        .arg(color).arg(v, 0, 'f', 3).arg(interp);
        } else {
            desc += "&nbsp;&nbsp;&middot;&nbsp;&nbsp;CROWDSAP not available";
        }
    }
    _previewDesc->setText(desc);

    const QString path = previewPath(e.filename);
    QPixmap pm;
    if (QFileInfo::exists(path) && pm.load(path) && !pm.isNull()) {
        _previewPixmap = pm;
        _previewImage->setToolTip(path);
        _previewImage->setStyleSheet("");
        rescalePreviewImage();
    } else {
        _previewPixmap = QPixmap();
        _previewImage->clear();
        _previewImage->setText(QString("(no %1 preview yet)").arg(e.title));
        _previewImage->setToolTip(QString());
        _previewImage->setStyleSheet("color: gray;");
    }
}

void LightcurveFetchDialog::rescalePreviewImage()
{
    if (!_previewImage || _previewPixmap.isNull()) return;

    const QSize tgt = (_previewImage->size().isValid() && _previewImage->width() > 64)
                        ? _previewImage->size()
                        : QSize(640, 640);
    _previewImage->setPixmap(
        _previewPixmap.scaled(tgt, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

bool LightcurveFetchDialog::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == _previewImage && event->type() == QEvent::Resize)
        rescalePreviewImage();
    return QDialog::eventFilter(watched, event);
}

void LightcurveFetchDialog::refreshViewerSourceCombo()
{
    if (!_viewerSourceCombo) return;
    const QString prev = _viewerSourceCombo->currentText();
    QSignalBlocker b(_viewerSourceCombo);
    _viewerSourceCombo->clear();

    auto phot = _star ? _star->getPhotometry() : nullptr;
    if (phot) {
        auto sources = phot->getLightcurveSources();
        std::sort(sources.begin(), sources.end());
        for (const auto& src : sources)
            _viewerSourceCombo->addItem(src);
    }
    if (!prev.isEmpty()) {
        const int idx = _viewerSourceCombo->findText(prev);
        if (idx >= 0) _viewerSourceCombo->setCurrentIndex(idx);
    }
    const bool any = _viewerSourceCombo->count() > 0;
    _deleteLcBtn->setEnabled(any);
    _recomputeBjdBtn->setEnabled(any);

    refreshViewerMetaInfo();
}

namespace {

QString bjdToUtcString(double jd)
{
    const qint64 msecs = qint64((jd - 2440587.5) * 86400000.0);
    const QDateTime dt = QDateTime::fromMSecsSinceEpoch(msecs, QTimeZone::utc());
    return dt.toString(QStringLiteral("yyyy-MM-dd"));
}

/// Median spacing (seconds) between consecutive sorted BJD values.
double medianSpacingSeconds(QVector<double>& bjds)
{
    if (bjds.size() < 2) return std::nan("");
    std::sort(bjds.begin(), bjds.end());
    QVector<double> dt;
    dt.reserve(bjds.size() - 1);
    for (int i = 1; i < bjds.size(); ++i) {
        const double d = bjds[i] - bjds[i - 1];
        if (d > 0.0) dt.push_back(d);
    }
    if (dt.isEmpty()) return std::nan("");
    std::nth_element(dt.begin(), dt.begin() + dt.size() / 2, dt.end());
    return dt[dt.size() / 2] * 86400.0;
}

} // anon

void LightcurveFetchDialog::refreshViewerMetaInfo()
{
    if (!_viewerMetaLayout) return;

    // Clear all sections (keep the trailing stretch item).
    while (_viewerMetaLayout->count() > 1) {
        QLayoutItem* it = _viewerMetaLayout->takeAt(0);
        if (it->widget()) it->widget()->deleteLater();
        delete it;
    }

    auto phot = _star ? _star->getPhotometry() : nullptr;
    if (!phot) return;

    auto sources = phot->getLightcurveSources();
    std::sort(sources.begin(), sources.end());

    int insertPos = 0;
    for (const auto& source : sources) {
        const auto pts = phot->getLightcurve(source);

        auto* box = new QGroupBox(source);
        auto* lay = new QVBoxLayout(box);
        lay->setContentsMargins(8, 4, 8, 6);
        lay->setSpacing(2);

        auto addLine = [&](const QString& html) {
            auto* l = new QLabel(html);
            l->setWordWrap(true);
            l->setTextInteractionFlags(Qt::TextSelectableByMouse);
            lay->addWidget(l);
        };

        if (pts.empty()) {
            addLine(tr("<i>No points loaded.</i>"));
            _viewerMetaLayout->insertWidget(insertPos++, box);
            continue;
        }

        // Point count + filter breakdown
        QMap<QString, int> filterCounts;
        for (const auto& pt : pts)
            ++filterCounts[pt.filter];

        addLine(tr("<b>%L1</b> points").arg(qint64(pts.size())));

        const bool hasFilters =
            filterCounts.size() > 1 ||
            (!filterCounts.isEmpty() && !filterCounts.firstKey().isEmpty() &&
             filterCounts.firstKey() != source);
        if (hasFilters) {
            QStringList parts;
            for (auto it = filterCounts.cbegin(); it != filterCounts.cend(); ++it)
                parts << tr("%1 (%L2)").arg(it.key().isEmpty()
                                            ? tr("unspecified") : it.key())
                                       .arg(it.value());
            addLine(tr("Filters: %1").arg(parts.join(QStringLiteral(", "))));
        }

        // Date range (BJD + human readable)
        double minBjd = std::numeric_limits<double>::infinity();
        double maxBjd = -std::numeric_limits<double>::infinity();
        for (const auto& pt : pts) {
            const auto b = pt.time.bjd();
            if (!b.has_value() || *b <= 0.0) continue;
            minBjd = std::min(minBjd, *b);
            maxBjd = std::max(maxBjd, *b);
        }
        if (std::isfinite(minBjd) && std::isfinite(maxBjd)) {
            addLine(tr("BJD %1 – %2")
                        .arg(minBjd, 0, 'f', 3).arg(maxBjd, 0, 'f', 3));
            addLine(tr("%1 – %2 (%3 d)")
                        .arg(bjdToUtcString(minBjd), bjdToUtcString(maxBjd))
                        .arg(maxBjd - minBjd, 0, 'f', 1));
        } else {
            addLine(tr("<i>No BJD values (recompute?)</i>"));
        }

        // TESS: sector breakdown with cadence classification
        if (source.contains(QStringLiteral("TESS"), Qt::CaseInsensitive)) {
            QMap<int, QVector<double>> bySector;   // sorted by sector number
            int unmatched = 0;
            for (const auto& pt : pts) {
                const auto b = pt.time.bjd();
                if (!b.has_value() || *b <= 0.0) continue;
                const int sec = TessSectors::sectorForJd(*b);
                if (sec > 0) bySector[sec].push_back(*b);
                else         ++unmatched;
            }
            if (!bySector.isEmpty()) {
                QStringList secLines;
                for (auto it = bySector.begin(); it != bySector.end(); ++it) {
                    const double dt = medianSpacingSeconds(it.value());
                    secLines << tr("S%1: %L2 pts, %3")
                                    .arg(it.key()).arg(it.value().size())
                                    .arg(TessSectors::cadenceLabel(dt, it.key()));
                }
                addLine(tr("Sectors:<br>&nbsp;&nbsp;%1")
                            .arg(secLines.join(QStringLiteral("<br>&nbsp;&nbsp;"))));
            }
            if (unmatched > 0)
                addLine(tr("<i>%L1 points outside known sectors</i>").arg(unmatched));
        }

        _viewerMetaLayout->insertWidget(insertPos++, box);
    }
}

void LightcurveFetchDialog::onDeleteLightcurveClicked()
{
    if (!_viewerSourceCombo || _viewerSourceCombo->count() == 0) return;
    const QString source = _viewerSourceCombo->currentText();
    auto phot = _star->getPhotometry();
    if (!phot) return;

    const auto pts = phot->getLightcurve(source);
    const auto ret = QMessageBox::warning(this, tr("Delete lightcurve"),
        tr("Delete the <b>%1</b> lightcurve for this star?\n\n"
           "This will remove %2 points and cannot be undone.")
            .arg(source.toHtmlEscaped()).arg(int(pts.size())),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes) return;

    _dbm->removeLightcurve(_star->getId(), source);

    LOG_INFO("LCViewer",
        QString("Deleted lightcurve \"%1\" for star %2 (%3 points)")
            .arg(source).arg(_star->getId()).arg(int(pts.size())));

    if (_lcPanel)          _lcPanel->refresh();
    if (_fitLcPanel)       _fitLcPanel->refresh();
    if (_periodogramPanel) pushSeriesIntoPanel();
    refreshViewerSourceCombo();
}

void LightcurveFetchDialog::onRecomputeBjdClicked()
{
    if (!_viewerSourceCombo || _viewerSourceCombo->count() == 0) return;
    const QString source = _viewerSourceCombo->currentText();
    auto phot = _star->getPhotometry();
    if (!phot) return;

    auto pts = phot->getLightcurve(source);
    if (pts.empty()) {
        QMessageBox::information(this, tr("Recompute BJD"),
            tr("No points loaded for source \"%1\".").arg(source));
        return;
    }
    if (!Star::isSet(_star->getRa()) || !Star::isSet(_star->getDec())) {
        QMessageBox::warning(this, tr("Recompute BJD"),
            tr("Star has no RA/Dec - cannot compute BJD."));
        return;
    }
    auto inst = _dbm ? _dbm->resolveInstrumentString(source) : nullptr;

    // Build a small modal dialog with a time-scale selector.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Recompute BJD - %1").arg(source));
    auto* v = new QVBoxLayout(&dlg);

    auto* hint = new QLabel(tr(
        "Re-interpret the native timestamps of <b>%1</b> (%2 points) as the "
        "time scale chosen below, then recompute BJD for every point using "
        "the instrument's location and the star's coordinates.")
            .arg(source).arg(int(pts.size())));
    hint->setWordWrap(true);
    v->addWidget(hint);

    auto* form = new QFormLayout;
    auto* scaleCombo = new QComboBox;
    struct E { TimeScale ts; const char* label; };
    static const QList<E> scales = {
        { TimeScale::JD,      "JD" },
        { TimeScale::MJD,     "MJD" },
        { TimeScale::BJD,     "BJD" },
        { TimeScale::HJD,     "HJD" },
        { TimeScale::BTJD,    "BTJD (TESS, BJD − 2457000)" },
        { TimeScale::BKJD,    "BKJD (Kepler, BJD − 2454833)" },
        { TimeScale::GaiaTCB, "Gaia TCB (BJD − 2455197.5)" },
    };
    const TimeScale current = pts.front().time.nativeScale();
    int preselect = 1; // MJD default
    for (int i = 0; i < scales.size(); ++i) {
        scaleCombo->addItem(scales[i].label, int(scales[i].ts));
        if (scales[i].ts == current) preselect = i;
    }
    scaleCombo->setCurrentIndex(preselect);
    form->addRow(tr("Original time scale:"), scaleCombo);
    v->addLayout(form);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(bb);
    connect(bb, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    const TimeScale chosen =
        static_cast<TimeScale>(scaleCombo->currentData().toInt());

    const bool nativeIsBjd =
        (chosen == TimeScale::BJD  ||
         chosen == TimeScale::BTJD ||
         chosen == TimeScale::BKJD ||
         chosen == TimeScale::GaiaTCB);

    if (!nativeIsBjd && !inst) {
        QMessageBox::warning(this, tr("Recompute BJD"),
            tr("No instrument record matches source \"%1\" - cannot "
               "compute BJD from a non-barycentric scale.").arg(source));
        return;
    }

    // Rebuild the Time object on every point so any stale (e.g. zeroed)
    // _bjd cache is dropped, then force the lazy computation now so it
    // gets serialised to disk by saveLightcurveForStar.
    int recomputed = 0;
    for (auto& pt : pts) {
        const double nv  = pt.time.nativeValue();
        const double exp = pt.time.exposureTimeSec();
        pt.time = Time(nv, chosen);
        if (exp >= 0.0) pt.time.setExposureTime(exp);

        if (!nativeIsBjd)
            pt.time.setAutoConvertInfo(inst, _star->getRa(), _star->getDec());

        if (pt.time.bjd().has_value()) ++recomputed;
    }

    // Replace outright - same points, but their Time objects have been rebuilt.
    phot->addLightcurve(source, pts);
    if (_dbm && !_dbm->saveLightcurveForStar(_star->getId(), source, phot.get())) {
        QMessageBox::warning(this, tr("Recompute BJD"),
            tr("Recomputed in memory but failed to persist to the database."));
    }

    LOG_INFO("LCViewer",
        QString("Recomputed BJD for %1 (%2/%3 points) using scale %4")
            .arg(source).arg(recomputed).arg(int(pts.size()))
            .arg(Time::scaleToString(chosen)));

    if (_lcPanel)          _lcPanel->refresh();
    if (_fitLcPanel)       _fitLcPanel->refresh();
    if (_periodogramPanel) pushSeriesIntoPanel();
    refreshViewerMetaInfo();

    QMessageBox::information(this, tr("Recompute BJD"),
        tr("Recomputed BJD for %1 / %2 points of \"%3\".")
            .arg(recomputed).arg(int(pts.size())).arg(source));
}

void LightcurveFetchDialog::refreshFitPeriodList()
{
    if (!_fitPeriodList) return;
    const double prevP = selectedFitPeriod();

    QSignalBlocker b(_fitPeriodList);
    _fitPeriodList->clear();

    QVector<double> added;
    auto isDuplicate = [&](double P) {
        for (double Q : added)
            if (P > 0 && Q > 0 && std::abs(Q - P) / P < 1e-6) return true;
        return false;
    };

    auto addItem = [&](double P, double sigma, const QString& source){
        if (P <= 0 || isDuplicate(P)) return;
        QString txt = (sigma > 0)
            ? QString("P = %1 ± %2 d   [%3]")
                .arg(P, 0, 'g', 8).arg(sigma, 0, 'g', 2).arg(source)
            : QString("P = %1 d   [%2]")
                .arg(P, 0, 'g', 8).arg(source);
        auto* it = new QListWidgetItem(txt);
        it->setData(Qt::UserRole, P);
        _fitPeriodList->addItem(it);
        added.append(P);
    };

    if (_star) {
        if (auto rv = _star->getRVCurve()) {
            if (auto bf = rv->getBestFit(); bf && bf->getPeriod() > 0)
                addItem(bf->getPeriod(), bf->getPeriodError(), "RV best fit");
        }
    }
    if (_star && Star::isSet(_star->getPhotPeriod())) {
        addItem(_star->getPhotPeriod(),
                Star::isSet(_star->getPhotEPeriod()) ? _star->getPhotEPeriod() : 0.0,
                "Phot best fit");
    }
    for (const auto& pk : _peaks)
        addItem(pk.period, pk.periodError, pk.sourceLabel);

    int rowToSelect = 0;
    if (prevP > 0) {
        for (int i = 0; i < _fitPeriodList->count(); ++i) {
            const double P = _fitPeriodList->item(i)->data(Qt::UserRole).toDouble();
            if (P > 0 && std::abs(P - prevP) / prevP < 1e-9) {
                rowToSelect = i; break;
            }
        }
    }
    if (_fitPeriodList->count() > 0)
        _fitPeriodList->setCurrentRow(rowToSelect);

    onFitPeriodSelectionChanged();
}

double LightcurveFetchDialog::selectedFitPeriod() const
{
    if (!_fitPeriodList) return 0.0;
    auto* it = _fitPeriodList->currentItem();
    if (!it) return 0.0;
    return it->data(Qt::UserRole).toDouble();
}

void LightcurveFetchDialog::onFitPeriodSelectionChanged()
{
    const double P = selectedFitPeriod();

    if (_fitLcPanel) {
        if (P > 0) {
            _fitLcPanel->setFoldPeriod(P);
            _fitLcPanel->setFolded(true);
            if (_fitBinsSpin)
                _fitLcPanel->setUniformFoldedBins(_fitBinsSpin->value());
        } else {
            _fitLcPanel->setFolded(false);
        }
    }

    const bool any = (P > 0);
    if (_fitRunBtn) {
        _fitRunBtn->setEnabled(any);
        _fitRunBtn->setText(any
            ? tr("Fit LC  (P = %1 d, %2 bins)")
                .arg(P, 0, 'g', 6)
                .arg(_fitBinsSpin ? _fitBinsSpin->value() : 0)
            : tr("Fit LC"));
    }

    if (_fitInfoLabel) {
        if (!any) {
            _fitInfoLabel->setText(tr(
                "Select a period to fold and bin.\n"
                "Add peaks in the Periodogram tab if none are listed."));
        } else {
            const auto pts = computeBinnedFitLightcurve();
            const int nReq = _fitBinsSpin ? _fitBinsSpin->value() : 0;
            _fitInfoLabel->setText(tr(
                "Folding on P = %1 d.\n"
                "%2 / %3 phase bins will carry data points.")
                .arg(P, 0, 'g', 8)
                .arg(pts.size())
                .arg(nReq));
        }
    }
}

void LightcurveFetchDialog::onFitBinsChanged()
{
    if (_fitLcPanel && _fitBinsSpin)
        _fitLcPanel->setUniformFoldedBins(_fitBinsSpin->value());
    onFitPeriodSelectionChanged();
}

LCBinning::Combiner LightcurveFetchDialog::fitBinCombiner() const {
    if (!_fitCombinerCombo)
        return LCBinning::Combiner::WeightedMean;
    return static_cast<LCBinning::Combiner>(
        _fitCombinerCombo->currentData().toInt());
}

std::vector<LCBinning::RawPoint>
LightcurveFetchDialog::collectRawFitPoints() const {
    std::vector<LCBinning::RawPoint> out;
    if (!_fitLcPanel)
        return out;

    const QString wantSource =
        _fitSourceCombo ? _fitSourceCombo->currentText() : QString();
    const QString wantFilter =
        _fitFilterCombo ? _fitFilterCombo->currentData().toString() : QString();

    for (const auto &s : _fitLcPanel->seriesData(false)) {
        if (!wantSource.isEmpty() && s.source != wantSource)
            continue;
        if (!wantFilter.isEmpty() && s.filter != wantFilter)
            continue;

        // Every series is put on a common scale by dividing through its own
        // median, so series from different reductions can share a fit.
        QVector<double> sample;
        sample.reserve(s.y.size());
        for (double v : s.y)
            if (std::isfinite(v))
                sample.append(v);
        double med = 1.0;
        if (!sample.isEmpty()) {
            std::sort(sample.begin(), sample.end());
            med = sample[sample.size() / 2];
            if (std::abs(med) < 1e-30)
                med = 1.0;
        }

        for (int i = 0; i < s.t.size(); ++i) {
            if (!std::isfinite(s.t[i]) || !std::isfinite(s.y[i]))
                continue;
            LCBinning::RawPoint p;
            p.time      = s.t[i];
            p.flux      = s.y[i] / med;
            p.fluxError = (std::isfinite(s.e[i]) && s.e[i] > 0.0)
                              ? s.e[i] / std::abs(med)
                              : 0.0;
            out.push_back(p);
        }
    }
    return out;
}

QVector<LightcurveFetchDialog::BinnedFitPoint>
LightcurveFetchDialog::computeBinnedFitLightcurve() const {
    QVector<BinnedFitPoint> out;
    const double            P = selectedFitPeriod();
    const int nBins = _fitBinsSpin ? _fitBinsSpin->value() : 0;
    if (P <= 0 || nBins <= 0)
        return out;

    const auto binned = LCBinning::fold(collectRawFitPoints(), P, nBins,
                                        fitBinCombiner());
    out.reserve(int(binned.points.size()));
    for (const auto &b : binned.points) {
        BinnedFitPoint p;
        p.phase      = b.phase;
        p.deltaPhase = b.dPhase;
        p.flux       = b.flux;
        p.fluxError  = b.fluxError;
        p.weight     = b.weight;
        p.factor     = b.factor;
        out.append(p);
    }
    return out;
}

bool LightcurveFetchDialog::writeBinnedFitLightcurve(const QString& path) const
{
    const auto pts = computeBinnedFitLightcurve();
    if (pts.isEmpty()) return false;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    QTextStream s(&f);
    s.setRealNumberNotation(QTextStream::SmartNotation);
    s.setRealNumberPrecision(17);
    for (const auto& p : pts) {
        s << p.phase      << ' '
          << p.deltaPhase << ' '
          << p.flux       << ' '
          << p.fluxError  << ' '
          << p.weight     << ' '
          << p.factor     << '\n';
    }
    return true;
}

void LightcurveFetchDialog::onFitRunClicked() {
    const double P = selectedFitPeriod();
    if (P <= 0)
        return;
    const auto pts = computeBinnedFitLightcurve();
    if (pts.isEmpty()) {
        QMessageBox::warning(this, tr("Fit LC"),
                             tr("No usable binned data points for the current "
                                "source / filter / period."));
        return;
    }

    const QString source =
        _fitSourceCombo ? _fitSourceCombo->currentText() : QString("TESS");
    const QString filter =
        _fitFilterCombo ? _fitFilterCombo->currentData().toString() : QString();

    double pErr = 0.0;
    for (const auto &pk : _peaks)
        if (std::abs(pk.period - P) / P < 1e-9) {
            pErr = pk.periodError;
            break;
        }

    LCFitDialog::Inputs in;
    in.star             = _star;
    in.dbm              = _dbm;
    in.controller       = _controller;
    in.settings         = _controller ? _controller->settings() : nullptr;
    in.projectId        = _projectId;
    in.lightcurveSource = source;
    in.filter           = filter;
    in.wavelengthNm     = FilterWavelength::lookupNm(filter);
    in.period           = P;
    in.periodError      = pErr;
    // The raw samples travel with the binned ones so the fit dialog can clip
    // outliers where they actually live and re-bin what survives.
    in.rawPoints   = collectRawFitPoints();
    in.nBins       = _fitBinsSpin ? _fitBinsSpin->value() : 0;
    in.binCombiner = fitBinCombiner();
    in.binnedPoints.reserve(pts.size());
    for (const auto &bp : pts) {
        LCFitDataPoint d;
        d.phase     = bp.phase;
        d.dPhase    = bp.deltaPhase;
        d.flux      = bp.flux;
        d.fluxError = bp.fluxError;
        d.weight    = bp.weight;
        d.factor    = bp.factor;
        in.binnedPoints.push_back(d);
    }

    LCFitDialog dlg(in, this);
    dlg.exec();
    // Fits may have been saved regardless of how the dialog was closed
    // (persistFit mirrors them into the in-memory Photometry), so refresh
    // the existing-fits tree and the plots unconditionally.
    refreshExistingFitsTree();
    if (_fitLcPanel)
        _fitLcPanel->refresh();
    if (_lcPanel)
        _lcPanel->refresh();
}

void LightcurveFetchDialog::onAddRVPeriodClicked()
{
    if (!_star) return;
    auto rv = _star->getRVCurve();
    auto bf = rv ? rv->getBestFit() : nullptr;
    if (!bf || bf->getPeriod() <= 0) {
        QMessageBox::information(this, tr("Add RV period"),
            tr("No RV best-fit period available for this star."));
        return;
    }
    PeriodogramPanel::PeriodPeak pk;
    pk.period       = bf->getPeriod();
    pk.periodError  = bf->getPeriodError();
    pk.frequency    = (pk.period > 0) ? 1.0 / pk.period : 0.0;
    pk.power        = 0.0;
    pk.sourceLabel  = "RV best fit";
    addPeak(pk);
}

void LightcurveFetchDialog::onAddPhotPeriodClicked()
{
    if (!_star || !Star::isSet(_star->getPhotPeriod())) {
        QMessageBox::information(this, tr("Add photometric period"),
            tr("No stored photometric best-fit period for this star."));
        return;
    }
    PeriodogramPanel::PeriodPeak pk;
    pk.period      = _star->getPhotPeriod();
    pk.periodError = Star::isSet(_star->getPhotEPeriod())
                       ? _star->getPhotEPeriod() : 0.0;
    pk.frequency   = (pk.period > 0) ? 1.0 / pk.period : 0.0;
    pk.power       = 0.0;
    pk.sourceLabel = "Phot best fit";
    addPeak(pk);
}

void LightcurveFetchDialog::refreshExistingFitsTree() {
    if (!_existingFitsTree)
        return;
    _existingFitsTree->clear();
    _plotFitBtn->setEnabled(false);
    _setBestFitBtn->setEnabled(false);
    _deleteFitBtn->setEnabled(false);

    auto phot = _star ? _star->getPhotometry() : nullptr;
    if (!phot)
        return;

    auto sources = phot->getLightcurveSources();
    std::sort(sources.begin(), sources.end());

    for (const auto &src : sources) {
        auto allInSrc = phot->getLCFits(src);
        if (allInSrc.empty())
            continue;

        QSet<QString> filtSet;
        for (const auto &f : allInSrc)
            filtSet.insert(f->filter);
        QStringList filters(filtSet.begin(), filtSet.end());
        std::sort(filters.begin(), filters.end());

        auto *srcItem = new QTreeWidgetItem(_existingFitsTree);
        srcItem->setText(0, src);
        srcItem->setFirstColumnSpanned(true);
        QFont f = srcItem->font(0);
        f.setBold(true);
        srcItem->setFont(0, f);
        srcItem->setExpanded(true);

        for (const auto &filt : filters) {
            auto fits = phot->getLCFits(src, filt);
            if (fits.empty())
                continue;

            std::sort(fits.begin(), fits.end(),
                      [](const std::shared_ptr<LCFit> &a,
                         const std::shared_ptr<LCFit> &b) {
                          return a->creationDate > b->creationDate;
                      });

            auto *filtItem = new QTreeWidgetItem(srcItem);
            filtItem->setText(0, filt.isEmpty() ? tr("(unfiltered)")
                                                : tr("Filter: %1").arg(filt));
            filtItem->setFirstColumnSpanned(true);
            filtItem->setExpanded(true);

            for (const auto &fit : fits) {
                auto   *it = new QTreeWidgetItem(filtItem);
                QString lbl =
                    fit->label.isEmpty()
                        ? fit->creationDate.toString("yyyy-MM-dd hh:mm")
                        : fit->label;
                if (fit->isBestFit)
                    lbl = QString::fromUtf8("★ ") + lbl;
                it->setText(0, lbl);
                it->setText(1, QString::number(fit->period, 'g', 8));
                it->setText(2, fit->chi2 > 0
                                   ? QString::number(fit->chi2, 'g', 4)
                                   : "-");
                it->setData(0, Qt::UserRole, fit->getId());
                it->setData(0, Qt::UserRole + 1, src);
                it->setData(0, Qt::UserRole + 2, filt);
                if (fit->isBestFit) {
                    QFont bf = it->font(0);
                    bf.setBold(true);
                    it->setFont(0, bf);
                    it->setFont(1, bf);
                    it->setFont(2, bf);
                }
                it->setToolTip(
                    0,
                    tr("P = %1 d\nT₀ = %2 BJD\nχ² = %3\nRMS = %4\nCreated: %5")
                        .arg(fit->period, 0, 'g', 8)
                        .arg(fit->t0BJD, 0, 'g', 12)
                        .arg(fit->chi2, 0, 'g', 4)
                        .arg(fit->rms, 0, 'g', 4)
                        .arg(fit->creationDate.toString(Qt::ISODate)));
            }
        }
    }

    // Auto-select the first leaf fit so the buttons enable immediately and
    // the feature is discoverable.
    bool                    selected = false;
    QTreeWidgetItemIterator it(_existingFitsTree);
    while (*it) {
        if (!(*it)->data(0, Qt::UserRole).toString().isEmpty()) {
            _existingFitsTree->setCurrentItem(*it);
            selected = true;
            break;
        }
        ++it;
    }
    if (!selected) {
        if (_plotFitBtn)
            _plotFitBtn->setEnabled(false);
        if (_setBestFitBtn)
            _setBestFitBtn->setEnabled(false);
        if (_deleteFitBtn)
            _deleteFitBtn->setEnabled(false);
        updateSelectedFitDetails();
    }

    // Button enabling on selection change is handled by the connection made
    // once in buildFitTab(); re-connecting here (per refresh) would stack
    // duplicate connections, and Qt::UniqueConnection asserts on lambdas.
}

void LightcurveFetchDialog::updateSelectedFitDetails() {
    if (!_fitDetailsLabel)
        return;
    QString    src, filt;
    const auto fit = selectedExistingFit(&src, &filt);
    if (!fit) {
        _fitDetailsLabel->setText(tr("Select a fit to see its parameters."));
        return;
    }

    auto err = [](double e, double up, double down) -> QString {
        if (AsymErr::hasAsymmetric(up, down))
            return QString(" +%1 / −%2")
                .arg(QString::number(AsymErr::upOr(up, e), 'g', 3),
                     QString::number(AsymErr::downOr(down, e), 'g', 3));
        return e > 0 ? QString(" ± %1").arg(QString::number(e, 'g', 3))
                     : QString();
    };
    QString rows;
    auto    row = [&](const QString &name, double v, double e = 0.0,
                      double up = AsymErr::unset, double down = AsymErr::unset,
                      int prec = 6) {
        rows += QString("<tr><td>%1&nbsp;&nbsp;</td>"
                        "<td style='white-space:nowrap;'>%2%3</td></tr>")
                    .arg(name, QString::number(v, 'g', prec), err(e, up, down));
    };
    row(tr("q = M₂/M₁"), fit->q, fit->qError, fit->qErrorUp, fit->qErrorDown);
    row(tr("i [°]"), fit->inclination, fit->inclinationError,
        fit->inclinationErrorUp, fit->inclinationErrorDown);
    row(tr("r₁ (R₁/a)"), fit->r1, fit->r1Error, fit->r1ErrorUp,
        fit->r1ErrorDown);
    row(tr("r₂ (R₂/a)"), fit->r2, fit->r2Error, fit->r2ErrorUp,
        fit->r2ErrorDown);
    row(tr("v_scale [km/s]"), fit->velocityScale, fit->velocityScaleError,
        fit->velocityScaleErrorUp, fit->velocityScaleErrorDown);

    // ── Physical scale ────────────────────────────────────────────────
    //  r₁ and r₂ are fractions of the orbital separation, so on their own
    //  they say nothing about how big the stars actually are.  a follows
    //  from v_scale and the period for every fit, so R = r·a is always
    //  available — show it rather than making the reader do the algebra.
    if (fit->velocityScale > 0 && fit->period > 0) {
        const double aRsun = fit->velocityScale * fit->period *
                             LCFitPhysics::kDay2Sec / (2.0 * M_PI) /
                             LCFitPhysics::kRsunKm;
        // Products of independent quantities: relative errors in quadrature,
        // each side of an asymmetric interval propagated on its own.
        auto rel = [](double v, double e) {
            return (v > 0 && e > 0 && std::isfinite(e)) ? e / v : 0.0;
        };
        auto sideErr = [&](double R, double frac, double fracErr,
                           double vsErr, double pErr) {
            return R * std::sqrt(std::pow(rel(frac, fracErr), 2) +
                                 std::pow(rel(fit->velocityScale, vsErr), 2) +
                                 std::pow(rel(fit->period, pErr), 2));
        };
        const double vsUp = AsymErr::upOr(fit->velocityScaleErrorUp,
                                          fit->velocityScaleError);
        const double vsDn = AsymErr::downOr(fit->velocityScaleErrorDown,
                                            fit->velocityScaleError);
        const double pUp = AsymErr::upOr(fit->periodErrorUp, fit->periodError);
        const double pDn =
            AsymErr::downOr(fit->periodErrorDown, fit->periodError);

        auto derivedRow = [&](const QString &label, double frac, double fracErr,
                              double fracUp, double fracDown) {
            if (!(frac > 0))
                return;
            const double v = frac * aRsun;
            const double u =
                sideErr(v, frac, AsymErr::upOr(fracUp, fracErr), vsUp, pUp);
            const double d =
                sideErr(v, frac, AsymErr::downOr(fracDown, fracErr), vsDn, pDn);
            row(label, v, 0.5 * (u + d), u, d, 4);
        };
        // a itself is the frac = 1 case: no fractional-radius term.
        derivedRow(tr("a [R☉]"), 1.0, 0.0, AsymErr::unset, AsymErr::unset);
        derivedRow(tr("R₁ [R☉]"), fit->r1, fit->r1Error, fit->r1ErrorUp,
                   fit->r1ErrorDown);
        derivedRow(tr("R₂ [R☉]"), fit->r2, fit->r2Error, fit->r2ErrorUp,
                   fit->r2ErrorDown);
    }

    row(tr("T₁ [K]"), fit->t1, fit->t1Error, fit->t1ErrorUp, fit->t1ErrorDown);
    row(tr("T₂ [K]"), fit->t2, fit->t2Error, fit->t2ErrorUp, fit->t2ErrorDown);
    row(tr("P [d]"), fit->period, fit->periodError, fit->periodErrorUp,
        fit->periodErrorDown, 8);
    row(tr("T₀ [BJD]"), fit->t0BJD, fit->t0BJDError, fit->t0BJDErrorUp,
        fit->t0BJDErrorDown, 12);
    row(tr("χ²"), fit->chi2, 0.0, AsymErr::unset, AsymErr::unset, 4);
    row(tr("RMS"), fit->rms, 0.0, AsymErr::unset, AsymErr::unset, 4);

    const QString head =
        tr("<b>%1</b>%2<br><span style='color:gray;'>%3 / %4 · %5</span>")
            .arg(fit->label.isEmpty() ? fit->getId() : fit->label,
                 fit->isBestFit ? tr(" · ★ best") : QString(), src,
                 filt.isEmpty() ? tr("(unfiltered)") : filt,
                 fit->creationDate.toString("yyyy-MM-dd hh:mm"));
    _fitDetailsLabel->setText(head + "<table>" + rows + "</table>");
}

std::shared_ptr<LCFit>
LightcurveFetchDialog::selectedExistingFit(QString *outSource,
                                           QString *outFilter) const {
    if (!_existingFitsTree)
        return nullptr;
    auto *it = _existingFitsTree->currentItem();
    if (!it)
        return nullptr;
    const QString id   = it->data(0, Qt::UserRole).toString();
    const QString src  = it->data(0, Qt::UserRole + 1).toString();
    const QString filt = it->data(0, Qt::UserRole + 2).toString();
    if (id.isEmpty() || src.isEmpty())
        return nullptr;

    auto phot = _star ? _star->getPhotometry() : nullptr;
    if (!phot)
        return nullptr;
    // Search the whole source bucket - don't rely on filter equality semantics.
    for (const auto &f : phot->getLCFits(src)) {
        if (f->getId() == id) {
            if (outSource)
                *outSource = src;
            if (outFilter)
                *outFilter = filt;
            return f;
        }
    }
    return nullptr;
}

void LightcurveFetchDialog::onPlotExistingFitClicked() {
    QString src, filt;
    auto    fit = selectedExistingFit(&src, &filt);
    if (!fit || fit->period <= 0 || !_lcPanel)
        return;
    _lcPanel->setPreviewFit(src, filt, fit);
    _lcPanel->setFoldPeriod(fit->period, fit->t0BJD);
    _lcPanel->setFolded(true);
    _lcPanel->refresh();
    _tabs->setCurrentIndex(0);
}

void LightcurveFetchDialog::onSetSelectedAsBestClicked() {
    QString src, filt;
    auto    fit = selectedExistingFit(&src, &filt);
    if (!fit || !_dbm || !_star)
        return;
    if (!_dbm->setBestLCFit(_star->getId(), src, filt, fit->getId())) {
        QMessageBox::warning(
            this, tr("Set best fit"),
            tr("Failed to update best-fit selection in the database."));
        return;
    }
    if (auto phot = _star->getPhotometry()) {
        for (const auto &f : phot->getLCFits(src, filt))
            f->isBestFit = (f->getId() == fit->getId());
    }
    if (_lcPanel)
        _lcPanel->clearPreviewFit();
    if (_fitLcPanel)
        _fitLcPanel->clearPreviewFit();
    refreshExistingFitsTree();
    if (_lcPanel)
        _lcPanel->refresh();
    if (_fitLcPanel)
        _fitLcPanel->refresh();
    LOG_INFO("LCFit", QString("Set best LC fit for %1/%2/%3 → %4")
                          .arg(_star->getId(), src, filt.isEmpty() ? "-" : filt,
                               fit->getId()));
}

void LightcurveFetchDialog::onDeleteSelectedFitClicked() {
    QString src, filt;
    auto    fit = selectedExistingFit(&src, &filt);
    if (!fit || !_dbm || !_star)
        return;

    const auto ret = QMessageBox::warning(
        this, tr("Delete fit"),
        tr("Delete this LC fit?\n\n%1\n\nThis cannot be undone.")
            .arg(fit->label.isEmpty() ? fit->getId() : fit->label),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret != QMessageBox::Yes)
        return;

    if (!_dbm->deleteLCFit(fit->getId())) {
        QMessageBox::warning(this, tr("Delete fit"),
                             tr("Failed to delete the fit from the database."));
        return;
    }
    if (auto phot = _star->getPhotometry())
        phot->removeLCFit(src, fit->getId());

    refreshExistingFitsTree();
    if (_lcPanel)
        _lcPanel->refresh();
    if (_fitLcPanel)
        _fitLcPanel->refresh();
}