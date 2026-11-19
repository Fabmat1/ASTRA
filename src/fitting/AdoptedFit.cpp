#include "fitting/AdoptedFit.h"

#include "core/Star.h"
#include "fitting/FitJobFactory.h"
#include "spectra/Spectrum.h"

#include <cmath>
#include <limits>

namespace astra::fitting {

namespace {

struct Candidate {
    std::shared_ptr<SpectralFit> fit;
    std::shared_ptr<Spectrum>    spectrum;
    bool   hasChi2 = false;
    double chi2    = 0.0;       ///< reduced
    double epoch   = 0.0;       ///< MJD, +inf when the spectrum has no time
};

Candidate candidate(const std::shared_ptr<Spectrum>& s, const std::shared_ptr<SpectralFit>& f)
{
    Candidate c;
    c.fit      = f;
    c.spectrum = s;
    // A stored chi2 of zero is a fit that never recorded one (imports).
    const double rc = f->reducedChi2();
    c.hasChi2 = std::isfinite(rc) && rc > 0.0;
    c.chi2    = c.hasChi2 ? rc : 0.0;

    c.epoch = std::numeric_limits<double>::infinity();
    if (const auto mjd = s->time().mjd(); mjd && *mjd > 0.0) c.epoch = *mjd;
    else if (const auto bjd = s->time().bjd(); bjd && *bjd > 0.0) c.epoch = *bjd - 2400000.5;
    return c;
}

bool hasTeff(const SpectralFit& f)
{
    return std::isfinite(f.teff) && f.teff > 0.0;
}

// Lowest reduced chi2 first; fits without one after those; then the earliest
// spectrum; ids last, so the order never depends on how the rows were loaded.
bool before(const Candidate& a, const Candidate& b)
{
    if (a.hasChi2 != b.hasChi2) return a.hasChi2;
    if (a.hasChi2 && a.chi2 != b.chi2) return a.chi2 < b.chi2;
    if (a.epoch != b.epoch) return a.epoch < b.epoch;
    if (a.spectrum->getId() != b.spectrum->getId())
        return a.spectrum->getId() < b.spectrum->getId();
    return a.fit->getId() < b.fit->getId();
}

AdoptedChoice pick(const std::vector<Candidate>& set, bool byUser)
{
    AdoptedChoice choice;
    if (set.empty()) return choice;
    const Candidate* best = &set.front();
    for (const Candidate& c : set)
        if (before(c, *best)) best = &c;
    choice.fit          = best->fit;
    choice.spectrum     = best->spectrum;
    choice.chosenByUser = byUser;
    choice.candidates   = static_cast<int>(set.size());
    return choice;
}

}   // namespace

AdoptedChoice chooseAdoptedFit(const std::vector<std::shared_ptr<Spectrum>>& spectra,
                               bool allowFlagged)
{
    std::vector<Candidate> markedCoadd, marked, best, flaggedBest;
    for (const auto& s : spectra) {
        if (!s) continue;
        // A saved co-add only ever defines the star when the user adopted one
        // of its fits, and then it outranks every single-epoch fit: stacking
        // was done precisely to get a better atmosphere than any one epoch.
        const bool derived = s->isDerived();
        for (const auto& f : s->getSpectralFits()) {
            if (!f || !hasTeff(*f)) continue;
            const bool usable = !s->isFlagged() && !f->isFlagged;
            if (derived) {
                if (f->isAdopted && usable) markedCoadd.push_back(candidate(s, f));
                continue;
            }
            if (f->isAdopted && usable) marked.push_back(candidate(s, f));
            if (f->isBestFit) (usable ? best : flaggedBest).push_back(candidate(s, f));
        }
    }
    if (!markedCoadd.empty()) return pick(markedCoadd, true);
    if (!marked.empty()) return pick(marked, true);
    if (!best.empty()) return pick(best, false);
    return allowFlagged ? pick(flaggedBest, false) : AdoptedChoice{};
}

std::shared_ptr<SpectralFit>
adoptedSpectralFit(const std::vector<std::shared_ptr<Spectrum>>& spectra)
{
    return chooseAdoptedFit(spectra).fit;
}

std::shared_ptr<SpectralFit> adoptedSpectralFit(Star& star)
{
    return chooseAdoptedFit(star.getSpectra()).fit;
}

bool applyAdoptedFitToStar(const std::shared_ptr<Star>& star)
{
    if (!star) return false;
    const AdoptedChoice choice = chooseAdoptedFit(star->getSpectra(), true);
    if (!choice) return false;

    // applyFitParamsToStar reports "changed" whenever the fit has values;
    // compare against what the star carried so callers only write real changes.
    const double teff = star->getTeff(), logg = star->getLogg(), he = star->getHe();
    const double eTeff = star->getETeff(), eLogg = star->getELogg(), eHe = star->getEHe();
    if (!applyFitParamsToStar(star, choice.fit)) return false;
    const auto differs = [](double a, double b) {
        return !(a == b || (std::isnan(a) && std::isnan(b)));
    };
    return differs(teff, star->getTeff()) || differs(logg, star->getLogg()) ||
           differs(he, star->getHe()) || differs(eTeff, star->getETeff()) ||
           differs(eLogg, star->getELogg()) || differs(eHe, star->getEHe());
}

}   // namespace astra::fitting
