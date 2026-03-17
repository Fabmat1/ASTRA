#ifndef LIGHTCURVEIMPORTPAGE_H
#define LIGHTCURVEIMPORTPAGE_H

#include <QWizardPage>
#include <memory>
#include <vector>
#include "models/Photometry.h"
#include "models/Time.h"

class QListWidget;
class QPushButton;
class QProgressBar;
class QTreeWidget;
class QTreeWidgetItem;
class QLabel;
class QCheckBox;
class QRadioButton;
class QLineEdit;
class QComboBox;
class QGroupBox;
class QStackedWidget;
class QFormLayout;

class ApplicationController;
class DatabaseManager;
class ImportStagingArea;
class Star;

// ── One discovered lightcurve file ──────────────────────────

struct LightcurveScanEntry
{
    QString starIdentifier;
    QString filePath;
    QString instrument;
    TimeScale detectedTimeScale = TimeScale::Unknown;
    int     numPoints   = 0;
    QStringList filters;

    // Match result
    QString matchedStarId;
    QString matchedStarDisplay;

    // Parsed data
    std::vector<LightcurvePoint> points;

    // UI state
    bool    selected     = true;
    bool    hasError     = false;
    QString errorMessage;
};

// ══════════════════════════════════════════════════════════════

class LightcurveImportPage : public QWizardPage
{
    Q_OBJECT
public:
    explicit LightcurveImportPage(QWidget* parent = nullptr);

    void setStagingArea(ImportStagingArea* staging);

    void initializePage() override;
    bool validatePage()   override;
    bool isComplete()     const override;
    int  nextId()         const override;

private slots:
    void browseRootDirectory();
    void browseCSVFile();
    void scanLightcurves();
    void onModeChanged();
    void onItemChanged(QTreeWidgetItem* item, int column);
    void onTimeScaleOverrideChanged();
    void selectAll();
    void selectNone();
    void selectMatched();

private:
    void scanFolderStructure(const QString& rootPath);
    void scanCSVManifest(const QString& csvPath);
    bool parseLightcurveFile(const QString& filePath,
                             TimeScale scale,
                             std::vector<LightcurvePoint>& outPoints,
                             QStringList& outFilters,
                             QString& outError);

    QString instrumentFromFilename(const QString& filename) const;
    void matchEntriesToStars();
    QString starDisplayName(std::shared_ptr<Star> star) const;
    void stageSelectedLightcurves();
    void populateTree();
    void updateSummary();
    void buildTimeScaleOverrides();

    ImportStagingArea* _staging = nullptr;

    QRadioButton*   _folderModeRadio = nullptr;
    QRadioButton*   _csvModeRadio    = nullptr;
    QStackedWidget* _modeStack       = nullptr;

    QLineEdit*   _rootDirEdit     = nullptr;
    QPushButton* _browseDirBtn    = nullptr;
    QComboBox*   _starIdTypeCombo = nullptr;

    QLineEdit*   _csvFileEdit  = nullptr;
    QPushButton* _browseCSVBtn = nullptr;

    QPushButton*  _scanBtn       = nullptr;
    QProgressBar* _progressBar   = nullptr;
    QCheckBox*    _createNewCb   = nullptr;
    QTreeWidget*  _resultsTree   = nullptr;
    QGroupBox*    _tsOverrideGroup = nullptr;
    QFormLayout*  _tsOverrideLayout = nullptr;
    QLabel*       _summaryLabel  = nullptr;

    std::vector<LightcurveScanEntry> _entries;
    QHash<QString, TimeScale> _instrumentTimeScales;
    QHash<QString, QComboBox*>          _timeScaleCombos;
    bool _scanned = false;
    bool _staged  = false;
};

#endif // LIGHTCURVEIMPORTPAGE_H