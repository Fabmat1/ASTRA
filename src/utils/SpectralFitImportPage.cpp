#include "SpectralFitImportPage.h"
#include "StarImportWizard.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include "models/Spectrum.h"
#include "Logger.h"
#include "utils/DatabaseManager.h"

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
        "Mark all imported fits as best fit (replaces existing best fit)");
    _markBestFitCheck->setChecked(false);
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

    _importedStars = importWizard->getImportedStars();
    LOG_INFO("FitImport", QString("Received %1 stars from wizard")
             .arg(_importedStars.size()));

    // Diagnostic: check what we actually have
    for (size_t i = 0; i < _importedStars.size() && i < 5; ++i) {
        auto& star = _importedStars[i];
        LOG_INFO("FitImport", QString("  Star[%1]: id='%2' sourceId='%3' alias='%4'")
                 .arg(i).arg(star->getId(), star->getSourceId(), star->getAlias()));

        // Force-trigger spectrum loading and check
        auto spectra = star->getSpectra();
        LOG_INFO("FitImport", QString("    → %1 spectra").arg(spectra.size()));
        for (size_t j = 0; j < spectra.size() && j < 3; ++j) {
            LOG_INFO("FitImport", QString("      Spectrum[%1]: id='%2' file='%3'")
                     .arg(j).arg(spectra[j]->getId(), spectra[j]->getFile()));
        }
        if (spectra.size() > 3)
            LOG_INFO("FitImport", QString("      ... and %1 more")
                     .arg(spectra.size() - 3));
    }
    if (_importedStars.size() > 5)
        LOG_INFO("FitImport", QString("  ... and %1 more stars")
                 .arg(_importedStars.size() - 5));

    _asyncBusy  = false;
    _indexBuilt = false;
    _diggaDirs.clear();
    _previewTree->clear();

    buildSpectrumLookupIndex();

    int totalSpectra = 0;
    for (auto it = _starSpectraIndex.cbegin(); it != _starSpectraIndex.cend(); ++it)
        totalSpectra += static_cast<int>(it.value().size());

    _statusLabel->setText(
        QString("Ready to import spectral fits for %1 stars (%2 spectra). "
                "Select a folder and scan.")
        .arg(_importedStars.size()).arg(totalSpectra));
}

void SpectralFitImportPage::buildSpectrumLookupIndex()
{
    // Always rebuild — don't cache, since spectra may have been
    // added to DB between initializePage() and scan time
    LOG_INFO("FitImport", "=== Building spectrum lookup indices ===");

    _filenameIndex.clear();
    _sourceIdIndex.clear();
    _starSpectraIndex.clear();
    _indexBuilt = false;

    QRegularExpression numericRe("(\\d{10,})");

    // ── Get database access ─────────────────────────────────────
    StarImportWizard* importWizard = qobject_cast<StarImportWizard*>(wizard());
    DatabaseManager* dbManager = nullptr;
    if (importWizard && importWizard->controller()) {
        dbManager = importWizard->controller()->databaseManager();
    }

    // ── Strategy 1: Use _importedStars from wizard ──────────────
    LOG_INFO("FitImport", QString("Wizard provided %1 imported stars")
             .arg(_importedStars.size()));

    // ── Strategy 2: Also load ALL stars from the current project ─
    //    This catches stars that were saved to DB but whose
    //    in-memory Star objects don't have spectra attached yet.
    std::vector<std::shared_ptr<Star>> allStars;

    if (dbManager) {
        auto project = importWizard->project();
        if (project) {
            allStars = dbManager->loadStars(project->getId());
            LOG_INFO("FitImport", QString("DB returned %1 stars for project '%2'")
                     .arg(allStars.size()).arg(project->getId()));
        }
    }

    // Merge: wizard stars + DB stars (DB stars take priority since
    // they're freshly loaded and we can query their spectra)
    QHash<QString, std::shared_ptr<Star>> starById;

    // Add wizard stars first
    for (const auto& star : _importedStars) {
        starById[star->getId()] = star;
    }
    // Overwrite with DB stars (they have correct IDs for DB queries)
    for (const auto& star : allStars) {
        starById[star->getId()] = star;
    }

    LOG_INFO("FitImport", QString("Total unique stars to index: %1 "
             "(wizard=%2, DB=%3)")
             .arg(starById.size())
             .arg(_importedStars.size())
             .arg(allStars.size()));

    int starIdx = 0;
    int totalSpectraFound = 0;

    for (auto it = starById.cbegin(); it != starById.cend(); ++it) {
        starIdx++;
        const auto& star = it.value();
        QString starId   = star->getId();
        QString sourceId = star->getSourceId();
        QString alias    = star->getAlias();

        LOG_DEBUG("FitImport", QString("Star %1/%2: id='%3' sourceId='%4' alias='%5'")
                  .arg(starIdx).arg(starById.size())
                  .arg(starId, sourceId, alias));

        // ── Source-ID / alias / starId index ────────────────────
        if (!sourceId.isEmpty()) {
            _sourceIdIndex[sourceId] = star;
            // Also store the pure numeric part (Gaia DR3 → just the number)
            QRegularExpressionMatch m = numericRe.match(sourceId);
            if (m.hasMatch())
                _sourceIdIndex[m.captured(1)] = star;
            // Store lowercased version too
            _sourceIdIndex[sourceId.toLower()] = star;
        }
        if (!alias.isEmpty()) {
            _sourceIdIndex[alias] = star;
            _sourceIdIndex[alias.toLower()] = star;
        }
        if (!starId.isEmpty()) {
            _sourceIdIndex[starId] = star;
        }

        // ── Load spectra: try in-memory, then DB ────────────────
        auto spectra = star->getSpectra();
        LOG_DEBUG("FitImport", QString("  getSpectra() returned %1 spectra")
                  .arg(spectra.size()));

        // DB fallback
        if (spectra.empty() && dbManager) {
            LOG_DEBUG("FitImport", QString("  DB fallback: loadSpectra('%1')")
                      .arg(starId));
            spectra = dbManager->loadSpectra(starId);
            LOG_DEBUG("FitImport", QString("  DB fallback returned %1 spectra")
                      .arg(spectra.size()));

            // Attach to star
            if (!spectra.empty()) {
                for (const auto& sp : spectra) {
                    star->addSpectrum(sp);
                }
                LOG_INFO("FitImport", QString("  Attached %1 spectra from DB to star '%2'")
                         .arg(spectra.size()).arg(starId));
            }
        }

        if (spectra.empty()) {
            LOG_WARNING("FitImport", QString("  Star '%1' (sourceId='%2') has 0 spectra!")
                        .arg(starId, sourceId));
            continue;
        }

        totalSpectraFound += static_cast<int>(spectra.size());
        _starSpectraIndex[starId] = spectra;

        // ── Filename index ──────────────────────────────────────
        for (const auto& spectrum : spectra) {
            QString rawFile = spectrum->getFile();
            QString specId  = spectrum->getId();

            if (rawFile.isEmpty()) {
                LOG_WARNING("FitImport", QString("    Spectrum '%1' has empty file path!")
                            .arg(specId));
                continue;
            }

            QFileInfo fi(rawFile);
            QString completeBaseName = fi.completeBaseName().toLower();
            QString baseName         = fi.baseName().toLower();
            QString fileName         = fi.fileName().toLower();

            LOG_DEBUG("FitImport",
                      QString("    Spectrum id='%1' file='%2' → keys: "
                              "complete='%3' base='%4' filename='%5'")
                      .arg(specId, rawFile, completeBaseName, baseName, fileName));

            if (!completeBaseName.isEmpty())
                _filenameIndex[completeBaseName] = {star, spectrum};
            if (!baseName.isEmpty() && baseName != completeBaseName)
                _filenameIndex[baseName] = {star, spectrum};
            if (!fileName.isEmpty() && fileName != completeBaseName)
                _filenameIndex[fileName] = {star, spectrum};
        }
    }

    _indexBuilt = true;

    LOG_INFO("FitImport",
             QString("=== Index complete: %1 sourceId entries, %2 filename entries, "
                     "%3 spectra across %4 stars ===")
             .arg(_sourceIdIndex.size())
             .arg(_filenameIndex.size())
             .arg(totalSpectraFound)
             .arg(_starSpectraIndex.size()));

    if (_filenameIndex.isEmpty()) {
        LOG_ERROR("FitImport",
            "Filename index is EMPTY! No spectra could be loaded from "
            "any source. Check that spectra were saved to DB before "
            "reaching this page, and that loadSpectra() works correctly.");
    }

    // Dump for debugging
    LOG_DEBUG("FitImport", "--- Filename index keys ---");
    for (auto it = _filenameIndex.cbegin(); it != _filenameIndex.cend(); ++it) {
        LOG_DEBUG("FitImport", QString("  '%1' → star='%2' spectrum='%3' file='%4'")
                  .arg(it.key(),
                       it.value().first->getId(),
                       it.value().second->getId(),
                       it.value().second->getFile()));
    }

    LOG_DEBUG("FitImport", "--- SourceId index keys ---");
    for (auto it = _sourceIdIndex.cbegin(); it != _sourceIdIndex.cend(); ++it) {
        LOG_DEBUG("FitImport", QString("  '%1' → star='%2'")
                  .arg(it.key(), it.value()->getId()));
    }
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
// DIGGA: async scan
// ════════════════════════════════════════════════════════════════

void SpectralFitImportPage::onScanDigga()
{
    if (_asyncBusy) return;

    QString rootFolder = _diggaFolderEdit->text().trimmed();
    if (rootFolder.isEmpty()) return;

    _asyncBusy = true;
    _diggaScanButton->setEnabled(false);
    _diggaProgress->setVisible(true);
    _diggaProgress->setRange(0, 0);   // indeterminate
    _diggaRootFolder = rootFolder;
    _statusLabel->setText("Scanning for DIGGA output directories...");

    // ── Background: recursive filesystem scan + file reading ────
    auto future = QtConcurrent::run(
        [rootFolder]() -> std::vector<DiggaScanResult>
    {
        std::vector<DiggaScanResult> results;

        // Collect all directories to check (root + recursive children)
        QStringList dirsToCheck;
        dirsToCheck << rootFolder;

        QDirIterator it(rootFolder,
                        QDir::Dirs | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext())
            dirsToCheck << it.next();

        for (const QString& dirPath : dirsToCheck) {
            QDir dir(dirPath);
            if (!dir.exists("fit_parameters.csv") ||
                !dir.exists("fit_report.tex"))
                continue;

            DiggaScanResult scan;
            scan.dirPath       = dirPath;
            scan.gridName      = dir.dirName();

            QDir parentDir(dirPath);
            parentDir.cdUp();
            scan.parentDirName = parentDir.dirName();

            // Read fit_report.tex
            {
                QFile f(dir.filePath("fit_report.tex"));
                if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    scan.fitReportContent = QTextStream(&f).readAll();
                    f.close();
                } else {
                    scan.valid = false;
                    scan.error = "Cannot read fit_report.tex";
                }
            }

            // Read fit_parameters.csv
            {
                QFile f(dir.filePath("fit_parameters.csv"));
                if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    scan.fitParametersContent = QTextStream(&f).readAll();
                    f.close();
                } else {
                    scan.valid = false;
                    scan.error = "Cannot read fit_parameters.csv";
                }
            }

            // Find *_plotdata.csv files
            const QStringList pdFiles =
                dir.entryList({"*_plotdata.csv"}, QDir::Files);
            for (const QString& pdf : pdFiles) {
                // key = basename with _plotdata.csv stripped, lowered
                QString key = pdf;
                key.chop(static_cast<int>(
                    QString("_plotdata.csv").length()));
                scan.plotdataFiles[key.toLower()] = dir.filePath(pdf);
            }

            results.push_back(std::move(scan));
        }

        return results;
    });

    auto* watcher =
        new QFutureWatcher<std::vector<DiggaScanResult>>(this);

    connect(watcher,
            &QFutureWatcher<std::vector<DiggaScanResult>>::finished,
            this, [this, watcher]() {
                auto results = watcher->result();
                watcher->deleteLater();

                // ── Back on main thread: parse + match + display ────
                _asyncBusy = false;
                _diggaScanButton->setEnabled(true);
                _diggaProgress->setVisible(false);

                LOG_INFO("FitImport",
                         QString("Scan found %1 DIGGA output directories")
                         .arg(results.size()));

                if (results.empty()) {
                    _statusLabel->setText(
                        "No DIGGA output directories found. Each directory "
                        "must contain both fit_parameters.csv and "
                        "fit_report.tex.");
                    return;
                }

                // Parse all scan results
                _diggaDirs.clear();
                _diggaDirs.reserve(results.size());
                for (const auto& scan : results) {
                    _diggaDirs.push_back(parseDiggaDirectory(scan));
                }

                // Match spectra to DB
                buildSpectrumLookupIndex();
                matchDiggaDirectories();

                // Update UI
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

    // Parse fit_report.tex → spec index to filename mapping
    dir.specIndexToFilename = parseDiggaFitReport(scan.fitReportContent);
    if (dir.specIndexToFilename.isEmpty()) {
        dir.parseOk    = false;
        dir.parseError = "No spectrum identifiers found in fit_report.tex";
        return dir;
    }

    // Parse fit_parameters.csv → shared + per-spectrum parameters
    parseDiggaFitParameters(scan.fitParametersContent, dir);

    dir.totalSpectra = dir.specIndexToFilename.size();
    return dir;
}

QMap<int, QString> SpectralFitImportPage::parseDiggaFitReport(
    const QString& content)
{
    QMap<int, QString> result;

    LOG_INFO("FitImport", "=== Parsing fit_report.tex ===");
    LOG_DEBUG("FitImport", QString("fit_report.tex content length: %1 chars")
              .arg(content.length()));

    // Log first ~500 chars so we can see the actual format
    LOG_DEBUG("FitImport", QString("fit_report.tex preview:\n%1")
              .arg(content.left(800)));

    // Strategy 1: spec N & \verb|filename|
    {
        static QRegularExpression re(
            R"(spec\s+(\d+)\s*&\s*\\verb\|([^|]+)\|)");
        auto matches = re.globalMatch(content);
        while (matches.hasNext()) {
            auto m  = matches.next();
            int idx = m.captured(1).toInt();
            QString fn = m.captured(2).trimmed();
            result[idx] = fn;
            LOG_DEBUG("FitImport", QString("  Pattern 1 match: spec %1 → '%2'")
                      .arg(idx).arg(fn));
        }
    }

    if (!result.isEmpty()) {
        LOG_INFO("FitImport", QString("Pattern 1 (\\verb) matched %1 spectra")
                 .arg(result.size()));
        return result;
    }

    // Strategy 2: spec N & filename (no \verb)
    {
        static QRegularExpression re(
            R"(spec\s+(\d+)\s*&\s*([^\s&\\]+))");
        auto matches = re.globalMatch(content);
        while (matches.hasNext()) {
            auto m  = matches.next();
            int idx = m.captured(1).toInt();
            QString fn = m.captured(2).trimmed();
            result[idx] = fn;
            LOG_DEBUG("FitImport", QString("  Pattern 2 match: spec %1 → '%2'")
                      .arg(idx).arg(fn));
        }
    }

    if (!result.isEmpty()) {
        LOG_INFO("FitImport", QString("Pattern 2 (plain) matched %1 spectra")
                 .arg(result.size()));
        return result;
    }

    // Strategy 3: look for any line containing "spec" and a number
    // followed by a filename-like string
    {
        static QRegularExpression re(
            R"(spec(?:trum)?\s*(\d+)\s*[&:=\s]+\s*[\|\"']?([^\s\|\"'\\&]+\.\w+))");
        auto matches = re.globalMatch(content);
        while (matches.hasNext()) {
            auto m  = matches.next();
            int idx = m.captured(1).toInt();
            QString fn = m.captured(2).trimmed();
            result[idx] = fn;
            LOG_DEBUG("FitImport", QString("  Pattern 3 match: spec %1 → '%2'")
                      .arg(idx).arg(fn));
        }
    }

    if (!result.isEmpty()) {
        LOG_INFO("FitImport", QString("Pattern 3 (loose) matched %1 spectra")
                 .arg(result.size()));
    } else {
        LOG_WARNING("FitImport",
            "No spectrum identifiers found with any pattern! "
            "Check the fit_report.tex format.");

        // Log all lines containing "spec" for diagnostics
        QStringList lines = content.split('\n');
        for (const QString& line : lines) {
            if (line.contains("spec", Qt::CaseInsensitive)) {
                LOG_DEBUG("FitImport", QString("  Line with 'spec': %1")
                          .arg(line.trimmed()));
            }
        }
    }

    return result;
}

void SpectralFitImportPage::parseDiggaFitParameters(
    const QString& content, DiggaFitDirectory& dir)
{
    LOG_INFO("FitImport", QString("=== Parsing fit_parameters.csv for '%1' ===")
             .arg(dir.dirPath));
    LOG_DEBUG("FitImport", QString("Content length: %1 chars, preview:\n%2")
              .arg(content.length()).arg(content.left(500)));

    static QRegularExpression vradUntiedRe(R"(^c1_vrad_d(\d+)$)");

    QTextStream stream(const_cast<QString*>(&content), QIODevice::ReadOnly);
    QString header = stream.readLine();
    LOG_DEBUG("FitImport", QString("Header line: '%1'").arg(header));

    int lineCount = 0;
    int parsedCount = 0;

    while (!stream.atEnd()) {
        QString line = stream.readLine().trimmed();
        if (line.isEmpty()) continue;
        lineCount++;

        QStringList parts = line.split(',');
        if (parts.size() < 3) {
            LOG_DEBUG("FitImport", QString("  Skipping short line: '%1'").arg(line));
            continue;
        }

        QString param = parts[0].trimmed();
        bool okVal, okErr;
        double value = parts[1].trimmed().toDouble(&okVal);
        double error = parts[2].trimmed().toDouble(&okErr);

        if (!okVal) {
            LOG_DEBUG("FitImport", QString("  Cannot parse value for param '%1': '%2'")
                      .arg(param, parts[1].trimmed()));
            continue;
        }
        if (!okErr) error = 0.0;

        parsedCount++;

        if      (param == "final_chi2") { dir.chi2 = value; }
        else if (param == "c1_teff")    { dir.teff = value; dir.teffError = error; }
        else if (param == "c1_logg")    { dir.logg = value; dir.loggError = error; }
        else if (param == "c1_he")      { dir.he   = value; dir.heError   = error; }
        else if (param == "c1_vsini")   { dir.vsini = value; dir.vsiniError = error; }
        else if (param == "c1_zeta")    { dir.zeta  = value; dir.zetaError  = error; }
        else if (param == "c1_xi")      { dir.xi    = value; dir.xiError    = error; }
        else if (param == "c1_z")       { dir.z     = value; dir.zError     = error; }
        else if (param == "c1_vrad") {
            dir.vradTied      = true;
            dir.tiedVrad      = value;
            dir.tiedVradError = error;
        }
        else {
            QRegularExpressionMatch vm = vradUntiedRe.match(param);
            if (vm.hasMatch()) {
                int specIdx = vm.captured(1).toInt();
                dir.vradPerSpectrum[specIdx] = {value, error};
            }
        }
    }

    LOG_INFO("FitImport", QString("Parsed %1 parameters from %2 lines. "
             "teff=%3 logg=%4 chi2=%5 vradTied=%6")
             .arg(parsedCount).arg(lineCount)
             .arg(dir.teff).arg(dir.logg).arg(dir.chi2)
             .arg(dir.vradTied ? "yes" : "no"));
}

// ════════════════════════════════════════════════════════════════
// DIGGA: matching to DB spectra
// ════════════════════════════════════════════════════════════════

void SpectralFitImportPage::matchDiggaDirectories()
{
    LOG_INFO("FitImport", "=== Matching DIGGA directories to DB spectra ===");

    for (auto& dir : _diggaDirs) {
        dir.specMatches.clear();
        dir.matchedSpectra = 0;

        LOG_INFO("FitImport", QString("--- Directory: '%1' (grid='%2' parent='%3') ---")
                 .arg(dir.dirPath, dir.gridName, dir.parentDirName));

        if (!dir.parseOk) {
            LOG_WARNING("FitImport", QString("  Skipping: parse error '%1'")
                        .arg(dir.parseError));
            continue;
        }

        LOG_DEBUG("FitImport", QString("  specIndexToFilename has %1 entries")
                  .arg(dir.specIndexToFilename.size()));
        for (auto it = dir.specIndexToFilename.cbegin();
             it != dir.specIndexToFilename.cend(); ++it) {
            LOG_DEBUG("FitImport", QString("    spec %1 → '%2'")
                      .arg(it.key()).arg(it.value()));
        }

        LOG_DEBUG("FitImport", QString("  plotdataFiles has %1 entries")
                  .arg(dir.plotdataFiles.size()));
        for (auto it = dir.plotdataFiles.cbegin();
             it != dir.plotdataFiles.cend(); ++it) {
            LOG_DEBUG("FitImport", QString("    key='%1' → '%2'")
                      .arg(it.key(), it.value()));
        }

        // ── Step 1: identify the star ───────────────────────────
        std::shared_ptr<Star> dirStar;

        // Try parent directory name as source_id
        LOG_DEBUG("FitImport", QString("  Trying parent dir '%1' as sourceId...")
                  .arg(dir.parentDirName));
        {
            auto it = _sourceIdIndex.find(dir.parentDirName);
            if (it != _sourceIdIndex.end()) {
                dirStar = it.value();
                LOG_INFO("FitImport", QString("  ✓ Star found via parent dir: '%1'")
                         .arg(dirStar->getId()));
            } else {
                LOG_DEBUG("FitImport", "  ✗ Not found in sourceId index");
            }
        }

        // Also try lowered parent dir
        if (!dirStar) {
            auto it = _sourceIdIndex.find(dir.parentDirName.toLower());
            if (it != _sourceIdIndex.end()) {
                dirStar = it.value();
                LOG_INFO("FitImport", QString("  ✓ Star found via lowered parent dir: '%1'")
                         .arg(dirStar->getId()));
            }
        }

        // Also try grid name as star identifier
        if (!dirStar) {
            LOG_DEBUG("FitImport", QString("  Trying grid name '%1' as sourceId...")
                      .arg(dir.gridName));
            auto it = _sourceIdIndex.find(dir.gridName);
            if (it != _sourceIdIndex.end()) {
                dirStar = it.value();
                LOG_INFO("FitImport", QString("  ✓ Star found via grid name: '%1'")
                         .arg(dirStar->getId()));
            }
        }

        // Probe spectrum filenames to find the star
        if (!dirStar) {
            LOG_DEBUG("FitImport", "  Probing spectrum filenames for star match...");
            for (auto it = dir.specIndexToFilename.cbegin();
                 it != dir.specIndexToFilename.cend(); ++it) {

                QString rawFn = it.value();
                QFileInfo fi(rawFn);
                QString completeBase = fi.completeBaseName().toLower();
                QString base         = fi.baseName().toLower();
                QString fullName     = fi.fileName().toLower();

                LOG_DEBUG("FitImport",
                          QString("    Probing spec %1: raw='%2' "
                                  "completeBase='%3' base='%4' fileName='%5'")
                          .arg(it.key()).arg(rawFn, completeBase, base, fullName));

                // Try each variant
                for (const QString& key : {completeBase, base, fullName, rawFn.toLower()}) {
                    auto fnIt = _filenameIndex.find(key);
                    if (fnIt != _filenameIndex.end()) {
                        dirStar = fnIt.value().first;
                        LOG_INFO("FitImport",
                                 QString("    ✓ Star found via filename key '%1': star='%2'")
                                 .arg(key, dirStar->getId()));
                        break;
                    }
                }
                if (dirStar) break;
            }
        }

        if (!dirStar) {
            LOG_WARNING("FitImport",
                "  ✗ Could not identify star for this directory by any method!");
        }

        // ── Step 2: match each spectrum ─────────────────────────
        for (auto it = dir.specIndexToFilename.cbegin();
             it != dir.specIndexToFilename.cend(); ++it)
        {
            int specIdx      = it.key();
            QString filename = it.value();

            LOG_DEBUG("FitImport", QString("  Matching spec %1: '%2'")
                      .arg(specIdx).arg(filename));

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

            // Plotdata file
            QFileInfo fi(filename);
            QString completeBase = fi.completeBaseName().toLower();
            QString base         = fi.baseName().toLower();
            QString fullName     = fi.fileName().toLower();

            // Try plotdata with various key forms
            if (dir.plotdataFiles.contains(completeBase)) {
                sm.plotdataFile = dir.plotdataFiles[completeBase];
            } else if (dir.plotdataFiles.contains(base)) {
                sm.plotdataFile = dir.plotdataFiles[base];
            } else if (dir.plotdataFiles.contains(fullName)) {
                sm.plotdataFile = dir.plotdataFiles[fullName];
            }

            LOG_DEBUG("FitImport",
                      QString("    Keys: completeBase='%1' base='%2' fileName='%3' plotdata='%4'")
                      .arg(completeBase, base, fullName,
                           sm.plotdataFile.isEmpty() ? "(none)" : "found"));

            // ── Attempt matching via filename index ─────────────
            bool matched = false;
            for (const QString& key : {completeBase, base, fullName, filename.toLower()}) {
                auto fnIt = _filenameIndex.find(key);
                if (fnIt != _filenameIndex.end()) {
                    sm.matchedStar     = fnIt.value().first;
                    sm.matchedSpectrum = fnIt.value().second;
                    sm.matched         = true;
                    matched            = true;
                    LOG_INFO("FitImport",
                             QString("    ✓ Matched via filename key '%1' → "
                                     "star='%2' spectrum='%3' (file='%4')")
                             .arg(key,
                                  sm.matchedStar->getId(),
                                  sm.matchedSpectrum->getId(),
                                  sm.matchedSpectrum->getFile()));
                    if (!dirStar) dirStar = sm.matchedStar;
                    break;
                }
            }

            // ── Fallback: search within the star's spectra ──────
            if (!matched && dirStar) {
                LOG_DEBUG("FitImport", "    Trying fallback: per-star spectrum search...");
                auto sIt = _starSpectraIndex.find(dirStar->getId());
                if (sIt != _starSpectraIndex.end()) {
                    for (const auto& sp : sIt.value()) {
                        QFileInfo spFi(sp->getFile());
                        QString spCompleteBase = spFi.completeBaseName().toLower();
                        QString spBase         = spFi.baseName().toLower();
                        QString spFileName     = spFi.fileName().toLower();

                        LOG_DEBUG("FitImport",
                                  QString("      Comparing with spectrum '%1': "
                                          "completeBase='%2' base='%3' fileName='%4'")
                                  .arg(sp->getId(), spCompleteBase, spBase, spFileName));

                        if (spCompleteBase == completeBase ||
                            spBase == base ||
                            spBase == completeBase ||
                            spCompleteBase == base ||
                            spFileName == fullName) {
                            sm.matchedStar     = dirStar;
                            sm.matchedSpectrum = sp;
                            sm.matched         = true;
                            matched            = true;
                            LOG_INFO("FitImport",
                                     QString("    ✓ Matched via fallback star search → "
                                             "spectrum='%1' (file='%2')")
                                     .arg(sp->getId(), sp->getFile()));
                            break;
                        }
                    }
                } else {
                    LOG_DEBUG("FitImport", "    Star has no spectra in _starSpectraIndex");
                }
            }

            if (!matched) {
                LOG_WARNING("FitImport",
                            QString("    ✗ No match for spec %1 '%2'")
                            .arg(specIdx).arg(filename));
            }

            if (sm.matched) dir.matchedSpectra++;
            dir.specMatches.push_back(std::move(sm));
        }

        LOG_INFO("FitImport", QString("  Result: %1/%2 spectra matched")
                 .arg(dir.matchedSpectra).arg(dir.totalSpectra));
    }
}

// ════════════════════════════════════════════════════════════════
// Plotdata loading
// ════════════════════════════════════════════════════════════════

bool SpectralFitImportPage::loadPlotdata(
    const QString& filepath,
    std::vector<double>& wavelengths,
    std::vector<double>& modelFluxes)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    QTextStream in(&file);
    in.readLine();   // skip header: lambda,flux,sigma,model,spline,ignore

    wavelengths.clear();
    modelFluxes.clear();

    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;

        QStringList parts = line.split(',');
        if (parts.size() < 4) continue;

        bool okL, okM;
        double lambda = parts[0].toDouble(&okL);
        double model  = parts[3].toDouble(&okM);   // "model" column

        if (okL && okM) {
            wavelengths.push_back(lambda);
            modelFluxes.push_back(model);
        }
    }

    file.close();
    return !wavelengths.empty();
}

// ════════════════════════════════════════════════════════════════
// Preview table
// ════════════════════════════════════════════════════════════════

void SpectralFitImportPage::updateDiggaPreviewTable()
{
    _previewTree->clear();

    int totalDirs      = 0;
    int fullyMatched   = 0;
    int partialMatched = 0;
    int unmatched      = 0;
    int totalSpecMatch = 0;
    int totalSpecAll   = 0;

    for (const auto& dir : _diggaDirs) {
        totalDirs++;
        totalSpecAll += dir.totalSpectra;

        // ── Directory-level item ────────────────────────────────
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

        // Column 1: grid name
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
            QString params;
            if (dir.teff > 0)
                params += QString("Teff=%1").arg(dir.teff, 0, 'f', 0);
            if (dir.logg > 0) {
                if (!params.isEmpty()) params += ", ";
                params += QString("logg=%1").arg(dir.logg, 0, 'f', 2);
            }
            if (dir.he != 0) {
                if (!params.isEmpty()) params += ", ";
                params += QString("He=%1").arg(dir.he, 0, 'f', 2);
            }
            dirItem->setText(3, params);
        }

        // Column 4: match status
        if (!dir.parseOk) {
            dirItem->setText(4, dir.parseError);
            dirItem->setForeground(4, QBrush(Qt::red));
            unmatched++;
        } else if (dir.matchedSpectra == dir.totalSpectra &&
                   dir.totalSpectra > 0) {
            dirItem->setText(4, QString("%1/%1 spectra matched")
                                .arg(dir.totalSpectra));
            dirItem->setForeground(4, QBrush(QColor(0, 150, 0)));
            fullyMatched++;
        } else if (dir.matchedSpectra > 0) {
            dirItem->setText(4, QString("%1/%2 spectra matched")
                                .arg(dir.matchedSpectra)
                                .arg(dir.totalSpectra));
            dirItem->setForeground(4, QBrush(QColor(200, 150, 0)));
            partialMatched++;
        } else {
            dirItem->setText(4, QString("0/%1 spectra matched")
                                .arg(dir.totalSpectra));
            dirItem->setForeground(4, QBrush(Qt::red));
            unmatched++;
        }

        totalSpecMatch += dir.matchedSpectra;

        // ── Child items for each spectrum ───────────────────────
        for (const auto& sm : dir.specMatches) {
            QTreeWidgetItem* specItem = new QTreeWidgetItem;

            specItem->setText(0, QString("spec %1: %2")
                                 .arg(sm.specIndex)
                                 .arg(sm.diggaFilename));

            // Column 2: matched spectrum
            if (sm.matched && sm.matchedSpectrum) {
                QString specName = QFileInfo(
                    sm.matchedSpectrum->getFile()).fileName();
                if (specName.isEmpty())
                    specName = sm.matchedSpectrum->getId();
                specItem->setText(2, specName);
            } else {
                specItem->setText(2, "(no match)");
                specItem->setForeground(2, QBrush(Qt::red));
            }

            // Column 3: per-spectrum vrad
            specItem->setText(3, QString("vrad = %1 ± %2 km/s")
                                 .arg(sm.vrad, 0, 'f', 1)
                                 .arg(sm.vradError, 0, 'f', 1));

            // Column 4: status + plotdata
            QStringList statusParts;
            statusParts << (sm.matched ? "✓ matched" : "✗ no match");
            if (!sm.plotdataFile.isEmpty())
                statusParts << "plotdata ✓";
            else
                statusParts << "plotdata ✗";
            specItem->setText(4, statusParts.join(" | "));
            if (!sm.matched)
                specItem->setForeground(4, QBrush(Qt::red));

            dirItem->addChild(specItem);
        }

        _previewTree->addTopLevelItem(dirItem);
    }

    // Expand all for visibility
    _previewTree->expandAll();

    for (int i = 0; i < _previewTree->columnCount(); ++i)
        _previewTree->resizeColumnToContents(i);

    _statusLabel->setText(
        QString("Found %1 DIGGA directories — %2 fully matched, "
                "%3 partially matched, %4 unmatched. "
                "(%5/%6 spectra matched total)")
        .arg(totalDirs).arg(fullyMatched).arg(partialMatched)
        .arg(unmatched).arg(totalSpecMatch).arg(totalSpecAll));
}

// ════════════════════════════════════════════════════════════════
// Import
// ════════════════════════════════════════════════════════════════

void SpectralFitImportPage::importDiggaFits()
{
    StarImportWizard* importWizard =
        qobject_cast<StarImportWizard*>(wizard());
    if (!importWizard) return;

    auto controller = importWizard->controller();
    auto project    = importWizard->project();
    if (!controller || !project) return;

    int imported = 0;

    for (const auto& dir : _diggaDirs) {
        if (!dir.parseOk) continue;

        for (const auto& sm : dir.specMatches) {
            if (!sm.matched || !sm.matchedStar || !sm.matchedSpectrum)
                continue;

            auto fit = std::make_shared<SpectralFit>();

            // Shared (tied) parameters
            fit->teff               = dir.teff;
            fit->teffError          = dir.teffError;
            fit->logg               = dir.logg;
            fit->loggError          = dir.loggError;
            fit->he                 = dir.he;
            fit->heError            = dir.heError;
            fit->vsini              = dir.vsini;
            fit->vsiniError         = dir.vsiniError;
            fit->macroturbulence      = dir.zeta;
            fit->macroturbulenceError = dir.zetaError;
            fit->microturbulence      = dir.xi;
            fit->microturbulenceError = dir.xiError;
            fit->metallicity          = dir.z;
            fit->metallicityError     = dir.zError;
            fit->chi2               = dir.chi2;

            // Per-spectrum radial velocity
            fit->radialVelocity      = sm.vrad;
            fit->radialVelocityError = sm.vradError;

            // Grid / model ID
            fit->modelId = dir.gridName;

            // Best fit flag
            if (_markBestFitCheck->isChecked())
                fit->isBestFit = true;

            // Load model data from plotdata file
            if (!sm.plotdataFile.isEmpty()) {
                std::vector<double> wl, mf;
                if (loadPlotdata(sm.plotdataFile, wl, mf)) {
                    fit->modelWavelengths = std::move(wl);
                    fit->modelFluxes      = std::move(mf);
                }
            }

            // Add to spectrum object + persist to DB
            sm.matchedSpectrum->addSpectralFit(fit);
            controller->databaseManager()->saveSpectralFit(
                sm.matchedStar->getId(),
                sm.matchedSpectrum->getId(),
                fit);

            imported++;
        }
    }

    LOG_INFO("FitImport",
             QString("Imported %1 DIGGA spectral fits to database")
             .arg(imported));
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

    // ISIS / mapping stubs — just skip
    if (!_diggaRadio->isChecked()) {
        return true;
    }

    if (_diggaDirs.empty()) {
        QMessageBox::information(this, "No Fits",
            "No spectral fits have been scanned. "
            "You can skip this step.");
        return true;
    }

    // Count matches
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
        "Continue?")
        .arg(_diggaDirs.size()).arg(totalSpectra)
        .arg(totalMatched).arg(totalUnmatched);

    if (QMessageBox::question(this, "Confirm Import", msg,
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return false;
    }

    importDiggaFits();

    _statusLabel->setText(
        QString("Imported %1 spectral fits.").arg(totalMatched));
    return true;
}

int SpectralFitImportPage::nextId() const
{
    return StarImportWizard::Page_RadialVelocity;
}