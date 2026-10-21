#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>
#include <vector>

#include "fitting/FitTypes.h"

class DatabaseManager;
class Instrument;
class SpectralFit;
class Spectrum;
class Star;

namespace astra::fitting {

// The spectrum-fitting core, lifted out of FitSetupWidget so that anything
// which is not a per-star widget - the mass fitter above all - can build the
// same jobs and write back the same rows instead of reimplementing them.
// Every function here is deliberately free of UI state: what the widget used
// to read off its own members is passed in.

// ────────────────────────────────────────────────────────────────────
// Grouping
// ────────────────────────────────────────────────────────────────────

// Fit regions belong to an instrument *mode*, not to an instrument: the same
// spectrograph run at two resolutions needs two configurations. Keying on the
// pair is what keeps those apart.
struct ModeKey {
    QString instrumentId;
    QString modeKey;

    bool operator==(const ModeKey& o) const
    {
        return instrumentId == o.instrumentId && modeKey == o.modeKey;
    }
};

inline size_t qHash(const ModeKey& k, size_t seed = 0)
{
    return ::qHash(k.instrumentId, seed) ^ (::qHash(k.modeKey, seed) << 1);
}

/// Resolves the instrument a spectrum was taken with, preferring the explicit
/// link and falling back to string resolution for rows written before it
/// existed. Returns the mode key through @p modeKeyOut when asked.
std::shared_ptr<Instrument> instrumentForSpectrum(
    const std::shared_ptr<Spectrum>& s,
    DatabaseManager* dbm,
    QString* modeKeyOut = nullptr);

// ────────────────────────────────────────────────────────────────────
// Configuration
// ────────────────────────────────────────────────────────────────────

/// Default fit configuration for one spectrum, in three layers: the data's own
/// wavelength extent, hardcoded fallback ignore regions and anchor bands, then
/// the instrument mode's resolution model and saved GaelFitDefaults on top.
/// @p inst and @p modeKey are already resolved by the caller.
SpectrumFitConfig makeDefaultConfig(const std::shared_ptr<Spectrum>& s,
                                    const std::shared_ptr<Instrument>& inst,
                                    const QString& modeKey);

// ────────────────────────────────────────────────────────────────────
// Seeding one fit from another
// ────────────────────────────────────────────────────────────────────

/// One value out of a fitted parameter vector: tied parameters carry a single
/// entry, untied ones carry one per spectrum, so an index past the end is the
/// tied case and reads back the shared value.
double pickFittedValue(const QVector<FittedParameter>& v, int idx);

/// Copies the starting values of @p source onto @p components. Grids, freeze
/// flags and everything else stay as @p components had them: seeding means
/// "start where the other one ended", not "run the other one again".
///
/// Only what belongs to the star travels. The continuum spline, the ignore
/// regions and the fit window are per-spectrum settings and stay put - two
/// spectra rarely share anchor points - and so does the radial velocity,
/// which is the one number that genuinely differs from epoch to epoch.
void seedComponentsFrom(QVector<StellarComponent>& components,
                        const QVector<StellarComponent>& source);

/// Starts the first component at an earlier fit's atmosphere: its Teff and
/// log g, and its log y where the component fits helium (a frozen helium
/// value belongs to the setup, e.g. a DA's "no helium"). Unknown values, and a
/// log y below -50 (that same "no helium" in the earlier fit), are left alone;
/// vsini, z and the rest stay as configured, since most setups freeze them.
void seedAtmosphereFrom(QVector<StellarComponent>& components,
                        double teff, double logg, double he);

/// The fitted values of a finished job, in the shape seedComponentsFrom()
/// wants them: @p asRun is the component list the job was run with, and
/// @p specIndex picks the spectrum whose values to read for untied
/// parameters (0 for a job covering a single spectrum).
QVector<StellarComponent> componentsFromResult(
    const SpectralFitResult& r,
    const QVector<StellarComponent>& asRun,
    int specIndex);

// ────────────────────────────────────────────────────────────────────
// Job assembly
// ────────────────────────────────────────────────────────────────────

/// Wavelength ranges a fit cannot use, from runs of at least four consecutive
/// samples whose flux is non-finite or zero or less. Archives write a flux of
/// exactly 0 where a pixel holds no measurement (ESO Phase 3 does it for every
/// pixel it flags in QUAL), and a backend that drops those samples then
/// interpolates over the hole, so the ranges are handed to it as ignore
/// regions rather than left to be silently filled in. Ranges reach to the
/// midpoint of the neighbouring usable samples and are returned in ascending
/// wavelength order.
///
/// A run of at least eight samples carrying exactly the same flux is also
/// unusable: no real measurement repeats to the last bit, so such a stretch
/// is a filled-in value (a reduction's padding or an interpolated gap).
QVector<IgnoreRegion> unusableRegions(const std::vector<double>& wl,
                                      const std::vector<double>& fl);

/// What a spectrum's fit actually sees, once its own data is taken into
/// account.
struct FitWindow {
    double wlMin = 0.0;               ///< the observation's waveCut
    double wlMax = 0.0;
    QVector<IgnoreRegion>    ignore;  ///< merged, sorted, inside the window
    QVector<ContinuumAnchor> anchors; ///< clipped to what the data can pin
    bool empty() const { return !(wlMax > wlMin); }
};

/// Builds the fit window of one spectrum from its config and its data.
///
///  1. The window is the config's wlMin..wlMax, cut to the spectrum's own
///     coverage less cfg.trimBlue / cfg.trimRed.
///  2. Ignored are the config's regions, the stretches unusableRegions()
///     finds, and - when cfg.maskArmSeams - each of @p armSeams widened by
///     cfg.seamPad.
///  3. An ignored stretch touching an end of the window moves that end
///     instead: the backend puts a continuum anchor right at each end of the
///     data, and one there with no usable pixel beside it is frozen at a
///     value estimated from the very data that was ignored.
///  4. Anchor ranges are clipped to the window, and every ignored stretch at
///     least as wide as a range's spacing is cut out of it, for the same
///     reason: an anchor with no usable pixel between its neighbours cannot
///     be fitted and would hold the continuum at a guess. A range with equal
///     ends is a single knot: kept unchanged when it lies inside the window,
///     at least 5 A from either end and outside every ignored stretch,
///     dropped otherwise.
///
/// The result is empty when nothing usable is left.
FitWindow refineFitWindow(const SpectrumFitConfig& cfg,
                          const std::vector<double>& wl,
                          const std::vector<double>& fl,
                          const std::vector<std::pair<double, double>>& armSeams);

/// Writes a spectrum's (wavelength, flux) pairs to an ASCII file in @p dir for
/// the backend to read. Returns the path, or an empty string when the spectrum
/// carries no data.
QString exportSpectrumToTemp(const std::shared_ptr<Spectrum>& s,
                             const QString& dir);

/// Assembles one joint job: one observation per enabled spectrum that has
/// continuum anchors, in the order @p spectra are given. Spectra without a
/// config, disabled ones, ones without anchors and ones whose export yields no
/// data are skipped. @p tempFilesOut receives the temporary directory holding
/// the exported spectra, which the caller owns and must clean up.
SpectralFitJob buildJob(const std::vector<std::shared_ptr<Spectrum>>& spectra,
                        const QHash<QString, SpectrumFitConfig>& configs,
                        const QVector<StellarComponent>& components,
                        const JobGlobals& globals,
                        QStringList& tempFilesOut);

// ────────────────────────────────────────────────────────────────────
// Persistence
// ────────────────────────────────────────────────────────────────────

struct PersistOutcome {
    QStringList fitIds;                        // in creation order
    QHash<QString, QString> fitIdBySpectrumId;
    int nFits = 0;
};

/// Copies component 1's teff/logg/he and their errors from an adopted fit onto
/// the star, and reports whether anything actually changed so the caller can
/// skip a pointless database write. A value that is NaN or exactly zero is not
/// a measurement and is left alone, which is the rule the single-star dialog
/// has always applied.
///
/// Deliberately does not touch the database or emit anything: the single-star
/// dialog and the mass fitter persist and notify in their own ways, and the
/// mass fitter can only run this on the GUI thread because the Star belongs
/// to it.
bool applyFitParamsToStar(const std::shared_ptr<Star>& star,
                          const std::shared_ptr<SpectralFit>& fit);

/// Turns a result into SpectralFit rows on the star's spectra and saves them.
/// @p markBestIfNone reproduces the single-star behaviour of adopting a fit
/// when its spectrum has none yet; the mass fitter passes false because it
/// decides adoption itself once every attempt for a star is in.
PersistOutcome persistFitResult(const std::shared_ptr<Star>& star,
                                const std::vector<std::shared_ptr<Spectrum>>& spectra,
                                const SpectralFitResult& result,
                                const SpectralFitJob& job,
                                DatabaseManager* dbm,
                                const QString& projectId,
                                bool markBestIfNone);

} // namespace astra::fitting
