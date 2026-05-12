#ifndef RADIALVELOCITYREPOSITORY_H
#define RADIALVELOCITYREPOSITORY_H

#include <memory>
#include <vector>
#include <QString>

class DBAccess;
class Star;
class RadialVelocityCurve;
class RadialVelocityPoint;
class RVFit;

class RadialVelocityRepository {
public:
    explicit RadialVelocityRepository(DBAccess& db);

    bool saveRadialVelocityCurve(std::shared_ptr<RadialVelocityCurve> curve, const QString& starId);
    bool saveRadialVelocityPoint(std::shared_ptr<RadialVelocityPoint> point, const QString& curveId);
    bool saveRVFit(std::shared_ptr<RVFit> fit, const QString& curveId);
    std::shared_ptr<RadialVelocityCurve> loadRadialVelocityCurve( const QString& starId);
    std::vector<std::shared_ptr<RadialVelocityPoint>> loadRadialVelocityPoints( const QString& curveId);
    std::shared_ptr<RVFit> loadRVFit(const QString& curveId);
    std::vector<std::shared_ptr<RVFit>> loadRVFits(const QString& curveId);
    bool deleteRadialVelocityCurve(const QString& curveId);
    void loadRVCurveBatch(std::vector<std::shared_ptr<Star>>& stars);
    bool deleteRVFit(const QString& fitId);
    bool deleteRadialVelocityPoint(const QString& pointId);
    
private:
    DBAccess& _db;
};

#endif // RADIALVELOCITYREPOSITORY_H
