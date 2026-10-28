#include "db/StarLoaders.h"

#include "app/Logger.h"
#include "core/Instrument.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "lightcurve/Photometry.h"
#include "rv/RadialVelocity.h"
#include "spectra/Spectrum.h"

#include <QUuid>

#include <cmath>

void attachStarLoaders(DatabaseManager* dbMgr, const std::vector<std::shared_ptr<Star>>& stars,
                       const QString& projectId)
{
    // Create loaders ONCE, capture by value (shared ownership)
    auto photometryLoader = [dbMgr](const QString& starId) {
        return dbMgr->loadPhotometry(starId);
    };
    auto spectraLoader = [dbMgr](const QString& starId) {
        return dbMgr->loadSpectra(starId);
    };

    for (const auto& star : stars) {
        star->setPhotometryLoader(photometryLoader);
        star->setSpectraLoader(spectraLoader);

        std::weak_ptr<Star> wstar = star;

        star->setRVCurveFactory([dbMgr, wstar](const QString& starId)
            -> std::shared_ptr<RadialVelocityCurve>
        {
            auto curve = std::make_shared<RadialVelocityCurve>();
            curve->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));
            curve->setStarId(starId);

            curve->setPointPersistCallback(
                [dbMgr](const std::shared_ptr<RadialVelocityPoint>& p) {
                    dbMgr->saveRadialVelocityPoint(p, p->getCurveId());
                });

            curve->setPointDeleteCallback(
                [dbMgr](const QString& pointId) {
                    dbMgr->deleteRadialVelocityPoint(pointId);
                });

            curve->setBjdResolverCallback(
                [dbMgr, wstar](const std::shared_ptr<RadialVelocityPoint>& p) {
                    if (!p) return;
                    const double existing = p->getBJD();
                    if (existing > 0.0 && !std::isnan(existing)) return;
                    const double mjd = p->getMJD();
                    if (!(mjd > 0.0) || std::isnan(mjd)) return;
                    auto s = wstar.lock();
                    if (!s) return;
                    const double ra = s->getRa(), dec = s->getDec();
                    if (std::isnan(ra) || std::isnan(dec)) return;

                    std::shared_ptr<Instrument> inst = p->getInstrument();
                    if (!inst) {
                        if (auto sp = p->getSourceSpectrum().lock()) {
                            if (!sp->getInstrumentId().isEmpty())
                                inst = dbMgr->getInstrumentById(sp->getInstrumentId());
                            if (!inst && !sp->getInstrument().isEmpty())
                                inst = dbMgr->getInstrumentByName(sp->getInstrument());
                        }
                    }
                    if (!inst) return;
                    p->time().computeBJD(*inst, ra, dec);
                });

            // Persist the curve row immediately so subsequent point inserts have
            // a valid FK target.
            dbMgr->saveRadialVelocityCurve(curve, starId);
            return curve;
        });
        star->setRVLoader([dbMgr, wstar](const QString& starId) {
            auto curve = dbMgr->loadRadialVelocityCurve(starId);
            if (!curve) return curve;

            curve->setPointPersistCallback(
                [dbMgr](const std::shared_ptr<RadialVelocityPoint>& p) {
                    dbMgr->saveRadialVelocityPoint(p, p->getCurveId());
                });

            curve->setPointDeleteCallback(
                [dbMgr](const QString& pointId) {
                    dbMgr->deleteRadialVelocityPoint(pointId);
                });

            curve->setBjdResolverCallback(
                [dbMgr, wstar](const std::shared_ptr<RadialVelocityPoint>& p) {
                    if (!p) { LOG_DEBUG("BjdResolver", "null point"); return; }
                    const double existing = p->getBJD();
                    if (existing > 0.0 && !std::isnan(existing)) {
                        LOG_DEBUG("BjdResolver", QString("already has BJD=%1").arg(existing));
                        return;
                    }
                    const double mjd = p->getMJD();
                    if (!(mjd > 0.0) || std::isnan(mjd)) {
                        LOG_WARNING("BjdResolver", QString("MJD invalid (%1) - cannot compute BJD").arg(mjd));
                        return;
                    }
                    auto s = wstar.lock();
                    if (!s) { LOG_WARNING("BjdResolver", "star expired"); return; }
                    const double ra  = s->getRa();
                    const double dec = s->getDec();
                    if (std::isnan(ra) || std::isnan(dec)) {
                        LOG_WARNING("BjdResolver", QString("star %1 has no ra/dec").arg(s->getSourceId()));
                        return;
                    }

                    std::shared_ptr<Instrument> inst = p->getInstrument();
                    QString instSource = "point";
                    if (!inst) {
                        if (auto sp = p->getSourceSpectrum().lock()) {
                            LOG_DEBUG("BjdResolver", QString("spectrum: id='%1' name='%2'")
                                .arg(sp->getInstrumentId(), sp->getInstrument()));
                            if (!sp->getInstrumentId().isEmpty()) {
                                inst = dbMgr->getInstrumentById(sp->getInstrumentId());
                                instSource = "id";
                            }
                            if (!inst && !sp->getInstrument().isEmpty()) {
                                inst = dbMgr->getInstrumentByName(sp->getInstrument());
                                instSource = "name";
                            }
                        } else {
                            LOG_WARNING("BjdResolver", "source spectrum weak_ptr expired");
                        }
                    }
                    if (!inst) {
                        LOG_WARNING("BjdResolver", "could not resolve instrument");
                        return;
                    }
                    LOG_INFO("BjdResolver",
                        QString("computing BJD: mjd=%1 ra=%2 dec=%3 inst=%4 (via %5)")
                            .arg(mjd).arg(ra).arg(dec).arg(inst->getName(), instSource));
                    p->time().computeBJD(*inst, ra, dec);
                    LOG_INFO("BjdResolver", QString("→ BJD=%1").arg(p->getBJD()));
                });

            return curve;
        });

        star->setSummaryPersistCallback([dbMgr, wstar, projectId]() {
            if (auto s = wstar.lock())
                dbMgr->updateStarRow(projectId, s);
        });
    }
}
