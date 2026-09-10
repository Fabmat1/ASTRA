#pragma once
#include <QString>

namespace FilterWavelength {
    // Effective wavelength in nm for common photometric bands.
    // Returns 0.0 if unknown.
    double lookupNm(const QString& filter);
}