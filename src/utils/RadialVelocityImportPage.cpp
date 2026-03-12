#include "RadialVelocityImportPage.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/RadialVelocity.h"
#include "utils/DatabaseManager.h"
#include "Logger.h"
#include "BackgroundTaskManager.h"
#include "StarImportWizard.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QRadioButton>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QProgressBar>
#include <QButtonGroup>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QTextStream>
#include <QApplication>
#include <QRegularExpression>
#include <QHeaderView>
#include <QScrollArea>
#include <QTimer>
#include <QUuid>

#include <cmath>
#include <algorithm>
#include <numeric>

// ════════════════════════════════════════════════════════════════
// RadialVelocityImportPage — Construction & UI
// ════════════════════════════════════════════════════════════════

RadialVelocityImportPage::RadialVelocityImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import Radial Velocity Data");
    setSubTitle("Import RV measurements, time series, and orbital fit parameters");
    setupUi();
}

void RadialVelocityImportPage::setupUi()
{
    // Outer layout holds only the scroll area
    QVBoxLayout* outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);

    QScrollArea* scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    QWidget* contentWidget = new QWidget;
    QVBoxLayout* mainLayout = new QVBoxLayout(contentWidget);

    // ── Mode selection ──────────────────────────────────────────
    QGroupBox* modeGroup = new QGroupBox("RV Data Source");
    QHBoxLayout* modeLayout = new QHBoxLayout;

    _fromFitsRadio    = new QRadioButton("From imported spectral fits");
    _fromFoldersRadio = new QRadioButton("From per-star RV folders");
    _fromTableRadio   = new QRadioButton("From a single RV table");
    _fromFitsRadio->setChecked(true);

    QButtonGroup* bg = new QButtonGroup(this);
    bg->addButton(_fromFitsRadio);
    bg->addButton(_fromFoldersRadio);
    bg->addButton(_fromTableRadio);

    modeLayout->addWidget(_fromFitsRadio);
    modeLayout->addWidget(_fromFoldersRadio);
    modeLayout->addWidget(_fromTableRadio);
    modeLayout->addStretch();
    modeGroup->setLayout(modeLayout);
    mainLayout->addWidget(modeGroup);

    connect(_fromFitsRadio, &QRadioButton::toggled,
            this, &RadialVelocityImportPage::onImportModeChanged);
    connect(_fromFoldersRadio, &QRadioButton::toggled,
            this, &RadialVelocityImportPage::onImportModeChanged);

    // ── Stacked pages ───────────────────────────────────────────
    _modeStack = new QStackedWidget;
    setupFromFitsPage();
    setupFromFoldersPage();
    setupFromTablePage();
    _modeStack->addWidget(_fromFitsPage);     // 0
    _modeStack->addWidget(_fromFoldersPage);  // 1
    _modeStack->addWidget(_fromTablePage);    // 2
    mainLayout->addWidget(_modeStack);
    _fromFoldersPage->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);

    // ── Fit parameters (shared, optional) ───────────────────────
    setupFitParamsGroup(mainLayout);

    // ── Preview ─────────────────────────────────────────────────
    QGroupBox* previewGroup = new QGroupBox("Preview");
    QVBoxLayout* previewLayout = new QVBoxLayout;

    _previewTree = new QTreeWidget;
    _previewTree->setHeaderLabels({
        "Star", "# RV Points", "RV Summary", "Status"
    });
    _previewTree->setAlternatingRowColors(true);
    _previewTree->setRootIsDecorated(true);
    _previewTree->header()->setStretchLastSection(true);
    _previewTree->setMaximumHeight(250);
    previewLayout->addWidget(_previewTree);

    _statusLabel = new QLabel(
        "Select a data source and extract/scan/process RV data.");
    _statusLabel->setWordWrap(true);
    previewLayout->addWidget(_statusLabel);

    previewGroup->setLayout(previewLayout);
    mainLayout->addWidget(previewGroup);
    scrollArea->setWidget(contentWidget);
    outerLayout->addWidget(scrollArea);
}

void RadialVelocityImportPage::setupFromFitsPage()
{
    _fromFitsPage = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(_fromFitsPage);

    QLabel* info = new QLabel(
        "Extract radial velocity values from all spectral fits available "
        "for stars in this project (both newly imported and previously saved). Each spectrum's fit provides one "
        "RV data point, linked to its source spectrum and timestamp.");
    info->setWordWrap(true);
    layout->addWidget(info);

    _bestFitOnlyCheck = new QCheckBox("Use only best fit per spectrum");
    _bestFitOnlyCheck->setChecked(true);
    layout->addWidget(_bestFitOnlyCheck);

    _skipZeroRVCheck = new QCheckBox("Skip fits with RV = 0 (likely unfitted)");
    _skipZeroRVCheck->setChecked(true);
    layout->addWidget(_skipZeroRVCheck);

    _extractFitsBtn = new QPushButton("Extract RV from Spectral Fits");
    connect(_extractFitsBtn, &QPushButton::clicked,
            this, &RadialVelocityImportPage::onExtractFromFits);
    layout->addWidget(_extractFitsBtn);
    layout->addStretch();
}

void RadialVelocityImportPage::setupFromFoldersPage()
{
    _fromFoldersPage = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(_fromFoldersPage);

    // Folder path
    QGroupBox* folderGroup = new QGroupBox("Root Folder");
    QVBoxLayout* fLayout = new QVBoxLayout;

    QHBoxLayout* pathLayout = new QHBoxLayout;
    _rootFolderEdit = new QLineEdit;
    _rootFolderEdit->setPlaceholderText(
        "Root folder containing per-star RV subfolders...");
    pathLayout->addWidget(_rootFolderEdit);
    QPushButton* browseBtn = new QPushButton("Browse...");
    connect(browseBtn, &QPushButton::clicked,
            this, &RadialVelocityImportPage::onBrowseRootFolder);
    pathLayout->addWidget(browseBtn);
    fLayout->addLayout(pathLayout);

    QHBoxLayout* namingLayout = new QHBoxLayout;
    namingLayout->addWidget(new QLabel("Folder names represent:"));
    _folderNamingCombo = new QComboBox;
    _folderNamingCombo->addItems({
        "Gaia Source ID", "Star Alias/Name", "RA_Dec (e.g. 123.456_+45.678)"
    });
    namingLayout->addWidget(_folderNamingCombo);
    namingLayout->addStretch();
    fLayout->addLayout(namingLayout);

    folderGroup->setLayout(fLayout);
    layout->addWidget(folderGroup);

    // File format options
    QGroupBox* formatGroup = new QGroupBox("RV Table File Format");
    QGridLayout* fmtLayout = new QGridLayout;
    int row = 0;

    fmtLayout->addWidget(new QLabel("Delimiter:"), row, 0);
    _folderDelimCombo = new QComboBox;
    _folderDelimCombo->addItems({
        "Auto-detect", "Comma (,)", "Tab", "Space", "Semicolon (;)"
    });
    fmtLayout->addWidget(_folderDelimCombo, row++, 1);

    _folderHeaderCheck = new QCheckBox("First row is header");
    _folderHeaderCheck->setChecked(true);
    fmtLayout->addWidget(_folderHeaderCheck, row++, 0, 1, 2);

    fmtLayout->addWidget(new QLabel("Timestamp column:"), row, 0);
    _folderTimeColCombo = new QComboBox;
    fmtLayout->addWidget(_folderTimeColCombo, row, 1);
    _folderTimeTypeCombo = new QComboBox;
    _folderTimeTypeCombo->addItems({"MJD", "BJD"});
    fmtLayout->addWidget(_folderTimeTypeCombo, row++, 2);

    fmtLayout->addWidget(new QLabel("RV column:"), row, 0);
    _folderRVColCombo = new QComboBox;
    fmtLayout->addWidget(_folderRVColCombo, row++, 1);

    fmtLayout->addWidget(new QLabel("RV error column:"), row, 0);
    _folderRVErrColCombo = new QComboBox;
    fmtLayout->addWidget(_folderRVErrColCombo, row++, 1);

    formatGroup->setLayout(fmtLayout);
    layout->addWidget(formatGroup);

    QHBoxLayout* scanLayout = new QHBoxLayout;
    _scanFoldersBtn = new QPushButton("Scan Folders && Parse");
    _scanFoldersBtn->setEnabled(false);
    connect(_scanFoldersBtn, &QPushButton::clicked,
            this, &RadialVelocityImportPage::onScanFolders);
    scanLayout->addWidget(_scanFoldersBtn);
    _folderProgress = new QProgressBar;
    _folderProgress->setVisible(false);
    scanLayout->addWidget(_folderProgress);
    scanLayout->addStretch();
    layout->addLayout(scanLayout);

    connect(_rootFolderEdit, &QLineEdit::textChanged, this, [this](const QString& t) {
        _scanFoldersBtn->setEnabled(!t.trimmed().isEmpty());
        // Try to detect columns from first file found
        if (!t.trimmed().isEmpty() && QDir(t).exists()) {
            detectFolderColumns();
        }
    });

    layout->addStretch();
}

void RadialVelocityImportPage::setupFromTablePage()
{
    _fromTablePage = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(_fromTablePage);

    // File path
    QHBoxLayout* fileLayout = new QHBoxLayout;
    _tableFileEdit = new QLineEdit;
    _tableFileEdit->setPlaceholderText("Select RV table file...");
    fileLayout->addWidget(_tableFileEdit);
    QPushButton* browseBtn = new QPushButton("Browse...");
    connect(browseBtn, &QPushButton::clicked,
            this, &RadialVelocityImportPage::onBrowseTableFile);
    fileLayout->addWidget(browseBtn);
    layout->addLayout(fileLayout);

    QHBoxLayout* optLayout = new QHBoxLayout;
    optLayout->addWidget(new QLabel("Delimiter:"));
    _tableDelimCombo = new QComboBox;
    _tableDelimCombo->addItems({
        "Auto-detect", "Comma (,)", "Tab", "Space", "Semicolon (;)"
    });
    optLayout->addWidget(_tableDelimCombo);
    _tableHeaderCheck = new QCheckBox("First row is header");
    _tableHeaderCheck->setChecked(true);
    optLayout->addWidget(_tableHeaderCheck);
    optLayout->addStretch();
    layout->addLayout(optLayout);

    // Column mapping
    QGroupBox* colGroup = new QGroupBox("Column Mapping");
    QGridLayout* colLayout = new QGridLayout;
    int row = 0;

    colLayout->addWidget(new QLabel("Star identifier column:"), row, 0);
    _tableIdColCombo = new QComboBox;
    colLayout->addWidget(_tableIdColCombo, row, 1);
    _tableIdTypeCombo = new QComboBox;
    _tableIdTypeCombo->addItems({
        "Gaia Source ID", "Alias/Name", "RA (pair with Dec)", "Dec (pair with RA)"
    });
    colLayout->addWidget(_tableIdTypeCombo, row++, 2);

    colLayout->addWidget(new QLabel("Timestamp column:"), row, 0);
    _tableTimeColCombo = new QComboBox;
    colLayout->addWidget(_tableTimeColCombo, row, 1);
    _tableTimeTypeCombo = new QComboBox;
    _tableTimeTypeCombo->addItems({"MJD", "BJD"});
    colLayout->addWidget(_tableTimeTypeCombo, row++, 2);

    colLayout->addWidget(new QLabel("RV column:"), row, 0);
    _tableRVColCombo = new QComboBox;
    colLayout->addWidget(_tableRVColCombo, row++, 1);

    colLayout->addWidget(new QLabel("RV error column:"), row, 0);
    _tableRVErrColCombo = new QComboBox;
    colLayout->addWidget(_tableRVErrColCombo, row++, 1);

    colGroup->setLayout(colLayout);
    layout->addWidget(colGroup);

    _processTableBtn = new QPushButton("Process Table");
    _processTableBtn->setEnabled(false);
    connect(_processTableBtn, &QPushButton::clicked,
            this, &RadialVelocityImportPage::onProcessTable);
    layout->addWidget(_processTableBtn);
    layout->addStretch();
}

void RadialVelocityImportPage::setupFitParamsGroup(QVBoxLayout* parentLayout)
{
    _fitParamsGroup = new QGroupBox("RV Orbital Fit Parameters (Optional)");
    QVBoxLayout* fpLayout = new QVBoxLayout;

    _importFitParamsCheck = new QCheckBox(
        "Import orbital fit parameters (K, γ, P, T₀, e, ω)");
    connect(_importFitParamsCheck, &QCheckBox::toggled,
            this, &RadialVelocityImportPage::onFitParamsToggled);
    fpLayout->addWidget(_importFitParamsCheck);

    // File path
    QHBoxLayout* fpFileLayout = new QHBoxLayout;
    fpFileLayout->addWidget(new QLabel("Fit parameters file:"));
    _fitFileEdit = new QLineEdit;
    _fitFileEdit->setPlaceholderText("CSV/ASCII with one row per star...");
    _fitFileEdit->setEnabled(false);
    fpFileLayout->addWidget(_fitFileEdit);
    QPushButton* fpBrowse = new QPushButton("Browse...");
    fpBrowse->setObjectName("fitParamsBrowse");
    connect(fpBrowse, &QPushButton::clicked,
            this, &RadialVelocityImportPage::onBrowseFitParamsFile);
    fpFileLayout->addWidget(fpBrowse);
    fpLayout->addLayout(fpFileLayout);

    QHBoxLayout* fpOptLayout = new QHBoxLayout;
    fpOptLayout->addWidget(new QLabel("Delimiter:"));
    _fitDelimCombo = new QComboBox;
    _fitDelimCombo->addItems({
        "Auto-detect", "Comma (,)", "Tab", "Space", "Semicolon (;)"
    });
    _fitDelimCombo->setEnabled(false);
    fpOptLayout->addWidget(_fitDelimCombo);
    _fitHeaderCheck = new QCheckBox("Header row");
    _fitHeaderCheck->setChecked(true);
    _fitHeaderCheck->setEnabled(false);
    fpOptLayout->addWidget(_fitHeaderCheck);
    fpOptLayout->addStretch();
    fpLayout->addLayout(fpOptLayout);

    // Column mapping grid
    QGridLayout* fpColLayout = new QGridLayout;
    int row = 0;

    fpColLayout->addWidget(new QLabel("Star identifier:"), row, 0);
    _fitIdColCombo = new QComboBox; _fitIdColCombo->setEnabled(false);
    fpColLayout->addWidget(_fitIdColCombo, row, 1);
    _fitIdTypeCombo = new QComboBox;
    _fitIdTypeCombo->addItems({"Gaia Source ID", "Alias/Name"});
    _fitIdTypeCombo->setEnabled(false);
    fpColLayout->addWidget(_fitIdTypeCombo, row++, 2);

    auto addParamRow = [&](const QString& label, QComboBox*& combo) {
        fpColLayout->addWidget(new QLabel(label), row, 0);
        combo = new QComboBox;
        combo->setEnabled(false);
        fpColLayout->addWidget(combo, row++, 1);
    };

    addParamRow("Half-amplitude K:", _fitKColCombo);
    addParamRow("Systemic velocity γ:", _fitGammaColCombo);
    addParamRow("Period P:", _fitPeriodColCombo);
    addParamRow("Phase zero T₀:", _fitT0ColCombo);
    addParamRow("Eccentricity e:", _fitEccColCombo);
    addParamRow("Argument of periastron ω:", _fitOmegaColCombo);

    fpLayout->addLayout(fpColLayout);

    QLabel* fpNote = new QLabel(
        "<i>Error columns are auto-detected by naming convention "
        "(e.g. K_error, e_K, K_err adjacent to K).</i>");
    fpNote->setWordWrap(true);
    fpLayout->addWidget(fpNote);

    _fitParamsGroup->setLayout(fpLayout);
    parentLayout->addWidget(_fitParamsGroup);
}

// ════════════════════════════════════════════════════════════════
// Lifecycle
// ════════════════════════════════════════════════════════════════

void RadialVelocityImportPage::initializePage()
{
    LOG_INFO("RVImport", "Initializing RadialVelocityImportPage");

    StarImportWizard* wiz = qobject_cast<StarImportWizard*>(wizard());
    if (!wiz) return;

    auto controller = wiz->controller();
    auto project = wiz->project();

    if (controller && project) {
        if (!project->starsLoaded())
            controller->openProject(project->getId());
        _importedStars = project->getAllStars();
    }
    if (_importedStars.empty())
        _importedStars = wiz->getImportedStars();

    _results.clear();
    _resultsReady = false;
    _asyncBusy = false;
    _indexBuilt = false;
    _previewTree->clear();

    buildStarLookupIndex();

    // If background tasks still running, poll
    if (isBackgroundBusy()) {
        _extractFitsBtn->setEnabled(false);
        _statusLabel->setText(
            "⏳ Background import tasks still running. Please wait...");
        auto* timer = new QTimer(this);
        timer->setInterval(500);
        connect(timer, &QTimer::timeout, this, [this, timer]() {
            if (!isBackgroundBusy()) {
                timer->stop();
                timer->deleteLater();
                _extractFitsBtn->setEnabled(true);
                _indexBuilt = false;
                buildStarLookupIndex();
                _statusLabel->setText(
                    QString("Ready — %1 stars available for RV import.")
                    .arg(_importedStars.size()));
            }
        });
        timer->start();
        return;
    }

    _statusLabel->setText(
        QString("Ready — %1 stars available. Select a data source.")
        .arg(_importedStars.size()));
}

bool RadialVelocityImportPage::isBackgroundBusy() const
{
    StarImportWizard* wiz = qobject_cast<StarImportWizard*>(
        const_cast<QWizard*>(wizard()));
    if (!wiz || !wiz->controller()) return false;
    auto* tm = wiz->controller()->backgroundTaskManager();
    return tm && tm->hasActiveTasks();
}

void RadialVelocityImportPage::onImportModeChanged()
{
    if (_fromFitsRadio->isChecked())        _modeStack->setCurrentIndex(0);
    else if (_fromFoldersRadio->isChecked()) _modeStack->setCurrentIndex(1);
    else                                     _modeStack->setCurrentIndex(2);

    // Hide fit params for "from fits" mode (spectral fits ≠ orbital fits)
    _fitParamsGroup->setVisible(!_fromFitsRadio->isChecked());

    _results.clear();
    _resultsReady = false;
    _previewTree->clear();
}

// ════════════════════════════════════════════════════════════════
// Star lookup index
// ════════════════════════════════════════════════════════════════

void RadialVelocityImportPage::buildStarLookupIndex()
{
    if (_indexBuilt) return;

    _sourceIdIndex.clear();
    _aliasIndex.clear();

    QRegularExpression numRe("(\\d{10,})");

    for (const auto& star : _importedStars) {
        QString sid = star->getSourceId();
        if (!sid.isEmpty()) {
            _sourceIdIndex[sid] = star;
            _sourceIdIndex[sid.trimmed()] = star;
            QRegularExpressionMatch m = numRe.match(sid);
            if (m.hasMatch())
                _sourceIdIndex[m.captured(1)] = star;
        }
        QString alias = star->getAlias();
        if (!alias.isEmpty()) {
            _aliasIndex[alias.trimmed().toLower()] = star;
            // Also store without spaces/dashes for fuzzy matching
            QString crushed = alias.toUpper().remove(' ').remove('-').remove('_');
            _aliasIndex[crushed.toLower()] = star;
        }
    }
    _indexBuilt = true;
}

std::shared_ptr<Star> RadialVelocityImportPage::findStarByIdentifier(
    const QString& id, const QString& idType) const
{
    QString clean = id.trimmed();
    if (clean.isEmpty()) return nullptr;

    if (idType == "source_id" || idType == "Gaia Source ID") {
        auto it = _sourceIdIndex.find(clean);
        if (it != _sourceIdIndex.end()) return it.value();
        // Try numeric extraction
        QRegularExpression numRe("(\\d{10,})");
        QRegularExpressionMatch m = numRe.match(clean);
        if (m.hasMatch()) {
            it = _sourceIdIndex.find(m.captured(1));
            if (it != _sourceIdIndex.end()) return it.value();
        }
    } else if (idType == "alias" || idType == "Alias/Name") {
        auto it = _aliasIndex.find(clean.toLower());
        if (it != _aliasIndex.end()) return it.value();
        QString crushed = clean.toUpper().remove(' ').remove('-').remove('_').toLower();
        it = _aliasIndex.find(crushed);
        if (it != _aliasIndex.end()) return it.value();
    } else if (idType == "ra_dec" || idType.startsWith("RA")) {
        // Parse "RA_DEC" or "RA+DEC" from folder name
        QRegularExpression coordRe(
            "([\\d.]+)[_+\\-\\s]([+\\-]?[\\d.]+)");
        QRegularExpressionMatch m = coordRe.match(clean);
        if (m.hasMatch()) {
            bool okRa, okDec;
            double ra  = m.captured(1).toDouble(&okRa);
            double dec = m.captured(2).toDouble(&okDec);
            if (okRa && okDec) {
                // Find nearest star within 5 arcsec
                double bestDist = 5.0 / 3600.0; // degrees
                std::shared_ptr<Star> best;
                for (const auto& star : _importedStars) {
                    double dRa = (ra - star->getRa())
                                 * std::cos(star->getDec() * M_PI / 180.0);
                    double dDec = dec - star->getDec();
                    double dist = std::sqrt(dRa * dRa + dDec * dDec);
                    if (dist < bestDist) {
                        bestDist = dist;
                        best = star;
                    }
                }
                return best;
            }
        }
    }
    return nullptr;
}

// ════════════════════════════════════════════════════════════════
// CSV helpers
// ════════════════════════════════════════════════════════════════

QChar RadialVelocityImportPage::getDelimiter(QComboBox* combo) const
{
    int idx = combo->currentIndex();
    switch (idx) {
        case 1: return ',';
        case 2: return '\t';
        case 3: return ' ';
        case 4: return ';';
        default: return '\0';  // auto-detect
    }
}

QChar RadialVelocityImportPage::detectDelimiter(const QString& line) const
{
    int commas = line.count(',');
    int tabs   = line.count('\t');
    int semis  = line.count(';');
    int spaces = line.split(QRegularExpression("\\s+"),
                            Qt::SkipEmptyParts).size() - 1;

    int best = commas;
    QChar ch = ',';
    if (tabs > best)   { best = tabs;   ch = '\t'; }
    if (semis > best)  { best = semis;  ch = ';'; }
    if (spaces > best) { ch = ' '; }
    return ch;
}

QStringList RadialVelocityImportPage::parseLine(
    const QString& line, QChar delimiter) const
{
    if (delimiter == ' ')
        return line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);

    QStringList result;
    QString current;
    bool inQuotes = false;
    for (int i = 0; i < line.length(); ++i) {
        QChar c = line[i];
        if (c == '"') {
            inQuotes = !inQuotes;
        } else if (c == delimiter && !inQuotes) {
            result << current.trimmed();
            current.clear();
        } else {
            current += c;
        }
    }
    result << current.trimmed();
    return result;
}

bool RadialVelocityImportPage::loadCSVFile(
    const QString& filepath, QComboBox* delimCombo, QCheckBox* headerCheck,
    QStringList& outColumns, std::vector<QStringList>& outRows)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&file);
    QStringList lines;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        lines << line;
    }
    file.close();

    if (lines.isEmpty()) return false;

    QChar delim = getDelimiter(delimCombo);
    if (delim == '\0')
        delim = detectDelimiter(lines.first());

    outColumns.clear();
    outRows.clear();

    int startRow = 0;
    if (headerCheck->isChecked()) {
        outColumns = parseLine(lines[0], delim);
        startRow = 1;
    } else {
        int ncols = parseLine(lines[0], delim).size();
        for (int i = 0; i < ncols; ++i)
            outColumns << QString("Column_%1").arg(i);
    }

    for (int i = startRow; i < lines.size(); ++i)
        outRows.push_back(parseLine(lines[i], delim));

    return true;
}

void RadialVelocityImportPage::populateColumnCombos(
    const QStringList& columns,
    const QList<QPair<QComboBox*, QStringList>>& comboPatterns)
{
    for (auto& [combo, patterns] : comboPatterns) {
        combo->blockSignals(true);
        combo->clear();
        combo->addItem("(none)");
        combo->addItems(columns);

        // Auto-detect by pattern matching
        int bestIdx = 0;
        for (int i = 0; i < columns.size(); ++i) {
            QString col = columns[i].toLower();
            for (const QString& pat : patterns) {
                if (col == pat || col.contains(pat)) {
                    bestIdx = i + 1;  // +1 for "(none)" at index 0
                    break;
                }
            }
            if (bestIdx > 0) break;
        }
        combo->setCurrentIndex(bestIdx);
        combo->blockSignals(false);
    }
}

// ════════════════════════════════════════════════════════════════
// Mode 1: From Spectral Fits
// ════════════════════════════════════════════════════════════════

void RadialVelocityImportPage::onExtractFromFits()
{
    extractFromSpectralFits();
}

void RadialVelocityImportPage::extractFromSpectralFits()
{
    _results.clear();

    StarImportWizard* wiz = qobject_cast<StarImportWizard*>(wizard());
    if (!wiz || !wiz->controller()) {
        LOG_WARNING("RVImport", "No wizard or controller available");
        return;
    }
    DatabaseManager* dbm = wiz->controller()->databaseManager();
    auto project = wiz->project();

    _statusLabel->setText("Extracting RV from spectral fits...");
    QApplication::processEvents();

    bool bestOnly = _bestFitOnlyCheck->isChecked();
    bool skipZero = _skipZeroRVCheck->isChecked();

    int totalPoints = 0;
    int starsWithRV = 0;

    // ── Merge wizard stars + DB stars ──
    QHash<QString, std::shared_ptr<Star>> starById;

    for (const auto& star : _importedStars)
        starById[star->getId()] = star;

    if (dbm && project) {
        auto dbStars = dbm->loadStars(project->getId());
        LOG_INFO("RVImport", QString("DB returned %1 stars").arg(dbStars.size()));
        for (const auto& star : dbStars)
            starById[star->getId()] = star;
    }

    LOG_INFO("RVImport", QString("Processing %1 unique stars").arg(starById.size()));

    // ── Diagnostics counters ──
    int starsWithSpectra = 0;
    int totalSpectra = 0;
    int totalFits = 0;
    int fitsWithZeroRV = 0;
    int fitsWithNonZeroRV = 0;
    int bestFitNull = 0;
    int bestFitFound = 0;

    int processed = 0;
    int total = starById.size();
    bool loggedSample = false;

    for (auto it = starById.cbegin(); it != starById.cend(); ++it) {
        const auto& star = it.value();

        if (++processed % 500 == 0) {
            _statusLabel->setText(QString("Extracting RV... %1/%2 stars")
                .arg(processed).arg(total));
            QApplication::processEvents();
        }

        auto spectra = star->getSpectra();
        if (spectra.empty() && dbm) {
            spectra = dbm->loadSpectra(star->getId());
        }

        if (spectra.empty()) continue;

        starsWithSpectra++;
        totalSpectra += static_cast<int>(spectra.size());

        auto curve = std::make_shared<RadialVelocityCurve>();
        curve->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

        for (const auto& spectrum : spectra) {
            auto fits = spectrum->getSpectralFits();
            totalFits += static_cast<int>(fits.size());

            // Log first star with fits for diagnostics
            if (!loggedSample && !fits.empty()) {
                auto& f = fits.front();
                LOG_INFO("RVImport", QString("Sample fit: RV=%1, isBest=%2, "
                    "teff=%3, chi2=%4, spectrum=%5")
                    .arg(f->radialVelocity)
                    .arg(f->isBestFit)
                    .arg(f->teff)
                    .arg(f->chi2)
                    .arg(spectrum->getFile()));

                auto best = spectrum->getBestFit();
                LOG_INFO("RVImport", QString("Sample getBestFit(): %1")
                    .arg(best ? QString("RV=%1").arg(best->radialVelocity) : "nullptr"));
                loggedSample = true;
            }

            if (bestOnly) {
                std::shared_ptr<SpectralFit> best = spectrum->getBestFit();
                
                // Fallback: pick fit with lowest RV error
                if (!best && !fits.empty()) {
                    double lowestErr = std::numeric_limits<double>::max();
                    for (const auto& f : fits) {
                        if (f->radialVelocityError >= 0 && f->radialVelocityError < lowestErr) {
                            lowestErr = f->radialVelocityError;
                            best = f;
                        }
                    }
                    // If all errors are 0 or equal, just take the first
                    if (!best)
                        best = fits.front();
                }
                
                if (!best) { bestFitNull++; continue; }
                bestFitFound++;
                if (skipZero && std::abs(best->radialVelocity) < 1e-10) {
                    fitsWithZeroRV++;
                    continue;
                }
                fitsWithNonZeroRV++;
            
                auto rvPt = RadialVelocityPoint::createFromSpectralFit(
                    best, spectrum);
                if (rvPt) {
                    rvPt->setId(QUuid::createUuid().toString(
                        QUuid::WithoutBraces));
                    curve->addRVPoint(rvPt);
                }
            } else {
                for (const auto& fit : fits) {
                    if (std::abs(fit->radialVelocity) < 1e-10) {
                        fitsWithZeroRV++;
                        if (skipZero) continue;
                    }
                    fitsWithNonZeroRV++;

                    auto rvPt = RadialVelocityPoint::createFromSpectralFit(
                        fit, spectrum);
                    if (rvPt) {
                        rvPt->setId(QUuid::createUuid().toString(
                            QUuid::WithoutBraces));
                        curve->addRVPoint(rvPt);
                    }
                }
            }
        }

        if (curve->getNumPoints() > 0) {
            StarRVResult result;
            result.star = star;
            result.curve = curve;
            _results.push_back(result);
            totalPoints += static_cast<int>(curve->getNumPoints());
            starsWithRV++;
        }
    }

    LOG_INFO("RVImport", QString("Diagnostics: %1 stars with spectra, "
        "%2 total spectra, %3 total fits")
        .arg(starsWithSpectra).arg(totalSpectra).arg(totalFits));
    LOG_INFO("RVImport", QString("Fits: %1 zero RV, %2 non-zero RV, "
        "bestFitFound=%3, bestFitNull=%4")
        .arg(fitsWithZeroRV).arg(fitsWithNonZeroRV)
        .arg(bestFitFound).arg(bestFitNull));
    LOG_INFO("RVImport", QString("Extraction complete: %1 RV points for %2 stars")
        .arg(totalPoints).arg(starsWithRV));

    _resultsReady = true;
    computeAllMetadata();
    updatePreviewTree();

    _statusLabel->setText(
        QString("Extracted %1 RV points for %2 stars from spectral fits.")
        .arg(totalPoints).arg(starsWithRV));
}

// ════════════════════════════════════════════════════════════════
// Mode 2: From Per-Star Folders
// ════════════════════════════════════════════════════════════════

void RadialVelocityImportPage::onBrowseRootFolder()
{
    QString folder = QFileDialog::getExistingDirectory(
        this, "Select Root RV Folder", _rootFolderEdit->text());
    if (!folder.isEmpty())
        _rootFolderEdit->setText(folder);
}

void RadialVelocityImportPage::detectFolderColumns()
{
    QString rootPath = _rootFolderEdit->text().trimmed();
    if (rootPath.isEmpty()) return;

    QDir root(rootPath);
    QStringList subDirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (subDirs.isEmpty()) return;

    // Find first .csv/.txt/.dat file in the first subfolder
    for (const QString& subDir : subDirs) {
        QDir sd(root.filePath(subDir));
        QStringList files = sd.entryList(
            {"*.csv", "*.txt", "*.dat", "*.tsv"}, QDir::Files);
        if (files.isEmpty()) continue;

        QString sampleFile = sd.filePath(files.first());
        QFile file(sampleFile);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

        QTextStream in(&file);
        QString firstLine;
        while (!in.atEnd()) {
            QString line = in.readLine().trimmed();
            if (!line.isEmpty() && !line.startsWith('#')) {
                firstLine = line;
                break;
            }
        }
        file.close();

        if (firstLine.isEmpty()) continue;

        QChar delim = getDelimiter(_folderDelimCombo);
        if (delim == '\0') delim = detectDelimiter(firstLine);

        QStringList cols;
        if (_folderHeaderCheck->isChecked()) {
            cols = parseLine(firstLine, delim);
        } else {
            int n = parseLine(firstLine, delim).size();
            for (int i = 0; i < n; ++i)
                cols << QString("Column_%1").arg(i);
        }
        _folderDetectedColumns = cols;

        populateColumnCombos(cols, {
            {_folderTimeColCombo,   {"mjd", "bjd", "time", "jd", "timestamp", "epoch"}},
            {_folderRVColCombo,     {"rv", "vrad", "radial_velocity", "v_rad",
                                     "radvel", "velocity"}},
            {_folderRVErrColCombo,  {"rv_err", "rv_error", "e_rv", "vrad_err",
                                     "vrad_error", "e_vrad", "sigma", "err"}},
        });

        // Auto-detect BJD vs MJD from column name
        int timeIdx = _folderTimeColCombo->currentIndex() - 1;
        if (timeIdx >= 0 && timeIdx < cols.size()) {
            QString tn = cols[timeIdx].toLower();
            if (tn.contains("bjd"))
                _folderTimeTypeCombo->setCurrentIndex(1);
            else
                _folderTimeTypeCombo->setCurrentIndex(0);
        }
        break;
    }
}

void RadialVelocityImportPage::onScanFolders()
{
    scanAndParseFolders();
}

void RadialVelocityImportPage::scanAndParseFolders()
{
    _results.clear();

    QString rootPath = _rootFolderEdit->text().trimmed();
    if (rootPath.isEmpty()) return;

    QDir root(rootPath);
    QStringList subDirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    if (subDirs.isEmpty()) {
        _statusLabel->setText("No subdirectories found.");
        return;
    }

    buildStarLookupIndex();

    int timeCol  = _folderTimeColCombo->currentIndex() - 1;
    int rvCol    = _folderRVColCombo->currentIndex() - 1;
    int rvErrCol = _folderRVErrColCombo->currentIndex() - 1;
    bool isBJD   = (_folderTimeTypeCombo->currentIndex() == 1);

    if (timeCol < 0 || rvCol < 0) {
        QMessageBox::warning(this, "Missing Columns",
            "Please select at least the timestamp and RV columns.");
        return;
    }

    QChar delim = getDelimiter(_folderDelimCombo);
    bool hasHeader = _folderHeaderCheck->isChecked();

    // Determine folder naming type
    QString namingType;
    switch (_folderNamingCombo->currentIndex()) {
        case 0: namingType = "source_id"; break;
        case 1: namingType = "alias"; break;
        case 2: namingType = "ra_dec"; break;
    }

    _folderProgress->setVisible(true);
    _folderProgress->setRange(0, subDirs.size());
    _statusLabel->setText("Scanning folders...");

    int matched = 0, unmatched = 0, totalPoints = 0;

    for (int i = 0; i < subDirs.size(); ++i) {
        _folderProgress->setValue(i);
        QApplication::processEvents();

        QString dirName = subDirs[i];
        QDir sd(root.filePath(dirName));

        // Match folder to star
        auto star = findStarByIdentifier(dirName, namingType);
        if (!star) {
            unmatched++;
            continue;
        }

        // Find RV table files in this folder
        QStringList files = sd.entryList(
            {"*.csv", "*.txt", "*.dat", "*.tsv"}, QDir::Files);
        if (files.isEmpty()) {
            unmatched++;
            continue;
        }

        auto curve = std::make_shared<RadialVelocityCurve>();
        curve->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

        for (const QString& fileName : files) {
            QFile file(sd.filePath(fileName));
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;

            QTextStream in(&file);
            QStringList lines;
            while (!in.atEnd()) {
                QString line = in.readLine().trimmed();
                if (line.isEmpty() || line.startsWith('#')) continue;
                lines << line;
            }
            file.close();

            if (lines.isEmpty()) continue;

            QChar fileDelim = delim;
            if (fileDelim == '\0')
                fileDelim = detectDelimiter(lines.first());

            int startRow = hasHeader ? 1 : 0;

            for (int r = startRow; r < lines.size(); ++r) {
                QStringList fields = parseLine(lines[r], fileDelim);

                if (timeCol >= fields.size() || rvCol >= fields.size())
                    continue;

                bool okTime, okRV;
                double time = fields[timeCol].toDouble(&okTime);
                double rv   = fields[rvCol].toDouble(&okRV);
                if (!okTime || !okRV) continue;

                double rvErr = 0.0;
                if (rvErrCol >= 0 && rvErrCol < fields.size()) {
                    bool okErr;
                    rvErr = fields[rvErrCol].toDouble(&okErr);
                    if (!okErr) rvErr = 0.0;
                }

                auto rvPt = std::make_shared<RadialVelocityPoint>();
                rvPt->setId(QUuid::createUuid().toString(
                    QUuid::WithoutBraces));

                // Store time in the appropriate field
                if (isBJD) {
                    rvPt->setBJD(time);
                    rvPt->setMJD(0.0);
                } else {
                    rvPt->setMJD(time);
                    rvPt->setBJD(0.0);
                }

                rvPt->setRV(rv);
                rvPt->setRVError(rvErr);
                rvPt->setSource(fileName);

                curve->addRVPoint(rvPt);
            }
        }

        if (curve->getNumPoints() > 0) {
            StarRVResult result;
            result.star = star;
            result.curve = curve;
            _results.push_back(result);
            totalPoints += static_cast<int>(curve->getNumPoints());
            matched++;
        } else {
            unmatched++;
        }
    }

    _folderProgress->setValue(subDirs.size());
    _folderProgress->setVisible(false);

    _resultsReady = true;
    computeAllMetadata();

    // If fit params file specified, apply
    if (_importFitParamsCheck->isChecked() && !_fitFileEdit->text().isEmpty())
        applyFitParams();

    updatePreviewTree();

    _statusLabel->setText(
        QString("Scanned %1 folders: %2 matched (%3 RV points), "
                "%4 unmatched.")
        .arg(subDirs.size()).arg(matched).arg(totalPoints).arg(unmatched));
}

// ════════════════════════════════════════════════════════════════
// Mode 3: From Single Table
// ════════════════════════════════════════════════════════════════

void RadialVelocityImportPage::onBrowseTableFile()
{
    QString file = QFileDialog::getOpenFileName(
        this, "Select RV Table File", QString(),
        "Data Files (*.csv *.txt *.dat *.tsv);;All Files (*)");
    if (file.isEmpty()) return;

    _tableFileEdit->setText(file);

    // Load & detect columns
    if (loadCSVFile(file, _tableDelimCombo, _tableHeaderCheck,
                    _tableColumns, _tableRows)) {
        populateColumnCombos(_tableColumns, {
            {_tableIdColCombo,      {"source_id", "star_id", "id", "name",
                                     "target", "object", "gaia"}},
            {_tableTimeColCombo,    {"mjd", "bjd", "time", "jd", "timestamp",
                                     "epoch"}},
            {_tableRVColCombo,      {"rv", "vrad", "radial_velocity", "v_rad",
                                     "radvel", "velocity"}},
            {_tableRVErrColCombo,   {"rv_err", "rv_error", "e_rv", "vrad_err",
                                     "vrad_error", "e_vrad", "sigma_rv",
                                     "err"}},
        });

        // Auto-detect time type from column name
        int timeIdx = _tableTimeColCombo->currentIndex() - 1;
        if (timeIdx >= 0 && timeIdx < _tableColumns.size()) {
            QString tn = _tableColumns[timeIdx].toLower();
            _tableTimeTypeCombo->setCurrentIndex(
                tn.contains("bjd") ? 1 : 0);
        }

        // Auto-detect ID type from column name
        int idIdx = _tableIdColCombo->currentIndex() - 1;
        if (idIdx >= 0 && idIdx < _tableColumns.size()) {
            QString cn = _tableColumns[idIdx].toLower();
            if (cn.contains("source_id") || cn.contains("gaia"))
                _tableIdTypeCombo->setCurrentIndex(0);
            else
                _tableIdTypeCombo->setCurrentIndex(1);
        }

        _processTableBtn->setEnabled(true);
        _statusLabel->setText(
            QString("Loaded table: %1 columns, %2 data rows.")
            .arg(_tableColumns.size()).arg(_tableRows.size()));
    } else {
        QMessageBox::warning(this, "Load Error",
            "Could not read or parse the selected file.");
    }
}

void RadialVelocityImportPage::onProcessTable()
{
    processTableData();
}

void RadialVelocityImportPage::processTableData()
{
    _results.clear();

    if (_tableColumns.isEmpty() || _tableRows.empty()) {
        _statusLabel->setText("No table data loaded.");
        return;
    }

    buildStarLookupIndex();

    int idCol    = _tableIdColCombo->currentIndex() - 1;
    int timeCol  = _tableTimeColCombo->currentIndex() - 1;
    int rvCol    = _tableRVColCombo->currentIndex() - 1;
    int rvErrCol = _tableRVErrColCombo->currentIndex() - 1;
    bool isBJD   = (_tableTimeTypeCombo->currentIndex() == 1);

    if (idCol < 0 || timeCol < 0 || rvCol < 0) {
        QMessageBox::warning(this, "Missing Columns",
            "Please select the star identifier, timestamp, and RV columns.");
        return;
    }

    // Determine ID type
    QString idType;
    switch (_tableIdTypeCombo->currentIndex()) {
        case 0: idType = "source_id"; break;
        case 1: idType = "alias"; break;
        case 2: idType = "ra"; break;
        case 3: idType = "dec"; break;
        default: idType = "source_id"; break;
    }

    // Group rows by star identifier
    QHash<QString, std::vector<int>> groupedRows;
    for (int i = 0; i < static_cast<int>(_tableRows.size()); ++i) {
        const QStringList& row = _tableRows[i];
        if (idCol >= row.size()) continue;
        QString key = row[idCol].trimmed();
        if (!key.isEmpty())
            groupedRows[key].push_back(i);
    }

    _statusLabel->setText(
        QString("Processing %1 unique identifiers...")
        .arg(groupedRows.size()));
    QApplication::processEvents();

    int matched = 0, unmatched = 0, totalPoints = 0;

    for (auto it = groupedRows.cbegin(); it != groupedRows.cend(); ++it) {
        const QString& starId = it.key();
        const std::vector<int>& rowIndices = it.value();

        auto star = findStarByIdentifier(starId, idType);
        if (!star) {
            unmatched++;
            continue;
        }

        auto curve = std::make_shared<RadialVelocityCurve>();
        curve->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

        for (int ri : rowIndices) {
            const QStringList& row = _tableRows[ri];
            if (timeCol >= row.size() || rvCol >= row.size())
                continue;

            bool okTime, okRV;
            double time = row[timeCol].toDouble(&okTime);
            double rv   = row[rvCol].toDouble(&okRV);
            if (!okTime || !okRV) continue;

            double rvErr = 0.0;
            if (rvErrCol >= 0 && rvErrCol < row.size()) {
                bool okErr;
                rvErr = row[rvErrCol].toDouble(&okErr);
                if (!okErr) rvErr = 0.0;
            }

            auto rvPt = std::make_shared<RadialVelocityPoint>();
            rvPt->setId(QUuid::createUuid().toString(
                QUuid::WithoutBraces));

            if (isBJD) {
                rvPt->setBJD(time);
                rvPt->setMJD(0.0);
            } else {
                rvPt->setMJD(time);
                rvPt->setBJD(0.0);
            }

            rvPt->setRV(rv);
            rvPt->setRVError(rvErr);
            rvPt->setSource("table_import");

            curve->addRVPoint(rvPt);
        }

        if (curve->getNumPoints() > 0) {
            StarRVResult result;
            result.star = star;
            result.curve = curve;
            _results.push_back(result);
            totalPoints += static_cast<int>(curve->getNumPoints());
            matched++;
        }
    }

    _resultsReady = true;
    computeAllMetadata();

    // If fit params file specified, apply
    if (_importFitParamsCheck->isChecked() && !_fitFileEdit->text().isEmpty())
        applyFitParams();

    updatePreviewTree();

    _statusLabel->setText(
        QString("Processed table: %1 stars matched (%2 RV points), "
                "%3 identifiers unmatched.")
        .arg(matched).arg(totalPoints).arg(unmatched));
}

// ════════════════════════════════════════════════════════════════
// Fit Parameters
// ════════════════════════════════════════════════════════════════

void RadialVelocityImportPage::onFitParamsToggled(bool checked)
{
    _fitFileEdit->setEnabled(checked);
    _fitDelimCombo->setEnabled(checked);
    _fitHeaderCheck->setEnabled(checked);
    _fitIdColCombo->setEnabled(checked);
    _fitIdTypeCombo->setEnabled(checked);
    _fitKColCombo->setEnabled(checked);
    _fitGammaColCombo->setEnabled(checked);
    _fitPeriodColCombo->setEnabled(checked);
    _fitT0ColCombo->setEnabled(checked);
    _fitEccColCombo->setEnabled(checked);
    _fitOmegaColCombo->setEnabled(checked);
    findChild<QPushButton*>("fitParamsBrowse")->setEnabled(checked);
}

void RadialVelocityImportPage::onBrowseFitParamsFile()
{
    QString file = QFileDialog::getOpenFileName(
        this, "Select Fit Parameters File", QString(),
        "Data Files (*.csv *.txt *.dat *.tsv);;All Files (*)");
    if (file.isEmpty()) return;

    _fitFileEdit->setText(file);
    parseFitParamsFile();
}

void RadialVelocityImportPage::parseFitParamsFile()
{
    QString filepath = _fitFileEdit->text().trimmed();
    if (filepath.isEmpty()) return;

    if (!loadCSVFile(filepath, _fitDelimCombo, _fitHeaderCheck,
                     _fitColumns, _fitRows)) {
        QMessageBox::warning(this, "Load Error",
            "Could not read or parse the fit parameters file.");
        return;
    }

    populateColumnCombos(_fitColumns, {
        {_fitIdColCombo,     {"source_id", "star_id", "id", "name",
                              "target", "object", "gaia"}},
        {_fitKColCombo,      {"k", "k1", "half_amp", "amplitude",
                              "semi_amplitude"}},
        {_fitGammaColCombo,  {"gamma", "v0", "systemic", "v_sys",
                              "vsys", "v_gamma"}},
        {_fitPeriodColCombo, {"period", "p", "per", "orbital_period"}},
        {_fitT0ColCombo,     {"t0", "t_0", "t_peri", "t_periastron",
                              "epoch_peri"}},
        {_fitEccColCombo,    {"ecc", "eccentricity", "e"}},
        {_fitOmegaColCombo,  {"omega", "w", "arg_peri", "argument_of_periastron"}},
    });

    _statusLabel->setText(
        QString("Fit params file: %1 columns, %2 rows loaded.")
        .arg(_fitColumns.size()).arg(_fitRows.size()));
}

void RadialVelocityImportPage::applyFitParams()
{
    if (_fitColumns.isEmpty() || _fitRows.empty() || _results.empty())
        return;

    int idCol     = _fitIdColCombo->currentIndex() - 1;
    int kCol      = _fitKColCombo->currentIndex() - 1;
    int gammaCol  = _fitGammaColCombo->currentIndex() - 1;
    int periodCol = _fitPeriodColCombo->currentIndex() - 1;
    int t0Col     = _fitT0ColCombo->currentIndex() - 1;
    int eccCol    = _fitEccColCombo->currentIndex() - 1;
    int omegaCol  = _fitOmegaColCombo->currentIndex() - 1;

    if (idCol < 0) return;

    QString idType;
    switch (_fitIdTypeCombo->currentIndex()) {
        case 0: idType = "source_id"; break;
        case 1: idType = "alias"; break;
        default: idType = "source_id"; break;
    }

    // Helper: find error column by naming convention
    auto findErrCol = [&](int paramCol) -> int {
        if (paramCol < 0 || paramCol >= _fitColumns.size()) return -1;
        QString base = _fitColumns[paramCol].toLower();
        QStringList errSuffixes = {
            "_error", "_err", "_unc", "_sigma"
        };
        QStringList errPrefixes = {"e_", "err_", "sigma_"};

        for (int i = 0; i < _fitColumns.size(); ++i) {
            if (i == paramCol) continue;
            QString cn = _fitColumns[i].toLower();
            for (const QString& suffix : errSuffixes) {
                if (cn == base + suffix) return i;
            }
            for (const QString& prefix : errPrefixes) {
                if (cn == prefix + base) return i;
            }
        }
        return -1;
    };

    int kErrCol     = findErrCol(kCol);
    int gammaErrCol = findErrCol(gammaCol);
    int periodErrCol= findErrCol(periodCol);
    int t0ErrCol    = findErrCol(t0Col);
    int eccErrCol   = findErrCol(eccCol);
    int omegaErrCol = findErrCol(omegaCol);

    // Build a lookup from star to result index
    QHash<QString, int> resultLookup;
    for (int i = 0; i < static_cast<int>(_results.size()); ++i) {
        auto& star = _results[i].star;
        if (!star->getSourceId().isEmpty())
            resultLookup[star->getSourceId()] = i;
        if (!star->getAlias().isEmpty())
            resultLookup[star->getAlias().trimmed().toLower()] = i;
    }

    auto getDouble = [](const QStringList& row, int col) -> double {
        if (col < 0 || col >= row.size()) return 0.0;
        bool ok;
        double v = row[col].toDouble(&ok);
        return ok ? v : 0.0;
    };

    int applied = 0;

    for (const auto& row : _fitRows) {
        if (idCol >= row.size()) continue;
        QString starId = row[idCol].trimmed();

        // Find the matching result
        auto star = findStarByIdentifier(starId, idType);
        if (!star) continue;

        int resultIdx = -1;
        for (int i = 0; i < static_cast<int>(_results.size()); ++i) {
            if (_results[i].star == star) {
                resultIdx = i;
                break;
            }
        }
        if (resultIdx < 0) continue;

        auto fit = std::make_shared<RVFit>();
        fit->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

        if (kCol >= 0) {
            fit->setK(getDouble(row, kCol));
            fit->setKError(getDouble(row, kErrCol));
        }
        if (gammaCol >= 0) {
            fit->setGamma(getDouble(row, gammaCol));
            fit->setGammaError(getDouble(row, gammaErrCol));
        }
        if (periodCol >= 0) {
            fit->setPeriod(getDouble(row, periodCol));
            fit->setPeriodError(getDouble(row, periodErrCol));
        }
        if (t0Col >= 0) {
            fit->setT0(getDouble(row, t0Col));
            fit->setT0Error(getDouble(row, t0ErrCol));
        }
        if (eccCol >= 0) {
            fit->setEccentricity(getDouble(row, eccCol));
            fit->setEccentricityError(getDouble(row, eccErrCol));
        }
        if (omegaCol >= 0) {
            fit->setOmega(getDouble(row, omegaCol));
            fit->setOmegaError(getDouble(row, omegaErrCol));
        }

        _results[resultIdx].fit = fit;
        applied++;
    }

    LOG_INFO("RVImport",
             QString("Applied fit parameters to %1 stars").arg(applied));
}

// ════════════════════════════════════════════════════════════════
// Metadata & Preview
// ════════════════════════════════════════════════════════════════

void RadialVelocityImportPage::computeAllMetadata()
{
    for (auto& result : _results) {
        auto& curve = result.curve;
        if (!curve || curve->getNumPoints() == 0) continue;

        // Compute logP (chi-squared variability test)
        double logP = curve->computeLogP();
        curve->setLogP(logP);
    }
}

void RadialVelocityImportPage::updatePreviewTree()
{
    _previewTree->clear();

    for (const auto& result : _results) {
        auto& star = result.star;
        auto& curve = result.curve;

        QTreeWidgetItem* item = new QTreeWidgetItem;

        // Star name
        QString name = star->getAlias();
        if (name.isEmpty())
            name = star->getSourceId();
        item->setText(0, name);

        // # Points
        int nPts = static_cast<int>(curve->getNumPoints());
        item->setText(1, QString::number(nPts));

        // RV summary
        QString summary;
        if (nPts > 0) {
            double meanRV = curve->getMeanRV();
            double stdRV  = curve->getStdDevRV();
            double minRV  = curve->getMinRV();
            double maxRV  = curve->getMaxRV();
            summary = QString("μ=%1 σ=%2 [%3, %4] km/s")
                    .arg(meanRV, 0, 'f', 2)
                    .arg(stdRV, 0, 'f', 2)
                    .arg(minRV, 0, 'f', 2)
                    .arg(maxRV, 0, 'f', 2);
        }
        item->setText(2, summary);

        // Status
        QString status = "Ready";
        if (result.fit)
            status += " + Fit";
        if (nPts < 3)
            status += " (few points)";
        item->setText(3, status);

        double logP = curve->getLogP();
        if (!std::isnan(logP)) {
            QTreeWidgetItem* pItem = new QTreeWidgetItem(item);
            pItem->setText(0, "  Variability");
            pItem->setText(2, QString("log₁₀(p) = %1").arg(logP, 0, 'f', 2));
            if (logP < -4)
                pItem->setText(3, "Variable");
            else if (logP < -1.3)
                pItem->setText(3, "Variability Candidate");
            else
                pItem->setText(3, "Consistent with constant");
        }

        // Child: Orbital fit
        if (result.fit) {
            QTreeWidgetItem* fitItem = new QTreeWidgetItem(item);
            fitItem->setText(0, "  Orbital Fit");
            auto fit = result.fit;
            fitItem->setText(2, QString("K=%.2f γ=%.2f P=%.3f e=%.4f")
                .arg(fit->getK()).arg(fit->getGamma())
                .arg(fit->getPeriod()).arg(fit->getEccentricity()));
        }

        _previewTree->addTopLevelItem(item);
    }

    _previewTree->expandAll();
    for (int i = 0; i < _previewTree->columnCount(); ++i)
        _previewTree->resizeColumnToContents(i);
}

// ════════════════════════════════════════════════════════════════
// Validation & Save
// ════════════════════════════════════════════════════════════════

bool RadialVelocityImportPage::validatePage()
{
    if (!_resultsReady || _results.empty()) {
        QMessageBox::StandardButton reply = QMessageBox::question(
            this, "No RV Data",
            "No radial velocity data has been imported. "
            "Continue without RV data?",
            QMessageBox::Yes | QMessageBox::No);
        return (reply == QMessageBox::Yes);
    }

    return saveResults();
}

bool RadialVelocityImportPage::saveResults()
{
    StarImportWizard* wiz = qobject_cast<StarImportWizard*>(wizard());
    if (!wiz || !wiz->controller()) return false;

    DatabaseManager* dbm = wiz->controller()->databaseManager();
    if (!dbm) {
        qWarning() << "RVImport: No DatabaseManager available.";
        return false;
    }

    // Get project ID for saveStar()
    QString projectId;
    auto project = wiz->project();
    if (project)
        projectId = project->getId();

    if (projectId.isEmpty()) {
        qWarning() << "RVImport: No project ID available.";
        return false;
    }

    _statusLabel->setText("Saving RV data to database...");
    QApplication::processEvents();

    int savedCurves = 0;
    int savedPoints = 0;
    int savedFits = 0;

    for (auto& result : _results) {
        auto& star = result.star;
        auto& curve = result.curve;

        if (!curve || curve->getNumPoints() == 0) continue;

        // Link curve to star
        curve->setStarId(star->getId());

        // Save RV curve
        if (dbm->saveRadialVelocityCurve(curve, star->getId())) {
            savedCurves++;

            // Save individual RV points
            for (const auto& pt : curve->getRVPoints()) {
                pt->setCurveId(curve->getId());
                if (dbm->saveRadialVelocityPoint(pt, curve->getId()))
                    savedPoints++;
            }

            // Save fit if present
            if (result.fit) {
                result.fit->setCurveId(curve->getId());
                result.fit->setBestFit(true);
                curve->addRVFit(result.fit);
                if (dbm->saveRVFit(result.fit, curve->getId()))
                    savedFits++;
            }

            // Attach curve to Star model and update metrics
            star->setRVCurve(curve);
            star->updateRVMetricsFromCurve();

            // Save updated star (with correct 2-argument signature)
            dbm->saveStar(projectId, star);
        }
    }

    _statusLabel->setText(
        QString("Saved %1 RV curves (%2 points, %3 fits) to database.")
        .arg(savedCurves).arg(savedPoints).arg(savedFits));

    LOG_INFO("RVImport",
        QString("Saved %1 curves, %2 points, %3 fits")
        .arg(savedCurves).arg(savedPoints).arg(savedFits));

    return true;
}

int RadialVelocityImportPage::nextId() const
{
    return StarImportWizard::Page_Photometry;
}