// src/utils/spectrafetch/WavelengthConvert.cpp

#include "WavelengthConvert.h"

namespace SpecFetch {

// Refraction formula from Morton (2000, ApJS 130, 403), the IAU standard
// used by SDSS: n = 1 + 8.336624e-5 + 0.02408927/(130.1065924522 - s^2)
//                  + 0.0001599740895/(38.92568793293 - s^2)
// with s = 1e4/lambda_vac [um^-1]. Direct one-step conversion (no iteration
// needed: the formula is defined as a function of the vacuum wavelength).
double vacToAirAngstrom(double lambdaVac) {
    if (!(lambdaVac > 2000.0))
        return lambdaVac;

    const double s2 = 1.0e8 / (lambdaVac * lambdaVac);   // (1e4/lambda)^2
    const double n  = 1.0 + 8.336624212083e-5
                    + 2.408926869968e-2 / (130.1065924522 - s2)
                    + 1.599740894897e-4 / (38.92568793293 - s2);
    return lambdaVac / n;
}

void vacToAir(std::vector<double>& wavelengths) {
    for (double& wl : wavelengths)
        wl = vacToAirAngstrom(wl);
}

}   // namespace SpecFetch
