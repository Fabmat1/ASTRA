#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <memory>
#include <vector>
#include <QSet>
#include <QString>
#include "models/Instrument.h"
#include "models/InstrumentMode.h"
#include <QHash>

class DBAccess;
class ProjectRepository;
class StarRepository;
class PhotometryRepository;
class SpectrumRepository;
class RadialVelocityRepository;
class InstrumentRepository;
class PeriodogramRepository;

class Project;
class Star;
class Photometry;
class Spectrum;
class SpectrumIndexRow;
class SpectralFit;
class SEDModel;
class LightcurveModel;
class RadialVelocityCurve;
class RadialVelocityPoint;
class RVFit;
class PeriodogramRecord;
class LCFit;

class DatabaseManager : public QObject
{
    Q_OBJECT

public:
    explicit DatabaseManager(QObject *parent = nullptr);
    ~DatabaseManager();

    bool openDatabase(const QString& path = "");
    void closeDatabase();
    bool isOpen() const;

    // Empty when the last integrity check passed; otherwise the SQLite
    // complaint (e.g. "database disk image is malformed").
    QString integrityError() const { return _integrityError; }
    bool    isHealthy() const { return _integrityError.isEmpty(); }
    void    checkIntegrity();
    QString getDataDirectory() const;
    std::vector<std::shared_ptr<Project>> loadProjects();
    bool saveProject(std::shared_ptr<Project> project);
    bool updateProject(std::shared_ptr<Project> project);
    bool deleteProject(const QString& projectId);
    bool saveStar(const QString& projectId, std::shared_ptr<Star> star);
    bool saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars);
    bool moveStarsToProject(const std::vector<QString>& starIds, const QString& targetProjectId);
    std::vector<std::shared_ptr<Star>> loadStars(const QString& projectId);
    bool updateStar(const QString& projectId, std::shared_ptr<Star> star);
    bool deleteStar(const QString& projectId, const QString& starId);
    size_t getStarCountForProject(const QString& projectId);
    bool importCSV(const QString& filepath, std::shared_ptr<Project> project);
    std::shared_ptr<Photometry> loadPhotometry(const QString& starId);
    bool savePhotometry(const QString              &starId, std::shared_ptr<Photometry> photometry);
    std::vector<std::shared_ptr<Spectrum>> loadSpectra(const QString& starId);
    std::vector<SpectrumIndexRow> loadSpectraIndex(const QString &projectId);
    bool saveSpectrum(const QString &starId, std::shared_ptr<Spectrum> spectrum,
                      bool cascadeFits = true);
    bool deleteSpectrum(const QString& spectrumId);
    bool deleteSpectralFit(const QString& fitId);
    bool saveSpectralFit(const QString& starId, const QString& spectrumId, std::shared_ptr<SpectralFit> fit);
    std::vector<std::shared_ptr<SpectralFit>> loadSpectralFits(const QString& spectrumId);
    bool updateSpectrumFlag(const QString& spectrumId, bool flagged);
    bool updateSpectrumInstrument(const QString& spectrumId,
                                  const QString& instrument,
                                  const QString& instrumentId,
                                  const QString& modeKey);
    bool updateSpectralFitFlag(const QString& fitId, bool flagged);
    bool updateBestFit(const QString& spectrumId, const QString& bestFitId);
    QSet<QString> spectrumOriginIdsForProject(const QString& projectId);
    bool deleteSpectraByOriginId(const QString& starId, const QString& originId);
    /// Spectrum counts for one star, straight from the database. See
    /// SpectrumRepository::spectraCounts.
    bool spectraCounts(const QString& starId, int* nSpectra,
                       int* nFitSpectra) const;
    bool saveRadialVelocityCurve(std::shared_ptr<RadialVelocityCurve> curve, const QString& starId);
    bool saveRadialVelocityPoint(std::shared_ptr<RadialVelocityPoint> point, const QString& curveId);
    bool saveRVFit(std::shared_ptr<RVFit> fit, const QString& curveId);
    std::shared_ptr<RadialVelocityCurve> loadRadialVelocityCurve( const QString& starId);
    std::vector<std::shared_ptr<RadialVelocityPoint>> loadRadialVelocityPoints( const QString& curveId);
    std::shared_ptr<RVFit> loadRVFit(const QString& curveId);
    std::vector<std::shared_ptr<RVFit>> loadRVFits(const QString& curveId);
    bool deleteRadialVelocityCurve(const QString& curveId);
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();
    bool updateStarRow(const QString& projectId, std::shared_ptr<Star> star);
    /// Writes only n_spectra / n_fit_spectra for one star. See
    /// StarRepository::updateSpectraCounts.
    bool updateSpectraCounts(const QString& starId, int nSpectra,
                             int nFitSpectra);
    QString findMatchingStarId(const QString& projectId, const QString& sourceId, const QString& alias, const QString& tic, const QString& jname, double ra, double dec, double toleranceArcsec = 2.0);
    bool saveSEDModelForStar(const QString& starId, std::shared_ptr<SEDModel> model);
    bool saveSedPhotometryPointsForStar(const QString& starId,
                                        std::shared_ptr<Photometry> photometry);
    bool deleteSEDModel(const QString& modelId);
    bool saveLightcurveForStar(const QString& starId, const QString& source, Photometry* photometry);
    bool removeLightcurve(const QString& starId, const QString& source);
    void initializeInstruments();
    std::shared_ptr<Instrument> getInstrumentById(const QString& id) const;
    std::shared_ptr<Instrument> getInstrumentByName(const QString& name) const;
    std::vector<std::shared_ptr<Instrument>> getAllInstruments() const;
    bool saveInstrument(std::shared_ptr<Instrument> instrument);
    bool updateInstrument(std::shared_ptr<Instrument> instrument);
    bool deleteInstrument(const QString& id);
    std::shared_ptr<Instrument> resolveInstrumentString( const QString& input, QString* modeKey = nullptr) const;
    void restoreDefaultInstruments();
    bool deleteRVFit(const QString& fitId);
    bool deleteRadialVelocityPoint(const QString& pointId);
    bool saveStarPeriodograms(const QString& starId, const std::vector<std::shared_ptr<PeriodogramRecord>>& records);
    std::vector<std::shared_ptr<PeriodogramRecord>> loadStarPeriodograms(const QString& starId);
    std::shared_ptr<PeriodogramRecord> loadPeriodogram(const QString& starId, const QString& source, const QString& filter = {});
    bool deleteStarPeriodograms(const QString& starId);
    bool saveCurveRVPeriodograms(const QString& starId, const QString& curveId,
        const std::vector<std::shared_ptr<PeriodogramRecord>>& records);
    std::vector<std::shared_ptr<PeriodogramRecord>> loadCurveRVPeriodograms(const QString& curveId);
    bool deleteCurveRVPeriodograms(const QString& curveId);
    bool    saveStarPhotPeaks(const QString& starId, const QString& peaksJson);
    QString loadStarPhotPeaks(const QString& starId);
    bool   saveStarTessCrowdsap(const QString& starId, double value);
    double loadStarTessCrowdsap(const QString& starId);
    bool    saveLCFitForStar(const QString &starId, const QString &source,
                             std::shared_ptr<LCFit> fit);

    std::vector<std::shared_ptr<LCFit>>
    loadLCFitsForSource(const QString &starId, const QString &source);

    std::vector<std::shared_ptr<LCFit>>
    loadLCFitsForSource(const QString &starId, const QString &source,
                        const QString &filter);

    bool deleteLCFit(const QString &fitId);

    bool setBestLCFit(const QString &starId, const QString &source,
                      const QString &filter, const QString &fitId);

  private:
    //void backfillSpectrumInstrumentIds();
    bool createTables();
    bool createIndexes();
    bool runMigrations();

    QString _integrityError;

    std::unique_ptr<DBAccess> _db;
    std::unique_ptr<ProjectRepository> _projects;
    std::unique_ptr<StarRepository> _stars;
    std::unique_ptr<PhotometryRepository> _photometry;
    std::unique_ptr<SpectrumRepository> _spectra;
    std::unique_ptr<RadialVelocityRepository> _rv;
    std::unique_ptr<InstrumentRepository> _instruments;
    std::unique_ptr<PeriodogramRepository> _periodograms;
};

#endif // DATABASEMANAGER_H
