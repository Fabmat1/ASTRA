#pragma once

#include "models/Quantity.h"

#include <QFlags>
#include <QString>

// ─────────────────────────────────────────────────────────────────────────────
// The single place that turns a Quantity into text: LaTeX, plain text, rich
// text (for the few places that must stay HTML) and the piece-wise strings the
// custom widgets paint.
//
// Copy behaviour is user-configurable (Settings > Numbers & Copying); the
// preferences are pushed here by AppSettings so dialogs that have no
// ApplicationController at hand can still format the same way.
// ─────────────────────────────────────────────────────────────────────────────
namespace QuantityFormat {

/// The addressable pieces of a rendered quantity, in visual order.
enum Part {
    NoPart   = 0x00,
    ValuePart   = 0x01,
    ErrUpPart   = 0x02,
    ErrDownPart = 0x04,
    ErrSymPart  = 0x08, ///< the "± e" piece shown instead of the stacked pair
    UnitPart    = 0x10,
    AllErrParts = ErrUpPart | ErrDownPart | ErrSymPart,
    AllParts    = ValuePart | AllErrParts | UnitPart,
};
Q_DECLARE_FLAGS(Parts, Part)

/// How much of a quantity a copy carries.
enum class CopyContent {
    Value = 0,      ///< the number alone
    ValueError,     ///< number and uncertainty
    ValueErrorUnit, ///< number, uncertainty and unit (default)
};

/// The notation a copy uses.
enum class CopyStyle {
    Latex = 0, ///< 12.34^{+0.52}_{-0.31}\,\mathrm{km\,s^{-1}}  (default)
    Plain,     ///< 12.34 +0.52 -0.31 km/s
};

struct Prefs {
    CopyContent content = CopyContent::ValueErrorUnit;
    CopyStyle   style   = CopyStyle::Latex;
    /// Wrap LaTeX output in $...$ (off: paste into an existing math context).
    bool latexWrapMath = false;
    /// Prefix "name = " when the quantity carries a name.
    bool latexIncludeName = false;
    /// Round the error to two significant digits and match the value's
    /// decimals to it when copying. Display precision is never affected.
    bool roundOnCopy = true;
};

const Prefs &prefs();
void         setPrefs(const Prefs &p);

/// The mask of parts a CopyContent selects.
Parts partsFor(CopyContent c);

// ── Number helpers ──────────────────────────────────────────────────────
/// Fixed-point formatting that also accepts negative `decimals`, meaning
/// "round to that power of ten" (decimals = -2 gives 25000 for 24973).
QString number(double v, int decimals);

/// Decimals that show `err` to three significant digits, or roughly six
/// significant digits of `value` when there is no error. For call sites whose
/// magnitudes vary too widely for one fixed precision, such as fit result
/// tables that mix fractional radii with barycentric Julian dates.
int autoDecimals(double value, double err);

/// Decimals a copy of `q` should use: two significant digits on the smaller
/// non-zero error side, or the quantity's own precision when it has no error.
int copyDecimals(const Quantity &q);

// ── Text products ───────────────────────────────────────────────────────
/// The pieces as displayed, at the quantity's own precision. Empty strings
/// for pieces that do not apply.
struct DisplayParts {
    QString value;
    QString errUp;   ///< "+0.52" (asymmetric case only)
    QString errDown; ///< "−0.31" (asymmetric case only)
    QString errSym;  ///< "± 0.52" (symmetric case only)
    QString unit;
    bool    asymmetric = false;
};
DisplayParts displayParts(const Quantity &q);

/// Copy text for a selection of parts in the given style.
QString copyText(const Quantity &q, Parts parts, CopyStyle style);
/// Copy text honouring the user's preferences.
QString copyText(const Quantity &q);
/// Copy text for one content level, honouring the preferred style.
QString copyText(const Quantity &q, CopyContent content);

/// One-line plain rendering, used for tooltips and log lines.
QString plainText(const Quantity &q);

/// HTML rendering for the contexts that must remain rich text. Sup/sub cannot
/// stack in Qt's rich text, so this is the fallback, not the main path.
QString richText(const Quantity &q);

/// LaTeX form of a display unit ("km/s" -> "\mathrm{km\,s^{-1}}").
QString latexUnit(const QString &displayUnit);
/// True for units that attach directly to the number without a thin space.
bool    latexUnitAttaches(const QString &displayUnit);

} // namespace QuantityFormat

Q_DECLARE_OPERATORS_FOR_FLAGS(QuantityFormat::Parts)
