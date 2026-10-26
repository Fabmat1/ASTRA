#include "fitting/AdoptedFit.h"

#include "core/Star.h"
#include "spectra/Spectrum.h"

#include <cmath>

namespace astra::fitting {

namespace {

bool same(double a, double b)
{
    return std::isfinite(a) && std::isfinite(b) &&
           std::abs(a - b) <= 1e-6 * std::max(std::abs(a), std::abs(b)) + 1e-9;
}

}   // namespace

std::shared_ptr<SpectralFit>
adoptedSpectralFit(const std::vector<std::shared_ptr<Spectrum>>& spectra,
                   double starTeff, double starLogg)
{
    std::shared_ptr<SpectralFit> first, lowestChi2;
    double bestChi2 = INFINITY;
    for (const auto& s : spectra) {
        if (!s || s->isFlagged()) continue;
        auto f = s->getBestFit();
        if (!f || f->isFlagged) continue;
        if (same(f->teff, starTeff) && same(f->logg, starLogg)) return f;
        if (!first) first = f;
        const double rc = f->reducedChi2();
        if (std::isfinite(rc) && rc < bestChi2) {
            bestChi2   = rc;
            lowestChi2 = f;
        }
    }
    return lowestChi2 ? lowestChi2 : first;
}

std::shared_ptr<SpectralFit> adoptedSpectralFit(Star& star)
{
    return adoptedSpectralFit(star.getSpectra(), star.getTeff(), star.getLogg());
}

}   // namespace astra::fitting
