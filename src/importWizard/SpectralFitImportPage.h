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
struct GaelScanResult {
    QString dirPath;
    QString gridName;
    QString parentDirName;

    QByteArray fitReportContent;       
    QByteArray fitParametersContent;   

    QMap<QString, QString> plotdataFiles;

    bool valid = true;
    QString error;
};

// ── Persistent: parsed GAEL directory with matching results ─────────
struct GaelFitDirectory {
    QString dirPath;
    QString gridName;
    QString parentDirName;

    // From fit_report.tex - spec index → spectrum filename
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
        QString gaelFilename;
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

// ── Transient: raw ISIS filesystem scan result ──────────────────────
struct IsisScanResult {
    QString dirPath;
    QString gridDirName; // directory name (fallback grid label)
    QString parentDirName;

    QByteArray propertiesContent; // spectrum_properties.txt
    QByteArray resultsTexContent; // spectroscopy_results.tex

    // spec index (1-based) → path of <id>_id_<N>.dat
    QMap<int, QString> modelDataFiles;

    bool    valid = true;
    QString error;
};

// ── Persistent: parsed ISIS directory with matching results ─────────
struct IsisFitDirectory {
    QString dirPath;
    QString gridDirName;
    QString parentDirName;
    QString grid; // grid name parsed from .tex (preferred modelId)
    double  chi2 = 0.0;

    struct SpecMatch {
        int     specIndex = 0; // 1-based
        QString isisFilename;  // from spectrum_properties.txt
        QString modelDataFile; // path to <id>_id_<N>.dat ("" if absent)

        // Per-spectrum parameters (tied values fall through from .tex)
        double teff = 0, teffError = 0;
        double logg = 0, loggError = 0;
        double he = 0, heError = 0;
        double vsini = 0, vsiniError = 0;
        double zeta = 0, zetaError = 0; // macroturbulence
        double xi = 0, xiError = 0;     // microturbulence
        double z = 0, zError = 0;       // metallicity
        double vrad = 0, vradError = 0;

        std::shared_ptr<Star>     matchedStar;
        std::shared_ptr<Spectrum> matchedSpectrum;
        bool                      matched = false;
    };
    std::vector<SpecMatch> specMatches;

    int     totalSpectra   = 0;
    int     matchedSpectra = 0;
    bool    parseOk        = true;
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
    // ISIS model-data loader (used by the import task)
    static bool loadIsisModelData(const QString        &filepath,
                                  std::vector<double>  &wavelengths,
                                  std::vector<double>  &modelFluxes,
                                  std::vector<double>  &rebinnedFluxes,
                                  std::vector<double>  &rebinnedSigmas,
                                  std::vector<double>  &modelSplines,
                                  std::vector<uint8_t> &modelIgnore);

  private slots:
    void onImportModeChanged();
    void onBrowseGaelFolder();
    void onScanGael();
    void onBrowseIsisFolder();
    void onScanIsis();

private:
    void setupUi();
    void setupGaelPage();
    void setupIsisPage();
    void setupMappingPage();

    // Index building (heavy - call from background thread)
    struct SpectrumIndex {
        QHash<QString, QPair<std::shared_ptr<Star>, std::shared_ptr<Spectrum>>> filenameIndex;
        QHash<QString, std::shared_ptr<Star>> sourceIdIndex;
        QHash<QString, std::vector<std::shared_ptr<Spectrum>>> starSpectraIndex;
        int totalSpectra = 0;
    };

    SpectrumIndex buildSpectrumLookupIndex();

    // GAEL parsing (pure - safe for background thread)
    static GaelFitDirectory parseGaelDirectory(const GaelScanResult& scan);
    static QMap<int, QString> parseGaelFitReport(const QByteArray& content);
    static void parseGaelFitParameters(const QByteArray& content, GaelFitDirectory& dir);

    // GAEL matching (uses index - safe for background thread)
    static void matchGaelDirectories(std::vector<GaelFitDirectory>& dirs,
                                      const SpectrumIndex& index);



    // Preview - only builds limited summary, no widget ops
    void updateGaelPreviewTable();

    // Import
    void importGaelFits();

    // Check spectra import task
    bool isSpectraImportRunning() const;

    // ISIS parsing (pure - background-safe)
    static IsisFitDirectory parseIsisDirectory(const IsisScanResult &scan);
    static void matchIsisDirectories(std::vector<IsisFitDirectory> &dirs,
                                     const SpectrumIndex           &index);
    void        updateIsisPreviewTable();
    void        importIsisFits();

    // ── UI: Mode selection ───────────────────────────────────────
    QRadioButton* _gaelRadio;
    QRadioButton* _isisRadio;
    QRadioButton* _mappingRadio;
    QStackedWidget* _modeStack;

    // ── UI: GAEL mode ──────────────────────────────────────────
    QWidget* _gaelPage;
    QLineEdit* _gaelFolderEdit;
    QPushButton* _gaelScanButton;
    QProgressBar* _gaelProgress;

    // ── UI: ISIS mode ───────────────────────────────────────────
    QWidget                      *_isisPage;
    QLineEdit                    *_isisFolderEdit = nullptr;
    QPushButton                  *_isisScanButton = nullptr;
    QProgressBar                 *_isisProgress   = nullptr;
    QString                       _isisRootFolder;
    std::vector<IsisFitDirectory> _isisDirs;

    // ── UI: Raw mapping mode (stub) ─────────────────────────────
    QWidget* _mappingPage;

    // ── UI: Shared ──────────────────────────────────────────────
    QCheckBox* _markBestFitCheck;
    QTreeWidget* _previewTree;
    QLabel* _statusLabel;

    // ── Data ────────────────────────────────────────────────────
    std::vector<std::shared_ptr<Star>> _importedStars;
    std::vector<GaelFitDirectory> _gaelDirs;
    QString _gaelRootFolder;   // for relative-path display
    QProgressBar *_indexBar  = nullptr; 
    bool          _asyncBusy = false;

    // ── Spectrum lookup index ───────────────────────────────────
    SpectrumIndex _specIndex;
    bool _indexBuilt = false;
};

#endif // SPECTRALFITIMPORTPAGE_H