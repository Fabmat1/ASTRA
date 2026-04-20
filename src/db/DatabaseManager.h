#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <memory>
#include <vector>
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

class Project;
class Star;
class Photometry;
class Spectrum;
class SpectralFit;
class SEDModel;
class LightcurveModel;
class RadialVelocityCurve;
class RadialVelocityPoint;
class RVFit;

class DatabaseManager : public QObject
{
    Q_OBJECT

public:
    explicit DatabaseManager(QObject *parent = nullptr);
    ~DatabaseManager();

    bool openDatabase(const QString& path = "");
    void closeDatabase();
    bool isOpen() const;
    QString getDataDirectory() const;
    std::vector<std::shared_ptr<Project>> loadProjects();
    bool saveProject(std::shared_ptr<Project> project);
    bool updateProject(std::shared_ptr<Project> project);
    bool deleteProject(const QString& projectId);
    bool saveStar(const QString& projectId, std::shared_ptr<Star> star);
    bool saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars);
    std::vector<std::shared_ptr<Star>> loadStars(const QString& projectId);
    bool updateStar(const QString& projectId, std::shared_ptr<Star> star);
    bool deleteStar(const QString& projectId, const QString& starId);
    size_t getStarCountForProject(const QString& projectId);
    bool importCSV(const QString& filepath, std::shared_ptr<Project> project);
    std::shared_ptr<Photometry> loadPhotometry(const QString& starId);
    std::vector<std::shared_ptr<Spectrum>> loadSpectra(const QString& starId);
    bool saveSpectrum(const QString& starId, std::shared_ptr<Spectrum> spectrum);
    bool saveSpectralFit(const QString& starId, const QString& spectrumId, std::shared_ptr<SpectralFit> fit);
    std::vector<std::shared_ptr<SpectralFit>> loadSpectralFits(const QString& spectrumId);
    bool updateSpectrumFlag(const QString& spectrumId, bool flagged);
    bool updateSpectralFitFlag(const QString& fitId, bool flagged);
    bool updateBestFit(const QString& spectrumId, const QString& bestFitId);
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
    QString findMatchingStarId(const QString& projectId, const QString& sourceId, const QString& alias, const QString& tic, const QString& jname, double ra, double dec);
    bool saveSEDModelForStar(const QString& starId, std::shared_ptr<SEDModel> model);
    bool deleteSEDModel(const QString& modelId);
    bool saveLightcurveForStar(const QString& starId, const QString& source, Photometry* photometry);
    void initializeInstruments();
    std::shared_ptr<Instrument> getInstrumentById(const QString& id) const;
    std::shared_ptr<Instrument> getInstrumentByName(const QString& name) const;
    std::vector<std::shared_ptr<Instrument>> getAllInstruments() const;
    bool saveInstrument(std::shared_ptr<Instrument> instrument);
    bool updateInstrument(std::shared_ptr<Instrument> instrument);
    bool deleteInstrument(const QString& id);
    std::shared_ptr<Instrument> resolveInstrumentString( const QString& input, QString* modeKey = nullptr) const;
    void restoreDefaultInstruments();

private:
    bool createTables();
    bool createIndexes();
    bool runMigrations();

    std::unique_ptr<DBAccess> _db;
    std::unique_ptr<ProjectRepository> _projects;
    std::unique_ptr<StarRepository> _stars;
    std::unique_ptr<PhotometryRepository> _photometry;
    std::unique_ptr<SpectrumRepository> _spectra;
    std::unique_ptr<RadialVelocityRepository> _rv;
    std::unique_ptr<InstrumentRepository> _instruments;
};

#endif // DATABASEMANAGER_H
