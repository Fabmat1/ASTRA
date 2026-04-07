#include "../importWizard/ImportStagingArea.h"

#include "../db/DatabaseManager.h"
#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/RadialVelocity.h"
#include "../utils/Logger.h"
#include "models/Photometry.h"

// ── Working set management ──────────────────────────────────────

void ImportStagingArea::addStar(std::shared_ptr<Star> star, bool isNew)
{
    QMutexLocker lock(&_mutex);
    if (!star) return;

    const QString id = star->getId();
    _workingStars[id] = star;
    if (isNew) {
        _newStarIds.insert(id);
    }
}

void ImportStagingArea::pullStarsFromDB(DatabaseManager* dbm, const QString& projectId,
                                         const QStringList& starIds)
{
    if (!dbm) return;

    // Load stars one by one, skip if already in working set
    // (You may want a batch load method on DatabaseManager — for now, individual)
    auto allDbStars = dbm->loadStars(projectId);

    QMutexLocker lock(&_mutex);
    for (const auto& star : allDbStars) {
        if (starIds.contains(star->getId()) && !_workingStars.contains(star->getId())) {
            _workingStars[star->getId()] = star;
        }
    }
}

void ImportStagingArea::pullSpectraFromDB(DatabaseManager* dbm)
{
    if (!dbm) return;

    QMutexLocker lock(&_mutex);
    for (auto it = _workingStars.begin(); it != _workingStars.end(); ++it) {
        auto& star = it.value();

        // Skip if spectra already loaded (either from a previous page or lazy load)
        if (star->hasSpectraLoaded()) continue;

        auto spectra = dbm->loadSpectra(star->getId());
        for (const auto& sp : spectra) {
            star->addSpectrum(sp);
        }
    }
}

void ImportStagingArea::pullFitsFromDB(DatabaseManager* dbm)
{
    if (!dbm) return;

    QMutexLocker lock(&_mutex);
    for (auto it = _workingStars.begin(); it != _workingStars.end(); ++it) {
        auto& star = it.value();
        for (const auto& spectrum : star->getSpectra()) {
            // Skip if fits already loaded
            if (!spectrum->getSpectralFits().empty()) continue;

            auto fits = dbm->loadSpectralFits(spectrum->getId());
            for (const auto& fit : fits) {
                spectrum->addSpectralFit(fit);
            }
        }
    }
}

void ImportStagingArea::pullRVFromDB(DatabaseManager* dbm)
{
    if (!dbm) return;

    QMutexLocker lock(&_mutex);
    for (auto it = _workingStars.begin(); it != _workingStars.end(); ++it) {
        auto& star = it.value();

        // Skip if RV already loaded
        if (star->getRVCurve()) continue;

        auto curve = dbm->loadRadialVelocityCurve(star->getId());
        if (curve) {
            auto points = dbm->loadRadialVelocityPoints(curve->getId());
            for (const auto& pt : points) {
                curve->addRVPoint(pt);
            }
            auto fits = dbm->loadRVFits(curve->getId());
            for (const auto& fit : fits) {
                curve->addRVFit(fit);
            }
            star->setRVCurve(curve);
        }
    }
}


void ImportStagingArea::pullPhotometryFromDB(DatabaseManager* dbm)
{
    if (!dbm) return;

    QMutexLocker lock(&_mutex);
    for (auto it = _workingStars.begin(); it != _workingStars.end(); ++it) {
        auto& star = it.value();

        // Skip if photometry already loaded
        if (star->getPhotometry()) continue;

        auto phot = dbm->loadPhotometry(star->getId());
        if (phot)
            star->setPhotometry(phot);
    }
}


// ── Queries ─────────────────────────────────────────────────────

std::shared_ptr<Star> ImportStagingArea::getStar(const QString& starId) const
{
    QMutexLocker lock(&_mutex);
    return _workingStars.value(starId);
}

std::vector<std::shared_ptr<Star>> ImportStagingArea::allStars() const
{
    QMutexLocker lock(&_mutex);
    std::vector<std::shared_ptr<Star>> result;
    result.reserve(_workingStars.size());
    for (auto it = _workingStars.cbegin(); it != _workingStars.cend(); ++it) {
        result.push_back(it.value());
    }
    return result;
}

bool ImportStagingArea::hasStar(const QString& starId) const
{
    QMutexLocker lock(&_mutex);
    return _workingStars.contains(starId);
}

bool ImportStagingArea::isEmpty() const
{
    QMutexLocker lock(&_mutex);
    return _workingStars.isEmpty();
}

// ── Tracking ────────────────────────────────────────────────────

void ImportStagingArea::markStarDirty(const QString& starId)
{
    QMutexLocker lock(&_mutex);
    if (_workingStars.contains(starId) && !_newStarIds.contains(starId)) {
        _dirtyStarIds.insert(starId);
    }
}

void ImportStagingArea::markSpectrumNew(const QString& spectrumId)
{
    QMutexLocker lock(&_mutex);
    _newSpectrumIds.insert(spectrumId);
}

void ImportStagingArea::markFitNew(const QString& fitId)
{
    QMutexLocker lock(&_mutex);
    _newFitIds.insert(fitId);
}

void ImportStagingArea::markRVCurveNew(const QString& curveId)
{
    QMutexLocker lock(&_mutex);
    _newRVCurveIds.insert(curveId);
}

void ImportStagingArea::markSEDModelNew(const QString& modelId)
{
    QMutexLocker lock(&_mutex);
    _newSEDModelIds.insert(modelId);
}

void ImportStagingArea::markLightcurveDirty(const QString& starId)
{
    QMutexLocker lock(&_mutex);
    _dirtyLightcurveStarIds.insert(starId);
}

int ImportStagingArea::dirtyLightcurveStarCount() const
{
    QMutexLocker lock(&_mutex);
    return _dirtyLightcurveStarIds.size();
}

// ── Counts ──────────────────────────────────────────────────────

int ImportStagingArea::totalStarCount() const
{
    QMutexLocker lock(&_mutex);
    return _workingStars.size();
}

int ImportStagingArea::newStarCount() const
{
    QMutexLocker lock(&_mutex);
    return _newStarIds.size();
}

int ImportStagingArea::newSpectrumCount() const
{
    QMutexLocker lock(&_mutex);
    return _newSpectrumIds.size();
}

int ImportStagingArea::newFitCount() const
{
    QMutexLocker lock(&_mutex);
    return _newFitIds.size();
}

int ImportStagingArea::newRVCurveCount() const
{
    QMutexLocker lock(&_mutex);
    return _newRVCurveIds.size();
}

int ImportStagingArea::newSEDModelCount() const
{
    QMutexLocker lock(&_mutex);
    return _newSEDModelIds.size();
}

// ── Clear ───────────────────────────────────────────────────────

void ImportStagingArea::clear()
{
    QMutexLocker lock(&_mutex);

    LOG_INFO("Staging", QString("Clearing: %1 stars (%2 new), %3 spectra, %4 fits, %5 RV curves")
             .arg(_workingStars.size()).arg(_newStarIds.size())
             .arg(_newSpectrumIds.size()).arg(_newFitIds.size())
             .arg(_newRVCurveIds.size()));

    _workingStars.clear();
    _newStarIds.clear();
    _dirtyStarIds.clear();
    _newSpectrumIds.clear();
    _newFitIds.clear();
    _newRVCurveIds.clear();
    _newSEDModelIds.clear();
    _dirtyLightcurveStarIds.clear();
}

// ── Commit ──────────────────────────────────────────────────────

bool ImportStagingArea::commitAll(DatabaseManager* dbm, const QString& projectId)
{
    QMutexLocker lock(&_mutex);

    // ── Dedup before anything else ──────────────────────────────
    deduplicateStars();

    // Log summary (tracking sets used for informational counts only)
    LOG_INFO("Staging", QString("Committing: %1 total stars (%2 new, %3 dirty), "
             "%4 new spectra, %5 new fits, %6 new RV curves, %7 new SED models, "
             "%8 stars with new lightcurves")
             .arg(_workingStars.size())
             .arg(_newStarIds.size())
             .arg(_dirtyStarIds.size())
             .arg(_newSpectrumIds.size())
             .arg(_newFitIds.size())
             .arg(_newRVCurveIds.size())
             .arg(_newSEDModelIds.size())
             .arg(_dirtyLightcurveStarIds.size()));

    // Cross-check: count actual in-memory data
    int actualSpectra = 0, actualFits = 0, actualRV = 0;
    for (auto it = _workingStars.cbegin(); it != _workingStars.cend(); ++it) {
        const auto& star = it.value();
        for (const auto& sp : star->getSpectra()) {
            actualSpectra++;
            actualFits += static_cast<int>(sp->getSpectralFits().size());
        }
        if (star->getRVCurve()) actualRV++;
    }
    LOG_INFO("Staging", QString("Actual in-memory: %1 spectra, %2 fits, %3 RV curves")
             .arg(actualSpectra).arg(actualFits).arg(actualRV));

    if (!dbm) return false;
    if (!dbm->beginTransaction()) {
        LOG_ERROR("Staging", "Failed to begin transaction");
        return false;
    }

    try {
        // ── Single pass over all stars ──────────────────────────
        for (auto it = _workingStars.cbegin(); it != _workingStars.cend(); ++it) {
            const QString& starId = it.key();
            const auto& star = it.value();
            if (!star) continue;

            const bool isNew = _newStarIds.contains(starId);
            const bool isDirty = _dirtyStarIds.contains(starId);

            // ── A. Star row ─────────────────────────────────────
            if (isNew) {
                // INSERT star row ONLY (no cascade — we handle children below)
                // Use saveStar which does INSERT OR REPLACE + cascades to
                // spectra/fits. We still need to handle RV separately.
                //
                // However, saveStar() will also save all spectra and their
                // fits via its own cascade. To avoid double-saving, we let
                // saveStar handle spectra/fits for new stars and only
                // supplement with RV below.
                if (!dbm->saveStar(projectId, star)) {
                    LOG_ERROR("Staging", QString("Failed to save new star %1").arg(starId));
                    dbm->rollbackTransaction();
                    return false;
                }
            } else if (isDirty) {
                // UPDATE existing star row (no cascade)
                if (!dbm->updateStarRow(projectId, star)) {
                    LOG_ERROR("Staging", QString("Failed to update star %1").arg(starId));
                    dbm->rollbackTransaction();
                    return false;
                }
            }

            // ── B. Spectra + fits (existing stars only) ─────────
            // For new stars, saveStar() already cascaded to all spectra/fits.
            // For existing stars, save new spectra and new fits on old spectra.
            if (!isNew) {
                for (const auto& sp : star->getSpectra()) {
                    if (_newSpectrumIds.contains(sp->getId())) {
                        // New spectrum — saveSpectrum cascades to its fits
                        if (!dbm->saveSpectrum(starId, sp)) {
                            LOG_ERROR("Staging", QString("Failed to save spectrum %1").arg(sp->getId()));
                            dbm->rollbackTransaction();
                            return false;
                        }
                    } else {
                        // Existing spectrum — check for new fits only
                        for (const auto& fit : sp->getSpectralFits()) {
                            if (_newFitIds.contains(fit->getId())) {
                                if (!dbm->saveSpectralFit(starId, sp->getId(), fit)) {
                                    LOG_ERROR("Staging", QString("Failed to save fit %1").arg(fit->getId()));
                                    dbm->rollbackTransaction();
                                    return false;
                                }
                            }
                        }
                    }
                }
            }

            // ── B2. SED models (existing stars only) ────────
            // For new stars, saveStar() already cascaded to photometry/SED.
            // For existing stars, save new SED models surgically to avoid
            // the CASCADE DELETE that INSERT OR REPLACE on photometry causes.
            if (!isNew) {
                auto phot = star->getPhotometry();
                if (phot) {
                    for (const auto& sedModel : phot->getSEDModels()) {
                        if (_newSEDModelIds.contains(sedModel->getId())) {
                            if (!dbm->saveSEDModelForStar(starId, sedModel)) {
                                LOG_ERROR("Staging",
                                    QString("Failed to save SED model %1 for star %2")
                                        .arg(sedModel->getId(), starId));
                                dbm->rollbackTransaction();
                                return false;
                            }
                        }
                    }
                }
            }

            // ── B3. Lightcurve data (stars with new LC data) ────
            if (_dirtyLightcurveStarIds.contains(starId)) {
                auto phot = star->getPhotometry();
                if (phot) {
                    for (const QString& source : phot->getLightcurveSources()) {
                        if (!dbm->saveLightcurveForStar(starId, source, phot.get())) {
                            LOG_ERROR("Staging",
                                      QString("Failed to save lightcurve '%1' for star %2")
                                          .arg(source, starId));
                            dbm->rollbackTransaction();
                            return false;
                        }
                    }
                }
            }

            // ── C. RV curve + points + fits (ALL stars) ─────────
            // This is the critical fix: never skip RV based on star newness.
            // saveStar() does NOT cascade to RV, so we always handle it here.
            auto curve = star->getRVCurve();
            if (curve && _newRVCurveIds.contains(curve->getId())) {
                if (!dbm->saveRadialVelocityCurve(curve, starId)) {
                    LOG_ERROR("Staging", QString("Failed to save RV curve %1").arg(curve->getId()));
                    dbm->rollbackTransaction();
                    return false;
                }

                for (const auto& pt : curve->getRVPoints()) {
                    pt->setCurveId(curve->getId());
                    if (!dbm->saveRadialVelocityPoint(pt, curve->getId())) {
                        LOG_ERROR("Staging", QString("Failed to save RV point for curve %1")
                                  .arg(curve->getId()));
                        dbm->rollbackTransaction();
                        return false;
                    }
                }

                for (const auto& fit : curve->getRVFits()) {
                    fit->setCurveId(curve->getId());
                    if (!dbm->saveRVFit(fit, curve->getId())) {
                        LOG_ERROR("Staging", QString("Failed to save RV fit for curve %1")
                                  .arg(curve->getId()));
                        dbm->rollbackTransaction();
                        return false;
                    }
                }
            }
        }

        // ── D. Commit ───────────────────────────────────────────
        if (!dbm->commitTransaction()) {
            LOG_ERROR("Staging", "Failed to commit transaction");
            return false;
        }

        LOG_INFO("Staging", "Commit successful");

        _workingStars.clear();
        _newStarIds.clear();
        _dirtyStarIds.clear();
        _newSpectrumIds.clear();
        _newFitIds.clear();
        _newRVCurveIds.clear();
        _newSEDModelIds.clear();
        _dirtyLightcurveStarIds.clear();

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("Staging", QString("Exception during commit: %1").arg(e.what()));
        dbm->rollbackTransaction();
        return false;
    }
}

void ImportStagingArea::deduplicateStars()
{
    // Group stars by source_id (primary key for dedup)
    // Stars without source_id are left as-is
    QHash<QString, QList<QString>> sourceIdToStarIds;  // source_id → list of star UUIDs

    for (auto it = _workingStars.cbegin(); it != _workingStars.cend(); ++it) {
        QString sourceId = it.value()->getSourceId().trimmed();
        if (!sourceId.isEmpty()) {
            sourceIdToStarIds[sourceId].append(it.key());
        }
    }

    int mergedCount = 0;

    for (auto it = sourceIdToStarIds.cbegin(); it != sourceIdToStarIds.cend(); ++it) {
        const QList<QString>& starIds = it.value();
        if (starIds.size() <= 1) continue;

        // Pick the "best" keeper: prefer one that has children, or is marked dirty
        // (meaning it existed in DB already), or just the first one
        QString keeperId;
        int bestScore = -1;

        for (const QString& sid : starIds) {
            auto star = _workingStars.value(sid);
            if (!star) continue;

            int score = 0;
            score += static_cast<int>(star->getSpectra().size()) * 10;
            if (star->getRVCurve()) score += 100;
            if (_dirtyStarIds.contains(sid)) score += 50;  // existed in DB
            if (!_newStarIds.contains(sid)) score += 200;   // definitely existed in DB

            if (score > bestScore) {
                bestScore = score;
                keeperId = sid;
            }
        }

        if (keeperId.isEmpty()) keeperId = starIds.first();

        auto keeper = _workingStars.value(keeperId);
        if (!keeper) continue;

        // Merge all others into keeper
        for (const QString& sid : starIds) {
            if (sid == keeperId) continue;

            auto donor = _workingStars.value(sid);
            if (!donor) continue;

            // Merge scalar fields: fill blanks on keeper from donor
            if (keeper->getAlias().isEmpty() && !donor->getAlias().isEmpty())
                keeper->setAlias(donor->getAlias());
            if (keeper->getTic().isEmpty() && !donor->getTic().isEmpty())
                keeper->setTic(donor->getTic());
            if (keeper->getJName().isEmpty() && !donor->getJName().isEmpty())
                keeper->setJName(donor->getJName());
            if (!Star::isSet(keeper->getRa()) && Star::isSet(donor->getRa()))
                keeper->setRa(donor->getRa());
            if (!Star::isSet(keeper->getDec()) && Star::isSet(donor->getDec()))
                keeper->setDec(donor->getDec());
            if (!Star::isSet(keeper->getPmra()) && Star::isSet(donor->getPmra()))
                keeper->setPmra(donor->getPmra());
            if (!Star::isSet(keeper->getPmdec()) && Star::isSet(donor->getPmdec()))
                keeper->setPmdec(donor->getPmdec());
            if (!Star::isSet(keeper->getPlx()) && Star::isSet(donor->getPlx()))
                keeper->setPlx(donor->getPlx());
            if (!Star::isSet(keeper->getEPmra()) && Star::isSet(donor->getEPmra()))
                keeper->setEPmra(donor->getEPmra());
            if (!Star::isSet(keeper->getEPmdec()) && Star::isSet(donor->getEPmdec()))
                keeper->setEPmdec(donor->getEPmdec());
            if (!Star::isSet(keeper->getEPlx()) && Star::isSet(donor->getEPlx()))
                keeper->setEPlx(donor->getEPlx());
            if (!Star::isSet(keeper->getGmag()) && Star::isSet(donor->getGmag()))
                keeper->setGmag(donor->getGmag());
            if (!Star::isSet(keeper->getBp()) && Star::isSet(donor->getBp()))
                keeper->setBp(donor->getBp());
            if (!Star::isSet(keeper->getRp()) && Star::isSet(donor->getRp()))
                keeper->setRp(donor->getRp());
            if (!Star::isSet(keeper->getBpRp()) && Star::isSet(donor->getBpRp()))
                keeper->setBpRp(donor->getBpRp());
            if (!Star::isSet(keeper->getTeff()) && Star::isSet(donor->getTeff()))
                keeper->setTeff(donor->getTeff());
            if (!Star::isSet(keeper->getLogg()) && Star::isSet(donor->getLogg()))
                keeper->setLogg(donor->getLogg());

            // Merge spectra: transfer all spectra from donor to keeper
            // Use a set to avoid duplicating spectra that are already on keeper
            QSet<QString> keeperSpecIds;
            for (const auto& sp : keeper->getSpectra())
                keeperSpecIds.insert(sp->getId());

            for (const auto& sp : donor->getSpectra()) {
                if (!keeperSpecIds.contains(sp->getId())) {
                    keeper->addSpectrum(sp);
                    // Transfer tracking
                    if (_newSpectrumIds.contains(sp->getId()))
                        _newSpectrumIds.insert(sp->getId());  // already there, no-op
                    for (const auto& fit : sp->getSpectralFits()) {
                        if (_newFitIds.contains(fit->getId()))
                            _newFitIds.insert(fit->getId());   // already there, no-op
                    }
                }
            }

            // Merge RV curve: keeper wins if it has one, else take donor's
            if (!keeper->getRVCurve() && donor->getRVCurve()) {
                auto curve = donor->getRVCurve();
                curve->setStarId(keeperId);
                keeper->setRVCurve(curve);
                if (_newRVCurveIds.contains(curve->getId()))
                    _newRVCurveIds.insert(curve->getId());
                } else if (keeper->getRVCurve() && donor->getRVCurve()) {
                    // Both have RV curves — merge points into keeper's curve
                    auto keeperCurve = keeper->getRVCurve();
                    auto donorCurve = donor->getRVCurve();
    
                    QSet<QString> existingPointIds;
                    for (const auto& pt : keeperCurve->getRVPoints())
                        existingPointIds.insert(pt->getId());
    
                    for (const auto& pt : donorCurve->getRVPoints()) {
                        if (!existingPointIds.contains(pt->getId())) {
                            pt->setCurveId(keeperCurve->getId());
                            keeperCurve->addRVPoint(pt);
                        }
                    }
    
                    // Merge RV fits from donor into keeper
                    QSet<QString> existingFitIds;
                    for (const auto& f : keeperCurve->getRVFits())
                        existingFitIds.insert(f->getId());
                    for (const auto& f : donorCurve->getRVFits()) {
                        if (!existingFitIds.contains(f->getId())) {
                            f->setCurveId(keeperCurve->getId());
                            keeperCurve->addRVFit(f);
                        }
                    }
    
                    // Remove donor curve from tracking (won't be saved)
                    _newRVCurveIds.remove(donorCurve->getId());
    
                    // FIX: Ensure keeper's curve IS tracked for saving,
                    // even if it was originally loaded from DB (not new).
                    // After merging new points into it, it must be persisted.
                    _newRVCurveIds.insert(keeperCurve->getId());
                }

            // Merge bibcodes
            for (const auto& bib : donor->getBibcodes()) {
                bool found = false;
                for (const auto& existing : keeper->getBibcodes()) {
                    if (existing == bib) { found = true; break; }
                }
                if (!found) keeper->addBibcode(bib);
            }

            // Remove donor from working set and tracking
            _workingStars.remove(sid);
            _newStarIds.remove(sid);
            _dirtyStarIds.remove(sid);

            mergedCount++;
        }

        // Keeper is always dirty after a merge
        _dirtyStarIds.insert(keeperId);
    }

    if (mergedCount > 0) {
        LOG_INFO("Staging", QString("Deduplication: merged %1 duplicate stars, "
                 "%2 unique stars remain")
                 .arg(mergedCount).arg(_workingStars.size()));
    }
}