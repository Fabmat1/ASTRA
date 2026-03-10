#ifndef SPECTRAIMPORTPAGE_H
#define SPECTRAIMPORTPAGE_H

#include <QWizardPage>
#include <memory>
#include <vector>
#include <optional>

#include "SpectrumReader.h"

class Star;
class Spectrum;
class QLineEdit;
class QRadioButton;
class QStackedWidget;
class QListWidget;
class QListWidgetItem;
class QDoubleSpinBox;
class QPushButton;
class QProgressBar;
class QTreeWidget;
class QLabel;
class QComboBox;
class QCheckBox;
class QSpinBox;

struct SpectrumImportEntry;  // Forward declaration

// Matching method enum
enum class MatchMethod {
    SourceId,
    Alias,
    Position
};

// Spectrum matching result
struct SpectrumMatchResult {
    QString spectrumFile;
    std::shared_ptr<Spectrum> spectrum;
    std::shared_ptr<Star> matchedStar;
    QString matchMethod;  // "position", "source_id", "alias", "manual"
    double matchDistance = -1.0;  // arcsec for position matching, -1 if not applicable
    bool hasWarnings = false;
    QStringList warnings;
    int sourceRowIndex = -1;  // Index in _mappingRows for full processing later
};

// ── Spatial grid index for fast position matching ────────────────
class SpatialStarIndex
{
public:
    SpatialStarIndex() = default;

    void build(const std::vector<std::shared_ptr<Star>>& stars, double cellSizeDeg = 0.05);
    std::shared_ptr<Star> findNearest(double ra, double dec, double radiusArcsec,
                                       double* outDistArcsec = nullptr) const;
    bool isBuilt() const { return _built; }

private:
    struct CellKey {
        int raCell;
        int decCell;
        bool operator==(const CellKey& o) const { return raCell == o.raCell && decCell == o.decCell; }
    };
    struct CellKeyHash {
        size_t operator()(const CellKey& k) const {
            return std::hash<long long>()(((long long)k.raCell << 32) | (unsigned int)k.decCell);
        }
    };

    CellKey cellFor(double ra, double dec) const;

    std::unordered_map<CellKey, std::vector<std::shared_ptr<Star>>, CellKeyHash> _grid;
    double _cellSizeDeg = 0.05;
    bool _built = false;
};

class SpectraImportPage : public QWizardPage
{
    Q_OBJECT

public:
    SpectraImportPage(QWidget* parent = nullptr);
    
    void initializePage() override;
    bool validatePage() override;
    int nextId() const override;
    
private slots:
    void onImportModeChanged();
    void onBrowseFitsFolder();
    void onBrowseFitsFiles();
    void onBrowseMappingFile();
    void onScanFiles();
    void onPreviewMatching();
    void onMappingFileLoaded();
    void onMappingColumnChanged();
    void onPreviewButtonClicked();
    void onMatchMethodMoveUp();
    void onMatchMethodMoveDown();
    void onMatchMethodItemChanged(QListWidgetItem* item);
    void onMatchMethodChanged();

    // Async completion slots
    void onScanComplete(std::vector<SpectrumMetadata> metadata);
    void onFullMappingComplete(std::vector<SpectrumMatchResult> results);
    
    
private:
    void setupUi();
    void setupFitsPage();
    void setupMappingPage();
    void setupMatchingOptionsGroup();
    void updatePreviewTable();
    void autoGeneratePreview();
    
    // FITS scanning and matching
    void scanFitsFilesAsync(const QStringList& files);
    std::vector<SpectrumMatchResult> matchSpectraToStars();
    
    // Star finding methods
    std::shared_ptr<Star> findStarByPosition(double ra, double dec, double radiusArcsec, double* outDistance = nullptr);
    std::shared_ptr<Star> findStarBySourceId(const QString& sourceId);
    std::shared_ptr<Star> findStarByAlias(const QString& alias);
    
    // Unified matching using configured priorities
    std::shared_ptr<Star> findMatchingStar(const QString& sourceId, const QString& alias,
                                            double ra, double dec, bool hasPosition,
                                            QString& outMatchMethod, double& outMatchDistance);
    
    // Thread-safe matching (uses only read-only indices)
    std::shared_ptr<Star> findMatchingStarThreadSafe(
        const QString& sourceId, const QString& alias,
        double ra, double dec, bool hasPosition,
        const std::vector<MatchMethod>& methods, double matchRadiusArcsec,
        QString& outMatchMethod, double& outMatchDistance) const;
    
    // Get ordered list of enabled matching methods
    std::vector<MatchMethod> getEnabledMatchMethods() const;
    
    // Mapping file parsing
    bool loadMappingFile(const QString& filepath);
    void detectMappingColumns();
    void applyMappingColumnSettings();
    std::vector<SpectrumMatchResult> processMapping(int maxRows = -1);
    void processFullMappingAsync();
    SpectrumMatchResult processOneRow(const QStringList& row, int rowIndex) const;
    
    // Build index for faster star lookups
    void buildStarLookupIndex();
    
    // Create import entries for background task
    std::vector<SpectrumImportEntry> createImportEntries(const std::vector<SpectrumMatchResult>& results);
    
    // Queue background import task
    void queueImportTask(std::vector<SpectrumImportEntry> entries);
    
    // UI - Mode selection
    QRadioButton* _fitsRadio;
    QRadioButton* _mappingRadio;
    QStackedWidget* _modeStack;
    
    // UI - FITS mode
    QWidget* _fitsPage;
    QLineEdit* _fitsFolderEdit;
    QListWidget* _fitsFilesList;
    QPushButton* _scanButton;
    QProgressBar* _scanProgress;
    
    // UI - Mapping mode
    QWidget* _mappingPage;
    QLineEdit* _mappingFileEdit;
    QComboBox* _delimiterCombo;
    QCheckBox* _hasHeaderCheck;
    QPushButton* _previewButton;
    QSpinBox* _previewRowsSpin;
    
    // UI - Matching options (shared between modes)
    QListWidget* _matchMethodList;
    QPushButton* _moveUpButton;
    QPushButton* _moveDownButton;
    QDoubleSpinBox* _matchRadiusSpin;
    QLabel* _matchRadiusLabel;
    QRadioButton* _matchPositionRadio;
    QRadioButton* _matchSourceIdRadio;
    QRadioButton* _matchObjectNameRadio;

    
    // Mapping column combos
    QComboBox* _filePathColumnCombo;
    QComboBox* _starIdColumnCombo;
    QComboBox* _sourceIdColumnCombo;
    QComboBox* _raColumnCombo;
    QComboBox* _decColumnCombo;
    QComboBox* _mjdColumnCombo;
    QComboBox* _bjdColumnCombo;
    QComboBox* _expTimeColumnCombo;
    QComboBox* _instrumentColumnCombo;
    QComboBox* _baryCorrColumnCombo;
    
    // UI - Preview (shared)
    QTreeWidget* _previewTree;
    QLabel* _statusLabel;
    
    // Data
    std::vector<std::shared_ptr<Star>> _importedStars;
    std::vector<SpectrumMetadata> _scannedMetadata;
    std::vector<SpectrumMatchResult> _matchResults;  // Preview results
    std::vector<SpectrumMatchResult> _fullMatchResults;  // Full results
    bool _fullResultsReady;
    bool _asyncBusy;  // True while a QtConcurrent operation is running
    
    // Mapping file data
    QStringList _mappingColumns;
    std::vector<QStringList> _mappingRows;
    QString _mappingBasePath;
    
    // Star lookup indices for faster matching
    QHash<QString, std::shared_ptr<Star>> _sourceIdIndex;
    QHash<QString, std::shared_ptr<Star>> _aliasIndex;
    SpatialStarIndex _spatialIndex;
    bool _indexBuilt;
    
    // Flags to prevent recursive updates
    bool _updatingMatchMethods;
    
    static const int DEFAULT_PREVIEW_ROWS = 100;
};

#endif // SPECTRAIMPORTPAGE_H