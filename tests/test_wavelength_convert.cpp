// ─────────────────────────────────────────────────────────────────────────────
// Vacuum-to-air conversion test (Morton 2000 / IAU standard, as used by SDSS).
//
// Reference values from the SDSS wavelength documentation and Morton (2000):
// a wrong conversion here silently shifts every fetched SDSS/LAMOST/APOGEE
// spectrum by ~1-2 Angstrom, which is a catastrophic RV error.
// ─────────────────────────────────────────────────────────────────────────────
#include "utils/spectrafetch/WavelengthConvert.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void checkNear(double got, double want, double tol, const std::string& what) {
    const bool ok = std::abs(got - want) <= tol;
    std::printf("%s  %s - got %.4f, want %.4f (tol %.4f)\n",
                ok ? "[ ok ]" : "[FAIL]", what.c_str(), got, want, tol);
    if (!ok) ++gFailures;
}

}   // namespace

int main() {
    using SpecFetch::vacToAirAngstrom;

    // Below the 2000 A cutoff the conversion must be the identity.
    checkNear(vacToAirAngstrom(1215.67), 1215.67, 1e-9, "Lyman alpha untouched");
    checkNear(vacToAirAngstrom(1999.99), 1999.99, 1e-9, "just below cutoff");

    // n(5000 A) = 1.000279 -> air wavelength about 1.39 A shorter.
    checkNear(vacToAirAngstrom(5000.0), 5000.0 / 1.0002793, 0.01,
              "5000 A refraction");

    // H-beta: 4862.683 vac -> 4861.325 air (standard line lists).
    checkNear(vacToAirAngstrom(4862.683), 4861.325, 0.01, "H-beta");

    // H-alpha: 6564.614 vac -> 6562.801 air.
    checkNear(vacToAirAngstrom(6564.614), 6562.801, 0.01, "H-alpha");

    // Ca II K: 3934.777 vac -> 3933.663 air.
    checkNear(vacToAirAngstrom(3934.777), 3933.663, 0.01, "Ca II K");

    // Array conversion applies element-wise.
    std::vector<double> wl = {1500.0, 6564.614};
    SpecFetch::vacToAir(wl);
    checkNear(wl[0], 1500.0, 1e-9, "array UV untouched");
    checkNear(wl[1], 6562.801, 0.01, "array H-alpha");

    std::printf("%s (%d failure(s))\n", gFailures ? "FAILED" : "PASSED",
                gFailures);
    return gFailures ? 1 : 0;
}
