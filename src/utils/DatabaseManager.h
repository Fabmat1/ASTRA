#ifndef DATABASEMANAGER_H
#define DATABASEMANAGER_H

#include <QObject>
#include <QSqlDatabase>
#include <memory>
#include <vector>

class Project;
class Star;
class Photometry;
class Spectrum;
class SpectralFit;
class SEDModel;
class LightcurveModel;

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

    // Project operations
    std::vector<std::shared_ptr<Project>> loadProjects();
    bool saveProject(std::shared_ptr<Project> project);
    bool updateProject(std::shared_ptr<Project> project);
    bool deleteProject(const QString& projectId);

    // Star operations
    bool saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars);
    std::vector<std::shared_ptr<Star>> loadStars(const QString& projectId);
    bool updateStar(const QString& projectId, std::shared_ptr<Star> star);
    bool deleteStar(const QString& projectId, const QString& sourceId);

    // Import operations
    bool importCSV(const QString& filepath, std::shared_ptr<Project> project);

private:
    bool createTables();
    bool executeQuery(const QString& query);
    QString generateUUID();
    QString getDataDirectory() const;

    // Star operations
    bool saveStar(const QString& projectId, std::shared_ptr<Star> star);

    // Photometry operations
    bool savePhotometry(const QString& starId, std::shared_ptr<Photometry> photometry);
    std::shared_ptr<Photometry> loadPhotometry(const QString& starId);
    bool saveSEDModel(const QString& photometryId, std::shared_ptr<SEDModel> model, const QString& photometryDir);
    bool saveLightcurveModel(const QString& lightcurveId, std::shared_ptr<LightcurveModel> model, const QString& photometryDir);
    std::vector<std::shared_ptr<SEDModel>> loadSEDModels(const QString& photometryId);
    std::vector<std::shared_ptr<LightcurveModel>> loadLightcurveModels(const QString& lightcurveId);

    // Spectrum operations
    bool saveSpectrum(const QString& starId, std::shared_ptr<Spectrum> spectrum);
    std::vector<std::shared_ptr<Spectrum>> loadSpectra(const QString& starId);
    bool saveSpectralFit(const QString& spectrumId, std::shared_ptr<SpectralFit> fit, const QString& spectrumDir);
    std::vector<std::shared_ptr<SpectralFit>> loadSpectralFits(const QString& spectrumId);

    QSqlDatabase _database;
    QString _databasePath;
};

#endif // DATABASEMANAGER_H