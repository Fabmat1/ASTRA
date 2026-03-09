// src/utils/SpectraImportPage.cpp

#include "SpectraImportPage.h"
#include "StarImportWizard.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include "models/Spectrum.h"
#include "Logger.h"
#include "BackgroundTaskManager.h"

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
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QProgressBar>
#include <QButtonGroup>
#include <QListWidget>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QApplication>
#include <QRegularExpression>
#include <QHeaderView>  
#include <QTimer>

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// SpectraImportPage Implementation
// ============================================================================

SpectraImportPage::SpectraImportPage(QWidget* parent)
    : QWizardPage(parent)
    , _fullResultsReady(false)
    , _indexBuilt(false)
    , _updatingMatchMethods(false)
{
    setTitle("Import Spectra");
    setSubTitle("Associate spectral data files with imported stars");
    
    setupUi();
}

void SpectraImportPage::setupUi()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);
    
    // Mode selection
    QGroupBox* modeGroup = new QGroupBox("Import Method");
    QHBoxLayout* modeLayout = new QHBoxLayout;
    
    _fitsRadio = new QRadioButton("Scan FITS files");
    _mappingRadio = new QRadioButton("Use mapping file (CSV/ASCII)");
    _fitsRadio->setChecked(true);
    
    QButtonGroup* modeButtonGroup = new QButtonGroup(this);
    modeButtonGroup->addButton(_fitsRadio);
    modeButtonGroup->addButton(_mappingRadio);
    
    modeLayout->addWidget(_fitsRadio);
    modeLayout->addWidget(_mappingRadio);
    modeLayout->addStretch();
    modeGroup->setLayout(modeLayout);
    mainLayout->addWidget(modeGroup);
    
    connect(_fitsRadio, &QRadioButton::toggled, this, &SpectraImportPage::onImportModeChanged);
    
    // Horizontal layout for mode-specific options and matching options
    QHBoxLayout* middleLayout = new QHBoxLayout;
    
    // Mode stack (FITS / Mapping pages)
    _modeStack = new QStackedWidget;
    setupFitsPage();
    setupMappingPage();
    _modeStack->addWidget(_fitsPage);
    _modeStack->addWidget(_mappingPage);
    middleLayout->addWidget(_modeStack, 2);
    
    // Matching options group (shared between modes)
    QGroupBox* matchGroupBox = new QGroupBox("Star Matching Options");
    QVBoxLayout* matchLayout = new QVBoxLayout(matchGroupBox);
    
    QLabel* matchInstructions = new QLabel("Check to enable, drag or use arrows to set priority (top = highest):");
    matchInstructions->setWordWrap(true);
    matchLayout->addWidget(matchInstructions);
    
    QHBoxLayout* listButtonLayout = new QHBoxLayout;
    
    _matchMethodList = new QListWidget;
    _matchMethodList->setDragDropMode(QAbstractItemView::InternalMove);
    _matchMethodList->setDefaultDropAction(Qt::MoveAction);
    _matchMethodList->setMaximumHeight(90);
    
    // Add matching methods with checkboxes
    auto addMethodItem = [this](const QString& text, MatchMethod method, bool checked) {
        QListWidgetItem* item = new QListWidgetItem(text);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
        item->setData(Qt::UserRole, static_cast<int>(method));
        _matchMethodList->addItem(item);
    };
    
    addMethodItem("Gaia Source ID", MatchMethod::SourceId, true);
    addMethodItem("Star Alias/Name", MatchMethod::Alias, true);
    addMethodItem("Position (RA/Dec)", MatchMethod::Position, true);
    
    connect(_matchMethodList, &QListWidget::itemChanged, 
            this, &SpectraImportPage::onMatchMethodItemChanged);
    connect(_matchMethodList->model(), &QAbstractItemModel::rowsMoved,
            this, [this](const QModelIndex&, int, int, const QModelIndex&, int) { 
                if (!_updatingMatchMethods) {
                    QTimer::singleShot(100, this, &SpectraImportPage::autoGeneratePreview);
                }
            });
    
    listButtonLayout->addWidget(_matchMethodList);
    
    // Up/Down buttons for reordering
    QVBoxLayout* arrowLayout = new QVBoxLayout;
    _moveUpButton = new QPushButton("▲");
    _moveUpButton->setFixedWidth(30);
    _moveUpButton->setToolTip("Move selected method up (higher priority)");
    connect(_moveUpButton, &QPushButton::clicked, this, &SpectraImportPage::onMatchMethodMoveUp);
    
    _moveDownButton = new QPushButton("▼");
    _moveDownButton->setFixedWidth(30);
    _moveDownButton->setToolTip("Move selected method down (lower priority)");
    connect(_moveDownButton, &QPushButton::clicked, this, &SpectraImportPage::onMatchMethodMoveDown);
    
    arrowLayout->addWidget(_moveUpButton);
    arrowLayout->addWidget(_moveDownButton);
    arrowLayout->addStretch();
    listButtonLayout->addLayout(arrowLayout);
    
    matchLayout->addLayout(listButtonLayout);
    
    // Position match radius
    QHBoxLayout* radiusLayout = new QHBoxLayout;
    QLabel* radiusLabel = new QLabel("Position radius (arcsec):");
    _matchRadiusSpin = new QDoubleSpinBox;
    _matchRadiusSpin->setRange(0.1, 60.0);
    _matchRadiusSpin->setValue(5.0);
    _matchRadiusSpin->setDecimals(1);
    _matchRadiusSpin->setSingleStep(0.5);
    connect(_matchRadiusSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double) { 
                if (!_updatingMatchMethods) {
                    QTimer::singleShot(100, this, &SpectraImportPage::autoGeneratePreview);
                }
            });
    
    radiusLayout->addWidget(radiusLabel);
    radiusLayout->addWidget(_matchRadiusSpin);
    radiusLayout->addStretch();
    matchLayout->addLayout(radiusLayout);
    
    matchLayout->addStretch();
    middleLayout->addWidget(matchGroupBox, 1);
    
    mainLayout->addLayout(middleLayout);
    
    // Preview section
    QGroupBox* previewGroup = new QGroupBox("Preview");
    QVBoxLayout* previewLayout = new QVBoxLayout;
    
    _previewTree = new QTreeWidget;
    _previewTree->setHeaderLabels({"Spectrum File", "Matched Star", "Match Method", "Warnings"});
    _previewTree->setAlternatingRowColors(true);
    _previewTree->setRootIsDecorated(false);
    _previewTree->header()->setStretchLastSection(true);
    previewLayout->addWidget(_previewTree);
    
    _statusLabel = new QLabel("Select files and click 'Scan' or load a mapping file to preview matches.");
    _statusLabel->setWordWrap(true);
    previewLayout->addWidget(_statusLabel);
    
    previewGroup->setLayout(previewLayout);
    mainLayout->addWidget(previewGroup);
}

void SpectraImportPage::setupFitsPage()
{
    _fitsPage = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(_fitsPage);
    
    // Folder/file selection
    QGroupBox* filesGroup = new QGroupBox("FITS Files");
    QVBoxLayout* filesLayout = new QVBoxLayout;
    
    QHBoxLayout* folderLayout = new QHBoxLayout;
    _fitsFolderEdit = new QLineEdit;
    _fitsFolderEdit->setPlaceholderText("Select folder containing FITS files...");
    folderLayout->addWidget(_fitsFolderEdit);
    
    QPushButton* browseFolderBtn = new QPushButton("Browse Folder...");
    connect(browseFolderBtn, &QPushButton::clicked, this, &SpectraImportPage::onBrowseFitsFolder);
    folderLayout->addWidget(browseFolderBtn);
    
    QPushButton* browseFilesBtn = new QPushButton("Select Files...");
    connect(browseFilesBtn, &QPushButton::clicked, this, &SpectraImportPage::onBrowseFitsFiles);
    folderLayout->addWidget(browseFilesBtn);
    
    filesLayout->addLayout(folderLayout);
    
    _fitsFilesList = new QListWidget;
    _fitsFilesList->setMaximumHeight(100);
    _fitsFilesList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    filesLayout->addWidget(_fitsFilesList);
    
    filesGroup->setLayout(filesLayout);
    layout->addWidget(filesGroup);
    
    // Matching options
    QGroupBox* matchGroup = new QGroupBox("Star Matching");
    QGridLayout* matchLayout = new QGridLayout;
    
    _matchPositionRadio = new QRadioButton("Match by position (RA/DEC from header)");
    _matchSourceIdRadio = new QRadioButton("Match by Gaia Source ID (from header)");
    _matchObjectNameRadio = new QRadioButton("Match by object name (from header)");
    _matchPositionRadio->setChecked(true);
    
    QButtonGroup* matchButtonGroup = new QButtonGroup(this);
    matchButtonGroup->addButton(_matchPositionRadio);
    matchButtonGroup->addButton(_matchSourceIdRadio);
    matchButtonGroup->addButton(_matchObjectNameRadio);
    
    matchLayout->addWidget(_matchPositionRadio, 0, 0, 1, 2);
    matchLayout->addWidget(_matchSourceIdRadio, 1, 0, 1, 2);
    matchLayout->addWidget(_matchObjectNameRadio, 2, 0, 1, 2);
    
    QLabel* radiusLabel = new QLabel("Position match radius (arcsec):");
    _matchRadiusSpin = new QDoubleSpinBox;
    _matchRadiusSpin->setRange(0.1, 60.0);
    _matchRadiusSpin->setValue(5.0);
    _matchRadiusSpin->setDecimals(1);
    _matchRadiusSpin->setSingleStep(0.5);
    matchLayout->addWidget(radiusLabel, 3, 0);
    matchLayout->addWidget(_matchRadiusSpin, 3, 1);
    
    connect(_matchPositionRadio, &QRadioButton::toggled, this, &SpectraImportPage::onMatchMethodChanged);
    
    matchGroup->setLayout(matchLayout);
    layout->addWidget(matchGroup);
    
    // Scan button
    QHBoxLayout* scanLayout = new QHBoxLayout;
    _scanButton = new QPushButton("Scan && Match");
    _scanButton->setEnabled(false);
    connect(_scanButton, &QPushButton::clicked, this, &SpectraImportPage::onScanFiles);
    scanLayout->addWidget(_scanButton);
    
    _scanProgress = new QProgressBar;
    _scanProgress->setVisible(false);
    scanLayout->addWidget(_scanProgress);
    scanLayout->addStretch();
    
    layout->addLayout(scanLayout);
    layout->addStretch();
}

void SpectraImportPage::setupMappingPage()
{
    _mappingPage = new QWidget;
    QVBoxLayout* layout = new QVBoxLayout(_mappingPage);
    
    // File selection
    QGroupBox* fileGroup = new QGroupBox("Mapping File");
    QVBoxLayout* fileLayout = new QVBoxLayout;
    
    QHBoxLayout* pathLayout = new QHBoxLayout;
    _mappingFileEdit = new QLineEdit;
    _mappingFileEdit->setPlaceholderText("Select CSV/ASCII mapping file...");
    pathLayout->addWidget(_mappingFileEdit);
    
    QPushButton* browseBtn = new QPushButton("Browse...");
    connect(browseBtn, &QPushButton::clicked, this, &SpectraImportPage::onBrowseMappingFile);
    pathLayout->addWidget(browseBtn);
    fileLayout->addLayout(pathLayout);
    
    QHBoxLayout* optionsLayout = new QHBoxLayout;
    QLabel* delimLabel = new QLabel("Delimiter:");
    _delimiterCombo = new QComboBox;
    _delimiterCombo->addItems({"Auto-detect", "Comma (,)", "Tab", "Semicolon (;)", "Space"});
    optionsLayout->addWidget(delimLabel);
    optionsLayout->addWidget(_delimiterCombo);
    
    _hasHeaderCheck = new QCheckBox("First row contains column names");
    _hasHeaderCheck->setChecked(true);
    optionsLayout->addWidget(_hasHeaderCheck);
    optionsLayout->addStretch();
    fileLayout->addLayout(optionsLayout);
    
    fileGroup->setLayout(fileLayout);
    layout->addWidget(fileGroup);
    
    // Column mapping
    QGroupBox* columnsGroup = new QGroupBox("Column Mapping");
    QGridLayout* columnsLayout = new QGridLayout;
    
    int row = 0;
    
    columnsLayout->addWidget(new QLabel("Spectrum File Path:"), row, 0);
    _filePathColumnCombo = new QComboBox;
    columnsLayout->addWidget(_filePathColumnCombo, row++, 1);
    
    columnsLayout->addWidget(new QLabel("Star Alias/Name:"), row, 0);
    _starIdColumnCombo = new QComboBox;
    columnsLayout->addWidget(_starIdColumnCombo, row++, 1);
    
    columnsLayout->addWidget(new QLabel("Gaia Source ID:"), row, 0);
    _sourceIdColumnCombo = new QComboBox;
    columnsLayout->addWidget(_sourceIdColumnCombo, row++, 1);
    
    columnsLayout->addWidget(new QLabel("RA (degrees):"), row, 0);
    _raColumnCombo = new QComboBox;
    columnsLayout->addWidget(_raColumnCombo, row++, 1);
    
    columnsLayout->addWidget(new QLabel("Dec (degrees):"), row, 0);
    _decColumnCombo = new QComboBox;
    columnsLayout->addWidget(_decColumnCombo, row++, 1);
    
    columnsLayout->addWidget(new QLabel("MJD:"), row, 0);
    _mjdColumnCombo = new QComboBox;
    columnsLayout->addWidget(_mjdColumnCombo, row++, 1);
    
    columnsLayout->addWidget(new QLabel("BJD:"), row, 0);
    _bjdColumnCombo = new QComboBox;
    columnsLayout->addWidget(_bjdColumnCombo, row++, 1);
    
    columnsLayout->addWidget(new QLabel("Exposure Time:"), row, 0);
    _expTimeColumnCombo = new QComboBox;
    columnsLayout->addWidget(_expTimeColumnCombo, row++, 1);
    
    columnsLayout->addWidget(new QLabel("Instrument:"), row, 0);
    _instrumentColumnCombo = new QComboBox;
    columnsLayout->addWidget(_instrumentColumnCombo, row++, 1);
    
    columnsLayout->addWidget(new QLabel("Barycentric Corrected:"), row, 0);
    _baryCorrColumnCombo = new QComboBox;
    columnsLayout->addWidget(_baryCorrColumnCombo, row++, 1);
    
    columnsGroup->setLayout(columnsLayout);
    layout->addWidget(columnsGroup);
    
    // Preview controls
    QHBoxLayout* previewControlLayout = new QHBoxLayout;
    
    QLabel* previewRowsLabel = new QLabel("Preview rows:");
    _previewRowsSpin = new QSpinBox;
    _previewRowsSpin->setRange(10, 1000);
    _previewRowsSpin->setValue(DEFAULT_PREVIEW_ROWS);
    _previewRowsSpin->setSingleStep(50);
    
    _previewButton = new QPushButton("Generate Preview");
    _previewButton->setEnabled(false);
    connect(_previewButton, &QPushButton::clicked, this, &SpectraImportPage::onPreviewButtonClicked);
    
    previewControlLayout->addWidget(previewRowsLabel);
    previewControlLayout->addWidget(_previewRowsSpin);
    previewControlLayout->addWidget(_previewButton);
    previewControlLayout->addStretch();
    
    layout->addLayout(previewControlLayout);
    layout->addStretch();
}

void SpectraImportPage::initializePage()
{
    LOG_DEBUG("SpectraImport", "Initializing SpectraImportPage");
    
    StarImportWizard* importWizard = qobject_cast<StarImportWizard*>(wizard());
    if (importWizard) {
        _importedStars = importWizard->getImportedStars();
        LOG_INFO("SpectraImport", QString("Received %1 stars for spectrum matching")
                 .arg(_importedStars.size()));
    }
    
    // Reset state
    _fullResultsReady = false;
    _indexBuilt = false;
    _matchResults.clear();
    _fullMatchResults.clear();
    
    _statusLabel->setText(QString("Ready to import spectra for %1 stars. "
                                  "Select FITS files to scan or load a mapping file.")
                          .arg(_importedStars.size()));
}

void SpectraImportPage::buildStarLookupIndex()
{
    if (_indexBuilt) return;
    
    LOG_DEBUG("SpectraImport", "Building star lookup indices");
    
    _sourceIdIndex.clear();
    _aliasIndex.clear();
    
    // Pre-compile regex for source ID extraction
    QRegularExpression re("(\\d{10,})");
    
    for (const auto& star : _importedStars) {
        // Index by source ID
        QString sourceId = star->getSourceId();
        if (!sourceId.isEmpty()) {
            // Store both raw and extracted numeric ID
            _sourceIdIndex[sourceId] = star;
            
            QRegularExpressionMatch match = re.match(sourceId);
            if (match.hasMatch()) {
                _sourceIdIndex[match.captured(1)] = star;
            }
        }
        
        // Index by alias (case-insensitive)
        QString alias = star->getAlias();
        if (!alias.isEmpty()) {
            _aliasIndex[alias.toLower()] = star;
        }
    }
    
    _indexBuilt = true;
    LOG_DEBUG("SpectraImport", QString("Built indices: %1 source IDs, %2 aliases")
              .arg(_sourceIdIndex.size()).arg(_aliasIndex.size()));
}

void SpectraImportPage::onImportModeChanged()
{
    _modeStack->setCurrentIndex(_fitsRadio->isChecked() ? 0 : 1);
    _previewTree->clear();
    _matchResults.clear();
    _fullResultsReady = false;
}

void SpectraImportPage::onPreviewMatching()
{
    if (_fitsRadio->isChecked()) {
        onScanFiles();
    } else {
        onPreviewButtonClicked();
    }
}

void SpectraImportPage::onBrowseFitsFolder()
{
    QString folder = QFileDialog::getExistingDirectory(this, "Select FITS Folder",
                                                        _fitsFolderEdit->text());
    if (!folder.isEmpty()) {
        _fitsFolderEdit->setText(folder);
        _fitsFilesList->clear();
        
        QDir dir(folder);
        QStringList filters = {"*.fits", "*.fit", "*.fts", "*.FITS", "*.FIT"};
        QStringList files = dir.entryList(filters, QDir::Files);
        
        for (const QString& file : files) {
            _fitsFilesList->addItem(file);
        }
        
        _scanButton->setEnabled(!files.isEmpty());
        _statusLabel->setText(QString("Found %1 FITS files in folder.").arg(files.size()));
        
        LOG_INFO("SpectraImport", QString("Found %1 FITS files in %2").arg(files.size()).arg(folder));
    }
}

void SpectraImportPage::onBrowseFitsFiles()
{
    QStringList files = QFileDialog::getOpenFileNames(this, "Select FITS Files",
                                                       _fitsFolderEdit->text(),
                                                       "FITS Files (*.fits *.fit *.fts);;All Files (*)");
    if (!files.isEmpty()) {
        _fitsFilesList->clear();
        _fitsFolderEdit->clear();
        
        for (const QString& file : files) {
            QFileInfo info(file);
            QListWidgetItem* item = new QListWidgetItem(info.fileName());
            item->setData(Qt::UserRole, file);
            _fitsFilesList->addItem(item);
        }
        
        _scanButton->setEnabled(true);
        _statusLabel->setText(QString("Selected %1 FITS files.").arg(files.size()));
    }
}

void SpectraImportPage::onMatchMethodMoveUp()
{
    int currentRow = _matchMethodList->currentRow();
    if (currentRow <= 0) return;
    
    _updatingMatchMethods = true;
    QListWidgetItem* item = _matchMethodList->takeItem(currentRow);
    _matchMethodList->insertItem(currentRow - 1, item);
    _matchMethodList->setCurrentRow(currentRow - 1);
    _updatingMatchMethods = false;
    
    autoGeneratePreview();
}

void SpectraImportPage::onMatchMethodMoveDown()
{
    int currentRow = _matchMethodList->currentRow();
    if (currentRow < 0 || currentRow >= _matchMethodList->count() - 1) return;
    
    _updatingMatchMethods = true;
    QListWidgetItem* item = _matchMethodList->takeItem(currentRow);
    _matchMethodList->insertItem(currentRow + 1, item);
    _matchMethodList->setCurrentRow(currentRow + 1);
    _updatingMatchMethods = false;
    
    autoGeneratePreview();
}

void SpectraImportPage::onMatchMethodItemChanged(QListWidgetItem* item)
{
    Q_UNUSED(item);
    if (!_updatingMatchMethods) {
        QTimer::singleShot(100, this, &SpectraImportPage::autoGeneratePreview);
    }
}

std::vector<MatchMethod> SpectraImportPage::getEnabledMatchMethods() const
{
    std::vector<MatchMethod> methods;
    
    for (int i = 0; i < _matchMethodList->count(); ++i) {
        QListWidgetItem* item = _matchMethodList->item(i);
        if (item->checkState() == Qt::Checked) {
            methods.push_back(static_cast<MatchMethod>(item->data(Qt::UserRole).toInt()));
        }
    }
    
    return methods;
}

void SpectraImportPage::autoGeneratePreview()
{
    // Only auto-generate if we have data loaded
    if (_mappingRadio->isChecked() && !_mappingRows.empty()) {
        int previewRows = _previewRowsSpin->value();
        
        _statusLabel->setText("Generating preview...");
        QApplication::processEvents();
        
        _matchResults = processMapping(previewRows);
        _fullResultsReady = false;
        
        updatePreviewTable();
    } else if (_fitsRadio->isChecked() && !_scannedMetadata.empty()) {
        _matchResults = matchSpectraToStars();
        updatePreviewTable();
    }
}

std::shared_ptr<Star> SpectraImportPage::findMatchingStar(const QString& sourceId, const QString& alias,
                                                          double ra, double dec, bool hasPosition,
                                                          QString& outMatchMethod, double& outMatchDistance)
{
    outMatchMethod.clear();
    outMatchDistance = -1.0;
    
    std::vector<MatchMethod> methods = getEnabledMatchMethods();
    
    for (MatchMethod method : methods) {
        switch (method) {
            case MatchMethod::SourceId:
                if (!sourceId.isEmpty()) {
                    auto star = findStarBySourceId(sourceId);
                    if (star) {
                        outMatchMethod = "source_id";
                        return star;
                    }
                }
                break;
                
            case MatchMethod::Alias:
                if (!alias.isEmpty()) {
                    auto star = findStarByAlias(alias);
                    if (star) {
                        outMatchMethod = "alias";
                        return star;
                    }
                }
                break;
                
            case MatchMethod::Position:
                if (hasPosition) {
                    double distance = -1.0;
                    auto star = findStarByPosition(ra, dec, _matchRadiusSpin->value(), &distance);
                    if (star) {
                        outMatchMethod = "position";
                        outMatchDistance = distance;
                        return star;
                    }
                }
                break;
        }
    }
    
    return nullptr;
}

void SpectraImportPage::onBrowseMappingFile()
{
    QString file = QFileDialog::getOpenFileName(this, "Select Mapping File",
                                                 QString(),
                                                 "CSV/Text Files (*.csv *.txt *.dat);;All Files (*)");
    if (!file.isEmpty()) {
        _mappingFileEdit->setText(file);
        _mappingBasePath = QFileInfo(file).absolutePath();
        onMappingFileLoaded();
    }
}

void SpectraImportPage::onMatchMethodChanged()
{
    _matchRadiusSpin->setEnabled(_matchPositionRadio->isChecked());
}

void SpectraImportPage::onScanFiles()
{
    LOG_INFO("SpectraImport", "Starting FITS file scan");
    
    QStringList files;
    QString basePath = _fitsFolderEdit->text();
    
    for (int i = 0; i < _fitsFilesList->count(); ++i) {
        QListWidgetItem* item = _fitsFilesList->item(i);
        QString fullPath = item->data(Qt::UserRole).toString();
        if (fullPath.isEmpty()) {
            fullPath = basePath + "/" + item->text();
        }
        files << fullPath;
    }
    
    scanFitsFiles(files);
    _matchResults = matchSpectraToStars();
    updatePreviewTable();
}

void SpectraImportPage::scanFitsFiles(const QStringList& files)
{
    _scannedMetadata.clear();
    _scanProgress->setVisible(true);
    _scanProgress->setRange(0, files.size());
    _scanProgress->setValue(0);
    
    auto reader = std::make_shared<DefaultFitsSpectrumReader>();
    
    for (int i = 0; i < files.size(); ++i) {
        _scanProgress->setValue(i);
        QApplication::processEvents();
        
        SpectrumMetadata metadata = reader->readMetadata(files[i]);
        _scannedMetadata.push_back(metadata);
        
        if (!metadata.warnings.isEmpty()) {
            LOG_DEBUG("SpectraImport", QString("%1: %2")
                      .arg(QFileInfo(files[i]).fileName())
                      .arg(metadata.warnings.join("; ")));
        }
    }
    
    _scanProgress->setValue(files.size());
    _scanProgress->setVisible(false);
    
    LOG_INFO("SpectraImport", QString("Scanned %1 FITS files").arg(files.size()));
}

std::shared_ptr<Star> SpectraImportPage::findStarByPosition(double ra, double dec, double radiusArcsec, double* outDistance)
{
    double radiusDeg = radiusArcsec / 3600.0;
    double cosDecFactor = std::cos(dec * M_PI / 180.0);
    
    std::shared_ptr<Star> bestMatch;
    double bestDistance = radiusDeg;
    
    for (const auto& star : _importedStars) {
        if (star->getRa() == 0.0 && star->getDec() == 0.0) continue;
        
        double dRa = (ra - star->getRa()) * cosDecFactor;
        double dDec = dec - star->getDec();
        double dist = std::sqrt(dRa * dRa + dDec * dDec);
        
        if (dist < bestDistance) {
            bestDistance = dist;
            bestMatch = star;
        }
    }
    
    if (bestMatch && outDistance) {
        *outDistance = bestDistance * 3600.0;  // Convert to arcsec
    }
    
    return bestMatch;
}

std::shared_ptr<Star> SpectraImportPage::findStarBySourceId(const QString& sourceId)
{
    // Build index on first use
    if (!_indexBuilt) {
        buildStarLookupIndex();
    }
    
    QString cleanId = sourceId.trimmed();
    
    // Try direct lookup first
    auto it = _sourceIdIndex.find(cleanId);
    if (it != _sourceIdIndex.end()) {
        return it.value();
    }
    
    // Try extracting numeric part
    QRegularExpression re("(\\d{10,})");
    QRegularExpressionMatch match = re.match(cleanId);
    if (match.hasMatch()) {
        it = _sourceIdIndex.find(match.captured(1));
        if (it != _sourceIdIndex.end()) {
            return it.value();
        }
    }
    
    return nullptr;
}

std::shared_ptr<Star> SpectraImportPage::findStarByAlias(const QString& alias)
{
    // Build index on first use
    if (!_indexBuilt) {
        buildStarLookupIndex();
    }
    
    QString cleanAlias = alias.trimmed().toLower();
    
    auto it = _aliasIndex.find(cleanAlias);
    if (it != _aliasIndex.end()) {
        return it.value();
    }
    
    return nullptr;
}

std::vector<SpectrumMatchResult> SpectraImportPage::matchSpectraToStars()
{
    std::vector<SpectrumMatchResult> results;
    
    for (const auto& metadata : _scannedMetadata) {
        SpectrumMatchResult result;
        result.spectrumFile = metadata.filepath;
        result.hasWarnings = !metadata.warnings.isEmpty();
        result.warnings = metadata.warnings;
        result.matchDistance = -1.0;
        
        // Extract data for matching
        QString sourceId = metadata.sourceId.value_or("");
        QString alias = metadata.objectName.value_or("");
        double ra = metadata.ra.value_or(0.0);
        double dec = metadata.dec.value_or(0.0);
        bool hasPosition = metadata.ra.has_value() && metadata.dec.has_value();
        
        // Use unified matching with priority
        result.matchedStar = findMatchingStar(sourceId, alias, ra, dec, hasPosition,
                                               result.matchMethod, result.matchDistance);
        
        if (!result.matchedStar) {
            result.warnings << "No matching star found";
            result.hasWarnings = true;
        }
        
        results.push_back(result);
    }
    
    return results;
}

void SpectraImportPage::onMappingFileLoaded()
{
    QString filepath = _mappingFileEdit->text();
    if (filepath.isEmpty()) return;
    
    if (loadMappingFile(filepath)) {
        detectMappingColumns();
        _previewButton->setEnabled(true);
        _fullResultsReady = false;
        
        _statusLabel->setText(QString("Loaded %1 rows. Generating preview...")
                              .arg(_mappingRows.size()));
        
        // Auto-generate preview with a small delay to let UI update
        QTimer::singleShot(100, this, &SpectraImportPage::autoGeneratePreview);
    }
}

bool SpectraImportPage::loadMappingFile(const QString& filepath)
{
    LOG_INFO("SpectraImport", QString("Loading mapping file: %1").arg(filepath));
    
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Error", QString("Cannot open file: %1").arg(file.errorString()));
        return false;
    }
    
    _mappingColumns.clear();
    _mappingRows.clear();
    
    QTextStream in(&file);
    bool firstLine = true;
    
    QChar delimiter = ',';
    int delimIndex = _delimiterCombo->currentIndex();
    if (delimIndex == 0) {
        // Auto-detect: read first line to check
        QString firstLineContent = in.readLine();
        in.seek(0);
        
        if (firstLineContent.count('\t') > firstLineContent.count(',')) {
            delimiter = '\t';
        } else if (firstLineContent.count(';') > firstLineContent.count(',')) {
            delimiter = ';';
        }
    } else {
        switch (delimIndex) {
            case 1: delimiter = ','; break;
            case 2: delimiter = '\t'; break;
            case 3: delimiter = ';'; break;
            case 4: delimiter = ' '; break;
        }
    }
    
    while (!in.atEnd()) {
        QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#')) continue;
        
        QStringList parts;
        if (delimiter == ' ') {
            parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        } else {
            parts = line.split(delimiter);
        }
        
        if (firstLine && _hasHeaderCheck->isChecked()) {
            _mappingColumns = parts;
            firstLine = false;
        } else {
            _mappingRows.push_back(parts);
            if (firstLine) {
                for (int i = 0; i < parts.size(); ++i) {
                    _mappingColumns << QString("Column %1").arg(i + 1);
                }
                firstLine = false;
            }
        }
    }
    
    file.close();
    
    LOG_INFO("SpectraImport", QString("Loaded %1 rows with %2 columns")
             .arg(_mappingRows.size()).arg(_mappingColumns.size()));
    
    return !_mappingRows.empty();
}

void SpectraImportPage::detectMappingColumns()
{
    QStringList options = {"(none)"};
    options.append(_mappingColumns);
    
    // Block signals to prevent triggering processing during setup
    QList<QComboBox*> combos = {_filePathColumnCombo, _starIdColumnCombo, _sourceIdColumnCombo,
                                 _raColumnCombo, _decColumnCombo, _mjdColumnCombo, _bjdColumnCombo,
                                 _expTimeColumnCombo, _instrumentColumnCombo, _baryCorrColumnCombo};
    
    for (auto* combo : combos) {
        combo->blockSignals(true);
        combo->clear();
        combo->addItems(options);
    }
    
    auto findColumn = [this](const QStringList& patterns) -> int {
        for (int i = 0; i < _mappingColumns.size(); ++i) {
            QString col = _mappingColumns[i].toLower();
            for (const QString& pattern : patterns) {
                if (col.contains(pattern.toLower())) {
                    return i + 1;
                }
            }
        }
        return 0;
    };
    
    _filePathColumnCombo->setCurrentIndex(findColumn({"file", "path", "spectrum", "fits"}));
    _starIdColumnCombo->setCurrentIndex(findColumn({"alias", "name", "star", "object", "target"}));
    _sourceIdColumnCombo->setCurrentIndex(findColumn({"source_id", "gaia", "dr3"}));
    _raColumnCombo->setCurrentIndex(findColumn({"ra", "right_asc"}));
    _decColumnCombo->setCurrentIndex(findColumn({"dec", "decl"}));
    _mjdColumnCombo->setCurrentIndex(findColumn({"mjd"}));
    _bjdColumnCombo->setCurrentIndex(findColumn({"bjd"}));
    _expTimeColumnCombo->setCurrentIndex(findColumn({"exp", "texp", "itime"}));
    _instrumentColumnCombo->setCurrentIndex(findColumn({"inst", "spec", "instrument"}));
    _baryCorrColumnCombo->setCurrentIndex(findColumn({"bary", "bcorr", "barycentric"}));
    
    // Unblock signals
    for (auto* combo : combos) {
        combo->blockSignals(false);
    }
}

void SpectraImportPage::onMappingColumnChanged()
{
    // Just mark that we need to regenerate preview, don't auto-process
    _fullResultsReady = false;
}

void SpectraImportPage::onPreviewButtonClicked()
{
    if (_mappingRows.empty()) return;
    
    int previewRows = _previewRowsSpin->value();
    
    _statusLabel->setText("Generating preview...");
    QApplication::processEvents();
    
    _matchResults = processMapping(previewRows);
    _fullResultsReady = false;
    
    updatePreviewTable();
    
    int totalRows = static_cast<int>(_mappingRows.size());
    if (previewRows < totalRows) {
        _statusLabel->setText(QString("Showing preview of %1/%2 rows. Full processing will occur on 'Next'.")
                              .arg(previewRows).arg(totalRows));
    }
}

SpectrumMatchResult SpectraImportPage::processOneRow(const QStringList& row, int rowIndex)
{
    SpectrumMatchResult result;
    result.matchDistance = -1.0;
    result.hasWarnings = false;
    result.sourceRowIndex = rowIndex;
    
    int fileCol = _filePathColumnCombo->currentIndex() - 1;
    int aliasCol = _starIdColumnCombo->currentIndex() - 1;
    int sourceIdCol = _sourceIdColumnCombo->currentIndex() - 1;
    int raCol = _raColumnCombo->currentIndex() - 1;
    int decCol = _decColumnCombo->currentIndex() - 1;
    int mjdCol = _mjdColumnCombo->currentIndex() - 1;
    int bjdCol = _bjdColumnCombo->currentIndex() - 1;
    int expCol = _expTimeColumnCombo->currentIndex() - 1;
    
    // Get file path
    if (fileCol >= 0 && fileCol < row.size()) {
        QString filePath = row[fileCol].trimmed();
        if (!QFileInfo(filePath).isAbsolute()) {
            filePath = _mappingBasePath + "/" + filePath;
        }
        result.spectrumFile = filePath;
    } else {
        result.spectrumFile = "";
        result.warnings << "No file path";
        result.hasWarnings = true;
        return result;
    }
    
    // Extract matching data from row
    QString sourceId;
    QString alias;
    double ra = 0.0, dec = 0.0;
    bool hasPosition = false;
    
    if (sourceIdCol >= 0 && sourceIdCol < row.size()) {
        sourceId = row[sourceIdCol].trimmed();
    }
    if (aliasCol >= 0 && aliasCol < row.size()) {
        alias = row[aliasCol].trimmed();
    }
    if (raCol >= 0 && decCol >= 0 && raCol < row.size() && decCol < row.size()) {
        bool okRa, okDec;
        ra = row[raCol].toDouble(&okRa);
        dec = row[decCol].toDouble(&okDec);
        hasPosition = okRa && okDec;
    }
    
    // Use unified matching with priority
    result.matchedStar = findMatchingStar(sourceId, alias, ra, dec, hasPosition,
                                           result.matchMethod, result.matchDistance);
    
    if (!result.matchedStar) {
        result.warnings << "No matching star found";
        result.hasWarnings = true;
    }
    
    // Check for metadata
    bool hasMjd = (mjdCol >= 0 && mjdCol < row.size() && !row[mjdCol].trimmed().isEmpty());
    bool hasBjd = (bjdCol >= 0 && bjdCol < row.size() && !row[bjdCol].trimmed().isEmpty());
    bool hasExp = (expCol >= 0 && expCol < row.size() && !row[expCol].trimmed().isEmpty());
    
    if (!hasMjd && !hasBjd) {
        result.warnings << "No observation time (MJD/BJD)";
        result.hasWarnings = true;
    }
    if (!hasExp) {
        result.warnings << "No exposure time";
        result.hasWarnings = true;
    }
    
    return result;
}

std::vector<SpectrumMatchResult> SpectraImportPage::processMapping(int maxRows)
{
    std::vector<SpectrumMatchResult> results;
    
    int fileCol = _filePathColumnCombo->currentIndex() - 1;
    if (fileCol < 0) {
        _statusLabel->setText("Please select the file path column.");
        return results;
    }
    
    // Build index before processing
    buildStarLookupIndex();
    
    int rowsToProcess = (maxRows < 0) ? static_cast<int>(_mappingRows.size()) 
                                       : std::min(maxRows, static_cast<int>(_mappingRows.size()));
    
    results.reserve(rowsToProcess);
    
    for (int i = 0; i < rowsToProcess; ++i) {
        results.push_back(processOneRow(_mappingRows[i], i));
    }
    
    return results;
}

std::vector<SpectrumMatchResult> SpectraImportPage::processMappingWithProgress()
{
    std::vector<SpectrumMatchResult> results;
    
    int fileCol = _filePathColumnCombo->currentIndex() - 1;
    if (fileCol < 0) {
        return results;
    }
    
    // Build index before processing
    buildStarLookupIndex();
    
    int totalRows = static_cast<int>(_mappingRows.size());
    results.reserve(totalRows);
    
    QProgressDialog progress("Processing spectra mapping...", "Cancel", 0, totalRows, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(500);  // Only show if takes > 500ms
    
    for (int i = 0; i < totalRows; ++i) {
        if (i % 100 == 0) {  // Update progress every 100 rows
            progress.setValue(i);
            QApplication::processEvents();
            
            if (progress.wasCanceled()) {
                results.clear();
                return results;
            }
        }
        
        results.push_back(processOneRow(_mappingRows[i], i));
    }
    
    progress.setValue(totalRows);
    
    return results;
}

void SpectraImportPage::updatePreviewTable()
{
    _previewTree->clear();
    
    int matched = 0;
    int warnings = 0;
    int unmatched = 0;
    
    for (const auto& result : _matchResults) {
        QTreeWidgetItem* item = new QTreeWidgetItem;
        
        item->setText(0, QFileInfo(result.spectrumFile).fileName());
        
        if (result.matchedStar) {
            QString starName = result.matchedStar->getAlias();
            if (starName.isEmpty()) {
                starName = result.matchedStar->getSourceId();
            }
            if (starName.isEmpty()) {
                starName = QString("Star at (%1, %2)")
                    .arg(result.matchedStar->getRa(), 0, 'f', 4)
                    .arg(result.matchedStar->getDec(), 0, 'f', 4);
            }
            item->setText(1, starName);
            matched++;
        } else {
            item->setText(1, "(no match)");
            item->setForeground(1, QBrush(Qt::red));
            unmatched++;
        }
        
        QString method = result.matchMethod;
        if (result.matchDistance >= 0) {
            method += QString(" (%1\")").arg(result.matchDistance, 0, 'f', 2);
        }
        item->setText(2, method);
        
        if (!result.warnings.isEmpty()) {
            item->setText(3, result.warnings.join("; "));
            item->setForeground(3, QBrush(QColor(200, 150, 0)));
            warnings++;
        }
        
        _previewTree->addTopLevelItem(item);
    }
    
    for (int i = 0; i < 4; ++i) {
        _previewTree->resizeColumnToContents(i);
    }
    
    int totalPreview = static_cast<int>(_matchResults.size());
    int totalRows = static_cast<int>(_mappingRows.size());
    
    QString statusText = QString("Preview: %1 spectra shown").arg(totalPreview);
    if (totalPreview < totalRows) {
        statusText += QString(" (of %1 total)").arg(totalRows);
    }
    statusText += QString(" - %1 matched, %2 with warnings, %3 unmatched")
                  .arg(matched).arg(warnings).arg(unmatched);
    
    _statusLabel->setText(statusText);
}

std::vector<SpectrumImportEntry> SpectraImportPage::createImportEntries(const std::vector<SpectrumMatchResult>& results)
{
    std::vector<SpectrumImportEntry> entries;
    entries.reserve(results.size());
    
    // Get column indices
    int mjdCol = _mjdColumnCombo->currentIndex() - 1;
    int bjdCol = _bjdColumnCombo->currentIndex() - 1;
    int expCol = _expTimeColumnCombo->currentIndex() - 1;
    int instCol = _instrumentColumnCombo->currentIndex() - 1;
    int baryCorrCol = _baryCorrColumnCombo->currentIndex() - 1;
    
    for (const auto& result : results) {
        // Skip unmatched entries
        if (!result.matchedStar) {
            continue;
        }
        
        SpectrumImportEntry entry;
        entry.spectrumFile = result.spectrumFile;
        entry.matchedStar = result.matchedStar;
        entry.sourceRowIndex = result.sourceRowIndex;
        
        // Extract metadata from mapping rows if available
        if (_mappingRadio->isChecked() && result.sourceRowIndex >= 0 && 
            result.sourceRowIndex < static_cast<int>(_mappingRows.size())) {
            
            const QStringList& row = _mappingRows[result.sourceRowIndex];
            
            if (mjdCol >= 0 && mjdCol < row.size()) {
                bool ok;
                double v = row[mjdCol].toDouble(&ok);
                if (ok) entry.mjd = v;
            }
            if (bjdCol >= 0 && bjdCol < row.size()) {
                bool ok;
                double v = row[bjdCol].toDouble(&ok);
                if (ok) entry.bjd = v;
            }
            if (expCol >= 0 && expCol < row.size()) {
                bool ok;
                double v = row[expCol].toDouble(&ok);
                if (ok) entry.exposureTime = v;
            }
            if (instCol >= 0 && instCol < row.size() && !row[instCol].trimmed().isEmpty()) {
                entry.instrument = row[instCol].trimmed();
            }
            if (baryCorrCol >= 0 && baryCorrCol < row.size()) {
                QString val = row[baryCorrCol].trimmed().toLower();
                entry.isBarycentricallyCorrected = (val == "1" || val == "true" || val == "yes" || val == "y");
            }
        }
        
        entries.push_back(entry);
    }
    
    return entries;
}

void SpectraImportPage::queueImportTask(std::vector<SpectrumImportEntry> entries)
{
    StarImportWizard* importWizard = qobject_cast<StarImportWizard*>(wizard());
    if (!importWizard) {
        LOG_ERROR("SpectraImport", "Could not access wizard");
        return;
    }
    
    auto controller = importWizard->controller();
    auto project = importWizard->project();
    
    if (!controller || !project) {
        LOG_ERROR("SpectraImport", "Controller or project not available");
        return;
    }
    
    // Create the background task
    auto task = new SpectraImportTask(std::move(entries), project->getId(), controller);
    
    // Connect signals for completion notification
    connect(task, &SpectraImportTask::importComplete, this, [this](int imported, int failed) {
        LOG_INFO("SpectraImport", QString("Background import complete: %1 imported, %2 failed")
                .arg(imported).arg(failed));
    });
    
    // Queue the task
    controller->backgroundTaskManager()->queueTask(task);
    
    LOG_INFO("SpectraImport", "Spectra import task queued for background processing");
}

bool SpectraImportPage::validatePage()
{
    // If using mapping mode and haven't processed all rows yet, do it now
    if (_mappingRadio->isChecked() && !_mappingRows.empty() && !_fullResultsReady) {
        LOG_INFO("SpectraImport", "Processing full mapping file...");
        
        _fullMatchResults = processMappingWithProgress();
        
        if (_fullMatchResults.empty() && !_mappingRows.empty()) {
            // User cancelled
            return false;
        }
        
        _fullResultsReady = true;
        LOG_INFO("SpectraImport", QString("Full processing complete: %1 entries")
                 .arg(_fullMatchResults.size()));
    }
    
    // Use full results if available, otherwise use preview results (for FITS mode)
    const auto& resultsToUse = _fullResultsReady ? _fullMatchResults : _matchResults;
    
    if (resultsToUse.empty()) {
        QMessageBox::information(this, "No Spectra", 
                                 "No spectra have been scanned or loaded. "
                                 "You can skip this step if you don't want to import spectra now.");
        return true;
    }
    
    int matched = 0;
    int unmatched = 0;
    int withWarnings = 0;
    
    for (const auto& result : resultsToUse) {
        if (result.matchedStar) {
            matched++;
        } else {
            unmatched++;
        }
        if (result.hasWarnings) {
            withWarnings++;
        }
    }
    
    // Show confirmation dialog
    QString message = QString("%1 spectra found.\n\n").arg(resultsToUse.size());
    message += QString("• %1 matched to stars (will be imported)\n").arg(matched);
    
    if (unmatched > 0) {
        message += QString("• %1 could not be matched (will be skipped)\n").arg(unmatched);
    }
    if (withWarnings > 0) {
        message += QString("• %1 have warnings (missing metadata)\n").arg(withWarnings);
    }
    
    message += "\nThe import will run in the background. Continue?";
    
    QMessageBox::StandardButton reply = QMessageBox::question(this, "Confirm Import", message,
                                                               QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes) {
        return false;
    }
    
    // Create import entries (only matched spectra)
    std::vector<SpectrumImportEntry> entries = createImportEntries(resultsToUse);
    
    if (entries.empty()) {
        QMessageBox::information(this, "No Spectra to Import",
                                 "No spectra could be matched to stars. Nothing to import.");
        return true;
    }
    
    LOG_INFO("SpectraImport", QString("Queueing %1 spectra for background import").arg(entries.size()));
    
    // Queue the background task
    queueImportTask(std::move(entries));
    
    // Show confirmation that task was queued
    _statusLabel->setText(QString("Import task queued: %1 spectra will be imported in the background.")
                          .arg(entries.size()));
    
    return true;
}

int SpectraImportPage::nextId() const
{
    return StarImportWizard::Page_RadialVelocity;
}