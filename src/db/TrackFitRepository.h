#pragma once

// Persistence of evolutionary-track fits (table track_fits), see evolution/TrackFit.h.
//
// The adoption flag is is_best_fit (the name the sync's best-fit repair looks for;
// at most one per star).  Which fit gets adopted after a run is decided by
// evolution/StarTracks; this class only stores the outcome.

#include "evolution/TrackFit.h"

#include <QHash>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>
#include <vector>

class DBAccess;

class TrackFitRepository {
public:
    explicit TrackFitRepository(DBAccess& db);

    /// CREATE TABLE statement (generated from the quantity table).
    static QString createTableSql();
    static QStringList indexSql();

    /// Insert or replace by id.
    bool saveFit(const astra::evolution::TrackFitRecord& fit);
    /// Several fits in one transaction (unless one is already open).
    bool saveFits(const std::vector<astra::evolution::TrackFitRecord>& fits);

    /// The star's fits, newest first.
    std::vector<astra::evolution::TrackFitRecord> fitsForStar(const QString& starId);
    std::optional<astra::evolution::TrackFitRecord> fitById(const QString& id);
    /// Number of fits per star of a project (stars without fits are absent).
    QHash<QString, int> fitCounts(const QString& projectId);

    bool deleteFit(const QString& id);
    bool deleteFitsForStar(const QString& starId);

    /// Make `fitId` the star's adopted fit (none if empty).  `manual` marks a pick by
    /// the user, which later runs leave in place.
    bool setAdopted(const QString& starId, const QString& fitId, bool manual);

    /// Write only the star's trk_* columns (the adopted fit's mirror; cleared when
    /// `summary` has no fit).  Leaves every other column of the star row alone.
    bool writeStarSummary(const QString& starId, const StarTrackSummary& summary);

    /// Run `body` in one transaction (or inside the one already open); rolled back
    /// when it returns false.
    bool inTransaction(const std::function<bool()>& body);

private:
    DBAccess& _db;
};
