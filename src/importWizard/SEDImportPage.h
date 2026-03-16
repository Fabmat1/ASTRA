#ifndef SEDIMPORTPAGE_H
#define SEDIMPORTPAGE_H

#include <QWizardPage>
#include <memory>
#include <vector>
#include "models/Photometry.h"
#include "../utils/ExtractSED.h"

class QListWidget;
class QPushButton;
class QProgressBar;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QCheckBox;

class ApplicationController;
class DatabaseManager;
class ImportStagingArea;
class Star;
class SEDModel;

// ── One discovered SED fit directory ────────────────────────────

struct SEDScanEntry
{
    QString dirPath;
    QString starIdentifier;     // from folder name
    QString objectName;         // from tex (e.g. "Gaia DR3 …")
    double  ra          = 0.0;
    double  dec         = 0.0;
    bool    coordsValid = false;
    int     numComponents = 0;

    // Match result
    QString matchedStarId;
    QString matchedStarDisplay;

    // Extracted data (kept until staging)
    std::shared_ptr<SEDModel>     model;
    std::vector<PhotometricPoint> photometricPoints;

    // UI state
    bool    selected     = true;
    bool    hasError     = false;
    QString errorMessage;
};

// ══════════════════════════════════════════════════════════════

class SEDImportPage : public QWizardPage
{
    Q_OBJECT
public:
    explicit SEDImportPage(QWidget* parent = nullptr);

    void setStagingArea(ImportStagingArea* staging);

    // QWizardPage interface
    void initializePage() override;
    bool validatePage()   override;
    bool isComplete()     const override;
    int  nextId()         const override;

private slots:
    void addRootDirectory();
    void removeSelectedDirectory();
    void scanDirectories();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void selectAll();
    void selectNone();
    void selectMatched();

private:
    // Scanning helpers
    void findSEDDirectories(const QString& rootPath, QStringList& out);
    bool parsePhotometryDatCoords(const QString& dirPath,
                                  double& ra, double& dec);
    QString extractStarIdentifier(const QString& sedDir);
    void matchEntriesToStars();
    QString starDisplayName(std::shared_ptr<Star> star) const;

    // Staging
    void stageSelectedSEDs();

    // UI update
    void populateTree();
    void updateSummary();

    // ── External references ──────────────────────────────
    ImportStagingArea*     _staging = nullptr;

    // ── UI widgets ───────────────────────────────────────
    QListWidget*  _dirList       = nullptr;
    QPushButton*  _addDirBtn     = nullptr;
    QPushButton*  _removeDirBtn  = nullptr;
    QPushButton*  _scanBtn       = nullptr;
    QProgressBar* _progressBar   = nullptr;
    QTreeWidget*  _resultsTree   = nullptr;
    QLabel*       _summaryLabel  = nullptr;
    QCheckBox*    _createNewCb   = nullptr;   // create stars for unmatched

    // ── Data ─────────────────────────────────────────────
    std::vector<SEDScanEntry> _entries;
    bool _scanned = false;
    bool _staged  = false;
};

#endif // SEDIMPORTPAGE_H