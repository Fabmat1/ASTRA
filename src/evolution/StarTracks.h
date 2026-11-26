#pragma once

// Evolutionary-track fits of stars: what goes in, how a run is made, and what is
// kept.
//
//   inputs   trackInputsFromStar() reads the star's adopted spectroscopic Teff and
//            log g and the SED luminosity, radius and mass of one SED component (the
//            hotter one of a composite SED unless told otherwise).
//   run      runTrackFits() fits every star with every requested family.  It touches
//            neither the database nor Star objects, so it runs on a worker thread.
//   store    storeTrackFits() saves the fits and applies the adoption rules, on the
//            thread that owns the stars: the latest run is adopted (the first family
//            of the run's order), unless the star has a fit the user picked, which
//            stays adopted until it is unticked.
//   redraw   Samples are not stored.  redrawTrackFit() re-runs a stored fit with its
//            per-star seed and configuration, which reproduces it exactly while the
//            track file is unchanged.

#include "evolution/TrackFit.h"
#include "evolution/TrackSetLibrary.h"

#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

class DatabaseManager;
class Star;

namespace astra::evolution {

/// A value with its errors to the 15.87 / 84.13 % quantiles (NaN = unknown).
struct TrackInputValue {
    double value = std::numeric_limits<double>::quiet_NaN();
    double up = std::numeric_limits<double>::quiet_NaN();
    double down = std::numeric_limits<double>::quiet_NaN();
    bool limit = false;  ///< the value is an upper or lower limit (no error)

    bool hasValue() const;
    bool hasError() const;
    bool usable() const { return hasValue() && hasError() && !limit; }

    QJsonObject toJson() const;
    static TrackInputValue fromJson(const QJsonValue& v);
    bool sameAs(const TrackInputValue& o) const;  ///< equal within rounding
};

/// What a fit uses of a star.
struct TrackInputs {
    TrackInputValue teff;    ///< [K]  adopted spectroscopic fit (SED Teff as fallback)
    TrackInputValue logg;    ///< [cgs dex]
    TrackInputValue lum;     ///< [Lsun]  SED, component sedComponent
    TrackInputValue radius;  ///< [Rsun]  SED (shown, not fitted)
    TrackInputValue mass;    ///< [Msun]  SED (walker start; prior if enabled)

    QString teffSource;      ///< "spectrum", "sed" or "manual"
    QString loggSource;      ///< "spectrum" or "manual"
    int sedComponent = 0;    ///< 1 or 2; 0 = no SED values
    int sedComponents = 0;   ///< components of the best SED model
    double sedTeff[2] = {std::numeric_limits<double>::quiet_NaN(),
                         std::numeric_limits<double>::quiet_NaN()};
    bool sedComponentAuto = true;  ///< chosen as the hotter one, not by the user
    bool overridden = false;       ///< a value was typed in by the user
    QStringList notes;             ///< for the user ("Composite SED: ...")

    bool hasHrd() const { return teff.usable() && lum.usable(); }
    bool hasKiel() const { return teff.usable() && logg.usable(); }

    /// The snapshot stored with a fit (and on the star as trk_inputs).
    QJsonObject toJson() const;
    static TrackInputs fromJson(const QJsonObject& o);
};

/// Read the star.  sedComponent: 0 = automatic (the hotter component of a composite
/// SED, else component 1), 1 or 2 = that component.  Loads the star's photometry to
/// compare the component temperatures of a composite SED.
TrackInputs trackInputsFromStar(Star& star, int sedComponent = 0);

/// "hrd" or "kiel" for a requested plane ("auto", "hrd", "kiel").  Auto takes the
/// HRD when the star has a usable luminosity.  Empty and `whyNot` set if the
/// requested plane cannot be fitted.
QString resolvePlane(const TrackInputs& in, const QString& requested, QString* whyNot = nullptr);

/// The observation for sdage in that plane (log Teff with log L or log g; the SED
/// mass if known).  False and `whyNot` set if a value or its error is missing.
bool toObservation(const TrackInputs& in, const QString& plane, sdage::Observation& out,
                   QString* whyNot = nullptr);

/// Half the width of [log10(v - down), log10(v + up)]: an error in dex.
double dexError(double value, double up, double down);

/// Short notes on a fit, only those that apply: "Stale", "Outside tracks",
/// "Bounded: EEP (lower), age (lower)", "42 % past He exhaustion".
QStringList trackFitNotes(bool stale, int inRegion, const QString& bounded, double fracPostEhb);

/// True when the star's inputs no longer match the snapshot of its adopted fit
/// (Teff, log g, L or R and their errors changed).  Values the user typed in are
/// not compared.  False for a star without an adopted fit.
bool trackInputsStale(const Star& star);

// ── Runs ────────────────────────────────────────────────────────────────────

struct TrackStarJob {
    QString starId;
    QString label;        ///< for progress and reports
    TrackInputs inputs;
    QString plane;        ///< "auto", "hrd" or "kiel"
};

struct TrackRunRequest {
    QStringList families;                 ///< keys, in the user's order
    QHash<QString, TrackSubset> subsets;  ///< per family; missing = default subset
    TrackModelSettings model;
    std::vector<TrackStarJob> stars;
    int threads = 0;                      ///< <= 0: all hardware threads
};

/// A star or family that was not fitted, and why.
struct TrackSkip {
    QString starId;   ///< empty: the whole family
    QString label;
    QString family;
    QString reason;
};

struct TrackRunOutcome {
    QString runId;
    QStringList familyOrder;                ///< families that ran, in order
    std::vector<TrackFitRecord> fits;       ///< successful fits (ok == true)
    std::vector<TrackSkip> skipped;         ///< not fitted or failed
    bool cancelled = false;
};

/// (stars finished, stars in total, what is running)
using TrackProgress = std::function<void(int done, int total, const QString& what)>;

/// Fit every star with every family.  Thread-safe with respect to the library; no
/// database access.  The star at index i of a family uses mix_seed(model.seed, i),
/// stored with its fit.
TrackRunOutcome runTrackFits(TrackSetLibrary& library, const TrackRunRequest& request,
                             const TrackProgress& progress = {},
                             const std::atomic<bool>* cancel = nullptr);

/// Turn one sdage result into a stored record (no id, star or run yet).
TrackFitRecord recordFromResult(const sdage::FitResult& r, const PreparedTrackSet& set,
                                const TrackModelSettings& model, const TrackInputs& inputs,
                                const QString& plane, quint64 seed);

/// Save the outcome's fits; adopt per the rules when `adopt` is set (latest run,
/// first family of its order, unless the star has a manual pick).  The stars in
/// `stars` (by id) get their summary updated in memory as well.  One transaction.
bool storeTrackFits(DatabaseManager& db, const TrackRunOutcome& outcome, bool adopt,
                    const QHash<QString, std::shared_ptr<Star>>& stars, QString* error = nullptr);

/// The fit the automatic rule picks among `fits`: the newest run, the first family of
/// that run's order.  Empty if there is none.
QString automaticAdoption(const std::vector<TrackFitRecord>& fits);

/// The user ticks a fit (manual pick) or unticks the adopted one (fitId empty: the
/// automatic rule picks again).  Updates the star row and `star`.
bool adoptTrackFit(DatabaseManager& db, Star& star, const QString& fitId, QString* error = nullptr);

/// Set the adoption exactly (fit and whether it is a manual pick), e.g. to undo a
/// change.  fitId empty: no adopted fit at all.
bool setTrackAdoption(DatabaseManager& db, Star& star, const QString& fitId, bool manual, QString* error = nullptr);

/// Delete a fit; if it was the adopted one, the automatic rule picks again (a manual
/// pick that is deleted is gone).
bool deleteTrackFit(DatabaseManager& db, Star& star, const QString& fitId, QString* error = nullptr);

/// The star mirror of `fitId` from the database (cleared summary if none).
StarTrackSummary summaryForAdopted(DatabaseManager& db, const QString& starId);

// ── Redraw ─────────────────────────────────────────────────────────────────

struct TrackRedraw {
    std::shared_ptr<const PreparedTrackSet> set;
    sdage::FitResult result;    ///< with the (thinned) chain
    QStringList warnings;       ///< track file changed, medians differ, ...
};

/// Re-run a stored fit deterministically (same subset, settings, seed and inputs) to
/// get its samples back.  `maxSamples` thins the returned chain.  Null result and
/// `whyNot` set if the family is not available here.
std::optional<TrackRedraw> redrawTrackFit(TrackSetLibrary& library, const TrackFitRecord& fit,
                                          int maxSamples = 20000, QString* whyNot = nullptr,
                                          const std::atomic<bool>* cancel = nullptr);

}  // namespace astra::evolution
