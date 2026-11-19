#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// How a saved co-add was made.
//
// A co-add saved from the spectra dialog becomes a spectrum row of its own
// (Spectrum::isCoaddProduct) so that it can be fitted, shown and adopted like
// any other spectrum. Its recipe rides along in spectra.derived_meta as JSON:
// which spectra went in, how they were stacked, and what the stack learnt
// that its fits need back (the final resolution and the ranges a member's fit
// ignored). "Restack" rebuilds the data from exactly this.
// ─────────────────────────────────────────────────────────────────────────────

#include <QDateTime>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>
#include <optional>
#include <vector>

class Spectrum;

namespace astra::spectra {

/// What is stacked.
///  - Normalized: each fit's rebinned flux divided by its continuum.
///  - Flux:       each fit's rebinned flux, brought to a common level.
///  - Raw:        the observed spectra, brought to a common level; needs no fit.
enum class CoaddStack { Normalized = 0, Flux = 1, Raw = 2 };

QString    stackKey(CoaddStack s);                  ///< "normalized" / "flux" / "raw"
CoaddStack stackFromKey(const QString& key);        ///< unknown ⇒ Normalized
QString    stackDisplayName(CoaddStack s);          ///< "Normalized" / ...

/// The kind recorded in spectra.derived_kind for a co-add.
inline constexpr const char* kCoaddDerivedKind = "coadd";

struct CoaddRecipe {
    QString     name;                   ///< user's label; may be empty
    QStringList members;                ///< spectrum ids, in stacking order
    CoaddStack  stack = CoaddStack::Normalized;
    bool        restFrame = true;
    double      pixelsPerResolutionElement = 3.0;
    QString     fitSource;              ///< model grid; empty ⇒ each best fit

    double      targetResolution = 0.0; ///< R of the stack; 0 ⇒ unknown
    /// Ranges (Å, in the co-add's own frame) some member's fit ignored. A fit
    /// of the co-add leaves them out too.
    QVector<QPair<double, double>> ignoredRanges;

    QDateTime   created;                ///< UTC, when the data was (re)stacked

    QString toJson() const;
    static std::optional<CoaddRecipe> fromJson(const QString& json);
};

/// The recipe of a saved co-add; nullopt for any other spectrum.
std::optional<CoaddRecipe> coaddRecipeOf(const Spectrum& s);

/// Short title of a saved co-add for tabs and lists: "Co-add <name>", or
/// "Co-add (n spectra)" when it has no name.
QString coaddTitle(const Spectrum& s);

/// Runs of `ignored != 0` as wavelength ranges reaching to the midpoints of
/// the neighbouring samples. Sizes must match; empty in, empty out.
QVector<QPair<double, double>> maskToRanges(const std::vector<double>&  wl,
                                            const std::vector<uint8_t>& ignored);

/// The reverse: 1 for every sample inside one of @p ranges.
std::vector<uint8_t> rangesToMask(const std::vector<double>& wl,
                                  const QVector<QPair<double, double>>& ranges);

} // namespace astra::spectra
