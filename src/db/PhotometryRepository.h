#ifndef PHOTOMETRYREPOSITORY_H
#define PHOTOMETRYREPOSITORY_H

#include <memory>
#include <vector>
#include <QString>

class DBAccess;
class Star;
class Photometry;
class SEDModel;
class LightcurveModel;

class PhotometryRepository {
public:
    explicit PhotometryRepository(DBAccess& db);

    std::shared_ptr<Photometry> loadPhotometry(const QString& starId);
    bool saveSEDModelForStar(const QString& starId, std::shared_ptr<SEDModel> model);
    bool deleteSEDModel(const QString& modelId);
    bool saveLightcurveForStar(const QString& starId, const QString& source, Photometry* photometry);
    void loadPhotometryBatch(std::vector<std::shared_ptr<Star>>& stars);
    bool savePhotometry(const QString& starId, std::shared_ptr<Photometry> photometry);
    bool saveSEDModel(const QString& starId, const QString& photometryId, std::shared_ptr<SEDModel> model);
    bool saveLightcurveModel(const QString& starId, const QString& photometryId, const QString& lightcurveId, std::shared_ptr<LightcurveModel> model);
    std::vector<std::shared_ptr<SEDModel>> loadSEDModels(const QString& photometryId);
    std::vector<std::shared_ptr<LightcurveModel>> loadLightcurveModels(const QString& lightcurveId);

private:
    DBAccess& _db;
};

#endif // PHOTOMETRYREPOSITORY_H
