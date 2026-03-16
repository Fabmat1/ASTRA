#pragma once

#include <QWizardPage>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QChar>

#include <memory>
#include <vector>

class QVBoxLayout;
class QStackedWidget;
class QRadioButton;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QLabel;
class QProgressBar;
class QGroupBox;
class Star;

class RadialVelocityImportPage : public QWizardPage
{
    Q_OBJECT

public:
    explicit RadialVelocityImportPage(QWidget* parent = nullptr);

    void initializePage() override;
    bool validatePage() override;
    int nextId() const override;

private slots:
    void onImportModeChanged();
    void onExtractFromFits();
    void onBrowseRootFolder();
    void onScanFolders();
    void onBrowseTableFile();
    void onProcessTable();
    void onFitParamsToggled(bool checked);
    void onBrowseFitParamsFile();

private:
    // UI setup
    void setupUi();
    void setupFromFitsPage();
    void setupFromFoldersPage();
    void setupFromTablePage();
    void setupFitParamsGroup(QVBoxLayout* parentLayout);

    // Star lookup
    void buildStarLookupIndex();
    std::shared_ptr<Star> findStarByIdentifier(
        const QString& id, const QString& idType) const;

    // CSV helpers
    QChar getDelimiter(QComboBox* combo) const;
    QChar detectDelimiter(const QString& line) const;
    QStringList parseLine(const QString& line, QChar delimiter) const;
    bool loadCSVFile(const QString& filepath, QComboBox* delimCombo,
                     QCheckBox* headerCheck, QStringList& outColumns,
                     std::vector<QStringList>& outRows);
    void populateColumnCombos(const QStringList& columns,
                              const QList<QPair<QComboBox*, QStringList>>& comboPatterns);

    // Folder detection
    void detectFolderColumns();

    // Fit parameters
    void parseFitParamsFile();
    void applyFitParamsToProject();          // ← NEW (replaces old applyFitParams)

    // Preview
    void updatePreviewFromProject();          // ← NEW (replaces old updatePreviewTree)

    // Helpers
    bool isBackgroundBusy() const;

    // ── REMOVED ─────────────────────────────────────
    // void computeAllMetadata();             // dead
    // void updatePreviewTree();              // replaced
    // bool saveResults();                    // dead, had the bug
    // struct StarRVResult { ... };           // no longer needed
    // std::vector<StarRVResult> _results;    // no longer needed
    // ────────────────────────────────────────────────

    // State
    bool _resultsReady = false;
    bool _asyncBusy = false;
    bool _indexBuilt = false;

    // Star data
    std::vector<std::shared_ptr<Star>> _importedStars;
    QHash<QString, std::shared_ptr<Star>> _sourceIdIndex;
    QHash<QString, std::shared_ptr<Star>> _aliasIndex;

    // Table import data (loaded from file, passed to task)
    QStringList _tableColumns;
    std::vector<QStringList> _tableRows;

    // Fit params file data
    QStringList _fitColumns;
    std::vector<QStringList> _fitRows;

    // Folder column detection cache
    QStringList _folderDetectedColumns;

    // ── UI widgets ──────────────────────────────────
    // Mode selection
    QRadioButton* _fromFitsRadio = nullptr;
    QRadioButton* _fromFoldersRadio = nullptr;
    QRadioButton* _fromTableRadio = nullptr;
    QStackedWidget* _modeStack = nullptr;

    // From fits page
    QWidget* _fromFitsPage = nullptr;
    QCheckBox* _bestFitOnlyCheck = nullptr;
    QCheckBox* _skipZeroRVCheck = nullptr;
    QPushButton* _extractFitsBtn = nullptr;

    // From folders page
    QWidget* _fromFoldersPage = nullptr;
    QLineEdit* _rootFolderEdit = nullptr;
    QComboBox* _folderNamingCombo = nullptr;
    QComboBox* _folderDelimCombo = nullptr;
    QCheckBox* _folderHeaderCheck = nullptr;
    QComboBox* _folderTimeColCombo = nullptr;
    QComboBox* _folderTimeTypeCombo = nullptr;
    QComboBox* _folderRVColCombo = nullptr;
    QComboBox* _folderRVErrColCombo = nullptr;
    QPushButton* _scanFoldersBtn = nullptr;
    QProgressBar* _folderProgress = nullptr;

    // From table page
    QWidget* _fromTablePage = nullptr;
    QLineEdit* _tableFileEdit = nullptr;
    QComboBox* _tableDelimCombo = nullptr;
    QCheckBox* _tableHeaderCheck = nullptr;
    QComboBox* _tableIdColCombo = nullptr;
    QComboBox* _tableIdTypeCombo = nullptr;
    QComboBox* _tableTimeColCombo = nullptr;
    QComboBox* _tableTimeTypeCombo = nullptr;
    QComboBox* _tableRVColCombo = nullptr;
    QComboBox* _tableRVErrColCombo = nullptr;
    QPushButton* _processTableBtn = nullptr;

    // Fit params group
    QGroupBox* _fitParamsGroup = nullptr;
    QCheckBox* _importFitParamsCheck = nullptr;
    QLineEdit* _fitFileEdit = nullptr;
    QComboBox* _fitDelimCombo = nullptr;
    QCheckBox* _fitHeaderCheck = nullptr;
    QComboBox* _fitIdColCombo = nullptr;
    QComboBox* _fitIdTypeCombo = nullptr;
    QComboBox* _fitKColCombo = nullptr;
    QComboBox* _fitGammaColCombo = nullptr;
    QComboBox* _fitPeriodColCombo = nullptr;
    QComboBox* _fitT0ColCombo = nullptr;
    QComboBox* _fitEccColCombo = nullptr;
    QComboBox* _fitOmegaColCombo = nullptr;

    // Preview
    QTreeWidget* _previewTree = nullptr;
    QLabel* _statusLabel = nullptr;
};