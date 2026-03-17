#ifndef SPECTRALFITIMPORTPAGE_H
#define SPECTRALFITIMPORTPAGE_H

#include <QWizardPage>
#include <QMap>
#include <QPair>
#include <memory>
#include <vector>

class Star;
class Spectrum;
class SpectralFit;
class ApplicationController;

class QLineEdit;
class QRadioButton;
class QStackedWidget;
class QPushButton;
class QProgressBar;
class QTreeWidget;
class QLabel;
class QCheckBox;

// ── Transient: raw filesystem scan result from background thread ─────
struct DiggaScanResult {
    QString dirPath;
    QString gridName;        // directory name containing fit files
    QString parentDirName;   // parent directory name (often source_id)

    QString fitReportContent;
    QString fitParametersContent;

    // plotdata files found: basename (lowercase, no _plotdata.csv) → full path
    QMap<QString, QString> plotdataFiles;

    bool valid = true;
    QString error;
};

// ── Persistent: parsed DIGGA directory with matching results ─────────
struct DiggaFitDirectory {
    QString dirPath;
    QString gridName;
    QString parentDirName;

    // From fit_report.tex — spec index → spectrum filename
    QMap<int, QString> specIndexToFilename;

    // Shared (tied) parameters from fit_parameters.csv
    double chi2 = 0.0;
    double teff = 0.0, teffError = 0.0;
    double logg = 0.0, loggError = 0.0;
    double he = 0.0, heError = 0.0;
    double vsini = 0.0, vsiniError = 0.0;
    double zeta = 0.0, zetaError = 0.0;   // macroturbulence
    double xi = 0.0, xiError = 0.0;       // microturbulence
    double z = 0.0, zError = 0.0;         // metallicity

    // Per-spectrum radial velocities: spec index → (value, error)
    QMap<int, QPair<double, double>> vradPerSpectrum;

    // Tied vrad (if not per-spectrum)
    bool vradTied = false;
    double tiedVrad = 0.0, tiedVradError = 0.0;

    // Plotdata file paths: basename (lowercase) → full path
    QMap<QString, QString> plotdataFiles;

    // Per-spectrum matching results
    struct SpecMatch {
        int specIndex = 0;
        QString diggaFilename;
        QString plotdataFile;
        std::shared_ptr<Star> matchedStar;
        std::shared_ptr<Spectrum> matchedSpectrum;
        double vrad = 0.0, vradError = 0.0;
        bool matched = false;
    };
    std::vector<SpecMatch> specMatches;

    int totalSpectra = 0;
    int matchedSpectra = 0;
    bool parseOk = true;
    QString parseError;
};

// ═════════════════════════════════════════════════════════════════════
class SpectralFitImportPage : public QWizardPage
{
    Q_OBJECT

public:
    explicit SpectralFitImportPage(QWidget* parent = nullptr);

    void initializePage() override;
    bool validatePage() override;
    int nextId() const override;

    // Plotdata loading
    static bool loadPlotdata(const QString& filepath,
                             std::vector<double>& wavelengths,
                             std::vector<double>& modelFluxes,
                             std::vector<double>& rebinnedFluxes,
                             std::vector<double>& rebinnedSigmas,
                             std::vector<double>& modelSplines,
                             std::vector<uint8_t>& modelIgnore);

private slots:
    void onImportModeChanged();
    void onBrowseDiggaFolder();
    void onScanDigga();

private:
    void setupUi();
    void setupDiggaPage();
    void setupIsisPage();
    void setupMappingPage();

    // Index building (heavy — call from background thread)
    struct SpectrumIndex {
        QHash<QString, QPair<std::shared_ptr<Star>, std::shared_ptr<Spectrum>>> filenameIndex;
        QHash<QString, std::shared_ptr<Star>> sourceIdIndex;
        QHash<QString, std::vector<std::shared_ptr<Spectrum>>> starSpectraIndex;
        int totalSpectra = 0;
    };

    SpectrumIndex buildSpectrumLookupIndex();

    // DIGGA parsing (pure — safe for background thread)
    static DiggaFitDirectory parseDiggaDirectory(const DiggaScanResult& scan);
    static QMap<int, QString> parseDiggaFitReport(const QString& content);
    static void parseDiggaFitParameters(const QString& content, DiggaFitDirectory& dir);

    // DIGGA matching (uses index — safe for background thread)
    static void matchDiggaDirectories(std::vector<DiggaFitDirectory>& dirs,
                                      const SpectrumIndex& index);



    // Preview — only builds limited summary, no widget ops
    void updateDiggaPreviewTable();

    // Import
    void importDiggaFits();

    // Check spectra import task
    bool isSpectraImportRunning() const;

    // ── UI: Mode selection ───────────────────────────────────────
    QRadioButton* _diggaRadio;
    QRadioButton* _isisRadio;
    QRadioButton* _mappingRadio;
    QStackedWidget* _modeStack;

    // ── UI: DIGGA mode ──────────────────────────────────────────
    QWidget* _diggaPage;
    QLineEdit* _diggaFolderEdit;
    QPushButton* _diggaScanButton;
    QProgressBar* _diggaProgress;

    // ── UI: ISIS mode (stub) ────────────────────────────────────
    QWidget* _isisPage;

    // ── UI: Raw mapping mode (stub) ─────────────────────────────
    QWidget* _mappingPage;

    // ── UI: Shared ──────────────────────────────────────────────
    QCheckBox* _markBestFitCheck;
    QTreeWidget* _previewTree;
    QLabel* _statusLabel;

    // ── Data ────────────────────────────────────────────────────
    std::vector<std::shared_ptr<Star>> _importedStars;
    std::vector<DiggaFitDirectory> _diggaDirs;
    QString _diggaRootFolder;   // for relative-path display
    bool _asyncBusy = false;

    // ── Spectrum lookup index ───────────────────────────────────
    SpectrumIndex _specIndex;
    bool _indexBuilt = false;
};

#endif // SPECTRALFITIMPORTPAGE_H