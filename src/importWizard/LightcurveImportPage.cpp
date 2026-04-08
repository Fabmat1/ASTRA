#include "LightcurveImportPage.h"
#include "StarImportWizard.h"
#include "ImportStagingArea.h"

#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include "models/Photometry.h"
#include "../db/DatabaseManager.h"
#include "../utils/Logger.h"
#include "models/Time.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QLabel>
#include <QPushButton>
#include <QRadioButton>
#include <QCheckBox>
#include <QLineEdit>
#include <QComboBox>
#include <QGroupBox>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QProgressBar>
#include <QFileDialog>
#include <QHeaderView>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QTextStream>
#include <QApplication>
#include <QRegularExpression>
#include <QUuid>
#include <QMessageBox>
#include <QButtonGroup>
#include <QSet>

#include <cmath>
#include <algorithm>

// ── Helpers for the timescale combo ─────────────────────────

static void populateTimeScaleCombo(QComboBox* combo,
                                   TimeScale selected)
{
    combo->blockSignals(true);
    combo->clear();

    struct Entry { TimeScale ts; QString label; };
    static const Entry entries[] = {
        { TimeScale::Unknown,  "Auto / Unknown" },
        { TimeScale::BJD,      "BJD"            },
        { TimeScale::MJD,      "MJD"            },
        { TimeScale::BTJD,     "BTJD (TESS)"   },
        { TimeScale::BKJD,     "BKJD (Kepler)" },
        { TimeScale::GaiaTCB,  "Gaia TCB"       },
        { TimeScale::HJD,      "HJD"            },
        { TimeScale::JD,       "JD"             },
    };

    int selIdx = 0;
    for (int i = 0; i < int(std::size(entries)); ++i) {
        combo->addItem(entries[i].label,
                       static_cast<int>(entries[i].ts));
        if (entries[i].ts == selected) selIdx = i;
    }
    combo->setCurrentIndex(selIdx);
    combo->blockSignals(false);
}

static TimeScale comboTimeScale(QComboBox* combo)
{
    return static_cast<TimeScale>(
        combo->currentData().toInt());
}

// ══════════════════════════════════════════════════════════════
// Construction
// ══════════════════════════════════════════════════════════════

LightcurveImportPage::LightcurveImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import Photometric Lightcurves");
    setSubTitle(
        "Import lightcurve data from a folder structure (one folder per star, "
        "files named by instrument) or from a CSV manifest pointing to files.");

    auto* mainLayout = new QVBoxLayout(this);

    // ── Mode selection ───────────────────────────────────────
    auto* modeGroup = new QGroupBox("Import Mode");
    auto* modeLayout = new QHBoxLayout(modeGroup);
    _folderModeRadio = new QRadioButton("Folder Structure");
    _csvModeRadio    = new QRadioButton("CSV Manifest");
    _folderModeRadio->setChecked(true);
    auto* modeButtons = new QButtonGroup(this);
    modeButtons->addButton(_folderModeRadio);
    modeButtons->addButton(_csvModeRadio);
    modeLayout->addWidget(_folderModeRadio);
    modeLayout->addWidget(_csvModeRadio);
    modeLayout->addStretch();
    mainLayout->addWidget(modeGroup);

    // ── Stacked settings ─────────────────────────────────────
    _modeStack = new QStackedWidget;

    // Page 0: Folder mode
    auto* folderWidget = new QWidget;
    {
        auto* fl = new QFormLayout(folderWidget);
        fl->setContentsMargins(0, 0, 0, 0);

        auto* dirRow = new QHBoxLayout;
        _rootDirEdit   = new QLineEdit;
        _rootDirEdit->setPlaceholderText("Select root directory…");
        _rootDirEdit->setReadOnly(true);
        _browseDirBtn  = new QPushButton("Browse…");
        dirRow->addWidget(_rootDirEdit, 1);
        dirRow->addWidget(_browseDirBtn);
        fl->addRow("Root Directory:", dirRow);

        _starIdTypeCombo = new QComboBox;
        _starIdTypeCombo->addItems({
            "Gaia Source ID", "Star Name / Alias", "RA_DEC"
        });
        fl->addRow("Folder names are:", _starIdTypeCombo);
    }
    _modeStack->addWidget(folderWidget);

    // Page 1: CSV mode
    auto* csvWidget = new QWidget;
    {
        auto* cl = new QFormLayout(csvWidget);
        cl->setContentsMargins(0, 0, 0, 0);

        auto* csvRow = new QHBoxLayout;
        _csvFileEdit = new QLineEdit;
        _csvFileEdit->setPlaceholderText("Select CSV manifest file…");
        _csvFileEdit->setReadOnly(true);
        _browseCSVBtn = new QPushButton("Browse…");
        csvRow->addWidget(_csvFileEdit, 1);
        csvRow->addWidget(_browseCSVBtn);
        cl->addRow("CSV File:", csvRow);
    }
    _modeStack->addWidget(csvWidget);

    mainLayout->addWidget(_modeStack);

    // ── Scan button + progress ───────────────────────────────
    auto* scanRow = new QHBoxLayout;
    _scanBtn = new QPushButton("Scan for Lightcurves");
    _scanBtn->setEnabled(false);
    _progressBar = new QProgressBar;
    _progressBar->setVisible(false);
    scanRow->addWidget(_scanBtn);
    scanRow->addWidget(_progressBar, 1);
    mainLayout->addLayout(scanRow);

    // ── Create-new option ────────────────────────────────────
    _createNewCb = new QCheckBox(
        "Create new stars for unmatched identifiers (using folder name)");
    _createNewCb->setChecked(false);
    mainLayout->addWidget(_createNewCb);

    // ── Results tree ─────────────────────────────────────────
    _resultsTree = new QTreeWidget;
    _resultsTree->setHeaderLabels({
        "Import", "Star ID", "File", "Instrument",
        "Time Scale", "Points", "Filters", "Matched Star", "Status"
    });
    _resultsTree->setRootIsDecorated(false);
    _resultsTree->setAlternatingRowColors(true);
    _resultsTree->header()->setStretchLastSection(true);
    _resultsTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _resultsTree->setColumnWidth(0, 50);
    mainLayout->addWidget(_resultsTree, 1);

    // ── Timescale overrides ──────────────────────────────────
    _tsOverrideGroup = new QGroupBox("Timescale Overrides (per instrument)");
    _tsOverrideLayout = new QFormLayout(_tsOverrideGroup);
    _tsOverrideGroup->setVisible(false);
    mainLayout->addWidget(_tsOverrideGroup);

    // ── Selection shortcuts ──────────────────────────────────
    auto* selRow = new QHBoxLayout;
    auto* selAll   = new QPushButton("Select All");
    auto* selNone  = new QPushButton("Select None");
    auto* selMatch = new QPushButton("Select Matched Only");
    selRow->addWidget(selAll);
    selRow->addWidget(selNone);
    selRow->addWidget(selMatch);
    selRow->addStretch();
    mainLayout->addLayout(selRow);

    // ── Summary ──────────────────────────────────────────────
    _summaryLabel = new QLabel;
    _summaryLabel->setStyleSheet("color: #888; font-style: italic;");
    mainLayout->addWidget(_summaryLabel);
    updateSummary();

    // ── Connections ──────────────────────────────────────────
    connect(_folderModeRadio, &QRadioButton::toggled,
            this, &LightcurveImportPage::onModeChanged);
    connect(_browseDirBtn, &QPushButton::clicked,
            this, &LightcurveImportPage::browseRootDirectory);
    connect(_browseCSVBtn, &QPushButton::clicked,
            this, &LightcurveImportPage::browseCSVFile);
    connect(_scanBtn, &QPushButton::clicked,
            this, &LightcurveImportPage::scanLightcurves);
    connect(_resultsTree, &QTreeWidget::itemChanged,
            this, &LightcurveImportPage::onItemChanged);
    connect(selAll,   &QPushButton::clicked, this, &LightcurveImportPage::selectAll);
    connect(selNone,  &QPushButton::clicked, this, &LightcurveImportPage::selectNone);
    connect(selMatch, &QPushButton::clicked, this, &LightcurveImportPage::selectMatched);
    connect(_createNewCb, &QCheckBox::toggled, this, [this]{
        updateSummary(); emit completeChanged();
    });
}

void LightcurveImportPage::setStagingArea(ImportStagingArea* staging)
{
    _staging = staging;
}

// ══════════════════════════════════════════════════════════════
// QWizardPage interface
// ══════════════════════════════════════════════════════════════

void LightcurveImportPage::initializePage()
{
    _staged = false;
}

bool LightcurveImportPage::validatePage()
{
    int sel = 0;
    for (const auto& e : _entries)
        if (e.selected && !e.hasError) ++sel;

    if (sel == 0) return true;   

    stageSelectedLightcurves();
    return true;
}

bool LightcurveImportPage::isComplete() const
{
    return true;
}

int LightcurveImportPage::nextId() const
{
    return StarImportWizard::Page_Photometry;
}

// ══════════════════════════════════════════════════════════════
// Mode & browsing
// ══════════════════════════════════════════════════════════════

void LightcurveImportPage::onModeChanged()
{
    _modeStack->setCurrentIndex(_csvModeRadio->isChecked() ? 1 : 0);
    _scanned = false;
    _entries.clear();
    _resultsTree->clear();
    _tsOverrideGroup->setVisible(false);
    updateSummary();

    bool ready = _folderModeRadio->isChecked()
                     ? !_rootDirEdit->text().isEmpty()
                     : !_csvFileEdit->text().isEmpty();
    _scanBtn->setEnabled(ready);
    emit completeChanged();
}

void LightcurveImportPage::browseRootDirectory()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select Root Lightcurve Directory",
        _rootDirEdit->text().isEmpty() ? QDir::homePath() : _rootDirEdit->text(),
        QFileDialog::ShowDirsOnly);
    if (dir.isEmpty()) return;

    _rootDirEdit->setText(dir);
    _scanned = false;
    _scanBtn->setEnabled(true);
    emit completeChanged();
}

void LightcurveImportPage::browseCSVFile()
{
    QString file = QFileDialog::getOpenFileName(
        this, "Select Lightcurve CSV Manifest",
        _csvFileEdit->text().isEmpty() ? QDir::homePath() : QFileInfo(_csvFileEdit->text()).absolutePath(),
        "CSV files (*.csv *.tsv *.txt);;All files (*)");
    if (file.isEmpty()) return;

    _csvFileEdit->setText(file);
    _scanned = false;
    _scanBtn->setEnabled(true);
    emit completeChanged();
}

// ══════════════════════════════════════════════════════════════
// Scanning
// ══════════════════════════════════════════════════════════════

void LightcurveImportPage::scanLightcurves()
{
    _entries.clear();
    _resultsTree->clear();
    _instrumentTimeScales.clear();
    _scanned = false;
    _staged  = false;
    _tsOverrideGroup->setVisible(false);

    _progressBar->setVisible(true);
    _progressBar->setRange(0, 0);
    _scanBtn->setEnabled(false);
    QApplication::processEvents();

    if (_folderModeRadio->isChecked()) {
        scanFolderStructure(_rootDirEdit->text());
    } else {
        scanCSVManifest(_csvFileEdit->text());
    }

    // ── Match to project stars ───────────────────────────────
    _progressBar->setFormat("Matching to project stars…");
    QApplication::processEvents();
    matchEntriesToStars();

    _progressBar->setVisible(false);
    _scanBtn->setEnabled(true);
    _scanned = true;

    populateTree();
    buildTimeScaleOverrides();
    updateSummary();
    emit completeChanged();
}

// ── Folder-structure scanner ─────────────────────────────────

void LightcurveImportPage::scanFolderStructure(const QString& rootPath)
{
    QDir root(rootPath);
    QStringList subDirs = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot);

    if (subDirs.isEmpty()) {
        _summaryLabel->setText("No subdirectories found in root directory.");
        return;
    }

    _progressBar->setRange(0, subDirs.size());
    _progressBar->setValue(0);
    _progressBar->setFormat("Scanning folders %v / %m …");

    static const QStringList lcExtensions = {
        "*.txt", "*.csv", "*.dat", "*.tsv"
    };

    for (int d = 0; d < subDirs.size(); ++d) {
        const QString& dirName = subDirs[d];
        QDir sd(root.filePath(dirName));
        QStringList files = sd.entryList(lcExtensions, QDir::Files);

        for (const QString& fileName : files) {
            QString fullPath = sd.absoluteFilePath(fileName);
            QString instrument = instrumentFromFilename(fileName);

            LightcurveScanEntry entry;
            entry.starIdentifier = dirName;
            entry.filePath       = fullPath;
            entry.instrument     = instrument;
            entry.detectedTimeScale = Time::guessScaleFromInstrument(instrument);

            TimeScale parseScale =
                (entry.detectedTimeScale != TimeScale::Unknown)
                    ? entry.detectedTimeScale
                    : TimeScale::MJD;

            QString parseErr;
            if (parseLightcurveFile(fullPath, parseScale,
                                    entry.points, entry.filters,
                                    parseErr))
            {
                entry.numPoints = static_cast<int>(entry.points.size());

                // If timescale was unknown, try to guess from first time value
                if (entry.detectedTimeScale == TimeScale::Unknown
                    && !entry.points.empty())
                {
                    entry.detectedTimeScale =
                        Time::guessScaleFromValue(
                            entry.points.front().originalTime());
                    if (entry.detectedTimeScale != TimeScale::Unknown
                        && entry.detectedTimeScale != parseScale)
                    {
                        for (auto& pt : entry.points) {
                            pt.time = Time(pt.time.nativeValue(), entry.detectedTimeScale);
                        }
                    }
                }
            } else {
                entry.hasError     = true;
                entry.errorMessage = parseErr;
                entry.selected     = false;
            }

            _entries.push_back(std::move(entry));
        }

        _progressBar->setValue(d + 1);
        if (d % 20 == 0) QApplication::processEvents();
    }
}

// ── CSV-manifest scanner ─────────────────────────────────────

void LightcurveImportPage::scanCSVManifest(const QString& csvPath)
{
    QFile file(csvPath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        _summaryLabel->setText("Failed to open CSV file.");
        return;
    }

    QTextStream in(&file);
    QStringList allLines;
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (!line.isEmpty()) allLines << line;
    }
    file.close();

    if (allLines.isEmpty()) {
        _summaryLabel->setText("CSV file is empty.");
        return;
    }

    // ── Detect delimiter ─────────────────────────────────────
    QChar delim = ',';
    {
        int commas = allLines.first().count(',');
        int tabs   = allLines.first().count('\t');
        int semis  = allLines.first().count(';');
        if (tabs > commas && tabs > semis) delim = '\t';
        if (semis > commas && semis > tabs) delim = ';';
    }

    QStringList headers = allLines.first().split(delim);
    for (auto& h : headers) h = h.trimmed().toLower().remove('"');

    // ── Map columns ──────────────────────────────────────────
    int fileCol = -1, instrCol = -1, starCol = -1;
    for (int i = 0; i < headers.size(); ++i) {
        const QString& h = headers[i];
        if (fileCol < 0 && (h == "file" || h == "path" || h == "filepath"
                            || h == "filename" || h == "file_path"))
            fileCol = i;
        if (instrCol < 0 && (h == "instrument" || h == "source"
                             || h == "survey" || h == "telescope"))
            instrCol = i;
        if (starCol < 0 && (h == "star" || h == "star_id" || h == "source_id"
                            || h == "gaia_id" || h == "id" || h == "name"
                            || h == "alias" || h == "identifier"))
            starCol = i;
    }

    if (fileCol < 0 || instrCol < 0) {
        // Try positional: first column = file, second = instrument
        if (headers.size() >= 2) {
            fileCol  = 0;
            instrCol = 1;
            if (headers.size() >= 3) starCol = 2;
        } else {
            _summaryLabel->setText(
                "CSV must have at least a file/path and instrument column.");
            return;
        }
    }

    QFileInfo csvInfo(csvPath);
    QString csvDir = csvInfo.absolutePath();

    _progressBar->setRange(0, allLines.size() - 1);
    _progressBar->setFormat("Parsing files %v / %m …");

    // ── Parse data rows (skip header) ────────────────────────
    for (int r = 1; r < allLines.size(); ++r) {
        QStringList cols = allLines[r].split(delim);

        if (fileCol >= cols.size() || instrCol >= cols.size())
            continue;

        QString rawPath   = cols[fileCol].trimmed().remove('"');
        QString instrument = cols[instrCol].trimmed().remove('"');
        QString starId;
        if (starCol >= 0 && starCol < cols.size())
            starId = cols[starCol].trimmed().remove('"');

        // Resolve relative paths against CSV directory
        QFileInfo fi(rawPath);
        QString fullPath = fi.isAbsolute() ? rawPath
                                           : QDir(csvDir).absoluteFilePath(rawPath);

        if (!QFile::exists(fullPath)) {
            LightcurveScanEntry entry;
            entry.starIdentifier = starId.isEmpty()
                ? QFileInfo(fullPath).dir().dirName() : starId;
            entry.filePath    = fullPath;
            entry.instrument  = instrument;
            entry.hasError    = true;
            entry.errorMessage = "File not found";
            entry.selected    = false;
            _entries.push_back(std::move(entry));
            _progressBar->setValue(r);
            continue;
        }

        // If no star identifier column, derive from parent directory
        if (starId.isEmpty())
            starId = QFileInfo(fullPath).dir().dirName();

        LightcurveScanEntry entry;
        entry.starIdentifier = starId;
        entry.filePath       = fullPath;
        entry.instrument     = instrument;
        entry.detectedTimeScale = Time::guessScaleFromInstrument(instrument);

        TimeScale parseScale =
            (entry.detectedTimeScale != TimeScale::Unknown)
                ? entry.detectedTimeScale : TimeScale::MJD;


        QString parseErr;
        if (parseLightcurveFile(fullPath, parseScale,
                                entry.points, entry.filters, parseErr))
        {
            entry.numPoints = static_cast<int>(entry.points.size());

            if (entry.detectedTimeScale == TimeScale::Unknown
                && !entry.points.empty())
            {
                entry.detectedTimeScale =
                    Time::guessScaleFromValue(
                        entry.points.front().originalTime());
                if (entry.detectedTimeScale != TimeScale::Unknown
                    && entry.detectedTimeScale != parseScale)
                {
                    for (auto& pt : entry.points) {
                        pt.time = Time(pt.time.nativeValue(), entry.detectedTimeScale);
                    }
                }
            }
        } else {
            entry.hasError     = true;
            entry.errorMessage = parseErr;
            entry.selected     = false;
        }

        _entries.push_back(std::move(entry));
        _progressBar->setValue(r);
        if (r % 50 == 0) QApplication::processEvents();
    }
}

// ══════════════════════════════════════════════════════════════
// File parsing
// ══════════════════════════════════════════════════════════════

bool LightcurveImportPage::parseLightcurveFile(
    const QString& filePath,
    TimeScale scale,
    std::vector<LightcurvePoint>& outPoints,
    QStringList& outFilters,
    QString& outError)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        outError = "Cannot open file";
        return false;
    }

    const QString content = QString::fromUtf8(file.readAll());
    file.close();

    const QStringList allLines = content.split('\n');

    QSet<QString> filterSet;
    outPoints.clear();
    outPoints.reserve(4096);

    static const QRegularExpression wsRe("\\s+");

    // Detect delimiter from first non-empty, non-comment line
    QChar delim = ',';
    int firstDataIdx = -1;

    for (int i = 0; i < allLines.size(); ++i) {
        const QString line = allLines[i].trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith("//"))
            continue;

        firstDataIdx = i;

        int commas = line.count(',');
        int tabs   = line.count('\t');
        int spaces = line.count(wsRe);
        if (tabs > commas)       delim = '\t';
        else if (commas == 0 && spaces > 0) delim = ' ';
        break;
    }

    if (firstDataIdx < 0) {
        outError = "File is empty or contains only comments";
        return false;
    }

    auto splitLine = [&](const QString& line) -> QStringList {
        if (delim == ' ')
            return line.split(wsRe, Qt::SkipEmptyParts);
        return line.split(delim);
    };

    for (int i = firstDataIdx; i < allLines.size(); ++i) {
        const QString line = allLines[i].trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith("//"))
            continue;

        QStringList fields = splitLine(line);
        if (fields.size() < 3) continue;

        bool okT, okF, okE;
        double time  = fields[0].trimmed().toDouble(&okT);
        double flux  = fields[1].trimmed().toDouble(&okF);
        double error = fields[2].trimmed().toDouble(&okE);

        if (!okT || !okF || !okE) continue;
        if (std::isnan(time) || std::isnan(flux) || std::isinf(time) || std::isinf(flux))
            continue;

        LightcurvePoint pt;
        pt.time      = Time(time, scale);
        pt.flux      = flux;
        pt.fluxError = std::isnan(error) ? 0.0 : error;

        // Optional filter column (index 3)
        if (fields.size() > 3) {
            QString filter = fields[3].trimmed().remove('"');
            if (!filter.isEmpty() && filter != "nan" && filter != "NaN") {
                pt.filter = filter;
                filterSet.insert(filter);
            }
        }

        // Optional magnitude column (index 4)
        if (fields.size() > 4) {
            bool okM;
            double mag = fields[4].trimmed().toDouble(&okM);
            if (okM && !std::isnan(mag) && !std::isinf(mag))
                pt.magnitude = mag;
        }

        // Optional magnitude error column (index 5)
        if (fields.size() > 5) {
            bool okME;
            double magErr = fields[5].trimmed().toDouble(&okME);
            if (okME && !std::isnan(magErr) && !std::isinf(magErr))
                pt.magnitudeError = magErr;
        }

        outPoints.push_back(std::move(pt));
    }

    // Sort by time
    std::sort(outPoints.begin(), outPoints.end(),
              [](const LightcurvePoint& a, const LightcurvePoint& b) {
                  return a.time < b.time;
              });

    outFilters = filterSet.values();
    outFilters.sort();

    // Reject files with no valid data points (e.g. all-NaN rows)
    if (outPoints.empty()) {
        outError = "No valid data points found";
        return false;
    }

    // If every detected filter value parses as a number, the column layout
    // is almost certainly not time/flux/err/filter — reject as trash
    if (!outFilters.isEmpty()) {
        bool allNumeric = true;
        for (const QString& f : outFilters) {
            bool ok;
            f.toDouble(&ok);
            if (!ok) { allNumeric = false; break; }
        }
        if (allNumeric) {
            outError = "All filter values are numeric — column structure "
                       "does not match expected lightcurve format";
            outPoints.clear();
            outFilters.clear();
            return false;
        }
    }

    return true;
}

// ══════════════════════════════════════════════════════════════
// Instrument detection from filename
// ══════════════════════════════════════════════════════════════

QString LightcurveImportPage::instrumentFromFilename(const QString& filename) const
{
    QString lower = filename.toLower();

    struct InstrumentPattern {
        QString keyword;
        QString instrument;
    };

    static const InstrumentPattern patterns[] = {
        { "tess",      "tess"      },
        { "kepler",    "kepler"    },
        { "k2",        "k2"        },
        { "atlas",     "atlas"     },
        { "asas",      "asas-sn"   },
        { "ztf",       "ztf"       },
        { "gaia",      "gaia"      },
        { "ogle",      "ogle"      },
        { "hipparcos", "hipparcos" },
        { "wasp",      "wasp"      },
        { "aavso",     "aavso"     },
        { "css",       "css"       },
        { "nsvs",      "nsvs"      },
        { "hatnet",    "hatnet"    },
        { "mascara",   "mascara"   },
        { "kelt",      "kelt"      },
    };

    for (const auto& p : patterns) {
        if (lower.contains(p.keyword))
            return p.instrument;
    }

    // Fall back to the file basename without extension
    QFileInfo fi(filename);
    return fi.completeBaseName().toLower();
}

// ══════════════════════════════════════════════════════════════
// Star matching
// ══════════════════════════════════════════════════════════════

void LightcurveImportPage::matchEntriesToStars()
{
    auto* importWizard = qobject_cast<StarImportWizard*>(wizard());
    if (!importWizard || !importWizard->controller()) return;

    auto proj = importWizard->project();
    if (!proj) return;

    // Start with stars already in staging (includes newly created ones from earlier pages)
    std::vector<std::shared_ptr<Star>> stars;
    QSet<QString> seen;

    if (_staging) {
        stars = _staging->allStars();
        for (const auto& s : stars)
            seen.insert(s->getId());
    }

    // Supplement with project stars not yet in staging
    for (const auto& s : proj->getAllStars()) {
        if (!seen.contains(s->getId())) {
            stars.push_back(s);
            seen.insert(s->getId());
        }
    }

    if (stars.empty()) {
        LOG_WARNING("LCImport", "No stars available for matching");
        return;
    }

    LOG_INFO("LCImport",
             QString("Matching against %1 stars (%2 staged, %3 from project)")
                 .arg(stars.size())
                 .arg(_staging ? _staging->totalStarCount() : 0)
                 .arg(stars.size() - (_staging ? _staging->totalStarCount() : 0)));

    // Build lookup maps
    QHash<QString, std::shared_ptr<Star>> bySourceId;
    QHash<QString, std::shared_ptr<Star>> byJName;
    QHash<QString, std::shared_ptr<Star>> byAlias;
    QHash<QString, std::shared_ptr<Star>> byTic;

    for (const auto& star : stars) {
        if (!star->getSourceId().isEmpty())
            bySourceId[star->getSourceId()] = star;

        QString jnameLower = star->getJName().toLower().trimmed();
        if (!jnameLower.isEmpty())
            byJName[jnameLower] = star;

        QString aliasLower = star->getAlias().toLower().trimmed();
        if (!aliasLower.isEmpty())
            byAlias[aliasLower] = star;

        if (!star->getTic().isEmpty())
            byTic[star->getTic()] = star;
    }

    int idType = _folderModeRadio->isChecked()
                     ? _starIdTypeCombo->currentIndex()
                     : -1;  // CSV mode: try all strategies

    for (auto& entry : _entries) {
        QString id = entry.starIdentifier.trimmed();
        QString idLower = id.toLower();
        std::shared_ptr<Star> matched;

        // Strategy 0: Gaia Source ID (exact match)
        if (!matched && (idType == 0 || idType < 0)) {
            auto it = bySourceId.find(id);
            if (it != bySourceId.end())
                matched = it.value();
        }

        // Strategy 1: JName or Alias (case-insensitive)
        if (!matched && (idType == 1 || idType < 0)) {
            auto it = byJName.find(idLower);
            if (it != byJName.end())
                matched = it.value();
        }
        if (!matched && (idType == 1 || idType < 0)) {
            auto it = byAlias.find(idLower);
            if (it != byAlias.end())
                matched = it.value();
        }

        // Strategy 2: RA_DEC positional match
        if (!matched && (idType == 2 || idType < 0)) {
            QStringList parts = id.split('_');
            if (parts.size() == 2) {
                bool okRA, okDEC;
                double ra  = parts[0].toDouble(&okRA);
                double dec = parts[1].toDouble(&okDEC);
                if (okRA && okDEC) {
                    double bestDist = 1.0 / 3600.0;  // 1 arcsecond
                    for (const auto& star : stars) {
                        if (!Star::isSet(star->getRa()) || !Star::isSet(star->getDec()))
                            continue;
                        double dra  = (star->getRa()  - ra) * std::cos(dec * M_PI / 180.0);
                        double ddec = star->getDec() - dec;
                        double dist = std::sqrt(dra * dra + ddec * ddec);
                        if (dist < bestDist) {
                            bestDist = dist;
                            matched  = star;
                        }
                    }
                }
            }
        }

        // Also try TIC as a fallback
        if (!matched && idType < 0) {
            auto it = byTic.find(id);
            if (it != byTic.end())
                matched = it.value();
        }

        if (matched) {
            entry.matchedStarId      = matched->getId();
            entry.matchedStarDisplay = starDisplayName(matched);
        }
    }
}

QString LightcurveImportPage::starDisplayName(std::shared_ptr<Star> star) const
{
    if (!star) return {};

    QString display = star->getJName();
    if (display.isEmpty())
        display = star->getAlias();

    if (!star->getSourceId().isEmpty()) {
        if (!display.isEmpty()) display += " ";
        display += "(Gaia " + star->getSourceId() + ")";
    }

    return display.isEmpty() ? star->getId() : display;
}

// ══════════════════════════════════════════════════════════════
// Tree population
// ══════════════════════════════════════════════════════════════

void LightcurveImportPage::populateTree()
{
    _resultsTree->blockSignals(true);
    _resultsTree->clear();

    for (int i = 0; i < static_cast<int>(_entries.size()); ++i) {
        const auto& e = _entries[i];
        auto* item = new QTreeWidgetItem;

        item->setCheckState(0, e.selected ? Qt::Checked : Qt::Unchecked);
        item->setData(0, Qt::UserRole, i);  // store index
        item->setText(1, e.starIdentifier);
        item->setText(2, QFileInfo(e.filePath).fileName());
        item->setText(3, e.instrument);
        item->setText(4, Time::scaleToString(e.detectedTimeScale));
        item->setText(5, QString::number(e.numPoints));
        item->setText(6, e.filters.join(", "));
        item->setText(7, e.matchedStarDisplay);

        if (e.hasError) {
            item->setText(8, "⚠ " + e.errorMessage);
            item->setForeground(8, QBrush(Qt::red));
            item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
        } else if (e.matchedStarId.isEmpty()) {
            item->setText(8, _createNewCb->isChecked()
                                 ? "Will create new star"
                                 : "No match");
            item->setForeground(8, QBrush(QColor(180, 140, 0)));
        } else {
            item->setText(8, "✓ Matched");
            item->setForeground(8, QBrush(QColor(0, 150, 0)));
        }

        _resultsTree->addTopLevelItem(item);
    }

    _resultsTree->blockSignals(false);
}

// ══════════════════════════════════════════════════════════════
// Timescale override controls
// ══════════════════════════════════════════════════════════════

void LightcurveImportPage::buildTimeScaleOverrides()
{
    // Clear existing
    while (_tsOverrideLayout->rowCount() > 0)
        _tsOverrideLayout->removeRow(0);
    _timeScaleCombos.clear();

    // Collect unique instruments
    QSet<QString> instruments;
    for (const auto& e : _entries) {
        if (!e.instrument.isEmpty())
            instruments.insert(e.instrument);
    }

    if (instruments.isEmpty()) {
        _tsOverrideGroup->setVisible(false);
        return;
    }

    QStringList sorted = instruments.values();
    sorted.sort();

    for (const QString& inst : sorted) {
        auto* combo = new QComboBox;

        // Find the most common detected timescale for this instrument
        TimeScale detected = TimeScale::Unknown;
        for (const auto& e : _entries) {
            if (e.instrument == inst
                && e.detectedTimeScale != TimeScale::Unknown) {
                detected = e.detectedTimeScale;
                break;
            }
        }

        populateTimeScaleCombo(combo, detected);
        _timeScaleCombos[inst] = combo;
        _tsOverrideLayout->addRow(inst + ":", combo);

        connect(combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
                this, &LightcurveImportPage::onTimeScaleOverrideChanged);
    }

    _tsOverrideGroup->setVisible(true);
}

void LightcurveImportPage::onTimeScaleOverrideChanged()
{
    // Reapply overrides and recompute BJD/MJD for affected entries
    for (auto it = _timeScaleCombos.constBegin();
         it != _timeScaleCombos.constEnd(); ++it)
    {
        QString inst = it.key();
        TimeScale newScale = comboTimeScale(it.value());
        _instrumentTimeScales[inst] = newScale;

        if (newScale == TimeScale::Unknown)
            continue;

        for (auto& entry : _entries) {
            if (entry.instrument != inst || entry.hasError)
                continue;

            entry.detectedTimeScale = newScale;
            for (auto& pt : entry.points) {
                pt.time = Time(pt.time.nativeValue(), newScale);
            }
        }
    }

    // Update displayed timescale column
    for (int i = 0; i < _resultsTree->topLevelItemCount(); ++i) {
        auto* item = _resultsTree->topLevelItem(i);
        int idx = item->data(0, Qt::UserRole).toInt();
        if (idx >= 0 && idx < static_cast<int>(_entries.size())) {
            item->setText(4, Time::scaleToString(
                _entries[idx].detectedTimeScale));
        }
    }
}

// ══════════════════════════════════════════════════════════════
// Selection helpers
// ══════════════════════════════════════════════════════════════

void LightcurveImportPage::onItemChanged(QTreeWidgetItem* item, int column)
{
    if (column != 0) return;
    int idx = item->data(0, Qt::UserRole).toInt();
    if (idx >= 0 && idx < static_cast<int>(_entries.size())) {
        _entries[idx].selected = (item->checkState(0) == Qt::Checked);
    }
    updateSummary();
    emit completeChanged();
}

void LightcurveImportPage::selectAll()
{
    _resultsTree->blockSignals(true);
    for (int i = 0; i < _resultsTree->topLevelItemCount(); ++i) {
        auto* item = _resultsTree->topLevelItem(i);
        int idx = item->data(0, Qt::UserRole).toInt();
        if (idx >= 0 && idx < static_cast<int>(_entries.size())
            && !_entries[idx].hasError) {
            item->setCheckState(0, Qt::Checked);
            _entries[idx].selected = true;
        }
    }
    _resultsTree->blockSignals(false);
    updateSummary();
    emit completeChanged();
}

void LightcurveImportPage::selectNone()
{
    _resultsTree->blockSignals(true);
    for (int i = 0; i < _resultsTree->topLevelItemCount(); ++i) {
        auto* item = _resultsTree->topLevelItem(i);
        int idx = item->data(0, Qt::UserRole).toInt();
        if (idx >= 0 && idx < static_cast<int>(_entries.size())) {
            item->setCheckState(0, Qt::Unchecked);
            _entries[idx].selected = false;
        }
    }
    _resultsTree->blockSignals(false);
    updateSummary();
    emit completeChanged();
}

void LightcurveImportPage::selectMatched()
{
    _resultsTree->blockSignals(true);
    for (int i = 0; i < _resultsTree->topLevelItemCount(); ++i) {
        auto* item = _resultsTree->topLevelItem(i);
        int idx = item->data(0, Qt::UserRole).toInt();
        if (idx >= 0 && idx < static_cast<int>(_entries.size())) {
            bool match = !_entries[idx].matchedStarId.isEmpty()
                         && !_entries[idx].hasError;
            item->setCheckState(0, match ? Qt::Checked : Qt::Unchecked);
            _entries[idx].selected = match;
        }
    }
    _resultsTree->blockSignals(false);
    updateSummary();
    emit completeChanged();
}

// ══════════════════════════════════════════════════════════════
// Summary
// ══════════════════════════════════════════════════════════════

void LightcurveImportPage::updateSummary()
{
    int total    = static_cast<int>(_entries.size());
    int selected = 0;
    int matched  = 0;
    int unmatched = 0;
    int errors   = 0;
    long long totalPoints = 0;

    for (const auto& e : _entries) {
        if (e.hasError) {
            ++errors;
            continue;
        }
        if (e.selected) {
            ++selected;
            totalPoints += e.numPoints;
            if (e.matchedStarId.isEmpty())
                ++unmatched;
            else
                ++matched;
        }
    }

    QString text = QString(
        "%1 files found, %2 selected (%3 matched, %4 unmatched), "
        "%5 errors, %L6 total points")
        .arg(total).arg(selected).arg(matched).arg(unmatched)
        .arg(errors).arg(totalPoints);

    if (unmatched > 0 && _createNewCb->isChecked()) {
        text += QString(" — %1 new star(s) will be created").arg(unmatched);
    }

    _summaryLabel->setText(text);
}

// ══════════════════════════════════════════════════════════════
// Staging
// ══════════════════════════════════════════════════════════════

void LightcurveImportPage::stageSelectedLightcurves()
{
    if (!_staging || _staged) return;

    auto* importWizard = qobject_cast<StarImportWizard*>(wizard());

    // Pull matched stars into staging area from DB if not already there
    if (importWizard && importWizard->controller()) {
        auto proj = importWizard->project();
        if (proj) {
            QStringList neededIds;
            for (const auto& entry : _entries) {
                if (entry.selected && !entry.hasError
                    && !entry.matchedStarId.isEmpty()
                    && !_staging->hasStar(entry.matchedStarId)) {
                    neededIds << entry.matchedStarId;
                }
            }
            neededIds.removeDuplicates();

            if (!neededIds.isEmpty()) {
                DatabaseManager* dbm = importWizard->controller()->databaseManager();
                _staging->pullStarsFromDB(dbm, proj->getId(), neededIds);
                _staging->pullPhotometryFromDB(dbm);
                LOG_INFO("LCImport",
                         QString("Pulled %1 stars into staging for LC import")
                             .arg(neededIds.size()));
            }
        }
    }

    int staged = 0;

    for (auto& entry : _entries) {
        if (!entry.selected || entry.hasError)
            continue;

        QString targetStarId = entry.matchedStarId;

        // Create new star for unmatched entries if requested
        if (targetStarId.isEmpty()) {
            if (!_createNewCb->isChecked())
                continue;

            // Check if we already created a star for this identifier
            bool alreadyCreated = false;
            for (const auto& other : _entries) {
                if (&other != &entry
                    && other.starIdentifier == entry.starIdentifier
                    && !other.matchedStarId.isEmpty()) {
                    targetStarId = other.matchedStarId;
                    entry.matchedStarId = targetStarId;
                    entry.matchedStarDisplay = other.matchedStarDisplay;
                    alreadyCreated = true;
                    break;
                }
            }

            if (!alreadyCreated) {
                auto newStar = std::make_shared<Star>();
                newStar->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
                newStar->setJName(entry.starIdentifier);

                // If the identifier looks like a Gaia source ID, set it
                bool isNumeric;
                entry.starIdentifier.toLongLong(&isNumeric);
                if (isNumeric)
                    newStar->setSourceId(entry.starIdentifier);

                _staging->addStar(newStar, /*isNew=*/true);
                targetStarId = newStar->getId();

                entry.matchedStarId = targetStarId;
                entry.matchedStarDisplay = entry.starIdentifier + " (new)";
            }
        }

        // Get the star's Photometry, create if needed
        auto star = _staging->getStar(targetStarId);
        if (!star) {
            LOG_WARNING("LCImport",
                        QString("Star %1 not found in staging area, skipping")
                            .arg(targetStarId));
            continue;
        }

        auto phot = star->getPhotometry();
        if (!phot) {
            phot = std::make_shared<Photometry>();
            phot->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
            star->setPhotometry(phot);
        }

        // Add lightcurve data via Photometry's existing API
        // source key = instrument name
        phot->addLightcurve(entry.instrument, entry.points);

        // Mark this star's lightcurve data as needing to be saved
        _staging->markLightcurveDirty(targetStarId);
        ++staged;
    }

    _staged = true;

    LOG_INFO("LCImport",
             QString("Staged %1 lightcurves for import").arg(staged));
}