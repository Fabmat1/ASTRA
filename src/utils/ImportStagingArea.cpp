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

        // 2. Save spectra attached to EXISTING stars only.
        //    Spectra for new stars were already saved by saveStar's cascade in step 1.
        for (const auto& entry : _newSpectra) {
            if (entry.starId.isEmpty()) {
                LOG_WARNING("Staging", QString("Skipping spectrum with empty starId (spectrum=%1)")
                            .arg(entry.spectrum ? entry.spectrum->getId() : "null"));
                continue;
            }

            // Skip if this spectrum belongs to a newly-created star — already saved in step 1
            if (newStarIds.contains(entry.starId)) {
                continue;
            }

            if (!dbm->saveSpectrum(entry.starId, entry.spectrum)) {
                LOG_ERROR("Staging", QString("Failed to save spectrum for star %1").arg(entry.starId));
                dbm->rollbackTransaction();
                return false;
            }
        }

        // 3. Save fits attached to existing spectra (skip those on new stars)
        for (const auto& entry : _newFits) {
            if (entry.starId.isEmpty() || entry.spectrumId.isEmpty()) {
                LOG_WARNING("Staging", QString("Skipping fit with empty starId/spectrumId (star=%1, spectrum=%2)")
                            .arg(entry.starId, entry.spectrumId));
                continue;
            }

            // Skip if this fit belongs to a newly-created star — already saved in step 1
            if (newStarIds.contains(entry.starId)) {
                continue;
            }

            if (!dbm->saveSpectralFit(entry.starId, entry.spectrumId, entry.fit)) {
                LOG_ERROR("Staging", QString("Failed to save fit for spectrum %1").arg(entry.spectrumId));
                dbm->rollbackTransaction();
                return false;
            }
        }

        // 4. Save RV curves, points, and fits
        for (const auto& result : _rvResults) {
            if (result.starId.isEmpty()) {
                LOG_WARNING("Staging", "Skipping RV result with empty starId");
                continue;
            }

            auto& curve = result.curve;
            curve->setStarId(result.starId);

            if (!dbm->saveRadialVelocityCurve(curve, result.starId)) {
                LOG_ERROR("Staging", QString("Failed to save RV curve for star %1").arg(result.starId));
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

            if (result.fit) {
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
                // Only update the row if the star isn't brand new (new stars get full save in step 1)
                if (!newStarIds.contains(result.starId)) {
                    if (!dbm->updateStarRow(projectId, result.star)) {
                        LOG_ERROR("Staging", QString("Failed to update star RV metrics for %1").arg(result.starId));
                        dbm->rollbackTransaction();
                        return false;
                    }
                }
            }
        }

        // 5. Update modified existing stars (Gaia/SIMBAD enrichment) — row only, no cascade
        for (const QString& starId : _modifiedExistingStarIds) {
            // Skip new stars — they're fully saved in step 1
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

        // Clear staging area after successful commit
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

std::vector<StagedSpectrum> ImportStagingArea::stagedSpectra() const
{
    QMutexLocker lock(&_mutex);
    return _newSpectra;
}

std::vector<StagedFit> ImportStagingArea::stagedFits() const
{
    QMutexLocker lock(&_mutex);
    return _newFits;
}