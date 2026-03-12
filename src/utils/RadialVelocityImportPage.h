#ifndef RADIALVELOCITYIMPORTPAGES_H
#define RADIALVELOCITYIMPORTPAGES_H

#include <QWizardPage>
#include <QHash>
#include <memory>
#include <vector>

class Star;
class Spectrum;
class SpectralFit;
class RadialVelocityCurve;
class RadialVelocityPoint;
class RVFit;
class ApplicationController;
class DatabaseManager;
class StarImportWizard;

class QLineEdit;
class QRadioButton;
class QStackedWidget;
class QPushButton;
class QProgressBar;
class QTreeWidget;
class QLabel;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QVBoxLayout; 

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
    void setupUi();
    void setupFromFitsPage();
    void setupFromFoldersPage();
    void setupFromTablePage();
    void setupFitParamsGroup(QVBoxLayout* parentLayout);

    // Star matching
    void buildStarLookupIndex();
    std::shared_ptr<Star> findStarByIdentifier(const QString& id,
                                                const QString& idType) const;

    // CSV helpers
    QChar getDelimiter(QComboBox* combo) const;
    QStringList parseLine(const QString& line, QChar delimiter) const;
    QChar detectDelimiter(const QString& line) const;
    void populateColumnCombos(const QStringList& columns,
                              const QList<QPair<QComboBox*, QStringList>>& comboPatterns);
    bool loadCSVFile(const QString& filepath, QComboBox* delimCombo,
                     QCheckBox* headerCheck,
                     QStringList& outColumns, std::vector<QStringList>& outRows);

    // Mode 1: From spectral fits
    void extractFromSpectralFits();

    // Mode 2: From per-star folders
    void detectFolderColumns();
    void scanAndParseFolders();

    // Mode 3: From single table
    void processTableData();

    // Fit parameters
    void parseFitParamsFile();
    void applyFitParams();

    // Metadata & preview
    void computeAllMetadata();
    void updatePreviewTree();

    // Save
    bool saveResults();
    bool isBackgroundBusy() const;

    // ── Collected result ────────────
    struct StarRVResult {
        std::shared_ptr<Star> star;
        std::shared_ptr<RadialVelocityCurve> curve;
        std::shared_ptr<RVFit> fit;   // may be null
    };
    std::vector<StarRVResult> _results;

    // ── UI: Mode ────────────────────
    QRadioButton* _fromFitsRadio;
    QRadioButton* _fromFoldersRadio;
    QRadioButton* _fromTableRadio;
    QStackedWidget* _modeStack;

    // ── UI: From Fits ───────────────
    QWidget* _fromFitsPage;
    QCheckBox* _bestFitOnlyCheck;
    QCheckBox* _skipZeroRVCheck;
    QPushButton* _extractFitsBtn;

    // ── UI: From Folders ────────────
    QWidget* _fromFoldersPage;
    QLineEdit* _rootFolderEdit;
    QComboBox* _folderNamingCombo;
    QComboBox* _folderDelimCombo;
    QCheckBox* _folderHeaderCheck;
    QComboBox* _folderTimeColCombo;
    QComboBox* _folderTimeTypeCombo;
    QComboBox* _folderRVColCombo;
    QComboBox* _folderRVErrColCombo;
    QPushButton* _scanFoldersBtn;
    QProgressBar* _folderProgress;

    // ── UI: From Table ──────────────
    QWidget* _fromTablePage;
    QLineEdit* _tableFileEdit;
    QComboBox* _tableDelimCombo;
    QCheckBox* _tableHeaderCheck;
    QComboBox* _tableIdColCombo;
    QComboBox* _tableIdTypeCombo;
    QComboBox* _tableTimeColCombo;
    QComboBox* _tableTimeTypeCombo;
    QComboBox* _tableRVColCombo;
    QComboBox* _tableRVErrColCombo;
    QPushButton* _processTableBtn;

    // ── UI: Fit params ──────────────
    QGroupBox* _fitParamsGroup;
    QCheckBox* _importFitParamsCheck;
    QLineEdit* _fitFileEdit;
    QComboBox* _fitDelimCombo;
    QCheckBox* _fitHeaderCheck;
    QComboBox* _fitIdColCombo;
    QComboBox* _fitIdTypeCombo;
    QComboBox* _fitKColCombo;
    QComboBox* _fitGammaColCombo;
    QComboBox* _fitPeriodColCombo;
    QComboBox* _fitT0ColCombo;
    QComboBox* _fitEccColCombo;
    QComboBox* _fitOmegaColCombo;

    // ── UI: Preview ─────────────────
    QTreeWidget* _previewTree;
    QLabel* _statusLabel;

    // ── Data ────────────────────────
    std::vector<std::shared_ptr<Star>> _importedStars;
    QHash<QString, std::shared_ptr<Star>> _sourceIdIndex;
    QHash<QString, std::shared_ptr<Star>> _aliasIndex;
    bool _indexBuilt = false;

    // Main table data (mode 3)
    QStringList _tableColumns;
    std::vector<QStringList> _tableRows;

    // Folder column names (mode 2, detected from first file)
    QStringList _folderDetectedColumns;

    // Fit params table data
    QStringList _fitColumns;
    std::vector<QStringList> _fitRows;

    bool _asyncBusy = false;
    bool _resultsReady = false;
};

#endif // RADIALVELOCITYIMPORTPAGES_H