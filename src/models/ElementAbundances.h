#ifndef ELEMENT_ABUNDANCES_H
#define ELEMENT_ABUNDANCES_H

#include <QString>
#include <QVector>

// ─────────────────────────────────────────────────────────────────────────────
// The elements a metal-line model grid can resolve.
//
// GAEL names an abundance by the grid's own species name -- the sub-directory
// of the grid the element's ratio spectra live in, e.g. "FE" -- and reports it
// as log10 of the fractional particle number relative to *all* particles,
// which is the convention ISIS's cN_<ELEMENT> parameters use.  Every grid
// shipped so far (Feros_3 … Feros_6) resolves the same 24, so that list is
// fixed here: it has to be, because each element gets its own column in the
// stars table.  An element a grid resolves that is not in this list is carried
// on the fit but not on the star; see SpectralFit::abundances.
//
// `solarLogN` is the solar reference in the same convention, from the table at
// the bottom of the grids' own abundances.dat ("values are base-10 logarithmic
// fractional particle numbers relative to all particles"), so [X/H] is simply
// value - solarLogN.
// ─────────────────────────────────────────────────────────────────────────────
namespace astra::elements {

struct ElementInfo {
    QString symbol;      ///< GAEL/grid species name, upper case: "FE"
    QString display;     ///< how it is shown to a human: "Fe"
    int     z;           ///< atomic number
    double  atomicMass;  ///< standard atomic weight [u]
    double  solarLogN;   ///< solar log10 n(X)/n_total
    QString dbSuffix;    ///< database column suffix, lower case: "fe"
};

/// All supported elements, ordered by atomic number (which for this set is the
/// same order as by atomic mass). The index into this vector is the index the
/// per-star abundance arrays use, so it is part of the on-disk format: append
/// only, never reorder.
const QVector<ElementInfo>& all();

/// Number of supported elements (24).
int count();

/// Look-up by grid species name, case-insensitive; nullptr if unsupported.
const ElementInfo* bySymbol(const QString& symbol);

/// Index into all() for a grid species name, or -1 if unsupported.
int indexOfSymbol(const QString& symbol);

/// Index into all() for a database column suffix ("fe"), or -1.
int indexOfDbSuffix(const QString& suffix);

/// Solar-relative [X/H] for an abundance in GAEL's convention, or NaN if the
/// element is unsupported or the value is unset.
double toSolarRelative(int elementIndex, double value);

/// ISIS's convention for "this element is not in the model at all": an
/// abundance of 10 or more. Such a value is never a measurement.
bool isSwitchedOff(double value);

} // namespace astra::elements

#endif // ELEMENT_ABUNDANCES_H
