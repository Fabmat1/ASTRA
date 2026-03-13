#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <QMutex>
#include <QThread>
#include <memory>
#include <vector>

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

    // Database operations
    bool openDatabase(const QString& path = "");
    void closeDatabase();
    bool isOpen() const;

    // Filesystem operations
    QString getDataDirectory() const;

    // Project operations
    std::vector<std::shared_ptr<Project>> loadProjects();
    bool saveProject(std::shared_ptr<Project> project);
    bool updateProject(std::shared_ptr<Project> project);
    bool deleteProject(const QString& projectId);
    void populateProjectStars(const QString& projectId);

    // Star operations
    bool saveStar(const QString& projectId, std::shared_ptr<Star> star);
    bool saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars);
    std::vector<std::shared_ptr<Star>> loadStars(const QString& projectId);
    bool updateStar(const QString& projectId, std::shared_ptr<Star> star);
    bool deleteStar(const QString& projectId, const QString& starId);
    size_t getStarCountForProject(const QString& projectId);

    // Import operations
    bool importCSV(const QString& filepath, std::shared_ptr<Project> project);

    std::shared_ptr<Photometry> loadPhotometry(const QString& starId);
    std::vector<std::shared_ptr<Spectrum>> loadSpectra(const QString& starId);

    // Spectrum operations
    bool saveSpectrum(const QString& starId, std::shared_ptr<Spectrum> spectrum);
    bool saveSpectralFit(const QString& starId, const QString& spectrumId,
        std::shared_ptr<SpectralFit> fit);
    std::vector<std::shared_ptr<SpectralFit>> loadSpectralFits(const QString& spectrumId);

    // ── Radial Velocity persistence ────────────────────────────────
    bool saveRadialVelocityCurve(std::shared_ptr<RadialVelocityCurve> curve,
                                const QString& starId);
    bool saveRadialVelocityPoint(std::shared_ptr<RadialVelocityPoint> point,
                                const QString& curveId);
    bool saveRVFit(std::shared_ptr<RVFit> fit, const QString& curveId);

    std::shared_ptr<RadialVelocityCurve> loadRadialVelocityCurve(
        const QString& starId);
    std::vector<std::shared_ptr<RadialVelocityPoint>> loadRadialVelocityPoints(
        const QString& curveId);
    std::shared_ptr<RVFit> loadRVFit(const QString& curveId);
    std::vector<std::shared_ptr<RVFit>> loadRVFits(const QString& curveId);

    bool deleteRadialVelocityCurve(const QString& curveId);

    // Transaction control for staged commits
    bool beginTransaction();
    bool commitTransaction();
    bool rollbackTransaction();

    // Update star row only (no cascade to spectra/photometry)
    bool updateStarRow(const QString& projectId, std::shared_ptr<Star> star);


private:
    bool createTables();
    bool createIndexes();
    bool executeQuery(const QString& query);
    QString generateUUID();

    // Star operations

    void loadPhotometryBatch(std::vector<std::shared_ptr<Star>>& stars);
    void loadSpectraBatch(std::vector<std::shared_ptr<Star>>& stars);

    // Photometry operations
    bool savePhotometry(const QString& starId, std::shared_ptr<Photometry> photometry);
    bool saveSEDModel(const QString& starId, const QString& photometryId,
                      std::shared_ptr<SEDModel> model);
    bool saveLightcurveModel(const QString& starId, const QString& photometryId,
                             const QString& lightcurveId,
                             std::shared_ptr<LightcurveModel> model);
    std::vector<std::shared_ptr<SEDModel>> loadSEDModels(const QString& photometryId);
    std::vector<std::shared_ptr<LightcurveModel>> loadLightcurveModels(const QString& lightcurveId);
    // In private section of DatabaseManager class:
    bool runMigrations();
    
    QSqlDatabase _database;
    QString _databasePath;

    QSqlDatabase threadConnection();
    QMutex _connectionMutex;

};

#endif // DATABASEMANAGER_H