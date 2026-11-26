#pragma once

// One stored evolutionary-track fit of a star (a row of track_fits).
//
// Several fits per star are kept; one is adopted (is_best_fit, the column name the
// sync's best-fit repair expects) and mirrored on the star's trk_* columns.  The
// posterior samples are not stored: a fit is redrawn by re-running it with its
// stored per-star seed and configuration.

#include "core/StarTrackSummary.h"

#include <QDateTime>
#include <QJsonObject>
#include <QString>

#include <array>
#include <limits>

namespace astra::evolution {

struct TrackFitRecord {
    static constexpr double nan = std::numeric_limits<double>::quiet_NaN();

    QString id;
    QString starId;
    QString runId;          ///< fits made together (one dialog run / bulk run)
    QDateTime createdAt;    ///< UTC
    QString family;         ///< track family key ("han", ...)
    QString plane;          ///< "hrd" or "kiel"
    bool adopted = false;   ///< is_best_fit
    bool adoptedManual = false;  ///< adopted by the user; sticks across new runs
    bool ok = false;
    QString message;        ///< why the fit failed, if !ok
    quint64 seed = 0;       ///< the per-star seed, mix_seed(run seed, index)
    QString trackFile;      ///< file name of the track set
    QString trackSha1;      ///< its fingerprint
    QJsonObject subset;     ///< TrackSubset::toJson
    QJsonObject config;     ///< TrackModelSettings::toJson
    QJsonObject inputs;     ///< the observables used (snapshot for staleness)
    QJsonObject results;    ///< every summary of the FitResult, diagnostics

    std::array<double, astra::tracks::QtyCount> value{};    ///< medians
    std::array<double, astra::tracks::QtyCount> errUp{};    ///< to the 84.13 % quantile
    std::array<double, astra::tracks::QtyCount> errDown{};  ///< to the 15.87 % quantile
    double fracPostEhb = nan;
    int inRegion = -1;
    QString bounded;
    double acceptance = nan;

    TrackFitRecord()
    {
        value.fill(nan);
        errUp.fill(nan);
        errDown.fill(nan);
    }

    /// The star mirror of this fit (values with errors in the AsymErr storage form).
    StarTrackSummary toStarSummary() const;

    /// JSON form used by star packages.
    QJsonObject toJson() const;
    static TrackFitRecord fromJson(const QJsonObject& o);
};

}  // namespace astra::evolution
