#pragma once

#include "fitting/FitTypes.h"

#include <QVector>

class AppSettings;

namespace astra::fitting {

/// The user's own starting point for a new spectrum fit window: the stellar
/// components (grid, parameters, freeze switches, abundances). Empty means
/// the built-in single default component.
///
/// Per-spectrum settings (fit range, ignore regions, continuum anchors,
/// resolution) are not part of this: they depend on the spectrograph and
/// live with the instrument mode (InstrumentMode's GaelFitDefaults).
struct UserFitDefaults {
    QVector<StellarComponent> components;

    bool isEmpty() const { return components.isEmpty(); }

    static UserFitDefaults load(const AppSettings& settings);
    void save(AppSettings& settings) const;
};

} // namespace astra::fitting
