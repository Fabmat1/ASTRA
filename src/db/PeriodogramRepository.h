#ifndef PERIODOGRAMREPOSITORY_H
#define PERIODOGRAMREPOSITORY_H

#include <memory>
#include <vector>
#include <QString>

class DBAccess;
class PeriodogramRecord;

class PeriodogramRepository
{
public:
    explicit PeriodogramRepository(DBAccess& db);

    /// Atomically replace all per-series records for this star.
    bool saveAllForStar(const QString& starId,
        const std::vector<std::shared_ptr<PeriodogramRecord>>& records);

    /// Load every per-series record for the star (data files read eagerly).
    std::vector<std::shared_ptr<PeriodogramRecord>>
        loadAllForStar(const QString& starId);

    /// Convenience single-record lookup. `filter` empty → first matching source.
    std::shared_ptr<PeriodogramRecord> load(const QString& starId,
                                             const QString& source,
                                             const QString& filter = {});

    bool deleteAllForStar(const QString& starId);

    // ── RV-curve periodograms ────────────────────────────────────────
    // Stored in a dedicated `rv_periodograms` table keyed by RV curve id so
    // they never mix with the per-series lightcurve periodograms above (those
    // get atomically replaced on LC recompute and folded into the LC product
    // used as the RV-MCMC prior). `starId` is only used to place the data file.
    bool saveAllForCurve(const QString& starId, const QString& curveId,
        const std::vector<std::shared_ptr<PeriodogramRecord>>& records);

    std::vector<std::shared_ptr<PeriodogramRecord>>
        loadAllForCurve(const QString& curveId);

    bool deleteAllForCurve(const QString& curveId);

private:
    DBAccess& _db;
};

#endif