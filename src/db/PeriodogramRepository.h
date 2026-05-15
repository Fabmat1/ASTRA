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

private:
    DBAccess& _db;
};

#endif