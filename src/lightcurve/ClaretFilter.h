#pragma once
#include <QString>

namespace ClaretFilter {
    // Map an internal filter code (as it appears on lightcurves -
    // "g", "T", "BP", "Kp", "V", ...) to the canonical filter key
    // understood by the Claret query backend ("SDSS-g", "TESS",
    // "Johnson-V", "Kepler", ...). Returns an empty string if the
    // band is unknown.
    QString canonical(const QString& internalFilter);

    // True when the filter *is* the band it maps to, i.e. both belong to the
    // same photometric system and the Claret table is the real thing rather
    // than a stand-in. False for deliberate substitutes (Gaia, ATLAS), where
    // the mapping is only somebody's judgement of "close enough" and a
    // wavelength-ranked alternative may well beat it.
    bool isNative(const QString& internalFilter);
}
