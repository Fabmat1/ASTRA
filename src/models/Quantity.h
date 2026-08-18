#pragma once

#include "AsymmetricErrors.h"

#include <QMetaType>
#include <QString>

#include <cmath>
#include <limits>

// ─────────────────────────────────────────────────────────────────────────────
// A measured value with its 1σ uncertainty, its unit and the precision it is
// meant to be shown at. This is the single currency passed to the display
// widgets (QuantityLabel / QuantityDelegate) and to the copy formatter
// (QuantityFormat), so every panel renders and copies parameters the same way.
//
// Errors follow the project-wide convention from AsymmetricErrors.h: `error`
// is the legacy symmetric 1σ, `errorUp` / `errorDown` are the optional
// positive distances to the 84.1th / 15.9th posterior percentiles, NaN when
// unset (then the symmetric error applies in both directions). A zero side is
// a valid value, not a defect.
// ─────────────────────────────────────────────────────────────────────────────
struct Quantity {
    double value     = std::numeric_limits<double>::quiet_NaN();
    double error     = std::numeric_limits<double>::quiet_NaN();
    double errorUp   = AsymErr::unset;
    double errorDown = AsymErr::unset;

    /// Unit in display form, e.g. "km/s", "M☉", "°". Empty for dimensionless.
    QString unit;
    /// Fixed decimals the display uses. Copy may re-round (see QuantityFormat).
    int precision = 3;
    /// Optional parameter name for name-carrying copy formats, e.g. "M_1".
    QString name;

    Quantity() = default;
    Quantity(double v, double err, int prec, QString u = {},
             double up = AsymErr::unset, double down = AsymErr::unset,
             QString nm = {})
        : value(v), error(err), errorUp(up), errorDown(down), unit(std::move(u)),
          precision(prec), name(std::move(nm)) {}

    bool hasValue() const { return std::isfinite(value); }

    /// Effective upper / lower error (asymmetric side when set, else symmetric).
    double up() const   { return AsymErr::upOr(errorUp, error); }
    double down() const { return AsymErr::downOr(errorDown, error); }

    /// True when any usable error is attached at all.
    bool hasError() const {
        const double u = up(), d = down();
        return (std::isfinite(u) && u > 0.0) || (std::isfinite(d) && d > 0.0);
    }

    /// True when the two sides genuinely differ and must be stacked. An
    /// explicit interval whose sides happen to be equal renders as "± e".
    bool isAsymmetric() const {
        if (!AsymErr::hasAsymmetric(errorUp, errorDown))
            return false;
        const double u = up(), d = down();
        if (!std::isfinite(u) || !std::isfinite(d))
            return false;
        if (u <= 0.0 && d <= 0.0)
            return false;
        return u != d;
    }

    /// The single error to quote when the interval collapses to one number.
    double symmetricError() const {
        return AsymErr::symmetrized(error, errorUp, errorDown);
    }
};

Q_DECLARE_METATYPE(Quantity)
