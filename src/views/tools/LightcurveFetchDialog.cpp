#include "LightcurveFetchDialog.h"
#include "views/panels/LCPanel.h"
#include "views/panels/PeriodogramPanel.h"
#include "views/panels/DetailPanel.h"
#include "models/Star.h"
#include "utils/Logger.h"
#include "utils/AppPaths.h"
#include "db/DatabaseManager.h"
#include "utils/AppSettings.h"
#include "controllers/ApplicationController.h"
#include "dialogs/ImportLightcurve.h"

#include <QPlainTextEdit>
#include <QCheckBox>
#include <QGroupBox>
#include <QProgressBar>
#include <QStandardPaths>
#include <QShortcut>
#include <QDir>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QSplitter>
#include <QTabWidget>
#include <QLabel>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QScrollArea>
#include <QListWidget>
#include <QListWidgetItem>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QToolButton>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QInputDialog>
#include <QMessageBox>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QTextCursor>
#include <QGridLayout>
#include <QPixmap>
#include <QFrame>
#include <QFileInfo>

#include <cmath>

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
    setWindowTitle(QString("Light Curves — %1").arg(
        _star->getAlias().isEmpty() ? _star->getSourceId() : _star->getAlias()));
    resize(1400, 850);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);

    _tabs = new QTabWidget;
    _tabs->addTab(buildViewerTab(),                          "Viewer");
    _periodogramTabIdx = _tabs->addTab(buildPeriodogramTab(), "Periodogram");
    _previewsTabIdx    = _tabs->addTab(buildPreviewsTab(),    "Previews");
    _tabs->addTab(buildFetchTab(),                           "Fetch");
    _tabs->addTab(buildFitTab(),                             "Fit");
    layout->addWidget(_tabs, 1);

    connect(_tabs, &QTabWidget::currentChanged, this, [this](int idx){
        if (idx == _periodogramTabIdx) onPeriodogramTabActivated();
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

    auto* tb = new QHBoxLayout;
    tb->setContentsMargins(6, 4, 6, 0);
    tb->addWidget(new QLabel(tr("Source:")));
    _viewerSourceCombo = new QComboBox;
    _viewerSourceCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    _viewerSourceCombo->setMinimumWidth(140);
    tb->addWidget(_viewerSourceCombo);

    _recomputeBjdBtn = new QPushButton(tr("Recompute BJD…"));
    _recomputeBjdBtn->setToolTip(tr(
        "Re-set the original time scale for this lightcurve and force "
        "BJD values to be recomputed from the native timestamps. "
        "Useful for fixing all-zero BJDs left over from a faulty import."));

    _deleteLcBtn = new QPushButton(tr("Delete…"));
    _deleteLcBtn->setToolTip(tr("Delete this lightcurve from the star."));

    tb->addWidget(_recomputeBjdBtn);
    tb->addWidget(_deleteLcBtn);
    tb->addStretch();
    root->addLayout(tb);

    DetailPanel::Context ctx;
    ctx.star       = _star;
    ctx.dbm        = _dbm;
    ctx.controller = _controller;
    ctx.projectId  = _projectId;
    _lcPanel = new LCPanel(ctx);
    root->addWidget(_lcPanel, 1);

    connect(_deleteLcBtn,     &QPushButton::clicked,
            this, &LightcurveFetchDialog::onDeleteLightcurveClicked);
    connect(_recomputeBjdBtn, &QPushButton::clicked,
            this, &LightcurveFetchDialog::onRecomputeBjdClicked);

    refreshViewerSourceCombo();
    return page;
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
    _fetchBusy = new QProgressBar;
    _fetchBusy->setRange(0, 0);
    _fetchBusy->setVisible(false);
    _fetchBusy->setMaximumHeight(18);
    _fetchStatus = new QLabel;
    _fetchStatus->setStyleSheet("color: gray;");
    btnRow->addWidget(_fetchBtn);
    btnRow->addWidget(_cancelFetch);
    btnRow->addWidget(_importCsvBtn);
    btnRow->addSpacing(8);
    btnRow->addWidget(_fetchBusy, 1);
    btnRow->addWidget(_fetchStatus, 2);
    root->addLayout(btnRow);

    connect(_fetchBtn,     &QPushButton::clicked, this, &LightcurveFetchDialog::onFetchClicked);
    connect(_cancelFetch,  &QPushButton::clicked, this, &LightcurveFetchDialog::onFetchCancelClicked);
    connect(_importCsvBtn, &QPushButton::clicked, this, &LightcurveFetchDialog::onImportCsvClicked);

    _fetchLog = new AnsiTerminalWidget;
    root->addWidget(_fetchLog, 1);

    AppSettings* settings = _controller ? _controller->settings() : nullptr;

    _fetcher = new LightcurveFetcher(this);
    _fetcher->setWorkingDir(QDir(AppPaths::root())
                            .absoluteFilePath("lcquery"));

    if (settings) {
        if (!settings->lcqueryPython().isEmpty())
            _fetcher->setPython(settings->lcqueryPython());
        if (!settings->lcqueryScript().isEmpty())
            _fetcher->setScript(settings->lcqueryScript());
        _fetcher->setAtlasToken(settings->atlasToken());
        _fetcher->setBlackgemScript(settings->blackgemScript());

        connect(settings, &AppSettings::lcquerySettingsChanged,
                this, [this, settings] {
            _fetcher->setPython(settings->lcqueryPython());
            _fetcher->setScript(settings->lcqueryScript());
            _fetcher->setAtlasToken(settings->atlasToken());
            _fetcher->setBlackgemScript(settings->blackgemScript());

            _fetchStatus->setStyleSheet("color: gray;");
            _fetchStatus->setText(tr("Re-checking…"));
            _fetchBtn->setEnabled(false);
            _fetcher->checkAvailableAsync();
        });
    }

    connect(_fetcher, &LightcurveFetcher::started,
            this, &LightcurveFetchDialog::onFetcherStarted);
    connect(_fetcher, &LightcurveFetcher::finished,
            this, &LightcurveFetchDialog::onFetcherFinished);
    connect(_fetcher, &LightcurveFetcher::failed,
            this, &LightcurveFetchDialog::onFetcherFailed);
    connect(_fetcher, &LightcurveFetcher::rawOutput,
            _fetchLog, QOverload<const QByteArray&>::of(&AnsiTerminalWidget::feed));

    connect(_fetcher, &LightcurveFetcher::availabilityChecked,
            this, [this](bool ok, const QString& msg) {
        if (ok) {
            _fetchStatus->setStyleSheet("color: gray;");
            _fetchStatus->setText(tr("Ready."));
            _fetchBtn->setEnabled(true);
        } else {
            _fetchStatus->setStyleSheet("color: #c46060;");
            _fetchStatus->setText(tr("⚠ %1").arg(msg.section('\n', 0, 0)));
            _fetchBtn->setEnabled(false);
            _fetchLog->feed("[availability] " + msg + '\n');
            _fetchLog->feed(tr("→ Open Settings → Lightcurve Fetching to configure.\n"));
        }
    });

    _fetchStatus->setStyleSheet("color: gray;");
    _fetchStatus->setText(tr("Checking Python setup…"));
    _fetchBtn->setEnabled(false);
    _fetcher->checkAvailableAsync();

    return page;
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
    connect(_periodogramPanel, &PeriodogramPanel::periodSelected,
            this, [this](double period) {
        if (period <= 0) return;
        // Estimate error from the currently-selected periodogram source if any,
        // else from any available result.
        Periodogram::Result res;
        QString label = _peakSourceCombo ? _peakSourceCombo->currentData().toString() : QString();
        if (!label.isEmpty()) res = _periodogramPanel->resultByLabel(label);
        if (!res.isValid()) {
            auto descs = _periodogramPanel->availableResults();
            if (!descs.isEmpty()) res = _periodogramPanel->resultByLabel(descs.front().label);
        }
        if (res.isValid())
            addPeak(PeriodogramPanel::estimatePeakAt(res, period));

        // Also fold the viewer immediately for the original UX.
        if (_lcPanel) {
            _lcPanel->setFoldPeriod(period);
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

    _minPSpin = new QDoubleSpinBox;
    _minPSpin->setDecimals(8); _minPSpin->setRange(0.0, 1e9);
    _minPSpin->setSpecialValueText("auto"); _minPSpin->setSuffix(" d");
    paramForm->addRow("Min P:", _minPSpin);

    _maxPSpin = new QDoubleSpinBox;
    _maxPSpin->setDecimals(6); _maxPSpin->setRange(0.0, 1e9);
    _maxPSpin->setSpecialValueText("auto"); _maxPSpin->setSuffix(" d");
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
    auto* peakBox = new QGroupBox("Period detection");
    auto* pLay    = new QVBoxLayout(peakBox);
    pLay->setContentsMargins(6, 6, 6, 6);
    pLay->setSpacing(4);

    auto* peakTop = new QHBoxLayout;
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
    _detectBtn    = new QPushButton("Detect peaks");
    _addManualBtn = new QPushButton("Add…");
    _removeBtn    = new QPushButton("Remove");
    _clearBtn     = new QPushButton("Clear");
    connect(_detectBtn,    &QPushButton::clicked, this, &LightcurveFetchDialog::onDetectPeaksClicked);
    connect(_addManualBtn, &QPushButton::clicked, this, &LightcurveFetchDialog::onAddManualPeakClicked);
    connect(_removeBtn,    &QPushButton::clicked, this, &LightcurveFetchDialog::onRemovePeakClicked);
    connect(_clearBtn,     &QPushButton::clicked, this, &LightcurveFetchDialog::onClearPeaksClicked);
    peakBtns->addWidget(_detectBtn);
    peakBtns->addWidget(_addManualBtn);
    peakBtns->addWidget(_removeBtn);
    peakBtns->addWidget(_clearBtn);
    pLay->addLayout(peakBtns);

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
    const auto src = _lcPanel->seriesData(false);   // exclude flagged
    QList<PeriodogramPanel::Series> conv;
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
            label += QString("  — skipped (<%1)").arg(_periodogramPanel->minPointsThreshold());

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
    double mn = 0, mx = 0;
    if (!_periodogramPanel->suggestAutoBounds(mn, mx)) {
        QMessageBox::warning(this, "Optimal",
            "Could not auto-resolve period bounds — check selection / min pts.");
        return;
    }
    if (_minPSpin->value() <= 0) _minPSpin->setValue(mn);
    if (_maxPSpin->value() <= 0) _maxPSpin->setValue(mx);
    _periodogramPanel->setGridParameters(_minPSpin->value(),
                                         _maxPSpin->value(),
                                         _nSampSpin->value(),
                                         _osSpin->value());
    const int nf = _periodogramPanel->suggestAutoNSamples();
    if (nf > 0) _nSampSpin->setValue(nf);
}

void LightcurveFetchDialog::onComputeClicked()
{
    if (!_periodogramPanel) return;
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

void LightcurveFetchDialog::onDetectPeaksClicked()
{
    if (!_periodogramPanel || !_peakSourceCombo) return;
    const QString label = _peakSourceCombo->currentData().toString();
    if (label.isEmpty()) {
        QMessageBox::information(this, "Detect peaks", "No periodograms available — compute first.");
        return;
    }
    const auto peaks = _periodogramPanel->detectPeaks(label, _peakCountSpin->value());
    for (const auto& pk : peaks) addPeak(pk);
}

void LightcurveFetchDialog::onAddManualPeakClicked()
{
    if (!_periodogramPanel) return;
    bool ok = false;
    const double p = QInputDialog::getDouble(this, "Add period",
        "Period (days):", 1.0, 1e-9, 1e9, 6, &ok);
    if (!ok || p <= 0) return;

    // Estimate error using the currently-selected periodogram if any.
    Periodogram::Result res;
    if (_peakSourceCombo) {
        const QString label = _peakSourceCombo->currentData().toString();
        if (!label.isEmpty()) res = _periodogramPanel->resultByLabel(label);
    }
    PeriodogramPanel::PeriodPeak pk;
    if (res.isValid()) pk = PeriodogramPanel::estimatePeakAt(res, p);
    else               { pk.period = p; pk.frequency = 1.0 / p; pk.sourceLabel = "manual"; }
    pk.sourceLabel += " (manual)";
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
    rebuildPeaksTable();   // display only — don't re-persist what we just read

    LOG_INFO("Periodogram",
        QString("Loaded %1 persisted peak(s) for star %2")
            .arg(_peaks.size()).arg(_star->getId()));
}

void LightcurveFetchDialog::persistPeaks()
{
    if (!_dbm || !_star) return;
    const QString json = PeriodogramPanel::peaksToJson(_peaks);
    _dbm->saveStarPhotPeaks(_star->getId(), json);
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
                     : QString("—"));
        setItem(2, QString::number(pk.power, 'g', 3));
        setItem(3, pk.sourceLabel);
    }
    const bool any = !_peaks.isEmpty();
    _foldBtn->setEnabled(any);
    _bestFitBtn->setEnabled(any);

    if (_periodogramPanel) _periodogramPanel->setMarkedPeaks(_peaks);   
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

    _star->setPhotPeriod(pk.period);
    _star->setPhotEPeriod(pk.periodError);
    _star->markSummaryDirty();                       // notify in-app listeners
    if (_dbm) _dbm->updateStar(_projectId, _star);   // persist to DB

    _bestFitLabel->setText(
        QString("Current best-fit P = %1 ± %2 d  (set just now)")
            .arg(pk.period,      0, 'g', 6)
            .arg(pk.periodError, 0, 'g', 2));

    LOG_INFO("Periodogram",
        QString("Saved best-fit photometric period for %1: P=%2 ±%3")
            .arg(_star->getId()).arg(pk.period).arg(pk.periodError));
}

// ──────────────────────────────────────────────────────────────────
// Fetch tab logic
// ──────────────────────────────────────────────────────────────────

void LightcurveFetchDialog::onFetchClicked()
{
    if (!_fetcher) return;

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

    opt.trimTess    = _trimTess->value();
    opt.ztfInnerArc = _ztfInner->value();
    opt.ztfOuterArc = _ztfOuter->value();

    _wasReattempt = _reattemptAll && _reattemptAll->isChecked();

    if (_wasReattempt) {
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
        if (ret != QMessageBox::Yes) {
            _wasReattempt = false;
            return;
        }

        const QString gaiaId = _star->getSourceId();
        const auto expected  = _fetcher->expectedOutputFiles(gaiaId);
        for (const QString& src : opt.sources) {
            const QString path = expected.value(src);
            if (!path.isEmpty() && QFile::exists(path)) {
                if (QFile::remove(path))
                    _fetchLog->feed(tr("[reattempt] removed %1\n").arg(path).toUtf8());
                else
                    _fetchLog->feed(tr("[reattempt] WARNING: could not remove %1\n").arg(path).toUtf8());
            }
        }

        // Per-source preview / aux files that we should also clear so stale
        // ones don't get displayed for sources whose fetch fails.
        const QString prevDir = previewDir();
        QStringList auxFiles;
        if (opt.sources.contains("TESS")) {
            auxFiles << "tess_preview.png" << "tess_crowdsap.txt";
        }
        if (opt.sources.contains("ZTF"))      auxFiles << "ztf_preview.png";
        for (const QString& f : auxFiles) {
            const QString p = QDir(prevDir).absoluteFilePath(f);
            if (QFile::exists(p)) QFile::remove(p);
        }
    }

    _fetchLog->clear();
    _fetchLog->feed(tr("Fetching: %1%2\n")
                    .arg(opt.sources.join(", "))
                    .arg(_wasReattempt ? tr("  (reattempt mode)") : QString()).toUtf8());

    const QString gaiaId = _star->getSourceId();
    _fetcher->start(gaiaId, opt);
}

void LightcurveFetchDialog::onFetchCancelClicked()
{
    if (_fetcher && _fetcher->isRunning()) {
        _fetchLog->feed(tr("— cancellation requested —\n"));
        _fetcher->cancel();
    }
}

void LightcurveFetchDialog::onFetcherStarted()
{
    _fetchBtn->setEnabled(false);
    _cancelFetch->setEnabled(true);
    _fetchBusy->setVisible(true);
    _fetchStatus->setStyleSheet("color: #dca84d;");
    _fetchStatus->setText(tr("Running…"));
}

void LightcurveFetchDialog::onFetcherLog(const QString& line)
{
    LOG_INFO("lightcurvequery", line);
}

void LightcurveFetchDialog::onFetcherFailed(const QString& reason)
{
    _fetchLog->feed("[fail] " + reason + '\n');
    _fetchStatus->setStyleSheet("color: #c46060;");
    _fetchStatus->setText(tr("Failed."));
    _fetchBtn->setEnabled(true);
    _cancelFetch->setEnabled(false);
    _fetchBusy->setVisible(false);
}

void LightcurveFetchDialog::onFetcherFinished(int code, bool ok)
{
    _fetchBusy->setVisible(false);
    _cancelFetch->setEnabled(false);
    _fetchBtn->setEnabled(true);

    auto status = [this](const QString& s) {
        _fetchLog->feed((s + '\n').toUtf8());
    };

    status(ok ? tr("— lightcurvequery finished successfully —")
              : tr("— lightcurvequery exited with code %1 —").arg(code));

    const QString gaiaId = _star->getSourceId();
    const auto expected  = _fetcher->expectedOutputFiles(gaiaId);

    auto phot = _star->getPhotometry();
    if (!phot) {
        phot = std::make_shared<Photometry>();
        _star->setPhotometry(phot);
    }

    const bool haveCoords =
        Star::isSet(_star->getRa()) && Star::isSet(_star->getDec());
    if (!haveCoords) {
        status(tr("Warning: star has no RA/Dec — BJDs will not be computed."));
    }

    QStringList imported, empty;
    int totalPoints = 0;

    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        const QString& source = it.key();
        const QString& path   = it.value();
        if (!QFile::exists(path)) continue;

        TimeScale ts = TimeScale::Unknown;
        auto pts = LightcurveFetcher::parseOutputFile(path, source, &ts);
        if (pts.empty()) { empty << source; continue; }

        const bool nativeIsBjd =
            (ts == TimeScale::BJD  ||
             ts == TimeScale::BTJD ||
             ts == TimeScale::BKJD ||
             ts == TimeScale::GaiaTCB);

        if (!nativeIsBjd && haveCoords && _dbm) {
            auto inst = _dbm->resolveInstrumentString(source);
            if (!inst) {
                status(tr("[%1] no instrument record found — BJD not computed")
                       .arg(source));
            } else {
                int converted = 0;
                for (auto& pt : pts) {
                    if (pt.time.hasBjd()) continue;
                    pt.time.setAutoConvertInfo(
                        inst, _star->getRa(), _star->getDec());
                    if (pt.time.bjd().has_value()) ++converted;
                }
                status(tr("[%1] computed BJD for %2 / %3 points")
                       .arg(source).arg(converted).arg(int(pts.size())));
            }
        }

        QString verb;
        if (_wasReattempt) {
            phot->addLightcurve(source, pts);
            verb = tr("replaced (reattempt)");
        } else {
            const auto result = phot->mergeLightcurve(source, pts);
            switch (result) {
                case Photometry::MergeResult::Identical: verb = tr("identical");  break;
                case Photometry::MergeResult::Replaced:  verb = tr("replaced");   break;
                case Photometry::MergeResult::Merged:    verb = tr("merged");     break;
                case Photometry::MergeResult::Added:     verb = tr("added");      break;
            }
        }
        status(tr("[%1] %2 points %3").arg(source).arg(pts.size()).arg(verb));

        if (_dbm && !_dbm->saveLightcurveForStar(_star->getId(), source, phot.get())) {
            status(tr("[%1] WARNING: failed to save to database").arg(source));
        }

        imported << QString("%1 (%2)").arg(source).arg(pts.size());
        totalPoints += int(pts.size());
    }

    {
        const QString crowdFile = previewPath("tess_crowdsap.txt");
        if (QFile::exists(crowdFile)) {
            const double v = readCrowdsapFile(crowdFile);
            if (!std::isnan(v)) {
                _star->setTessCrowdsap(v);
                if (_dbm) _dbm->saveStarTessCrowdsap(_star->getId(), v);
                status(tr("[TESS] CROWDSAP = %1").arg(v, 0, 'f', 3));
            } else {
                status(tr("[TESS] tess_crowdsap.txt present but could not be parsed"));
            }
        }
        refreshPreviewsTab();
    }

    if (!imported.isEmpty()) {
        _fetchStatus->setStyleSheet("color: #7dbd5e;");
        _fetchStatus->setText(tr("Imported %1 points: %2")
                              .arg(totalPoints).arg(imported.join(", ")));
        if (_lcPanel)          _lcPanel->refresh();
        if (_periodogramPanel) pushSeriesIntoPanel();
        refreshViewerSourceCombo();
    } else if (ok) {
        _fetchStatus->setStyleSheet("color: gray;");
        _fetchStatus->setText(tr("No data was produced."));
    } else {
        _fetchStatus->setStyleSheet("color: #c46060;");
        _fetchStatus->setText(tr("Failed (exit %1).").arg(code));
    }

    if (!empty.isEmpty())
        status(tr("(No data for: %1)").arg(empty.join(", ")));

    _wasReattempt = false;
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
    if (_periodogramPanel) pushSeriesIntoPanel();
    refreshViewerSourceCombo();

    _fetchStatus->setStyleSheet("color: #7dbd5e;");
    _fetchStatus->setText(tr("Imported %1 CSV points (%2).")
                          .arg(dlg.importedPoints().size())
                          .arg(dlg.sourceKey()));
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

    _previewDesc = new QLabel;
    _previewDesc->setAlignment(Qt::AlignCenter);
    _previewDesc->setWordWrap(true);
    _previewDesc->setStyleSheet("color: gray;");
    root->addWidget(_previewDesc);

    // CROWDSAP — only shown when the TESS image is on screen.
    _crowdsapTabLabel = new QLabel;
    _crowdsapTabLabel->setTextFormat(Qt::RichText);
    _crowdsapTabLabel->setAlignment(Qt::AlignCenter);
    _crowdsapTabLabel->setStyleSheet(
        "QLabel { font-size: 13px; padding: 4px 6px; }");
    root->addWidget(_crowdsapTabLabel);

    _previewImage = new QLabel;
    _previewImage->setAlignment(Qt::AlignCenter);
    _previewImage->setMinimumSize(480, 480);
    _previewImage->setStyleSheet("color: gray;");
    root->addWidget(_previewImage, 1);

    auto* nav = new QHBoxLayout;
    _prevPreviewBtn = new QPushButton("◀  Previous");
    _nextPreviewBtn = new QPushButton("Next  ▶");
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
    _previewDesc->setText(e.description);

    const QString path = previewPath(e.filename);
    QPixmap pm;
    if (QFileInfo::exists(path) && pm.load(path) && !pm.isNull()) {
        const QSize tgt = (_previewImage->size().isValid() && _previewImage->width() > 64)
                            ? _previewImage->size()
                            : QSize(640, 640);
        _previewImage->setPixmap(
            pm.scaled(tgt, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        _previewImage->setToolTip(path);
        _previewImage->setStyleSheet("");
    } else {
        _previewImage->clear();
        _previewImage->setText(QString("(no %1 preview yet)").arg(e.title));
        _previewImage->setToolTip(QString());
        _previewImage->setStyleSheet("color: gray;");
    }

    // CROWDSAP visible only when TESS preview is on screen.
    const bool isTess = (e.filename == QLatin1String("tess_preview.png"));
    _crowdsapTabLabel->setVisible(isTess);
    if (isTess) {
        const double v = _star->getTessCrowdsap();
        if (Star::isSet(v)) {
            QString interp = "uncontaminated";
            QString color  = "#7dbd5e";
            if      (v < 0.5)  { interp = "heavily contaminated";  color = "#c46060"; }
            else if (v < 0.8)  { interp = "contaminated";          color = "#dca84d"; }
            else if (v < 0.95) { interp = "slightly contaminated"; color = "#dca84d"; }
            _crowdsapTabLabel->setText(
                QString("TESS <b>CROWDSAP</b> = "
                        "<span style=\"color:%1;font-weight:bold;\">%2</span>  "
                        "<span style=\"color:gray;\">(%3)</span>  ")
                    .arg(color).arg(v, 0, 'f', 3).arg(interp));
        } else {
            _crowdsapTabLabel->setText(
                "<span style=\"color:gray;\">TESS CROWDSAP not available.</span>");
        }
    }
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
            tr("Star has no RA/Dec — cannot compute BJD."));
        return;
    }
    auto inst = _dbm ? _dbm->resolveInstrumentString(source) : nullptr;

    // Build a small modal dialog with a time-scale selector.
    QDialog dlg(this);
    dlg.setWindowTitle(tr("Recompute BJD — %1").arg(source));
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
            tr("No instrument record matches source \"%1\" — cannot "
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

    // Replace outright — same points, but their Time objects have been rebuilt.
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
    if (_periodogramPanel) pushSeriesIntoPanel();

    QMessageBox::information(this, tr("Recompute BJD"),
        tr("Recomputed BJD for %1 / %2 points of \"%3\".")
            .arg(recomputed).arg(int(pts.size())).arg(source));
}