#ifndef IMPORTSTAGINGAREA_H
#define IMPORTSTAGINGAREA_H

#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QString>
#include <memory>
#include <vector>

class Star;
class Spectrum;
class SpectralFit;
class RadialVelocityCurve;
class RadialVelocityPoint;
class RVFit;
class DatabaseManager;
class Project;

struct StagedSpectrum {
    QString starId;
    std::shared_ptr<Star> star;           // in-memory star that already has the spectrum added
    std::shared_ptr<Spectrum> spectrum;
};

struct StagedFit {
    QString starId;
    QString spectrumId;
    std::shared_ptr<Spectrum> spectrum;   // in-memory spectrum that already has the fit added
    std::shared_ptr<SpectralFit> fit;
};

struct StagedRVResult {
    QString starId;
    std::shared_ptr<Star> star;
    std::shared_ptr<RadialVelocityCurve> curve;
    std::shared_ptr<RVFit> fit;           // may be nullptr
};

class ImportStagingArea
{
public:
    ImportStagingArea() = default;

    // ── Staging methods (called from tasks / wizard pages) ──────

    void stageNewStars(const std::vector<std::shared_ptr<Star>>& stars);
    std::vector<StagedSpectrum> stagedSpectra() const;
    std::vector<StagedFit> stagedFits() const;
    void stageSpectrum(const StagedSpectrum& entry);
    void stageSpectra(const std::vector<StagedSpectrum>& entries);
    void stageFit(const StagedFit& entry);
    void stageFits(const std::vector<StagedFit>& entries);
    void stageRVResult(const StagedRVResult& result);
    void stageRVResults(const std::vector<StagedRVResult>& results);
    void stageModifiedStar(const QString& starId);

    // ── Commit / Rollback ───────────────────────────────────────

    /// Write everything to the database inside a single transaction.
    /// Returns true on success.
    bool commitAll(DatabaseManager* dbm,
                   std::shared_ptr<Project> project);

    /// Undo all in-memory mutations that were made during staging.
    void rollbackAll(std::shared_ptr<Project> project);

    // ── Queries ─────────────────────────────────────────────────

    bool isEmpty() const;
    int newStarCount() const;
    int newSpectraCount() const;
    int newFitCount() const;
    int rvResultCount() const;

    // Access staged new stars (needed by wizard pages to treat them
    // as part of the project's star list before commit)
    std::vector<std::shared_ptr<Star>> stagedNewStars() const;

private:
    mutable QMutex _mutex;

    std::vector<std::shared_ptr<Star>> _newStars;
    std::vector<StagedSpectrum>        _newSpectra;
    std::vector<StagedFit>             _newFits;
    std::vector<StagedRVResult>        _rvResults;
    QSet<QString>                      _modifiedExistingStarIds;
};

#endif // IMPORTSTAGINGAREA_H