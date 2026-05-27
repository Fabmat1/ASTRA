#ifndef PHOTOMETRYREPOSITORY_H
#define PHOTOMETRYREPOSITORY_H

#include <memory>
#include <vector>
#include <QString>

class DBAccess;
class Star;
class Photometry;
class SEDModel;
class LCFit;

class PhotometryRepository {
public:
    explicit PhotometryRepository(DBAccess& db);

    std::shared_ptr<Photometry> loadPhotometry(const QString& starId);
    bool saveSEDModelForStar(const QString& starId, std::shared_ptr<SEDModel> model);
    bool deleteSEDModel(const QString& modelId);
    bool saveLightcurveForStar(const QString& starId, const QString& source, Photometry* photometry);
    bool removeLightcurve(const QString& starId, const QString& source);
    void loadPhotometryBatch(std::vector<std::shared_ptr<Star>>& stars);
    bool savePhotometry(const QString& starId, std::shared_ptr<Photometry> photometry);
    bool saveSEDModel(const QString& starId, const QString& photometryId, std::shared_ptr<SEDModel> model);

    bool saveLCFit(const QString& starId, const QString& photometryId,
                   const QString& lightcurveId, std::shared_ptr<LCFit> fit);
    bool saveLCFitForStar(const QString& starId, const QString& source,
                          std::shared_ptr<LCFit> fit);
    std::vector<std::shared_ptr<LCFit>> loadLCFitsForLightcurve(const QString& lightcurveId);
    bool deleteLCFit(const QString& fitId);
    bool setBestLCFit(const QString& starId, const QString& source, const QString& fitId);

    std::vector<std::shared_ptr<SEDModel>> loadSEDModels(const QString& photometryId);

private:
    QString resolveLightcurveId(const QString& starId, const QString& source,
                                QString* photometryIdOut = nullptr);

    DBAccess& _db;
};

#endif