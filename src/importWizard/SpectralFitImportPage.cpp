#include <charconv>
#include "../importWizard/SpectralFitImportPage.h"
#include "../importWizard/StarImportWizard.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include "models/Spectrum.h"
#include "../utils/Logger.h"
#include "../db/DatabaseManager.h"
#include "../utils/BackgroundTaskManager.h"

#include <limits>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
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
#include <QtConcurrent>
#include <QFutureWatcher>

#include <cstdlib>
#include <cstring>

namespace {

// Parse a double from [p, end). Advances p. Returns true on success.
inline bool parseDouble(const char*& p, const char* end, double& out)
{
    auto [ptr, ec] = std::from_chars(p, end, out);
    if (ec != std::errc{}) return false;
    p = ptr;
    return true;
}

inline bool parseLong(const char*& p, const char* end, long& out)
{
    auto [ptr, ec] = std::from_chars(p, end, out);
    if (ec != std::errc{}) return false;
    p = ptr;
    return true;
}

} // anonymous namespace

// ════════════════════════════════════════════════════════════════
// Construction & UI setup
// ════════════════════════════════════════════════════════════════

SpectralFitImportPage::SpectralFitImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import Spectral Fits");
    setSubTitle("Import model fit results and associate them with existing spectra");
    setupUi();
}

void SpectralFitImportPage::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // ── Mode selection ──────────────────────────────────────────
    QGroupBox* modeGroup = new QGroupBox("Import Method");
    QHBoxLayout* modeLayout = new QHBoxLayout;

    _diggaRadio   = new QRadioButton("Import DIGGA fits (folder scan)");
    _isisRadio    = new QRadioButton("Import ISIS fits");
    _mappingRadio = new QRadioButton("Import raw parameters (table)");
    _diggaRadio->setChecked(true);

    QButtonGroup* modeButtonGroup = new QButtonGroup(this);
    modeButtonGroup->addButton(_diggaRadio);
    modeButtonGroup->addButton(_isisRadio);
    modeButtonGroup->addButton(_mappingRadio);

    modeLayout->addWidget(_diggaRadio);
    modeLayout->addWidget(_isisRadio);
    modeLayout->addWidget(_mappingRadio);
    modeLayout->addStretch();
    modeGroup->setLayout(modeLayout);
    mainLayout->addWidget(modeGroup);

    connect(_diggaRadio,   &QRadioButton::toggled,
            this, &SpectralFitImportPage::onImportModeChanged);
    connect(_isisRadio,    &QRadioButton::toggled,
            this, &SpectralFitImportPage::onImportModeChanged);

    // ── Mode stack ──────────────────────────────────────────────
    _modeStack = new QStackedWidget;
    setupDiggaPage();
    setupIsisPage();
    setupMappingPage();
    _modeStack->addWidget(_diggaPage);    // index 0
    _modeStack->addWidget(_isisPage);     // index 1
    _modeStack->addWidget(_mappingPage);  // index 2
    mainLayout->addWidget(_modeStack);

    // ── Mark best fit ───────────────────────────────────────────
    _markBestFitCheck = new QCheckBox(
        "Auto-determine best fit per spectrum (lowest reduced χ²)");
    _markBestFitCheck->setToolTip(
        "For each spectrum, compares the reduced χ² of all newly imported fits\n"
        "and any existing fits. Only the fit with the lowest χ² is marked as best.\n"
        "If an existing fit already has a lower χ² than all new fits, no change is made.");
    _markBestFitCheck->setChecked(true);
    mainLayout->addWidget(_markBestFitCheck);

    // ── Preview ─────────────────────────────────────────────────
    QGroupBox* previewGroup = new QGroupBox("Preview");
    QVBoxLayout* previewLayout = new QVBoxLayout;

    _previewTree = new QTreeWidget;
    _previewTree->setHeaderLabels({
        "Directory / Spectrum", "Grid", "Star", "Parameters", "Status"
    });
    _previewTree->setAlternatingRowColors(true);
    _previewTree->setRootIsDecorated(true);
    _previewTree->header()->setStretchLastSection(true);
    previewLayout->addWidget(_previewTree);

    _statusLabel = new QLabel(
        "Select a root folder and scan for DIGGA output directories.");
    _statusLabel->setWordWrap(true);
    previewLayout->addWidget(_statusLabel);

    previewGroup->setLayout(previewLayout);
    mainLayout->addWidget(previewGroup);
}

void SpectralFitImportPage::setupDiggaPage()
{
    _diggaPage = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(_diggaPage);

    QGroupBox* folderGroup = new QGroupBox("DIGGA Output Root Folder");
    QVBoxLayout* folderLayout = new QVBoxLayout;

    QHBoxLayout* pathLayout = new QHBoxLayout;
    _diggaFolderEdit = new QLineEdit;
    _diggaFolderEdit->setPlaceholderText(
        "Select root folder containing DIGGA output directories...");
    pathLayout->addWidget(_diggaFolderEdit);

    QPushButton* browseBtn = new QPushButton("Browse...");
    connect(browseBtn, &QPushButton::clicked,
            this, &SpectralFitImportPage::onBrowseDiggaFolder);
    pathLayout->addWidget(browseBtn);
    folderLayout->addLayout(pathLayout);

    QLabel* helpLabel = new QLabel(
        "Recursively searches for subdirectories containing both "
        "<b>fit_parameters.csv</b> and <b>fit_report.tex</b>.");
    helpLabel->setWordWrap(true);
    folderLayout->addWidget(helpLabel);

    QHBoxLayout* scanLayout = new QHBoxLayout;
    _diggaScanButton = new QPushButton("Scan for DIGGA Outputs");
    _diggaScanButton->setEnabled(false);
    connect(_diggaScanButton, &QPushButton::clicked,
            this, &SpectralFitImportPage::onScanDigga);
    scanLayout->addWidget(_diggaScanButton);

    _diggaProgress = new QProgressBar;
    _diggaProgress->setVisible(false);
    scanLayout->addWidget(_diggaProgress);
    scanLayout->addStretch();
    folderLayout->addLayout(scanLayout);

    folderGroup->setLayout(folderLayout);
    layout->addWidget(folderGroup);
    layout->addStretch();
}

void SpectralFitImportPage::setupIsisPage()
{
    _isisPage = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(_isisPage);
    QLabel* stub = new QLabel(
        "<i>ISIS fit import is not yet implemented. "
        "This will be available in a future version.</i>");
    stub->setWordWrap(true);
    stub->setAlignment(Qt::AlignCenter);
    layout->addWidget(stub);
    layout->addStretch();
}

void SpectralFitImportPage::setupMappingPage()
{
    _mappingPage = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(_mappingPage);
    QLabel* stub = new QLabel(
        "<i>Raw parameter table import is not yet implemented. "
        "This will be available in a future version.</i>");
    stub->setWordWrap(true);
    stub->setAlignment(Qt::AlignCenter);
    layout->addWidget(stub);
    layout->addStretch();
}

// ════════════════════════════════════════════════════════════════
// Lifecycle
// ════════════════════════════════════════════════════════════════

void SpectralFitImportPage::initializePage()
{
    LOG_INFO("FitImport", "=== Initializing SpectralFitImportPage ===");

    StarImportWizard* importWizard = qobject_cast<StarImportWizard*>(wizard());
    if (!importWizard) {
        LOG_ERROR("FitImport", "Cannot cast wizard to StarImportWizard!");
        return;
    }

    auto controller = importWizard->controller();
    ImportStagingArea* staging = importWizard->stagingArea();
    DatabaseManager* dbm = controller->databaseManager();

    // Pull spectra from DB for any stars that don't have them yet.
    // Stars that already got spectra from SpectraImportPage will be skipped
    // inside pullSpectraFromDB (it checks hasSpectraLoaded).
    staging->pullSpectraFromDB(dbm);

    // Pull existing fits from DB for spectra that don't have them yet.
    staging->pullFitsFromDB(dbm);

    // Single source of truth
    _importedStars = staging->allStars();

    LOG_INFO("FitImport", QString("Using %1 stars for fit matching")
             .arg(_importedStars.size()));

    _asyncBusy  = false;
    _indexBuilt = false;
    _diggaDirs.clear();
    _previewTree->clear();

    // If spectra import is still running, tell the user to wait
    if (isSpectraImportRunning()) {
        _diggaScanButton->setEnabled(false);
        _statusLabel->setText(
            "⏳ Spectra import is still running in the background. "
            "Please wait for it to finish before scanning for fits.");

        auto* pollTimer = new QTimer(this);
        pollTimer->setInterval(500);
        connect(pollTimer, &QTimer::timeout, this, [this, pollTimer]() {
            if (!isSpectraImportRunning()) {
                pollTimer->stop();
                pollTimer->deleteLater();
                _specIndex = buildSpectrumLookupIndex();
                _indexBuilt = true;
                _diggaScanButton->setEnabled(!_diggaFolderEdit->text().trimmed().isEmpty());
                _statusLabel->setText(
                    QString("Ready — %1 stars, %2 spectra indexed.")
                    .arg(_specIndex.sourceIdIndex.size())
                    .arg(_specIndex.totalSpectra));
            }
        });
        pollTimer->start();
        return;
    }

    _specIndex = buildSpectrumLookupIndex();
    _indexBuilt = true;

    _statusLabel->setText(
        QString("Ready — %1 spectra indexed across %2 stars.")
        .arg(_specIndex.totalSpectra)
        .arg(_specIndex.starSpectraIndex.size()));
}


bool SpectralFitImportPage::isSpectraImportRunning() const
{
    StarImportWizard* importWizard = qobject_cast<StarImportWizard*>(wizard());
    if (!importWizard || !importWizard->controller())
        return false;

    auto* taskMgr = importWizard->controller()->backgroundTaskManager();
    return taskMgr && taskMgr->hasActiveTasks();
}


// ════════════════════════════════════════════════════════════════
// Index building
// ════════════════════════════════════════════════════════════════

SpectralFitImportPage::SpectrumIndex SpectralFitImportPage::buildSpectrumLookupIndex()
{
    LOG_INFO("FitImport", "=== Building spectrum lookup index ===");

    SpectrumIndex idx;
    QRegularExpression numericRe("(\\d{10,})");

    LOG_INFO("FitImport", QString("Indexing %1 stars").arg(_importedStars.size()));

    for (const auto& star : _importedStars) {
        QString starId   = star->getId();
        QString sourceId = star->getSourceId();
        QString alias    = star->getAlias();

        // Source-ID / alias index (multiple keys per star)
        if (!sourceId.isEmpty()) {
            idx.sourceIdIndex[sourceId] = star;
            idx.sourceIdIndex[sourceId.toLower()] = star;
            QRegularExpressionMatch m = numericRe.match(sourceId);
            if (m.hasMatch())
                idx.sourceIdIndex[m.captured(1)] = star;
        }
        if (!alias.isEmpty()) {
            idx.sourceIdIndex[alias] = star;
            idx.sourceIdIndex[alias.toLower()] = star;
        }
        if (!starId.isEmpty())
            idx.sourceIdIndex[starId] = star;

        // Spectra are already on the star objects (loaded by staging)
        auto spectra = star->getSpectra();
        if (spectra.empty())
            continue;

        idx.totalSpectra += static_cast<int>(spectra.size());
        idx.starSpectraIndex[starId] = spectra;

        // Filename index — multiple keys per spectrum
        for (const auto& spectrum : spectra) {
            QString rawFile = spectrum->getFile();
            if (rawFile.isEmpty()) continue;

            QFileInfo fi(rawFile);
            QString completeBase = fi.completeBaseName().toLower();
            QString base         = fi.baseName().toLower();
            QString fileName     = fi.fileName().toLower();

            auto pair = qMakePair(star, spectrum);
            if (!completeBase.isEmpty())
                idx.filenameIndex.insert(completeBase, pair);
            if (!base.isEmpty() && base != completeBase)
                idx.filenameIndex.insert(base, pair);
            if (!fileName.isEmpty() && fileName != completeBase)
                idx.filenameIndex.insert(fileName, pair);
        }
    }

    LOG_INFO("FitImport",
             QString("=== Index complete: %1 sourceId keys, %2 filename keys, "
                     "%3 spectra across %4 stars ===")
             .arg(idx.sourceIdIndex.size())
             .arg(idx.filenameIndex.size())
             .arg(idx.totalSpectra)
             .arg(idx.starSpectraIndex.size()));

    return idx;
}

// ════════════════════════════════════════════════════════════════
// Mode switching
// ════════════════════════════════════════════════════════════════

void SpectralFitImportPage::onImportModeChanged()
{
    if (_diggaRadio->isChecked())        _modeStack->setCurrentIndex(0);
    else if (_isisRadio->isChecked())    _modeStack->setCurrentIndex(1);
    else                                 _modeStack->setCurrentIndex(2);

    _previewTree->clear();
    _diggaDirs.clear();
}

// ════════════════════════════════════════════════════════════════
// DIGGA: folder selection
// ════════════════════════════════════════════════════════════════

void SpectralFitImportPage::onBrowseDiggaFolder()
{
    QString folder = QFileDialog::getExistingDirectory(
        this, "Select DIGGA Output Root Folder", _diggaFolderEdit->text());
    if (folder.isEmpty()) return;

    _diggaFolderEdit->setText(folder);
    _diggaScanButton->setEnabled(true);
    _diggaDirs.clear();
    _previewTree->clear();
    _statusLabel->setText("Click 'Scan for DIGGA Outputs' to search.");
}

// ════════════════════════════════════════════════════════════════
// DIGGA: async scan — ALL heavy work in background thread
// ════════════════════════════════════════════════════════════════

void SpectralFitImportPage::onScanDigga()
{
    if (_asyncBusy) return;

    if (isSpectraImportRunning()) {
        _statusLabel->setText(
            "⏳ Spectra import is still running. Please wait for it to finish.");
        return;
    }

    QString rootFolder = _diggaFolderEdit->text().trimmed();
    if (rootFolder.isEmpty()) return;

    _asyncBusy = true;
    _diggaScanButton->setEnabled(false);
    _diggaProgress->setVisible(true);
    _diggaProgress->setRange(0, 0);
    _diggaRootFolder = rootFolder;
    _statusLabel->setText("Scanning for DIGGA output directories...");

    // Capture the index for the background thread (it's a value type with
    // shared_ptrs inside, so this is a cheap refcount bump).
    SpectrumIndex indexSnapshot = _specIndex;

    auto future = QtConcurrent::run(
        [rootFolder, indexSnapshot]() mutable
            -> QPair<std::vector<DiggaFitDirectory>, int>
    {
        // ── Phase 1: Filesystem scan ────────────────────────────
        // Collect candidate dirs in one pass
        std::vector<DiggaScanResult> scanResults;

        QDirIterator it(rootFolder,
                        QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);

        // Also check root itself
        {
            QDir rootDir(rootFolder);
            if (rootDir.exists("fit_parameters.csv") &&
                rootDir.exists("fit_report.tex"))
            {
                DiggaScanResult scan;
                scan.dirPath = rootFolder;
                scan.gridName = rootDir.dirName();
                QDir parent(rootFolder); parent.cdUp();
                scan.parentDirName = parent.dirName();
                scanResults.push_back(std::move(scan));
            }
        }

        while (it.hasNext()) {
            QString dirPath = it.next();
            QDir dir(dirPath);
            if (!dir.exists("fit_parameters.csv") ||
                !dir.exists("fit_report.tex"))
                continue;

            DiggaScanResult scan;
            scan.dirPath   = dirPath;
            scan.gridName  = dir.dirName();
            QDir parent(dirPath); parent.cdUp();
            scan.parentDirName = parent.dirName();

            // Read the two small text files
            {  
                QFile f(dir.filePath("fit_report.tex"));
                if (f.open(QIODevice::ReadOnly)) {
                    scan.fitReportContent = f.readAll();
                } else {
                    scan.valid = false;
                    scan.error = "Cannot read fit_report.tex";
                }
            }
            {
                QFile f(dir.filePath("fit_parameters.csv"));
                if (f.open(QIODevice::ReadOnly)) {
                    scan.fitParametersContent = f.readAll();
                } else {
                    scan.valid = false;
                    scan.error = "Cannot read fit_parameters.csv";
                }
            }

            // Plotdata files
            const QStringList pdFiles =
                dir.entryList({"*_plotdata.csv"}, QDir::Files);
            for (const QString& pdf : pdFiles) {
                QString key = pdf;
                key.chop(static_cast<int>(QString("_plotdata.csv").length()));
                scan.plotdataFiles[key.toLower()] = dir.filePath(pdf);
            }

            scanResults.push_back(std::move(scan));
        }

        int scanCount = static_cast<int>(scanResults.size());

        // ── Phase 2: Parse all dirs in parallel ────────────────
        std::vector<DiggaFitDirectory> dirs =
            QtConcurrent::blockingMapped<std::vector<DiggaFitDirectory>>(
                scanResults,
                [](const DiggaScanResult& s) -> DiggaFitDirectory {
                    return parseDiggaDirectory(s);
                });

        // ── Phase 3: Match against index ────────────────────────
        matchDiggaDirectories(dirs, indexSnapshot);

        return qMakePair(std::move(dirs), scanCount);
    });

    auto* watcher = new QFutureWatcher<
        QPair<std::vector<DiggaFitDirectory>, int>>(this);

    connect(watcher,
            &QFutureWatcher<QPair<std::vector<DiggaFitDirectory>, int>>::finished,
            this, [this, watcher]()
    {
        auto result = watcher->result();
        watcher->deleteLater();

        _diggaDirs = std::move(result.first);
        int scanCount = result.second;

        _asyncBusy = false;
        _diggaScanButton->setEnabled(true);
        _diggaProgress->setVisible(false);

        LOG_INFO("FitImport",
                 QString("Scan complete: %1 DIGGA directories found")
                 .arg(scanCount));

        if (_diggaDirs.empty()) {
            _statusLabel->setText(
                "No DIGGA output directories found. Each directory must "
                "contain both fit_parameters.csv and fit_report.tex.");
            return;
        }

        updateDiggaPreviewTable();
    });

    watcher->setFuture(future);
}

// ════════════════════════════════════════════════════════════════
// DIGGA: parsing
// ════════════════════════════════════════════════════════════════

DiggaFitDirectory SpectralFitImportPage::parseDiggaDirectory(
    const DiggaScanResult& scan)
{
    DiggaFitDirectory dir;
    dir.dirPath       = scan.dirPath;
    dir.gridName      = scan.gridName;
    dir.parentDirName = scan.parentDirName;
    dir.plotdataFiles = scan.plotdataFiles;

    if (!scan.valid) {
        dir.parseOk    = false;
        dir.parseError = scan.error;
        return dir;
    }

    dir.specIndexToFilename = parseDiggaFitReport(scan.fitReportContent);
    if (dir.specIndexToFilename.isEmpty()) {
        dir.parseOk    = false;
        dir.parseError = "No spectrum identifiers found in fit_report.tex";
        return dir;
    }

    parseDiggaFitParameters(scan.fitParametersContent, dir);
    dir.totalSpectra = dir.specIndexToFilename.size();
    return dir;
}

QMap<int, QString> SpectralFitImportPage::parseDiggaFitReport(
    const QByteArray& contentBytes)
{
    QMap<int, QString> result;

    // Regex needs QString, but this file is small and parsed once per dir.
    const QString content = QString::fromUtf8(contentBytes);

    // Strategy 1: spec N & \verb|filename|
    {
        static const QRegularExpression re(
            R"(spec\s+(\d+)\s*&\s*\\verb\|([^|]+)\|)");
        auto matches = re.globalMatch(content);
        while (matches.hasNext()) {
            auto m = matches.next();
            result[m.captured(1).toInt()] = m.captured(2).trimmed();
        }
    }
    if (!result.isEmpty()) return result;

    // Strategy 2: spec N & filename
    {
        static const QRegularExpression re(
            R"(spec\s+(\d+)\s*&\s*([^\s&\\]+))");
        auto matches = re.globalMatch(content);
        while (matches.hasNext()) {
            auto m = matches.next();
            result[m.captured(1).toInt()] = m.captured(2).trimmed();
        }
    }
    if (!result.isEmpty()) return result;

    // Strategy 3: loose
    {
        static const QRegularExpression re(
            R"(spec(?:trum)?\s*(\d+)\s*[&:=\s]+\s*[\|\"']?([^\s\|\"'\\&]+\.\w+))");
        auto matches = re.globalMatch(content);
        while (matches.hasNext()) {
            auto m = matches.next();
            result[m.captured(1).toInt()] = m.captured(2).trimmed();
        }
    }

    if (result.isEmpty())
        LOG_WARNING("FitImport", "No spectrum identifiers found in fit_report.tex");

    return result;
}

void SpectralFitImportPage::parseDiggaFitParameters(
    const QByteArray& content, DiggaFitDirectory& dir)
{
    const char* p   = content.constData();
    const char* end = p + content.size();

    while (p < end && *p != '\n') ++p;
    if (p < end) ++p;

    auto skipWs = [](const char*& cur, const char* lim) {
        while (cur < lim && (*cur == ' ' || *cur == '\t')) ++cur;
    };

    while (p < end) {
        skipWs(p, end);

        const char* paramStart = p;
        while (p < end && *p != ',' && *p != '\n' && *p != '\r') ++p;
        const char* paramEnd = p;
        while (paramEnd > paramStart &&
               (paramEnd[-1] == ' ' || paramEnd[-1] == '\t'))
            --paramEnd;

        if (p >= end || *p != ',') {
            while (p < end && *p != '\n') ++p;
            if (p < end) ++p;
            continue;
        }
        ++p;
        skipWs(p, end);

        double value = 0.0;
        const bool okVal = parseDouble(p, end, value);
        skipWs(p, end);

        double error = 0.0;
        if (p < end && *p == ',') {
            ++p;
            skipWs(p, end);
            parseDouble(p, end, error);
        }

        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;

        if (!okVal) continue;

        const size_t pLen = static_cast<size_t>(paramEnd - paramStart);
        auto eq = [&](const char* lit, size_t litLen) {
            return pLen == litLen && std::memcmp(paramStart, lit, litLen) == 0;
        };

        if      (eq("final_chi2", 10)) { dir.chi2 = value; }
        else if (eq("c1_teff",  7))    { dir.teff  = value; dir.teffError  = error; }
        else if (eq("c1_logg",  7))    { dir.logg  = value; dir.loggError  = error; }
        else if (eq("c1_he",    5))    { dir.he    = value; dir.heError    = error; }
        else if (eq("c1_vsini", 8))    { dir.vsini = value; dir.vsiniError = error; }
        else if (eq("c1_zeta",  7))    { dir.zeta  = value; dir.zetaError  = error; }
        else if (eq("c1_xi",    5))    { dir.xi    = value; dir.xiError    = error; }
        else if (eq("c1_z",     4))    { dir.z     = value; dir.zError     = error; }
        else if (eq("c1_vrad",  7)) {
            dir.vradTied      = true;
            dir.tiedVrad      = value;
            dir.tiedVradError = error;
        }
        else if (pLen > 9 &&
                 std::memcmp(paramStart, "c1_vrad_d", 9) == 0) {
            long specIdx;
            const char* sp = paramStart + 9;
            if (parseLong(sp, paramEnd, specIdx) && sp == paramEnd) {
                dir.vradPerSpectrum[static_cast<int>(specIdx)] = { value, error };
            }
        }
    }
}

// ════════════════════════════════════════════════════════════════
// DIGGA: matching — static, no per-item logging
// ════════════════════════════════════════════════════════════════

void SpectralFitImportPage::matchDiggaDirectories(
    std::vector<DiggaFitDirectory>& dirs,
    const SpectrumIndex& index)
{
    LOG_INFO("FitImport", QString("Matching %1 directories against index "
             "(%2 filename keys, %3 sourceId keys)")
             .arg(dirs.size())
             .arg(index.filenameIndex.size())
             .arg(index.sourceIdIndex.size()));

    int totalMatched = 0;

    for (auto& dir : dirs) {
        dir.specMatches.clear();
        dir.matchedSpectra = 0;

        if (!dir.parseOk)
            continue;

        // ── Step 1: identify the star ───────────────────────────
        std::shared_ptr<Star> dirStar;

        // Try parent dir as sourceId
        auto trySourceId = [&](const QString& key) -> bool {
            auto it = index.sourceIdIndex.find(key);
            if (it != index.sourceIdIndex.end()) {
                dirStar = it.value();
                return true;
            }
            return false;
        };

        trySourceId(dir.parentDirName) ||
        trySourceId(dir.parentDirName.toLower()) ||
        trySourceId(dir.gridName) ||
        trySourceId(dir.gridName.toLower());

        // Probe filenames if star not found yet
        if (!dirStar) {
            for (auto it = dir.specIndexToFilename.cbegin();
                 !dirStar && it != dir.specIndexToFilename.cend(); ++it) {
                QFileInfo fi(it.value());
                for (const QString& key : {
                         fi.completeBaseName().toLower(),
                         fi.baseName().toLower(),
                         fi.fileName().toLower(),
                         it.value().toLower()}) {
                    auto fnIt = index.filenameIndex.find(key);
                    if (fnIt != index.filenameIndex.end()) {
                        dirStar = fnIt.value().first;
                        break;
                    }
                }
            }
        }

        // ── Step 2: match each spectrum ─────────────────────────
        for (auto it = dir.specIndexToFilename.cbegin();
             it != dir.specIndexToFilename.cend(); ++it)
        {
            int specIdx      = it.key();
            QString filename = it.value();

            DiggaFitDirectory::SpecMatch sm;
            sm.specIndex     = specIdx;
            sm.diggaFilename = filename;

            // Radial velocity
            if (dir.vradTied) {
                sm.vrad      = dir.tiedVrad;
                sm.vradError = dir.tiedVradError;
            } else if (dir.vradPerSpectrum.contains(specIdx)) {
                sm.vrad      = dir.vradPerSpectrum[specIdx].first;
                sm.vradError = dir.vradPerSpectrum[specIdx].second;
            }

            // Plotdata file lookup
            QFileInfo fi(filename);
            QString completeBase = fi.completeBaseName().toLower();
            QString base         = fi.baseName().toLower();
            QString fullName     = fi.fileName().toLower();

            if (dir.plotdataFiles.contains(completeBase))
                sm.plotdataFile = dir.plotdataFiles[completeBase];
            else if (dir.plotdataFiles.contains(base))
                sm.plotdataFile = dir.plotdataFiles[base];
            else if (dir.plotdataFiles.contains(fullName))
                sm.plotdataFile = dir.plotdataFiles[fullName];

            // Filename index match
            bool matched = false;
            for (const QString& key : {completeBase, base, fullName, filename.toLower()}) {
                auto fnIt = index.filenameIndex.find(key);
                if (fnIt != index.filenameIndex.end()) {
                    sm.matchedStar     = fnIt.value().first;
                    sm.matchedSpectrum = fnIt.value().second;
                    sm.matched         = true;
                    matched            = true;
                    if (!dirStar) dirStar = sm.matchedStar;
                    break;
                }
            }

            // Fallback: search within the star's own spectra
            if (!matched && dirStar) {
                auto sIt = index.starSpectraIndex.find(dirStar->getId());
                if (sIt != index.starSpectraIndex.end()) {
                    for (const auto& sp : sIt.value()) {
                        QFileInfo spFi(sp->getFile());
                        QString spCB = spFi.completeBaseName().toLower();
                        QString spB  = spFi.baseName().toLower();
                        QString spFN = spFi.fileName().toLower();

                        if (spCB == completeBase || spB == base ||
                            spB == completeBase || spCB == base ||
                            spFN == fullName)
                        {
                            sm.matchedStar     = dirStar;
                            sm.matchedSpectrum = sp;
                            sm.matched         = true;
                            matched            = true;
                            break;
                        }
                    }
                }
            }

            if (sm.matched) dir.matchedSpectra++;
            dir.specMatches.push_back(std::move(sm));
        }

        totalMatched += dir.matchedSpectra;
    }

    LOG_INFO("FitImport", QString("Matching complete: %1 total spectra matched")
             .arg(totalMatched));
}

// ════════════════════════════════════════════════════════════════
// Plotdata loading
// ════════════════════════════════════════════════════════════════

bool SpectralFitImportPage::loadPlotdata(
    const QString& filepath,
    std::vector<double>& wavelengths,
    std::vector<double>& modelFluxes,
    std::vector<double>& rebinnedFluxes,
    std::vector<double>& rebinnedSigmas,
    std::vector<double>& modelSplines,
    std::vector<uint8_t>& modelIgnore)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QByteArray data = file.readAll();
    file.close();

    wavelengths.clear();
    modelFluxes.clear();
    rebinnedFluxes.clear();
    rebinnedSigmas.clear();
    modelSplines.clear();
    modelIgnore.clear();

    const char* const begin = data.constData();
    const char* const end   = begin + data.size();

    // Newline count via raw pointer (no operator[] bounds check)
    size_t newlines = 0;
    for (const char* c = begin; c != end; ++c)
        if (*c == '\n') ++newlines;
    if (newlines > 1) {
        wavelengths.reserve(newlines);
        modelFluxes.reserve(newlines);
        rebinnedFluxes.reserve(newlines);
        rebinnedSigmas.reserve(newlines);
        modelSplines.reserve(newlines);
        modelIgnore.reserve(newlines);
    }

    const char* p = begin;

    // Skip header
    while (p < end && *p != '\n') ++p;
    if (p < end) ++p;

    auto skipToNextLine = [&]() {
        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;
    };

    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
            ++p;
        if (p >= end) break;

        double lambda, flux, sigma, model, spline;
        long   ignore;

        if (!parseDouble(p, end, lambda)) { skipToNextLine(); continue; }
        if (p < end && *p == ',') ++p;
        if (!parseDouble(p, end, flux))   { skipToNextLine(); continue; }
        if (p < end && *p == ',') ++p;
        if (!parseDouble(p, end, sigma))  { skipToNextLine(); continue; }
        if (p < end && *p == ',') ++p;
        if (!parseDouble(p, end, model))  { skipToNextLine(); continue; }
        if (p < end && *p == ',') ++p;
        if (!parseDouble(p, end, spline)) { skipToNextLine(); continue; }
        if (p < end && *p == ',') ++p;
        if (!parseLong  (p, end, ignore)) { skipToNextLine(); continue; }

        skipToNextLine();

        wavelengths.push_back(lambda);
        rebinnedFluxes.push_back(flux);
        rebinnedSigmas.push_back(sigma);
        modelFluxes.push_back(model);
        modelSplines.push_back(spline);
        modelIgnore.push_back(static_cast<uint8_t>(ignore));
    }

    return !wavelengths.empty();
}

// ════════════════════════════════════════════════════════════════
// Preview table — LIMITED rows, no expandAll on large datasets
// ════════════════════════════════════════════════════════════════

void SpectralFitImportPage::updateDiggaPreviewTable()
{
    _previewTree->clear();
    _previewTree->setUpdatesEnabled(false);

    static constexpr int MAX_PREVIEW_DIRS     = 200;
    static constexpr int MAX_CHILDREN_PER_DIR = 10;

    int totalDirs      = static_cast<int>(_diggaDirs.size());
    int fullyMatched   = 0;
    int partialMatched = 0;
    int unmatched      = 0;
    int totalSpecMatch = 0;
    int totalSpecAll   = 0;

    // First pass: compute totals (cheap, no widget work)
    for (const auto& dir : _diggaDirs) {
        totalSpecAll += dir.totalSpectra;
        totalSpecMatch += dir.matchedSpectra;

        if (!dir.parseOk || dir.matchedSpectra == 0)
            unmatched++;
        else if (dir.matchedSpectra == dir.totalSpectra)
            fullyMatched++;
        else
            partialMatched++;
    }

    // Second pass: build limited preview items
    int dirsShown = 0;
    for (const auto& dir : _diggaDirs) {
        if (dirsShown >= MAX_PREVIEW_DIRS) break;
        dirsShown++;

        QTreeWidgetItem* dirItem = new QTreeWidgetItem;

        // Column 0: relative path
        QString relPath = dir.dirPath;
        if (!_diggaRootFolder.isEmpty() &&
            relPath.startsWith(_diggaRootFolder)) {
            relPath = relPath.mid(_diggaRootFolder.length());
            if (relPath.startsWith('/') || relPath.startsWith('\\'))
                relPath = relPath.mid(1);
        }
        if (relPath.isEmpty()) relPath = dir.gridName;
        dirItem->setText(0, relPath);

        // Column 1: grid
        dirItem->setText(1, dir.gridName);

        // Column 2: star
        if (!dir.parseOk) {
            dirItem->setText(2, "(parse error)");
            dirItem->setForeground(2, QBrush(Qt::red));
        } else if (!dir.specMatches.empty() &&
                   dir.specMatches.front().matchedStar) {
            auto star = dir.specMatches.front().matchedStar;
            QString name = star->getAlias();
            if (name.isEmpty()) name = star->getSourceId();
            if (name.isEmpty()) name = star->getId();
            dirItem->setText(2, name);
        } else {
            dirItem->setText(2, "(no star found)");
            dirItem->setForeground(2, QBrush(Qt::red));
        }

        // Column 3: key parameters
        if (dir.parseOk) {
            QStringList params;
            if (dir.teff > 0)
                params << QString("Teff=%1").arg(dir.teff, 0, 'f', 0);
            if (dir.logg > 0)
                params << QString("logg=%1").arg(dir.logg, 0, 'f', 2);
            if (dir.he != 0)
                params << QString("He=%1").arg(dir.he, 0, 'f', 2);
            dirItem->setText(3, params.join(", "));
        }

        // Column 4: match status
        if (!dir.parseOk) {
            dirItem->setText(4, dir.parseError);
            dirItem->setForeground(4, QBrush(Qt::red));
        } else if (dir.matchedSpectra == dir.totalSpectra &&
                   dir.totalSpectra > 0) {
            dirItem->setText(4, QString("%1/%1 matched")
                                .arg(dir.totalSpectra));
            dirItem->setForeground(4, QBrush(QColor(0, 150, 0)));
        } else if (dir.matchedSpectra > 0) {
            dirItem->setText(4, QString("%1/%2 matched")
                                .arg(dir.matchedSpectra)
                                .arg(dir.totalSpectra));
            dirItem->setForeground(4, QBrush(QColor(200, 150, 0)));
        } else {
            dirItem->setText(4, QString("0/%1 matched")
                                .arg(dir.totalSpectra));
            dirItem->setForeground(4, QBrush(Qt::red));
        }

        // Children: limited count per dir
        int childrenShown = 0;
        for (const auto& sm : dir.specMatches) {
            if (childrenShown >= MAX_CHILDREN_PER_DIR) {
                int remaining = static_cast<int>(dir.specMatches.size()) - childrenShown;
                QTreeWidgetItem* moreItem = new QTreeWidgetItem;
                moreItem->setText(0, QString("... and %1 more spectra").arg(remaining));
                moreItem->setForeground(0, QBrush(Qt::gray));
                dirItem->addChild(moreItem);
                break;
            }
            childrenShown++;

            QTreeWidgetItem* specItem = new QTreeWidgetItem;
            specItem->setText(0, QString("spec %1: %2")
                                 .arg(sm.specIndex).arg(sm.diggaFilename));

            if (sm.matched && sm.matchedSpectrum) {
                QString specName = QFileInfo(sm.matchedSpectrum->getFile()).fileName();
                if (specName.isEmpty()) specName = sm.matchedSpectrum->getId();
                specItem->setText(2, specName);
            } else {
                specItem->setText(2, "(no match)");
                specItem->setForeground(2, QBrush(Qt::red));
            }

            specItem->setText(3, QString("vrad=%1±%2")
                                 .arg(sm.vrad, 0, 'f', 1)
                                 .arg(sm.vradError, 0, 'f', 1));

            QString status = sm.matched ? "✓" : "✗";
            status += sm.plotdataFile.isEmpty() ? " plotdata ✗" : " plotdata ✓";
            specItem->setText(4, status);
            if (!sm.matched)
                specItem->setForeground(4, QBrush(Qt::red));

            dirItem->addChild(specItem);
        }

        _previewTree->addTopLevelItem(dirItem);
    }

    // Only expand top-level items (don't expand children by default)
    // Users can click to expand individual dirs
    // DON'T call expandAll() — it's O(n) widget updates

    // Resize columns once with the limited dataset
    for (int i = 0; i < _previewTree->columnCount(); ++i)
        _previewTree->resizeColumnToContents(i);

    _previewTree->setUpdatesEnabled(true);

    // Status with full accurate counts (not limited by preview)
    QString statusText = QString(
        "Found %1 DIGGA directories — %2 fully matched, "
        "%3 partial, %4 unmatched (%5/%6 spectra matched)")
        .arg(totalDirs).arg(fullyMatched).arg(partialMatched)
        .arg(unmatched).arg(totalSpecMatch).arg(totalSpecAll);

    if (totalDirs > MAX_PREVIEW_DIRS)
        statusText += QString(" — showing first %1 directories").arg(MAX_PREVIEW_DIRS);

    _statusLabel->setText(statusText);
}

// ════════════════════════════════════════════════════════════════
// Import
// ════════════════════════════════════════════════════════════════

void SpectralFitImportPage::importDiggaFits()
{
    StarImportWizard* importWizard = qobject_cast<StarImportWizard*>(wizard());
    if (!importWizard || !importWizard->controller()) return;

    auto* controller = importWizard->controller();
    bool autoBest = _markBestFitCheck->isChecked();

    // Build import entries (don't mark best fit yet)
    std::vector<DiggaFitImportEntry> entries;

    for (const auto& dir : _diggaDirs) {
        if (!dir.parseOk) continue;

        for (const auto& sm : dir.specMatches) {
            if (!sm.matched || !sm.matchedStar || !sm.matchedSpectrum)
                continue;

            auto fit = std::make_shared<SpectralFit>();
            fit->teff                 = dir.teff;
            fit->teffError            = dir.teffError;
            fit->logg                 = dir.logg;
            fit->loggError            = dir.loggError;
            fit->he                   = dir.he;
            fit->heError              = dir.heError;
            fit->vsini                = dir.vsini;
            fit->vsiniError           = dir.vsiniError;
            fit->macroturbulence      = dir.zeta;
            fit->macroturbulenceError = dir.zetaError;
            fit->microturbulence      = dir.xi;
            fit->microturbulenceError = dir.xiError;
            fit->metallicity          = dir.z;
            fit->metallicityError     = dir.zError;
            fit->chi2                 = dir.chi2;
            fit->radialVelocity       = sm.vrad;
            fit->radialVelocityError  = sm.vradError;
            fit->modelId              = dir.gridName;

            DiggaFitImportEntry entry;
            entry.starId       = sm.matchedStar->getId();
            entry.spectrumId   = sm.matchedSpectrum->getId();
            entry.spectrum     = sm.matchedSpectrum;
            entry.fit          = fit;
            entry.plotdataPath = sm.plotdataFile;
            entries.push_back(std::move(entry));
        }
    }

    if (entries.empty()) {
        _statusLabel->setText("No matched fits to import.");
        return;
    }

    // Auto-determine best fit per spectrum (lowest reduced χ²)
    if (autoBest) {
        QHash<QString, std::vector<int>> specGroups;
        for (int i = 0; i < static_cast<int>(entries.size()); ++i)
            specGroups[entries[i].spectrumId].push_back(i);

        int bestCount = 0;

        for (auto it = specGroups.cbegin(); it != specGroups.cend(); ++it) {
            const auto& indices = it.value();

            // Start from the existing best fit's chi² (if any)
            double bestChi2 = std::numeric_limits<double>::max();
            auto existingBest = entries[indices.front()].spectrum->getBestFit();
            if (existingBest && existingBest->chi2 > 0.0)
                bestChi2 = existingBest->chi2;

            // Find the new fit with the lowest chi²
            int bestIdx = -1;
            for (int idx : indices) {
                double chi2 = entries[idx].fit->chi2;
                if (chi2 > 0.0 && chi2 < bestChi2) {
                    bestChi2 = chi2;
                    bestIdx = idx;
                }
            }

            // Only mark if a new fit actually beats everything existing
            if (bestIdx >= 0) {
                entries[bestIdx].fit->isBestFit = true;
                bestCount++;
            }
        }

        LOG_INFO("FitImport",
                 QString("Auto-best: marked %1 fits as best across %2 spectra")
                 .arg(bestCount).arg(specGroups.size()));
    }

    int count = static_cast<int>(entries.size());
    _statusLabel->setText(
        QString("Queued %1 spectral fits for background import.").arg(count));

    LOG_INFO("FitImport",
             QString("Queuing %1 fits for background import").arg(count));

    auto* task = new DiggaFitImportTask(std::move(entries), controller);
    task->setStagingArea(importWizard->stagingArea());
    controller->backgroundTaskManager()->queueTask(task);
}

// ════════════════════════════════════════════════════════════════
// Validation (Next button)
// ════════════════════════════════════════════════════════════════

bool SpectralFitImportPage::validatePage()
{
    if (_asyncBusy) {
        QMessageBox::information(this, "Processing",
            "Background scanning is still running. Please wait.");
        return false;
    }

    if (!_diggaRadio->isChecked())
        return true;

    if (_diggaDirs.empty()) {
        auto reply = QMessageBox::question(this, "No Fits Scanned",
            "No spectral fit directories have been scanned.\n\n"
            "Do you want to skip this step and continue?",
            QMessageBox::Yes | QMessageBox::No);
        return reply == QMessageBox::Yes;
    }

    int totalMatched = 0, totalUnmatched = 0, totalSpectra = 0;
    for (const auto& dir : _diggaDirs) {
        for (const auto& sm : dir.specMatches) {
            totalSpectra++;
            if (sm.matched) totalMatched++;
            else            totalUnmatched++;
        }
    }

    QString msg = QString(
        "%1 DIGGA directories scanned, %2 total spectra.\n\n"
        "• %3 spectra matched (will receive fit data)\n"
        "• %4 spectra unmatched (will be skipped)\n\n"
        "Import will run in the background. Continue?")
        .arg(_diggaDirs.size()).arg(totalSpectra)
        .arg(totalMatched).arg(totalUnmatched);

    if (QMessageBox::question(this, "Confirm Import", msg,
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return false;

    importDiggaFits();
    return true;
}

int SpectralFitImportPage::nextId() const
{
    return StarImportWizard::Page_RadialVelocity;
}