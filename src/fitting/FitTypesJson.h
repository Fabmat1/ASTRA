#pragma once

#include <QJsonArray>
#include <QJsonObject>

#include "fitting/FitTypes.h"

// ─────────────────────────────────────────────────────────────────────────────
// JSON serialisation for the fit configuration types.
//
// The mass fitter stores whole plans - grids, initial parameters, freeze flags,
// per-mode regions, job knobs - as a single JSON blob beside the hot columns,
// exactly the way `lc_fits.config_json` stores an LCFitConfig. Nothing in the
// fitting layer could be serialised before this file existed.
//
// Two rules hold for every reader here:
//
//   * a missing key falls back to the struct's own default, never to a
//     hardcoded literal, so a plan written by an older build still loads and
//     picks up whatever the current default is for a field it never knew about;
//   * round-tripping is lossless, so re-saving a plan the user did not touch
//     produces the same configuration.
//
// Only the *input* side is covered. Results are not serialised: they live in
// `spectral_fits` rows, and the branch conditions read the denormalised
// scalars off `mass_fit_attempts` rather than a stored result blob.
// ─────────────────────────────────────────────────────────────────────────────

namespace astra::fitting {

QJsonObject toJson(const IgnoreRegion& v);
IgnoreRegion ignoreRegionFromJson(const QJsonObject& o);

QJsonObject toJson(const ContinuumAnchor& v);
ContinuumAnchor continuumAnchorFromJson(const QJsonObject& o);

QJsonObject toJson(const SpectrumFitConfig& v);
SpectrumFitConfig spectrumFitConfigFromJson(const QJsonObject& o);

QJsonObject toJson(const StellarComponent& v);
StellarComponent stellarComponentFromJson(const QJsonObject& o);

QJsonObject toJson(const IsisOptions& v);
IsisOptions isisOptionsFromJson(const QJsonObject& o);

QJsonObject toJson(const IsisInteractiveOptions& v);
IsisInteractiveOptions isisInteractiveOptionsFromJson(const QJsonObject& o);

QJsonObject toJson(const JobGlobals& v);
JobGlobals jobGlobalsFromJson(const QJsonObject& o);

QJsonObject toJson(const SpectrumFile& v);
SpectrumFile spectrumFileFromJson(const QJsonObject& o);

QJsonObject toJson(const Observation& v);
Observation observationFromJson(const QJsonObject& o);

/// Whole job, input side complete. Remote fitting persists this so a run can
/// be re-attached and harvested after ASTRA restarts.
QJsonObject toJson(const SpectralFitJob& v);
SpectralFitJob spectralFitJobFromJson(const QJsonObject& o);

// ── Small readers shared with MassFitPlan ────────────────────────────────
// Exposed because the plan model serialises the same way and there is no
// reason for it to grow a second copy of these.

namespace json {

double  readDouble(const QJsonObject& o, const char* key, double def);
int     readInt(const QJsonObject& o, const char* key, int def);
bool    readBool(const QJsonObject& o, const char* key, bool def);
QString readString(const QJsonObject& o, const char* key, const QString& def = {});
QStringList readStringList(const QJsonObject& o, const char* key,
                           const QStringList& def);

/// Writes a double that may carry the project's NaN "unset" sentinel. JSON has
/// no NaN, so an unset value becomes null and reads back as unset rather than
/// as a silent zero.
QJsonValue writeDouble(double v);
/// Counterpart of writeDouble: null, absent, or a non-finite literal all give
/// back @p def.
double readNanDouble(const QJsonObject& o, const char* key, double def);

/// Serialises a QVector of anything this header can write.
template <typename T, typename Fn>
QJsonArray toArray(const QVector<T>& items, Fn&& conv)
{
    QJsonArray a;
    for (const T& item : items) a.append(conv(item));
    return a;
}

/// Reads a QVector back. A missing or non-array key yields an empty vector,
/// which is the default for every vector field in these structs.
template <typename T, typename Fn>
QVector<T> fromArray(const QJsonObject& o, const char* key, Fn&& conv)
{
    QVector<T> out;
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isArray()) return out;
    const QJsonArray a = v.toArray();
    out.reserve(a.size());
    for (const QJsonValue& e : a) out.append(conv(e.toObject()));
    return out;
}

}   // namespace json

}   // namespace astra::fitting
