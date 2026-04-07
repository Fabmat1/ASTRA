#include "../importWizard/SEDImportPage.h"
#include "../importWizard/StarImportWizard.h"
#include "../importWizard/ImportStagingArea.h"
#include "../utils/ExtractSED.h"
#include "../db/DatabaseManager.h"
#include "../utils/Logger.h"

#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include "models/Photometry.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QListWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QProgressBar>
#include <QGroupBox>
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

#include <cmath>

// ══════════════════════════════════════════════════════════════
// Construction & setup
// ══════════════════════════════════════════════════════════════

SEDImportPage::SEDImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import SED Fits");
    setSubTitle("Select root directories to recursively search for ISIS SED "
                "fit folders. Each folder is matched to a project star by "
                "coordinates from photometry.dat.");

    auto* mainLayout = new QVBoxLayout(this);

    // ── Directory selection ──────────────────────────────────
    auto* dirGroup = new QGroupBox("Search Directories");
    auto* dirLayout = new QVBoxLayout(dirGroup);

    _dirList = new QListWidget;
    _dirList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _dirList->setMaximumHeight(100);
    dirLayout->addWidget(_dirList);

    auto* dirBtnLayout = new QHBoxLayout;
    _addDirBtn = new QPushButton("Add Directory…");
    _removeDirBtn = new QPushButton("Remove Selected");
    _removeDirBtn->setEnabled(false);
    dirBtnLayout->addWidget(_addDirBtn);
    dirBtnLayout->addWidget(_removeDirBtn);
    dirBtnLayout->addStretch();
    dirLayout->addLayout(dirBtnLayout);

    mainLayout->addWidget(dirGroup);

    // ── Scan controls ────────────────────────────────────────
    auto* scanLayout = new QHBoxLayout;
    _scanBtn = new QPushButton("Scan for SED Fits");
    _scanBtn->setEnabled(false);
    _progressBar = new QProgressBar;
    _progressBar->setVisible(false);
    scanLayout->addWidget(_scanBtn);
    scanLayout->addWidget(_progressBar, 1);
    mainLayout->addLayout(scanLayout);

    // ── Options ──────────────────────────────────────────────
    _createNewCb = new QCheckBox(
        "Create new stars for unmatched SED fits (using coordinates)");
    _createNewCb->setChecked(false);
    mainLayout->addWidget(_createNewCb);

    // ── Results tree ─────────────────────────────────────────
    _resultsTree = new QTreeWidget;
    _resultsTree->setHeaderLabels({
        "Import", "Folder", "Object", "RA", "DEC",
        "Matched Star", "Comp.", "Status"
    });
    _resultsTree->setRootIsDecorated(false);
    _resultsTree->setAlternatingRowColors(true);
    _resultsTree->header()->setStretchLastSection(true);
    _resultsTree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    _resultsTree->setColumnWidth(0, 50);
    mainLayout->addWidget(_resultsTree, 1);

    // ── Selection shortcuts ──────────────────────────────────
    auto* selLayout = new QHBoxLayout;
    auto* selAllBtn    = new QPushButton("Select All");
    auto* selNoneBtn   = new QPushButton("Select None");
    auto* selMatchBtn  = new QPushButton("Select Matched Only");
    selLayout->addWidget(selAllBtn);
    selLayout->addWidget(selNoneBtn);
    selLayout->addWidget(selMatchBtn);
    selLayout->addStretch();
    mainLayout->addLayout(selLayout);

    // ── Summary ──────────────────────────────────────────────
    _summaryLabel = new QLabel;
    _summaryLabel->setStyleSheet("color: #888; font-style: italic;");
    mainLayout->addWidget(_summaryLabel);
    updateSummary();

    // ── Connections ──────────────────────────────────────────
    connect(_addDirBtn,    &QPushButton::clicked, this, &SEDImportPage::addRootDirectory);
    connect(_removeDirBtn, &QPushButton::clicked, this, &SEDImportPage::removeSelectedDirectory);
    connect(_scanBtn,      &QPushButton::clicked, this, &SEDImportPage::scanDirectories);
    connect(selAllBtn,     &QPushButton::clicked, this, &SEDImportPage::selectAll);
    connect(selNoneBtn,    &QPushButton::clicked, this, &SEDImportPage::selectNone);
    connect(selMatchBtn,   &QPushButton::clicked, this, &SEDImportPage::selectMatched);
    connect(_resultsTree,  &QTreeWidget::itemChanged, this, &SEDImportPage::onItemChanged);
    connect(_createNewCb,  &QCheckBox::toggled, this, [this]{ updateSummary(); emit completeChanged(); });

    connect(_dirList, &QListWidget::itemSelectionChanged, this, [this]{
        _removeDirBtn->setEnabled(!_dirList->selectedItems().isEmpty());
    });
    connect(_dirList->model(), &QAbstractItemModel::rowsInserted, this, [this]{
        _scanBtn->setEnabled(_dirList->count() > 0);
    });
    connect(_dirList->model(), &QAbstractItemModel::rowsRemoved, this, [this]{
        _scanBtn->setEnabled(_dirList->count() > 0);
    });
}

void SEDImportPage::setStagingArea(ImportStagingArea* staging)
{
    _staging = staging;
}

// ══════════════════════════════════════════════════════════════
// QWizardPage interface
// ══════════════════════════════════════════════════════════════

void SEDImportPage::initializePage()
{
    _staged = false;
}

bool SEDImportPage::validatePage()
{
    // Count how many are selected
    int selectedCount = 0;
    for (const auto& e : _entries) {
        if (e.selected && !e.hasError)
            ++selectedCount;
    }
    if (selectedCount == 0) return true;   // nothing to do — allow finish

    stageSelectedSEDs();
    return true;
}

bool SEDImportPage::isComplete() const
{
    return true;
}


// ══════════════════════════════════════════════════════════════
// Directory management
// ══════════════════════════════════════════════════════════════

void SEDImportPage::addRootDirectory()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select Root Directory for SED Search",
        QDir::homePath(), QFileDialog::ShowDirsOnly);
    if (dir.isEmpty()) return;

    // Avoid duplicates
    for (int i = 0; i < _dirList->count(); ++i) {
        if (_dirList->item(i)->text() == dir) return;
    }
    _dirList->addItem(dir);
    _scanned = false;
    emit completeChanged();
}

void SEDImportPage::removeSelectedDirectory()
{
    auto items = _dirList->selectedItems();
    for (auto* item : items)
        delete item;
    _scanned = false;
    emit completeChanged();
}

// ══════════════════════════════════════════════════════════════
// Scanning
// ══════════════════════════════════════════════════════════════

void SEDImportPage::scanDirectories()
{
    _entries.clear();
    _resultsTree->clear();
    _scanned = false;
    _staged  = false;

    // ── Collect root paths ───────────────────────────────────
    QStringList roots;
    for (int i = 0; i < _dirList->count(); ++i)
        roots << _dirList->item(i)->text();

    if (roots.isEmpty()) return;

    // ── Phase 1: discover SED directories ────────────────────
    _progressBar->setVisible(true);
    _progressBar->setRange(0, 0);          // indeterminate
    _progressBar->setFormat("Discovering SED folders…");
    _scanBtn->setEnabled(false);
    QApplication::processEvents();

    QStringList sedDirs;
    for (const auto& root : roots)
        findSEDDirectories(root, sedDirs);

    if (sedDirs.isEmpty()) {
        _progressBar->setVisible(false);
        _scanBtn->setEnabled(true);
        _summaryLabel->setText("No SED fit directories found.");
        emit completeChanged();
        return;
    }

    // ── Phase 2: extract data from each directory ────────────
    _progressBar->setRange(0, sedDirs.size());
    _progressBar->setValue(0);
    _progressBar->setFormat("Extracting %v / %m …");
    _entries.reserve(sedDirs.size());

    for (int i = 0; i < sedDirs.size(); ++i) {
        const QString& dirPath = sedDirs[i];

        SEDScanEntry entry;
        entry.dirPath = dirPath;
        entry.starIdentifier = extractStarIdentifier(dirPath);

        // Parse coordinates from photometry.dat
        entry.coordsValid = parsePhotometryDatCoords(dirPath,
                                                     entry.ra, entry.dec);

        // Extract SED fit data
        auto result = ExtractSED::extractFromDirectory(dirPath);
        if (result.success) {
            entry.model            = result.model;
            entry.photometricPoints = result.photometricPoints;
            entry.objectName       = result.objectName;
            entry.numComponents    = result.model ? entry.model->numComponents : 0;
        } else {
            entry.hasError     = true;
            entry.errorMessage = result.errorMessage;
            entry.selected     = false;
        }

        _entries.push_back(std::move(entry));

        _progressBar->setValue(i + 1);
        if (i % 10 == 0) QApplication::processEvents();
    }

    // ── Phase 3: match to project stars ──────────────────────
    _progressBar->setFormat("Matching to project stars…");
    _progressBar->setRange(0, 0);
    QApplication::processEvents();

    matchEntriesToStars();

    // ── Done ─────────────────────────────────────────────────
    _progressBar->setVisible(false);
    _scanBtn->setEnabled(true);
    _scanned = true;

    populateTree();
    updateSummary();
    emit completeChanged();
}

void SEDImportPage::findSEDDirectories(const QString& rootPath,
                                       QStringList& out)
{
    QDirIterator it(rootPath, QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString path = it.next();
        if (ExtractSED::isSEDFitDirectory(path))
            out.append(path);
    }

    // Also check the root itself
    if (ExtractSED::isSEDFitDirectory(rootPath))
        out.append(rootPath);
}

bool SEDImportPage::parsePhotometryDatCoords(const QString& dirPath,
                                             double& ra, double& dec)
{
    QFile file(QDir(dirPath).filePath("photometry.dat"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;

    // Read up to 5 lines looking for the RA/DEC comment
    QTextStream in(&file);
    for (int i = 0; i < 5 && !in.atEnd(); ++i) {
        QString line = in.readLine();
        static QRegularExpression re(
            R"(RA\s*=\s*([+-]?[\d.]+)\s+DEC\s*=\s*([+-]?[\d.]+))");
        auto m = re.match(line);
        if (m.hasMatch()) {
            bool okRa, okDec;
            ra  = m.captured(1).toDouble(&okRa);
            dec = m.captured(2).toDouble(&okDec);
            return okRa && okDec;
        }
    }
    return false;
}

QString SEDImportPage::extractStarIdentifier(const QString& sedDir)
{
    QFileInfo info(sedDir);
    QString dirName = info.fileName();

    // If the directory itself has a generic name, step up to parent
    static const QStringList genericNames = {
        "SED", "sed", "fit", "FIT", "photometry", "model", "output", "results"
    };
    if (genericNames.contains(dirName, Qt::CaseInsensitive)) {
        dirName = QFileInfo(info.absolutePath()).fileName();
        // If still generic, step up once more
        if (genericNames.contains(dirName, Qt::CaseInsensitive))
            dirName = QFileInfo(QFileInfo(info.absolutePath()).absolutePath())
                          .fileName();
    }
    return dirName;
}

// ══════════════════════════════════════════════════════════════
// Star matching
// ══════════════════════════════════════════════════════════════

void SEDImportPage::matchEntriesToStars()
{
    auto* importWizard = qobject_cast<StarImportWizard*>(wizard());
    if (!importWizard || !importWizard->controller()) return;

    auto* dbm = importWizard->controller()->databaseManager();
    auto  proj = importWizard->project();
    if (!dbm || !proj) return;
    const QString projectId = proj->getId();

    // Pre-load all project stars for display-name lookup
    auto allStars = dbm->loadStars(projectId);
    QHash<QString, std::shared_ptr<Star>> starMap;
    for (const auto& s : allStars)
        starMap[s->getId()] = s;

    for (auto& entry : _entries) {
        if (entry.hasError) continue;

        // Build match identifiers
        QString sourceId;
        QString alias = entry.starIdentifier;
        QString jname;
        QString tic;
        double  ra  = entry.coordsValid ? entry.ra  : std::nan("");
        double  dec = entry.coordsValid ? entry.dec : std::nan("");

        // Extract Gaia DR3 source_id from object name
        if (!entry.objectName.isEmpty()) {
            static QRegularExpression gaiaRe(
                R"(Gaia\s+DR\d\s+(\d+))");
            auto m = gaiaRe.match(entry.objectName);
            if (m.hasMatch())
                sourceId = entry.objectName;   // full "Gaia DR3 ..."
        }

        // Try to extract J-name from folder identifier
        {
            static QRegularExpression jnameRe(
                R"((J\d{4,6}[.]\d{1,2}[+-]\d{4,6}[.]\d{1,2}))");
            auto m = jnameRe.match(entry.starIdentifier);
            if (m.hasMatch())
                jname = m.captured(1);
        }

        QString matchId = dbm->findMatchingStarId(
            projectId, sourceId, alias, tic, jname, ra, dec);

        if (!matchId.isEmpty()) {
            entry.matchedStarId = matchId;
            auto it = starMap.find(matchId);
            if (it != starMap.end())
                entry.matchedStarDisplay = starDisplayName(it.value());
            else
                entry.matchedStarDisplay = matchId;
        }
    }
}

QString SEDImportPage::starDisplayName(std::shared_ptr<Star> star) const
{
    if (!star) return QString();
    if (!star->getAlias().isEmpty())    return star->getAlias();
    if (!star->getJName().isEmpty())    return star->getJName();
    if (!star->getSourceId().isEmpty()) return star->getSourceId();
    if (!star->getTic().isEmpty())      return "TIC " + star->getTic();
    if (Star::isSet(star->getRa()) && Star::isSet(star->getDec()))
        return QString("(%1, %2)")
            .arg(star->getRa(), 0, 'f', 5)
            .arg(star->getDec(), 0, 'f', 5);
    return star->getId().left(8) + "…";
}

// ══════════════════════════════════════════════════════════════
// Tree display
// ══════════════════════════════════════════════════════════════

void SEDImportPage::populateTree()
{
    _resultsTree->blockSignals(true);
    _resultsTree->clear();

    for (int i = 0; i < static_cast<int>(_entries.size()); ++i) {
        const auto& e = _entries[i];

        auto* item = new QTreeWidgetItem;
        item->setData(0, Qt::UserRole, i);   // store index

        // Column 0: checkbox
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(0, e.selected ? Qt::Checked : Qt::Unchecked);

        // Column 1: folder (show last 2–3 path components)
        {
            QStringList parts = e.dirPath.split('/', Qt::SkipEmptyParts);
            int n = parts.size();
            QString shortPath = (n > 3)
                ? "…/" + parts.mid(n - 3).join('/')
                : e.dirPath;
            item->setText(1, shortPath);
            item->setToolTip(1, e.dirPath);
        }

        // Column 2: object name
        item->setText(2, e.objectName.isEmpty() ? e.starIdentifier : e.objectName);

        // Column 3–4: RA, DEC
        if (e.coordsValid) {
            item->setText(3, QString::number(e.ra,  'f', 6));
            item->setText(4, QString::number(e.dec, 'f', 6));
        } else {
            item->setText(3, "—");
            item->setText(4, "—");
        }

        // Column 5: matched star
        item->setText(5, e.matchedStarDisplay.isEmpty()
                             ? (e.hasError ? "" : "No match")
                             : e.matchedStarDisplay);

        // Column 6: components
        item->setText(6, e.hasError ? "" : QString::number(e.numComponents));

        // Column 7: status + colour
        if (e.hasError) {
            item->setText(7, "Error");
            item->setToolTip(7, e.errorMessage);
            for (int c = 0; c < 8; ++c)
                item->setForeground(c, QColor("#cc4444"));
        } else if (!e.matchedStarId.isEmpty()) {
            item->setText(7, "Matched");
            item->setForeground(7, QColor("#44aa44"));
        } else {
            item->setText(7, "Unmatched");
            item->setForeground(7, QColor("#cc8800"));
        }

        _resultsTree->addTopLevelItem(item);
    }

    _resultsTree->blockSignals(false);
}

void SEDImportPage::onItemChanged(QTreeWidgetItem* item, int column)
{
    if (column != 0) return;
    int idx = item->data(0, Qt::UserRole).toInt();
    if (idx < 0 || idx >= static_cast<int>(_entries.size())) return;
    _entries[idx].selected = (item->checkState(0) == Qt::Checked);
    updateSummary();
    emit completeChanged();
}

void SEDImportPage::selectAll()
{
    _resultsTree->blockSignals(true);
    for (int i = 0; i < _resultsTree->topLevelItemCount(); ++i) {
        auto* item = _resultsTree->topLevelItem(i);
        int idx = item->data(0, Qt::UserRole).toInt();
        if (idx >= 0 && idx < static_cast<int>(_entries.size()) && !_entries[idx].hasError) {
            item->setCheckState(0, Qt::Checked);
            _entries[idx].selected = true;
        }
    }
    _resultsTree->blockSignals(false);
    updateSummary();
    emit completeChanged();
}

void SEDImportPage::selectNone()
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

void SEDImportPage::selectMatched()
{
    _resultsTree->blockSignals(true);
    for (int i = 0; i < _resultsTree->topLevelItemCount(); ++i) {
        auto* item = _resultsTree->topLevelItem(i);
        int idx = item->data(0, Qt::UserRole).toInt();
        if (idx >= 0 && idx < static_cast<int>(_entries.size())) {
            bool matched = !_entries[idx].matchedStarId.isEmpty()
                           && !_entries[idx].hasError;
            item->setCheckState(0, matched ? Qt::Checked : Qt::Unchecked);
            _entries[idx].selected = matched;
        }
    }
    _resultsTree->blockSignals(false);
    updateSummary();
    emit completeChanged();
}

void SEDImportPage::updateSummary()
{
    int total = 0, matched = 0, unmatched = 0, errors = 0, selected = 0;
    for (const auto& e : _entries) {
        ++total;
        if (e.hasError) { ++errors; continue; }
        if (e.matchedStarId.isEmpty()) ++unmatched;
        else                           ++matched;
        if (e.selected && !e.hasError) ++selected;
    }

    QString text = QString("Found %1 SED fit(s): %2 matched, %3 unmatched, "
                           "%4 error(s).  Selected for import: %5")
                       .arg(total).arg(matched).arg(unmatched)
                       .arg(errors).arg(selected);
    _summaryLabel->setText(text);
}

// ══════════════════════════════════════════════════════════════
// Staging
// ══════════════════════════════════════════════════════════════

void SEDImportPage::stageSelectedSEDs()
{
    if (_staged) return;
    if (!_staging) return;

    auto* importWizard = qobject_cast<StarImportWizard*>(wizard());
    if (!importWizard || !importWizard->controller()) return;

    auto* dbm = importWizard->controller()->databaseManager();
    auto  proj = importWizard->project();
    if (!dbm || !proj) return;
    const QString projectId = proj->getId();

    bool createNew = _createNewCb->isChecked();

    // ── Collect star IDs that need pulling ───────────────────
    QStringList pullIds;
    for (const auto& e : _entries) {
        if (!e.selected || e.hasError || !e.model) continue;
        if (!e.matchedStarId.isEmpty() && !_staging->hasStar(e.matchedStarId))
            pullIds << e.matchedStarId;
    }

    // Pull existing stars into staging
    if (!pullIds.isEmpty())
        _staging->pullStarsFromDB(dbm, projectId, pullIds);

    // Pull photometry for all stars now in staging
    _staging->pullPhotometryFromDB(dbm);

    // ── Stage each selected entry ────────────────────────────
    int stagedCount = 0;
    for (auto& entry : _entries) {
        if (!entry.selected || entry.hasError || !entry.model) continue;

        if (!entry.matchedStarId.isEmpty()) {
            // ── Existing star ────────────────────────────────
            auto star = _staging->getStar(entry.matchedStarId);
            if (!star) {
                LOG_WARNING("SEDImport",
                    QString("Matched star %1 not found in staging")
                        .arg(entry.matchedStarId));
                continue;
            }

            // Ensure photometry object
            if (!star->getPhotometry()) {
                auto phot = dbm->loadPhotometry(star->getId());
                if (!phot) phot = std::make_shared<Photometry>();
                star->setPhotometry(phot);
            }

            entry.model->isBestFit = true;
            star->getPhotometry()->addSEDModel(entry.model);
            _staging->markStarDirty(entry.matchedStarId);
            _staging->markSEDModelNew(entry.model->getId());
            ++stagedCount;

        } else if (createNew) {
            // ── New star ─────────────────────────────────────
            auto star = std::make_shared<Star>();
            star->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

            if (entry.coordsValid) {
                star->setRa(entry.ra);
                star->setDec(entry.dec);
            }

            // Set identifiers
            if (!entry.objectName.isEmpty()) {
                static QRegularExpression gaiaRe(R"(Gaia\s+DR\d\s+(\d+))");
                auto m = gaiaRe.match(entry.objectName);
                if (m.hasMatch())
                    star->setSourceId(entry.objectName);
            }
            if (!entry.starIdentifier.isEmpty()) {
                star->setAlias(entry.starIdentifier);
                static QRegularExpression jnameRe(
                    R"((J\d{4,6}[.]\d{1,2}[+-]\d{4,6}[.]\d{1,2}))");
                auto m = jnameRe.match(entry.starIdentifier);
                if (m.hasMatch())
                    star->setJName(m.captured(1));
            }

            // Create photometry with SED model
            auto phot = std::make_shared<Photometry>();
            entry.model->isBestFit = true;
            phot->addSEDModel(entry.model);

            for (const auto& pp : entry.photometricPoints)
                phot->addPhotometricPoint(pp);

            star->setPhotometry(phot);
            _staging->addStar(star, /*isNew=*/true);
            _staging->markSEDModelNew(entry.model->getId());
            ++stagedCount;
        }
    }

    LOG_INFO("SEDImport",
        QString("Staged %1 SED model(s)").arg(stagedCount));

    _staged = true;
}

int SEDImportPage::nextId() const
{
    return StarImportWizard::Page_LightcurveImport;
}