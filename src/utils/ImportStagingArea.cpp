#include "ImportStagingArea.h"

#include "DatabaseManager.h"
#include "models/Project.h"
#include "models/Star.h"
#include "models/Spectrum.h"
#include "models/RadialVelocity.h"
#include "utils/Logger.h"

// ── Staging methods ─────────────────────────────────────────────

void ImportStagingArea::stageNewStars(const std::vector<std::shared_ptr<Star>>& stars)
{
    QMutexLocker lock(&_mutex);
    _newStars.insert(_newStars.end(), stars.begin(), stars.end());
}

void ImportStagingArea::stageSpectrum(const StagedSpectrum& entry)
{
    QMutexLocker lock(&_mutex);
    _newSpectra.push_back(entry);
}

void ImportStagingArea::stageSpectra(const std::vector<StagedSpectrum>& entries)
{
    QMutexLocker lock(&_mutex);
    _newSpectra.insert(_newSpectra.end(), entries.begin(), entries.end());
}

void ImportStagingArea::stageFit(const StagedFit& entry)
{
    QMutexLocker lock(&_mutex);
    _newFits.push_back(entry);
}

void ImportStagingArea::stageFits(const std::vector<StagedFit>& entries)
{
    QMutexLocker lock(&_mutex);
    _newFits.insert(_newFits.end(), entries.begin(), entries.end());
}

void ImportStagingArea::stageRVResult(const StagedRVResult& result)
{
    QMutexLocker lock(&_mutex);
    _rvResults.push_back(result);
}

void ImportStagingArea::stageRVResults(const std::vector<StagedRVResult>& results)
{
    QMutexLocker lock(&_mutex);
    _rvResults.insert(_rvResults.end(), results.begin(), results.end());
}

void ImportStagingArea::stageModifiedStar(const QString& starId)
{
    QMutexLocker lock(&_mutex);
    _modifiedExistingStarIds.insert(starId);
}

// ── Queries ─────────────────────────────────────────────────────

bool ImportStagingArea::isEmpty() const
{
    QMutexLocker lock(&_mutex);
    return _newStars.empty() && _newSpectra.empty() &&
           _newFits.empty() && _rvResults.empty() &&
           _modifiedExistingStarIds.isEmpty();
}

int ImportStagingArea::newStarCount() const
{
    QMutexLocker lock(&_mutex);
    return static_cast<int>(_newStars.size());
}

int ImportStagingArea::newSpectraCount() const
{
    QMutexLocker lock(&_mutex);
    return static_cast<int>(_newSpectra.size());
}

int ImportStagingArea::newFitCount() const
{
    QMutexLocker lock(&_mutex);
    return static_cast<int>(_newFits.size());
}

int ImportStagingArea::rvResultCount() const
{
    QMutexLocker lock(&_mutex);
    return static_cast<int>(_rvResults.size());
}

std::vector<std::shared_ptr<Star>> ImportStagingArea::stagedNewStars() const
{
    QMutexLocker lock(&_mutex);
    return _newStars;
}

// ── Commit ──────────────────────────────────────────────────────

bool ImportStagingArea::commitAll(DatabaseManager* dbm,
                                  std::shared_ptr<Project> project)
{
    QMutexLocker lock(&_mutex);

    if (!dbm || !project) return false;

    const QString projectId = project->getId();

    LOG_INFO("Staging", QString("Committing: %1 stars, %2 spectra, %3 fits, %4 RV results, %5 modified stars")
             .arg(_newStars.size()).arg(_newSpectra.size())
             .arg(_newFits.size()).arg(_rvResults.size())
             .arg(_modifiedExistingStarIds.size()));

    if (!dbm->beginTransaction()) {
        LOG_ERROR("Staging", "Failed to begin transaction");
        return false;
    }

    try {
        // ── Build a set of new-star IDs so we can skip duplicates in steps 2/3 ──
        // NOTE: star IDs may be empty at this point — they get assigned during saveStar
        QSet<QString> newStarIds;
        for (const auto& star : _newStars) {
            if (!star->getId().isEmpty())
                newStarIds.insert(star->getId());
        }

        // 1. Save new stars (saveStar cascades to their spectra and fits)
        for (const auto& star : _newStars) {
            if (!dbm->saveStar(projectId, star)) {
                LOG_ERROR("Staging", QString("Failed to save star %1").arg(star->getId()));
                dbm->rollbackTransaction();
                return false;
            }
        }

        // ── After step 1, star objects now have IDs assigned by saveStar. ──
        // ── Rebuild the newStarIds set with the real IDs.                 ──
        newStarIds.clear();
        for (const auto& star : _newStars) {
            if (!star->getId().isEmpty())
                newStarIds.insert(star->getId());
        }

        // 2. Save spectra attached to EXISTING stars only.
        //    Spectra for new stars were already saved by saveStar's cascade in step 1.
        for (const auto& entry : _newSpectra) {
            // Resolve starId from the live star object (it now has an ID after step 1)
            QString resolvedStarId = entry.starId;
            if (resolvedStarId.isEmpty() && entry.star)
                resolvedStarId = entry.star->getId();

            if (resolvedStarId.isEmpty()) {
                LOG_WARNING("Staging", QString("Skipping spectrum with unresolvable starId (spectrum=%1)")
                            .arg(entry.spectrum ? entry.spectrum->getId() : "null"));
                continue;
            }

            // Skip if this spectrum belongs to a newly-created star — already saved in step 1
            if (newStarIds.contains(resolvedStarId))
                continue;

            if (!dbm->saveSpectrum(resolvedStarId, entry.spectrum)) {
                LOG_ERROR("Staging", QString("Failed to save spectrum for star %1").arg(resolvedStarId));
                dbm->rollbackTransaction();
                return false;
            }
        }

        // 3. Save fits attached to existing spectra (skip those on new stars)
        for (const auto& entry : _newFits) {
            // Resolve starId from the live star object
            QString resolvedStarId = entry.starId;
            if (resolvedStarId.isEmpty() && entry.spectrum) {
                // Try to find the star that owns this spectrum
                for (const auto& star : _newStars) {
                    for (const auto& sp : star->getSpectra()) {
                        if (sp == entry.spectrum || sp->getId() == entry.spectrumId) {
                            resolvedStarId = star->getId();
                            break;
                        }
                    }
                    if (!resolvedStarId.isEmpty()) break;
                }
            }

            QString resolvedSpectrumId = entry.spectrumId;
            if (resolvedSpectrumId.isEmpty() && entry.spectrum)
                resolvedSpectrumId = entry.spectrum->getId();

            if (resolvedStarId.isEmpty() || resolvedSpectrumId.isEmpty()) {
                LOG_WARNING("Staging", QString("Skipping fit with unresolvable IDs (star=%1, spectrum=%2)")
                            .arg(resolvedStarId, resolvedSpectrumId));
                continue;
            }

            // Skip if this fit belongs to a newly-created star — already saved in step 1
            if (newStarIds.contains(resolvedStarId))
                continue;

            if (!dbm->saveSpectralFit(resolvedStarId, resolvedSpectrumId, entry.fit)) {
                LOG_ERROR("Staging", QString("Failed to save fit for spectrum %1").arg(resolvedSpectrumId));
                dbm->rollbackTransaction();
                return false;
            }
        }

        // 4. Save RV curves, points, and fits
        for (const auto& result : _rvResults) {
            // Resolve starId from the live star object (now has ID after step 1)
            QString resolvedStarId = result.starId;
            if (resolvedStarId.isEmpty() && result.star)
                resolvedStarId = result.star->getId();

            if (resolvedStarId.isEmpty()) {
                LOG_WARNING("Staging", "Skipping RV result with unresolvable starId");
                continue;
            }

            auto& curve = result.curve;
            curve->setStarId(resolvedStarId);

            if (!dbm->saveRadialVelocityCurve(curve, resolvedStarId)) {
                LOG_ERROR("Staging", QString("Failed to save RV curve for star %1").arg(resolvedStarId));
                dbm->rollbackTransaction();
                return false;
            }

            for (const auto& pt : curve->getRVPoints()) {
                pt->setCurveId(curve->getId());
                if (!dbm->saveRadialVelocityPoint(pt, curve->getId())) {
                    LOG_ERROR("Staging", QString("Failed to save RV point for curve %1").arg(curve->getId()));
                    dbm->rollbackTransaction();
                    return false;
                }
            }

            // Save ALL fits on the curve
            for (const auto& fit : curve->getRVFits()) {
                fit->setCurveId(curve->getId());
                if (!dbm->saveRVFit(fit, curve->getId())) {
                    LOG_ERROR("Staging", QString("Failed to save RV fit for curve %1").arg(curve->getId()));
                    dbm->rollbackTransaction();
                    return false;
                }
            }

            if (result.fit && curve->getRVFits().empty()) {
                result.fit->setCurveId(curve->getId());
                if (!dbm->saveRVFit(result.fit, curve->getId())) {
                    LOG_ERROR("Staging", QString("Failed to save RV fit for curve %1").arg(curve->getId()));
                    dbm->rollbackTransaction();
                    return false;
                }
            }

            // Update the star's RV metrics
            if (result.star) {
                result.star->updateRVMetricsFromCurve();
                if (!newStarIds.contains(resolvedStarId)) {
                    if (!dbm->updateStarRow(projectId, result.star)) {
                        LOG_ERROR("Staging", QString("Failed to update star RV metrics for %1").arg(resolvedStarId));
                        dbm->rollbackTransaction();
                        return false;
                    }
                }
            }
        }

        // 5. Update modified existing stars — row only, no cascade
        for (const QString& starId : _modifiedExistingStarIds) {
            if (newStarIds.contains(starId))
                continue;

            auto star = project->getStar(starId);
            if (star) {
                if (!dbm->updateStarRow(projectId, star)) {
                    LOG_ERROR("Staging", QString("Failed to update modified star %1").arg(starId));
                    dbm->rollbackTransaction();
                    return false;
                }
            }
        }

        if (!dbm->commitTransaction()) {
            LOG_ERROR("Staging", "Failed to commit transaction");
            return false;
        }

        LOG_INFO("Staging", "Commit successful");

        _newStars.clear();
        _newSpectra.clear();
        _newFits.clear();
        _rvResults.clear();
        _modifiedExistingStarIds.clear();

        return true;

    } catch (const std::exception& e) {
        LOG_ERROR("Staging", QString("Exception during commit: %1").arg(e.what()));
        dbm->rollbackTransaction();
        return false;
    } catch (...) {
        LOG_ERROR("Staging", "Unknown exception during commit");
        dbm->rollbackTransaction();
        return false;
    }
}

// ── Rollback ────────────────────────────────────────────────────

void ImportStagingArea::rollbackAll(std::shared_ptr<Project> project)
{
    QMutexLocker lock(&_mutex);

    LOG_INFO("Staging", QString("Rolling back: %1 stars, %2 spectra, %3 fits, %4 RV results")
             .arg(_newStars.size()).arg(_newSpectra.size())
             .arg(_newFits.size()).arg(_rvResults.size()));

    // Reverse order: RV → Fits → Spectra → Stars

    // 4. Remove RV curves from stars
    for (const auto& result : _rvResults) {
        if (result.star && result.star->getRVCurve() == result.curve) {
            result.star->setRVCurve(nullptr);
        }
    }

    // 3. Remove staged fits from their spectra
    for (const auto& entry : _newFits) {
        if (entry.spectrum && entry.fit) {
            entry.spectrum->removeSpectralFit(entry.fit->getId());
        }
    }

    // 2. Remove staged spectra from their stars
    for (const auto& entry : _newSpectra) {
        if (entry.star && entry.spectrum) {
            entry.star->removeSpectrum(entry.spectrum->getId());
        }
    }

    // 1. Remove new stars from the project
    if (project) {
        for (const auto& star : _newStars) {
            project->removeStar(star);
        }
    }

    // Note: We do NOT revert Gaia/SIMBAD in-memory enrichment on
    // _modifiedExistingStarIds — that data is still correct, it just
    // won't be persisted. The next project open will reload from DB.

    _newStars.clear();
    _newSpectra.clear();
    _newFits.clear();
    _rvResults.clear();
    _modifiedExistingStarIds.clear();
}