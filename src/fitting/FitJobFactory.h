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
QVector<IgnoreRegion> unusableRegions(const std::vector<double>& wl,
                                      const std::vector<double>& fl);

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
