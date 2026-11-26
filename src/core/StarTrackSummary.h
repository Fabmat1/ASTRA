#pragma once

// The adopted evolutionary-track fit of a star, as the
// star row carries it in its trk_* columns, and the table of quantities those
// columns are generated from.  One table drives the star columns, the track_fits
// value columns, the loaders, the field map, the column registry and the packages,
// so a quantity is added in exactly one place.
//
// Star columns per quantity X (starKey):  trk_X, trk_e_X, trk_e_X_up, trk_e_X_down
//   (median; symmetric error, or up/down when asymmetric: AsymErr::toStorage)
// track_fits columns per quantity (fitColumn):  X, X_error_up, X_error_down
//   (median and the distances to the 15.87 / 84.13 % quantiles, as fitted)

#include <QString>
#include <QList>
#include <QPair>
#include <QStringList>

#include <array>
#include <cmath>
#include <limits>

namespace astra::tracks {

enum Qty : int {
    CoreMass = 0,
    EnvMass,
    TotalMass,
    Age,
    AgeMax,     ///< lifetime: age at the end of the track
    AgeHe0,     ///< age at core-helium exhaustion
    HeCentral,  ///< central helium mass fraction
    Eep,
    QtyCount
};

struct QtyInfo {
    const char* fitColumn;  ///< column in track_fits
    const char* starKey;    ///< trk_<starKey> on the star
    const char* label;      ///< column header / summary label
    const char* unit;       ///< display unit ("" if none)
    const char* fieldUnit;  ///< unit as StarFieldRegistry spells it
    int precision;          ///< decimals for display
    const char* description;
};

inline const QtyInfo& info(int q)
{
    static const std::array<QtyInfo, QtyCount> table = {{
        {"core_mass", "core_mass", "M_core", "M☉", "Msun", 3, "Core mass from the evolutionary tracks"},
        {"env_mass", "env_mass", "M_env", "M☉", "Msun", 4, "Hydrogen envelope mass from the evolutionary tracks"},
        {"total_mass", "mass", "M (tracks)", "M☉", "Msun", 3, "Total mass from the evolutionary tracks"},
        {"age", "age", "Age", "Myr", "Myr", 1, "Age since the start of the track (sdfit's hot-subdwarf sets: the zero-age EHB; Driebe: cooling age)"},
        {"age_max", "age_max", "Lifetime", "Myr", "Myr", 1, "Age at the end of the track"},
        {"age_he0", "age_he0", "Age at He exhaustion", "Myr", "Myr", 1, "Age at core-helium exhaustion"},
        {"helium", "he_c", "Y_c", "", "", 3, "Central helium mass fraction"},
        {"eep", "eep", "EEP", "", "", 1, "Equivalent evolutionary point along the track"},
    }};
    return table[static_cast<std::size_t>(q)];
}

/// trk_<key>, trk_e_<key>, trk_e_<key>_up, trk_e_<key>_down
inline QString starValueColumn(int q) { return QStringLiteral("trk_") + QLatin1String(info(q).starKey); }
inline QString starErrorColumn(int q) { return QStringLiteral("trk_e_") + QLatin1String(info(q).starKey); }
inline QString starErrorUpColumn(int q) { return starErrorColumn(q) + QStringLiteral("_up"); }
inline QString starErrorDownColumn(int q) { return starErrorColumn(q) + QStringLiteral("_down"); }

/// Non-quantity columns of the star block, with their SQL types.
inline const QList<QPair<QString, QString>>& starExtraColumns()
{
    static const QList<QPair<QString, QString>> cols = {
        {QStringLiteral("trk_family"), QStringLiteral("TEXT")},
        {QStringLiteral("trk_plane"), QStringLiteral("TEXT")},
        {QStringLiteral("trk_fit_id"), QStringLiteral("TEXT")},
        {QStringLiteral("trk_stale"), QStringLiteral("INTEGER DEFAULT 0")},
        {QStringLiteral("trk_in_region"), QStringLiteral("INTEGER")},
        {QStringLiteral("trk_bounded"), QStringLiteral("TEXT")},
        {QStringLiteral("trk_inputs"), QStringLiteral("TEXT")},
        {QStringLiteral("trk_frac_post_ehb"), QStringLiteral("REAL")},
    };
    return cols;
}

/// Every trk_* column of the star row, in a fixed order, with its SQL type.
inline const QList<QPair<QString, QString>>& starColumns()
{
    static const QList<QPair<QString, QString>> cols = [] {
        QList<QPair<QString, QString>> c = starExtraColumns();
        for (int q = 0; q < QtyCount; ++q) {
            c.append({starValueColumn(q), QStringLiteral("REAL")});
            c.append({starErrorColumn(q), QStringLiteral("REAL")});
            c.append({starErrorUpColumn(q), QStringLiteral("REAL")});
            c.append({starErrorDownColumn(q), QStringLiteral("REAL")});
        }
        return c;
    }();
    return cols;
}

}  // namespace astra::tracks

/// The adopted track fit as mirrored on the star.  Empty (no fitId) when the star
/// has no adopted fit.
struct StarTrackSummary {
    static constexpr double nan = std::numeric_limits<double>::quiet_NaN();

    QString family;     ///< "han", "nicolas", ...
    QString plane;      ///< "hrd" (Teff + L) or "kiel" (Teff + log g)
    QString fitId;      ///< the adopted track_fits row
    bool stale = false; ///< the star's Teff / log g / L / R changed since the fit
    int inRegion = -1;  ///< 1 inside the tracks, 0 outside (extrapolated), -1 unknown
    QString bounded;    ///< posteriors running into a bound, e.g. "core_masses:upper"
    QString inputs;     ///< JSON snapshot of the inputs the fit used
    double fracPostEhb = nan;

    std::array<double, astra::tracks::QtyCount> value{};
    std::array<double, astra::tracks::QtyCount> err{};
    std::array<double, astra::tracks::QtyCount> errUp{};
    std::array<double, astra::tracks::QtyCount> errDown{};

    StarTrackSummary() { clearValues(); }
    void clearValues()
    {
        value.fill(nan);
        err.fill(nan);
        errUp.fill(nan);
        errDown.fill(nan);
        fracPostEhb = nan;
    }
    bool hasFit() const { return !fitId.isEmpty(); }
    void clear() { *this = StarTrackSummary(); }
};
