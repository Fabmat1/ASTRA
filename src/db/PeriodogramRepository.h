#ifndef PERIODOGRAMREPOSITORY_H
#define PERIODOGRAMREPOSITORY_H

#include <memory>
#include <optional>
#include <vector>
#include <QDateTime>
#include <QString>

class DBAccess;
class PeriodogramRecord;

/// One periodogram in the store: a star's light-curve periodogram (all its
/// series, saved and shown together) or one RV curve's periodogram. The entry
/// outlives the periodogram's data when the store evicts it, so its settings
/// and view count are there when it is computed again.
struct PeriodogramStoreEntry {
    static constexpr const char* kLightcurve = "lc";   ///< owner is a star
    static constexpr const char* kRV         = "rv";   ///< owner is an RV curve

    QString   kind;
    QString   ownerId;
    QString   starId;
    QString   settingsJson;   ///< PeriodogramSettings; empty if never recorded
    QDateTime computedAt;
    QDateTime lastViewedAt;
    int       viewCount = 0;
    bool      hasData   = false;
};

class PeriodogramRepository
{
public:
    explicit PeriodogramRepository(DBAccess& db);

    /// Atomically replace all per-series records for this star. A non-empty
    /// `settingsJson` (PeriodogramSettings) is kept in the store entry.
    bool saveAllForStar(const QString& starId,
        const std::vector<std::shared_ptr<PeriodogramRecord>>& records,
        const QString& settingsJson = {});

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
        const std::vector<std::shared_ptr<PeriodogramRecord>>& records,
        const QString& settingsJson = {});

    std::vector<std::shared_ptr<PeriodogramRecord>>
        loadAllForCurve(const QString& curveId);

    bool deleteAllForCurve(const QString& curveId);

    // ── The store ────────────────────────────────────────────────────
    std::optional<PeriodogramStoreEntry> storeEntry(const QString& kind,
                                                    const QString& ownerId);
    /// Counts one look at the entry; nothing if it has none.
    void markViewed(const QString& kind, const QString& ownerId);

    /// Keeps the `keepViewed` most viewed periodograms and the `keepRecent`
    /// most recently computed ones, and deletes the data of all others. Their
    /// entries stay, with the settings they were computed with. Periodograms
    /// saved before the store existed (or copied with a star) join it first.
    /// Returns how many were deleted.
    int enforceStore(int keepViewed, int keepRecent);

private:
    DBAccess& _db;
};

#endif