#pragma once
#include <QString>

namespace ClaretFilter {
    // Map an internal filter code (as it appears on lightcurves —
    // "g", "T", "BP", "Kp", "V", ...) to the canonical filter key
    // understood by the Claret query backend ("SDSS-g", "TESS",
    // "Johnson-V", "Kepler", ...). Returns an empty string if the
    // band is unknown.
    QString canonical(const QString& internalFilter);
}