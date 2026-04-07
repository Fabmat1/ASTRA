#include "RadialVelocityRepository.h"
#include "DBAccess.h"
#include "models/Star.h"
#include "models/RadialVelocity.h"
#include <QSqlQuery>
#include <QSqlError>

RadialVelocityRepository::RadialVelocityRepository(DBAccess& db) : _db(db) {}

bool RadialVelocityRepository::saveRadialVelocityCurve(
    std::shared_ptr<RadialVelocityCurve> curve, const QString& starId)
{
    if (!curve) return false;

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO rv_curves
        (id, star_id, num_points, mean_rv, std_rv, min_rv, max_rv,
         time_baseline, log_p)
        VALUES (:id, :star_id, :num_points, :mean_rv, :std_rv,
                :min_rv, :max_rv, :time_baseline, :log_p)
    )");

    query.bindValue(":id", curve->getId());
    query.bindValue(":star_id", starId);
    query.bindValue(":num_points",
                    static_cast<int>(curve->getNumPoints()));
    query.bindValue(":mean_rv", curve->getMeanRV());
    query.bindValue(":std_rv", curve->getStdDevRV());
    query.bindValue(":min_rv", curve->getMinRV());
    query.bindValue(":max_rv", curve->getMaxRV());
    query.bindValue(":time_baseline", curve->getTimeSpan());
    query.bindValue(":log_p", curve->getLogP());

    if (!query.exec()) {
        qDebug() << "Failed to save RV curve:" << query.lastError();
        return false;
    }
    return true;
}

bool RadialVelocityRepository::saveRadialVelocityPoint(
    std::shared_ptr<RadialVelocityPoint> point, const QString& curveId)
{
    if (!point) return false;

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO rv_points
        (id, curve_id, mjd, bjd, radial_velocity,
         rv_error, source, spectrum_id, spectral_fit_id)
        VALUES (:id, :curve_id, :mjd, :bjd, :rv,
                :rv_error, :source, :spectrum_id, :fit_id)
    )");

    query.bindValue(":id", point->getId());
    query.bindValue(":curve_id", curveId);
    query.bindValue(":mjd", point->getMJD());
    query.bindValue(":bjd", point->getBJD());
    query.bindValue(":rv", point->getRV());
    query.bindValue(":rv_error", point->getRVError());
    query.bindValue(":source", point->getSource());
    query.bindValue(":spectrum_id", point->getSpectrumId());
    query.bindValue(":fit_id", point->getSpectralFitId());

    if (!query.exec()) {
        qDebug() << "Failed to save RV point:" << query.lastError();
        return false;
    }
    return true;
}

bool RadialVelocityRepository::saveRVFit(
    std::shared_ptr<RVFit> fit, const QString& curveId)
{
    if (!fit) return false;

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO rv_fits
        (id, curve_id, k, k_error, gamma, gamma_error,
         period, period_error, phi, phi_error, t0, t0_error,
         eccentricity, eccentricity_error, omega, omega_error,
         is_best_fit, fit_method, chi2, rms)
        VALUES (:id, :curve_id, :k, :k_error, :gamma, :gamma_error,
                :period, :period_error, :phi, :phi_error, :t0, :t0_error,
                :ecc, :ecc_error, :omega, :omega_error,
                :is_best, :method, :chi2, :rms)
    )");

    query.bindValue(":id", fit->getId());
    query.bindValue(":curve_id", curveId);
    query.bindValue(":k", fit->getK());
    query.bindValue(":k_error", fit->getKError());
    query.bindValue(":gamma", fit->getGamma());
    query.bindValue(":gamma_error", fit->getGammaError());
    query.bindValue(":period", fit->getPeriod());
    query.bindValue(":period_error", fit->getPeriodError());
    query.bindValue(":phi", fit->getPhi());
    query.bindValue(":phi_error", fit->getPhiError());
    query.bindValue(":t0", fit->getT0());
    query.bindValue(":t0_error", fit->getT0Error());
    query.bindValue(":ecc", fit->getEccentricity());
    query.bindValue(":ecc_error", fit->getEccentricityError());
    query.bindValue(":omega", fit->getOmega());
    query.bindValue(":omega_error", fit->getOmegaError());
    query.bindValue(":is_best", fit->isBestFit() ? 1 : 0);
    query.bindValue(":method", fit->getFitMethod());
    query.bindValue(":chi2", fit->getChi2());
    query.bindValue(":rms", fit->getRms());

    if (!query.exec()) {
        qDebug() << "Failed to save RV fit:" << query.lastError();
        return false;
    }
    return true;
}

std::shared_ptr<RadialVelocityCurve> RadialVelocityRepository::loadRadialVelocityCurve(
    const QString& starId)
{
    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        SELECT * FROM rv_curves WHERE star_id = :star_id
        ORDER BY created_at DESC LIMIT 1
    )");
    query.bindValue(":star_id", starId);

    if (!query.exec() || !query.next()) return nullptr;

    auto curve = std::make_shared<RadialVelocityCurve>();
    curve->setId(query.value("id").toString());
    curve->setStarId(starId);
    curve->setLogP(query.value("log_p").toDouble());

    // Load points
    auto points = loadRadialVelocityPoints(curve->getId());
    for (const auto& pt : points)
        curve->addRVPoint(pt);

    // Load fits
    auto fits = loadRVFits(curve->getId());
    for (const auto& fit : fits)
        curve->addRVFit(fit);

    return curve;
}

std::vector<std::shared_ptr<RadialVelocityPoint>>
RadialVelocityRepository::loadRadialVelocityPoints(const QString& curveId)
{
    std::vector<std::shared_ptr<RadialVelocityPoint>> result;

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        SELECT * FROM rv_points WHERE curve_id = :curve_id
        ORDER BY mjd ASC, bjd ASC
    )");
    query.bindValue(":curve_id", curveId);

    if (!query.exec()) return result;

    while (query.next()) {
        auto pt = std::make_shared<RadialVelocityPoint>();
        pt->setId(query.value("id").toString());
        pt->setCurveId(curveId);
        pt->setTime(Time::fromMjdBjd(
            query.value("mjd").toDouble(),
            query.value("bjd").toDouble()));
        pt->setRV(query.value("radial_velocity").toDouble());
        pt->setRVError(query.value("rv_error").toDouble());
        pt->setSource(query.value("source").toString());
        pt->setSpectrumId(query.value("spectrum_id").toString());
        pt->setSpectralFitId(query.value("spectral_fit_id").toString());
        result.push_back(pt);
    }
    return result;
}

std::vector<std::shared_ptr<RVFit>> RadialVelocityRepository::loadRVFits(
    const QString& curveId)
{
    std::vector<std::shared_ptr<RVFit>> result;

    QSqlQuery query(_db.threadConnection());
    query.prepare("SELECT * FROM rv_fits WHERE curve_id = :curve_id");
    query.bindValue(":curve_id", curveId);

    if (!query.exec()) return result;

    while (query.next()) {
        auto fit = std::make_shared<RVFit>();
        fit->setId(query.value("id").toString());
        fit->setCurveId(curveId);
        fit->setK(query.value("k").toDouble());
        fit->setKError(query.value("k_error").toDouble());
        fit->setGamma(query.value("gamma").toDouble());
        fit->setGammaError(query.value("gamma_error").toDouble());
        fit->setPeriod(query.value("period").toDouble());
        fit->setPeriodError(query.value("period_error").toDouble());
        fit->setPhi(query.value("phi").toDouble());
        fit->setPhiError(query.value("phi_error").toDouble());
        fit->setT0(query.value("t0").toDouble());
        fit->setT0Error(query.value("t0_error").toDouble());
        fit->setEccentricity(query.value("eccentricity").toDouble());
        fit->setEccentricityError(
            query.value("eccentricity_error").toDouble());
        fit->setOmega(query.value("omega").toDouble());
        fit->setOmegaError(query.value("omega_error").toDouble());
        fit->setBestFit(query.value("is_best_fit").toInt() != 0);
        fit->setFitMethod(query.value("fit_method").toString());
        fit->setChi2(query.value("chi2").toDouble());
        fit->setRms(query.value("rms").toDouble());
        result.push_back(fit);
    }
    return result;
}

std::shared_ptr<RVFit> RadialVelocityRepository::loadRVFit(const QString& curveId)
{
    auto fits = loadRVFits(curveId);
    for (const auto& f : fits)
        if (f->isBestFit()) return f;
    return fits.empty() ? nullptr : fits.front();
}

bool RadialVelocityRepository::deleteRadialVelocityCurve(const QString& curveId)
{
    QSqlQuery query(_db.threadConnection());

    query.prepare("DELETE FROM rv_points WHERE curve_id = :curve_id");
    query.bindValue(":curve_id", curveId);
    query.exec();

    query.prepare("DELETE FROM rv_fits WHERE curve_id = :curve_id");
    query.bindValue(":curve_id", curveId);
    query.exec();

    query.prepare("DELETE FROM rv_curves WHERE id = :id");
    query.bindValue(":id", curveId);
    return query.exec();
}

// New method in DatabaseManager:
void RadialVelocityRepository::loadRVCurveBatch(std::vector<std::shared_ptr<Star>>& stars)
{
    for (auto& star : stars) {
        auto curve = loadRadialVelocityCurve(star->getId());
        if (curve) {
            star->setRVCurve(curve);
        }
    }
}
