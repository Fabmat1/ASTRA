#include "Star.h"
#include "ElementAbundances.h"
#include "Photometry.h"
#include "Spectrum.h"
#include "RadialVelocity.h"
#include "../utils/Logger.h"
#include <algorithm>
#include <QVariant>
#include <limits>
#include <cmath>


namespace {

bool nanSafeEqual(double a, double b) {
    if (std::isnan(a) && std::isnan(b)) return true;
    return a == b;
}

std::vector<double> captureSummaryValues(const Star& s) {
    std::vector<double> v = {
        double(s.getRVNPoints()), s.getRVTimespan(), s.getRVAvg(), s.getRVMed(),
        s.getERVAvg(), s.getERVMed(),
        s.getLogP(), s.getDeltaRV(),
        s.getRVK(), s.getRVEK(), s.getRVPeriod(), s.getRVEPeriod(),
        s.getRVGamma(), s.getRVEGamma(),
        s.getRVEKUp(), s.getRVEKDown(),
        s.getRVEPeriodUp(), s.getRVEPeriodDown(),
        s.getRVEGammaUp(), s.getRVEGammaDown(),
        s.getRVEcc(), s.getRVPhi(), s.getRVT0(), s.getRVChi2(), s.getRVRms(),
        double(s.getNSpectra()), double(s.getNFitSpectra()),
        s.getTeff(), s.getETeff(), s.getLogg(), s.getELogg(), s.getHe(), s.getEHe(),
        s.getETeffUp(), s.getETeffDown(), s.getELoggUp(), s.getELoggDown(),
        s.getEHeUp(), s.getEHeDown(),
        double(s.getHasTess()), double(s.getHasGaia()), double(s.getHasZtf()),
        double(s.getHasAtlas()), double(s.getHasBlackgem()),
        s.getSedMass1(), s.getSedEMass1(), s.getSedRadius1(), s.getSedERadius1(),
        s.getSedLum1(), s.getSedELum1(),
        s.getSedMass2(), s.getSedEMass2(), s.getSedRadius2(), s.getSedERadius2(),
        s.getSedLum2(), s.getSedELum2(),
    };

    // Abundances only join the capture once the star has any: reading them
    // otherwise would allocate the lazy per-element storage for every star on
    // every summary recomputation. A star gaining or losing abundances changes
    // the vector's size, which summaryChanged() already treats as a change.
    if (s.hasAbundances()) {
        for (int i = 0; i < astra::elements::count(); ++i) {
            v.push_back(s.getAbundance(i));
            v.push_back(s.getEAbundance(i));
            v.push_back(double(s.getAbundanceLimit(i)));
        }
    }
    return v;
}

bool summaryChanged(const std::vector<double>& before, const std::vector<double>& after) {
    if (before.size() != after.size()) return true;
    for (size_t i = 0; i < before.size(); ++i)
        if (!nanSafeEqual(before[i], after[i])) return true;
    return false;
}

} // anonymous namespace

Star::Star()
    : _ra(std::numeric_limits<double>::quiet_NaN())
    , _dec(std::numeric_limits<double>::quiet_NaN())
    , _pmra(std::numeric_limits<double>::quiet_NaN())
    , _pmdec(std::numeric_limits<double>::quiet_NaN())
    , _e_pmra(std::numeric_limits<double>::quiet_NaN())
    , _e_pmdec(std::numeric_limits<double>::quiet_NaN())
    , _plx(std::numeric_limits<double>::quiet_NaN())
    , _e_plx(std::numeric_limits<double>::quiet_NaN())
    , _pmra_pmdec_corr(std::numeric_limits<double>::quiet_NaN())
    , _plx_pmdec_corr(std::numeric_limits<double>::quiet_NaN())
    , _plx_pmra_corr(std::numeric_limits<double>::quiet_NaN())
    , _gmag(std::numeric_limits<double>::quiet_NaN())
    , _e_gmag(std::numeric_limits<double>::quiet_NaN())
    , _bp(std::numeric_limits<double>::quiet_NaN())
    , _e_bp(std::numeric_limits<double>::quiet_NaN())
    , _rp(std::numeric_limits<double>::quiet_NaN())
    , _e_rp(std::numeric_limits<double>::quiet_NaN())
    , _bp_rp(std::numeric_limits<double>::quiet_NaN())
    , _teff(std::numeric_limits<double>::quiet_NaN())
    , _e_teff(std::numeric_limits<double>::quiet_NaN())
    , _logg(std::numeric_limits<double>::quiet_NaN())
    , _e_logg(std::numeric_limits<double>::quiet_NaN())
    , _he(std::numeric_limits<double>::quiet_NaN())
    , _e_he(std::numeric_limits<double>::quiet_NaN())
    , _logp(std::numeric_limits<double>::quiet_NaN())
    , _deltaRV(std::numeric_limits<double>::quiet_NaN())
    , _e_deltaRV(std::numeric_limits<double>::quiet_NaN())
    , _rv_avg(std::numeric_limits<double>::quiet_NaN())
    , _e_rv_avg(std::numeric_limits<double>::quiet_NaN())
    , _rv_med(std::numeric_limits<double>::quiet_NaN())
    , _e_rv_med(std::numeric_limits<double>::quiet_NaN())
    , _photometryLoaded(false)
    , _spectraLoaded(false)
    , _RVLoaded(false)
{
}

Star::~Star()
{
    if (_rvCurve && _rvChangeToken != RadialVelocityCurve::kInvalidToken) {
        _rvCurve->removeChangeListener(_rvChangeToken);
    }
}

// ── Element abundances ──────────────────────────────────────────────────────
// The three parallel arrays are allocated on first touch, so the many stars
// that never get a metal-line fit keep costing nothing. clearAbundances()
// drops them again, which is why every accessor has to size them itself.
void Star::ensureAbundanceStorage() const
{
    const size_t n = static_cast<size_t>(astra::elements::count());
    if (_abundance.size() == n) return;

    constexpr double unset = std::numeric_limits<double>::quiet_NaN();
    _abundance.assign(n, unset);
    _e_abundance.assign(n, unset);
    _abundanceLimit.assign(n, 0);
}

double Star::getAbundance(int elementIndex) const
{
    if (elementIndex < 0 || elementIndex >= astra::elements::count())
        return std::numeric_limits<double>::quiet_NaN();
    ensureAbundanceStorage();
    return _abundance[static_cast<size_t>(elementIndex)];
}

void Star::setAbundance(int elementIndex, double v)
{
    if (elementIndex < 0 || elementIndex >= astra::elements::count()) return;
    ensureAbundanceStorage();
    _abundance[static_cast<size_t>(elementIndex)] = v;
}

double Star::getEAbundance(int elementIndex) const
{
    if (elementIndex < 0 || elementIndex >= astra::elements::count())
        return std::numeric_limits<double>::quiet_NaN();
    ensureAbundanceStorage();
    return _e_abundance[static_cast<size_t>(elementIndex)];
}

void Star::setEAbundance(int elementIndex, double v)
{
    if (elementIndex < 0 || elementIndex >= astra::elements::count()) return;
    ensureAbundanceStorage();
    _e_abundance[static_cast<size_t>(elementIndex)] = v;
}

int Star::getAbundanceLimit(int elementIndex) const
{
    if (elementIndex < 0 || elementIndex >= astra::elements::count())
        return 0;
    ensureAbundanceStorage();
    return _abundanceLimit[static_cast<size_t>(elementIndex)];
}

void Star::setAbundanceLimit(int elementIndex, int side)
{
    if (elementIndex < 0 || elementIndex >= astra::elements::count()) return;
    ensureAbundanceStorage();
    _abundanceLimit[static_cast<size_t>(elementIndex)] = side;
}

double Star::getAbundanceBySymbol(const QString& symbol) const
{
    return getAbundance(astra::elements::indexOfSymbol(symbol));
}

void Star::setAbundanceBySymbol(const QString& symbol, double v)
{
    setAbundance(astra::elements::indexOfSymbol(symbol), v);
}

void Star::clearAbundances()
{
    _abundance.clear();
    _e_abundance.clear();
    _abundanceLimit.clear();
}

bool Star::hasAbundances() const
{
    for (double v : _abundance)
        if (!std::isnan(v)) return true;
    return false;
}

// Helper: return QVariant for a double, blank string if NaN
static inline QVariant dblVar(double v)
{
    return std::isnan(v) ? QVariant(QString()) : QVariant(v);
}

static inline QVariant intVar(int v)
{
    return QVariant(v);
}

const std::unordered_map<QString, Star::FieldGetter>& Star::getFieldMap()
{
    static const std::unordered_map<QString, FieldGetter> map = [] {
    std::unordered_map<QString, FieldGetter> m = {
        // ── Identification ──────────────────────────────────────────────────
        { "alias",        [](const Star* s) { return QVariant(s->getAlias());    } },
        { "source_id",    [](const Star* s) { return QVariant(s->getSourceId()); } },
        { "tic",          [](const Star* s) { return QVariant(s->getTic());      } },
        { "jname",        [](const Star* s) { return QVariant(s->getJName());    } },

        // ── Astrometry ──────────────────────────────────────────────────────
        { "ra",           [](const Star* s) { return dblVar(s->getRa());   } },
        { "dec",          [](const Star* s) { return dblVar(s->getDec());  } },
        { "plx",          [](const Star* s) { return dblVar(s->getPlx()); } },
        { "e_plx",        [](const Star* s) { return dblVar(s->getEPlx()); } },
        { "pmra",         [](const Star* s) { return dblVar(s->getPmra()); } },
        { "pmdec",        [](const Star* s) { return dblVar(s->getPmdec()); } },
        { "e_pmra",       [](const Star* s) { return dblVar(s->getEPmra()); } },
        { "e_pmdec",      [](const Star* s) { return dblVar(s->getEPmdec()); } },
        { "pmra_pmdec_corr", [](const Star* s) { return dblVar(s->getPmraPmdecCorr()); } },
        { "plx_pmra_corr",   [](const Star* s) { return dblVar(s->getPlxPmraCorr()); } },
        { "plx_pmdec_corr",  [](const Star* s) { return dblVar(s->getPlxPmdecCorr()); } },

        // ── Gaia Photometry ─────────────────────────────────────────────────
        { "gmag",         [](const Star* s) { return dblVar(s->getGmag()); } },
        { "e_gmag",       [](const Star* s) { return dblVar(s->getEGmag()); } },
        { "bp",           [](const Star* s) { return dblVar(s->getBp()); } },
        { "e_bp",         [](const Star* s) { return dblVar(s->getEBp()); } },
        { "rp",           [](const Star* s) { return dblVar(s->getRp()); } },
        { "e_rp",         [](const Star* s) { return dblVar(s->getERp()); } },
        { "bp_rp",        [](const Star* s) { return dblVar(s->getBpRp()); } },

        // ── Atmospheric ─────────────────────────────────────────────────────
        { "spec_class",   [](const Star* s) { return QVariant(s->getSpecClass()); } },
        { "teff",         [](const Star* s) { return dblVar(s->getTeff()); } },
        { "e_teff",       [](const Star* s) { return dblVar(s->getETeff()); } },
        { "e_teff_up",    [](const Star* s) { return dblVar(s->getETeffUp()); } },
        { "e_teff_down",  [](const Star* s) { return dblVar(s->getETeffDown()); } },
        { "logg",         [](const Star* s) { return dblVar(s->getLogg()); } },
        { "e_logg",       [](const Star* s) { return dblVar(s->getELogg()); } },
        { "e_logg_up",    [](const Star* s) { return dblVar(s->getELoggUp()); } },
        { "e_logg_down",  [](const Star* s) { return dblVar(s->getELoggDown()); } },
        { "he",           [](const Star* s) { return dblVar(s->getHe()); } },
        { "e_he",         [](const Star* s) { return dblVar(s->getEHe()); } },
        { "e_he_up",      [](const Star* s) { return dblVar(s->getEHeUp()); } },
        { "e_he_down",    [](const Star* s) { return dblVar(s->getEHeDown()); } },
        { "n_spectra",    [](const Star* s) { return intVar(s->getNSpectra()); } },
        { "n_fit_spectra",[](const Star* s) { return intVar(s->getNFitSpectra()); } },

        // ── Radial Velocity (summary) ───────────────────────────────────────
        { "logp",         [](const Star* s) { return dblVar(s->getLogP()); } },
        { "delta_rv",     [](const Star* s) { return dblVar(s->getDeltaRV()); } },
        { "e_delta_rv",   [](const Star* s) { return dblVar(s->getEDeltaRV()); } },
        { "rv_avg",       [](const Star* s) { return dblVar(s->getRVAvg()); } },
        { "e_rv_avg",     [](const Star* s) { return dblVar(s->getERVAvg()); } },
        { "rv_med",       [](const Star* s) { return dblVar(s->getRVMed()); } },
        { "e_rv_med",     [](const Star* s) { return dblVar(s->getERVMed()); } },
        { "rv_timespan",  [](const Star* s) { return dblVar(s->getRVTimespan()); } },
        { "rv_npoints",   [](const Star* s) { return intVar(s->getRVNPoints()); } },
        { "rv_k",         [](const Star* s) { return dblVar(s->getRVK()); } },
        { "rv_e_k",       [](const Star* s) { return dblVar(s->getRVEK()); } },
        { "rv_e_k_up",    [](const Star* s) { return dblVar(s->getRVEKUp()); } },
        { "rv_e_k_down",  [](const Star* s) { return dblVar(s->getRVEKDown()); } },
        { "rv_k2",        [](const Star* s) { return dblVar(s->getRVK2()); } },
        { "rv_e_k2",      [](const Star* s) { return dblVar(s->getRVEK2()); } },
        { "rv_e_k2_up",   [](const Star* s) { return dblVar(s->getRVEK2Up()); } },
        { "rv_e_k2_down", [](const Star* s) { return dblVar(s->getRVEK2Down()); } },
        // Mass ratio q = M2/M1 = K1/K2, derived rather than stored.
        { "rv_q",         [](const Star* s) {
              const double k = s->getRVK(), k2 = s->getRVK2();
              return dblVar((std::isfinite(k2) && k2 > 0.0 && std::isfinite(k))
                                ? k / k2
                                : std::numeric_limits<double>::quiet_NaN());
          } },
        { "rv_period",    [](const Star* s) { return dblVar(s->getRVPeriod()); } },
        { "rv_e_period",  [](const Star* s) { return dblVar(s->getRVEPeriod()); } },
        { "rv_e_period_up",   [](const Star* s) { return dblVar(s->getRVEPeriodUp()); } },
        { "rv_e_period_down", [](const Star* s) { return dblVar(s->getRVEPeriodDown()); } },
        { "rv_gamma",     [](const Star* s) { return dblVar(s->getRVGamma()); } },
        { "rv_e_gamma",   [](const Star* s) { return dblVar(s->getRVEGamma()); } },
        { "rv_e_gamma_up",   [](const Star* s) { return dblVar(s->getRVEGammaUp()); } },
        { "rv_e_gamma_down", [](const Star* s) { return dblVar(s->getRVEGammaDown()); } },
        { "rv_ecc",       [](const Star* s) { return dblVar(s->getRVEcc()); } },
        { "rv_phi",       [](const Star* s) { return dblVar(s->getRVPhi()); } },
        { "rv_t0",        [](const Star* s) { return dblVar(s->getRVT0()); } },
        { "rv_chi2",      [](const Star* s) { return dblVar(s->getRVChi2()); } },
        { "rv_rms",       [](const Star* s) { return dblVar(s->getRVRms()); } },

        // ── SED ─────────────────────────────────────────────────────────────
        { "sed_mass1",      [](const Star* s) { return dblVar(s->getSedMass1()); } },
        { "sed_e_mass1",    [](const Star* s) { return dblVar(s->getSedEMass1()); } },
        { "sed_radius1",    [](const Star* s) { return dblVar(s->getSedRadius1()); } },
        { "sed_e_radius1",  [](const Star* s) { return dblVar(s->getSedERadius1()); } },
        { "sed_lum1",       [](const Star* s) { return dblVar(s->getSedLum1()); } },
        { "sed_e_lum1",     [](const Star* s) { return dblVar(s->getSedELum1()); } },
        { "sed_mass2",      [](const Star* s) { return dblVar(s->getSedMass2()); } },
        { "sed_e_mass2",    [](const Star* s) { return dblVar(s->getSedEMass2()); } },
        { "sed_radius2",    [](const Star* s) { return dblVar(s->getSedRadius2()); } },
        { "sed_e_radius2",  [](const Star* s) { return dblVar(s->getSedERadius2()); } },
        { "sed_lum2",       [](const Star* s) { return dblVar(s->getSedLum2()); } },
        { "sed_e_lum2",     [](const Star* s) { return dblVar(s->getSedELum2()); } },

        // ── Photometric LC ──────────────────────────────────────────────────
        { "phot_period",    [](const Star* s) { return dblVar(s->getPhotPeriod()); } },
        { "phot_e_period",  [](const Star* s) { return dblVar(s->getPhotEPeriod()); } },
        { "phot_e_period_up",   [](const Star* s) { return dblVar(s->getPhotEPeriodUp()); } },
        { "phot_e_period_down", [](const Star* s) { return dblVar(s->getPhotEPeriodDown()); } },
        { "phot_incl",      [](const Star* s) { return dblVar(s->getPhotIncl()); } },
        { "phot_e_incl",    [](const Star* s) { return dblVar(s->getPhotEIncl()); } },
        { "phot_e_incl_up",   [](const Star* s) { return dblVar(s->getPhotEInclUp()); } },
        { "phot_e_incl_down", [](const Star* s) { return dblVar(s->getPhotEInclDown()); } },
        { "phot_q",         [](const Star* s) { return dblVar(s->getPhotQ()); } },
        { "phot_e_q",       [](const Star* s) { return dblVar(s->getPhotEQ()); } },
        { "phot_e_q_up",    [](const Star* s) { return dblVar(s->getPhotEQUp()); } },
        { "phot_e_q_down",  [](const Star* s) { return dblVar(s->getPhotEQDown()); } },

        // ── Companion Mass ──────────────────────────────────────────────────
        { "comp_mass_min",   [](const Star* s) { return dblVar(s->getCompMassMin()); } },
        { "e_comp_mass_min", [](const Star* s) { return dblVar(s->getECompMassMin()); } },
        { "e_comp_mass_min_up",   [](const Star* s) { return dblVar(s->getECompMassMinUp()); } },
        { "e_comp_mass_min_down", [](const Star* s) { return dblVar(s->getECompMassMinDown()); } },
        { "comp_mass_true",  [](const Star* s) { return dblVar(s->getCompMassTrue()); } },
        { "e_comp_mass_true",[](const Star* s) { return dblVar(s->getECompMassTrue()); } },
        { "e_comp_mass_true_up",   [](const Star* s) { return dblVar(s->getECompMassTrueUp()); } },
        { "e_comp_mass_true_down", [](const Star* s) { return dblVar(s->getECompMassTrueDown()); } },

        // ── Galactic kinematics ─────────────────────────────────────────────
        { "gal_u",         [](const Star* s) { return dblVar(s->getGalU()); } },
        { "gal_e_u",       [](const Star* s) { return dblVar(s->getGalEU()); } },
        { "gal_e_u_up",    [](const Star* s) { return dblVar(s->getGalEUUp()); } },
        { "gal_e_u_down",  [](const Star* s) { return dblVar(s->getGalEUDown()); } },
        { "gal_v",         [](const Star* s) { return dblVar(s->getGalV()); } },
        { "gal_e_v",       [](const Star* s) { return dblVar(s->getGalEV()); } },
        { "gal_e_v_up",    [](const Star* s) { return dblVar(s->getGalEVUp()); } },
        { "gal_e_v_down",  [](const Star* s) { return dblVar(s->getGalEVDown()); } },
        { "gal_w",         [](const Star* s) { return dblVar(s->getGalW()); } },
        { "gal_e_w",       [](const Star* s) { return dblVar(s->getGalEW()); } },
        { "gal_e_w_up",    [](const Star* s) { return dblVar(s->getGalEWUp()); } },
        { "gal_e_w_down",  [](const Star* s) { return dblVar(s->getGalEWDown()); } },
        { "gal_x",         [](const Star* s) { return dblVar(s->getGalX()); } },
        { "gal_e_x",       [](const Star* s) { return dblVar(s->getGalEX()); } },
        { "gal_e_x_up",    [](const Star* s) { return dblVar(s->getGalEXUp()); } },
        { "gal_e_x_down",  [](const Star* s) { return dblVar(s->getGalEXDown()); } },
        { "gal_y",         [](const Star* s) { return dblVar(s->getGalY()); } },
        { "gal_e_y",       [](const Star* s) { return dblVar(s->getGalEY()); } },
        { "gal_e_y_up",    [](const Star* s) { return dblVar(s->getGalEYUp()); } },
        { "gal_e_y_down",  [](const Star* s) { return dblVar(s->getGalEYDown()); } },
        { "gal_z",         [](const Star* s) { return dblVar(s->getGalZ()); } },
        { "gal_e_z",       [](const Star* s) { return dblVar(s->getGalEZ()); } },
        { "gal_e_z_up",    [](const Star* s) { return dblVar(s->getGalEZUp()); } },
        { "gal_e_z_down",  [](const Star* s) { return dblVar(s->getGalEZDown()); } },
        { "gal_p_thin",    [](const Star* s) { return dblVar(s->getGalPThin()); } },
        { "gal_e_p_thin",  [](const Star* s) { return dblVar(s->getGalEPThin()); } },
        { "gal_p_thick",   [](const Star* s) { return dblVar(s->getGalPThick()); } },
        { "gal_e_p_thick", [](const Star* s) { return dblVar(s->getGalEPThick()); } },
        { "gal_p_halo",    [](const Star* s) { return dblVar(s->getGalPHalo()); } },
        { "gal_e_p_halo",  [](const Star* s) { return dblVar(s->getGalEPHalo()); } },
        { "gal_jz",         [](const Star* s) { return dblVar(s->getGalJz()); } },
        { "gal_e_jz",       [](const Star* s) { return dblVar(s->getGalEJz()); } },
        { "gal_e_jz_up",    [](const Star* s) { return dblVar(s->getGalEJzUp()); } },
        { "gal_e_jz_down",  [](const Star* s) { return dblVar(s->getGalEJzDown()); } },
        { "gal_ecc",        [](const Star* s) { return dblVar(s->getGalEcc()); } },
        { "gal_e_ecc",      [](const Star* s) { return dblVar(s->getGalEEcc()); } },
        { "gal_e_ecc_up",   [](const Star* s) { return dblVar(s->getGalEEccUp()); } },
        { "gal_e_ecc_down", [](const Star* s) { return dblVar(s->getGalEEccDown()); } },

        // ── TESS crowding ───────────────────────────────────────────────────
        { "tess_crowdsap",  [](const Star* s) { return dblVar(s->getTessCrowdsap()); } },

        // ── Dataset availability (boolean → rendered by delegate) ───────────
        { "has_tess",       [](const Star* s) { return QVariant(s->getHasTess()); } },
        { "has_gaia",       [](const Star* s) { return QVariant(s->getHasGaia()); } },
        { "has_ztf",        [](const Star* s) { return QVariant(s->getHasZtf()); } },
        { "has_atlas",      [](const Star* s) { return QVariant(s->getHasAtlas()); } },
        { "has_blackgem",   [](const Star* s) { return QVariant(s->getHasBlackgem()); } },
    };

    // ── Abundances ──────────────────────────────────────────────────────────
    // Three fields per element (abund_fe, e_abund_fe, abund_fe_limit), built
    // from the element table so they cannot drift from the database columns.
    const auto& elements = astra::elements::all();
    for (int i = 0; i < elements.size(); ++i) {
        const QString& suffix = elements[i].dbSuffix;
        m.emplace("abund_" + suffix,
                  [i](const Star* s) { return dblVar(s->getAbundance(i)); });
        m.emplace("e_abund_" + suffix,
                  [i](const Star* s) { return dblVar(s->getEAbundance(i)); });
        m.emplace("abund_" + suffix + "_limit",
                  [i](const Star* s) { return intVar(s->getAbundanceLimit(i)); });
    }
    return m;
    }();
    return map;
}

QVariant Star::getFieldValue(const QString& fieldName) const
{
    const auto& map = getFieldMap();
    auto it = map.find(fieldName);
    if (it != map.end())
        return it->second(this);
    return QVariant();
}

void Star::updateRVMetricsFromCurve()
{
    if (!_rvCurve || _rvCurve->getNumPoints() == 0) return;

    // Update star's existing RV fields from curve statistics
    _rv_avg = _rvCurve->getWeightedMeanRV();
    _e_rv_avg = _rvCurve->getWeightedStdDevRV();
    _rv_med = _rvCurve->getMedianRV();
    _e_rv_med = _rvCurve->getStdDevRV();

    // Delta RV = amplitude (max - min)
    _deltaRV = _rvCurve->getRVAmplitude();
    _e_deltaRV = 0.0; // Could be computed from errors if needed

    // Compute and store logP (chi-squared variability test)
    double logP = _rvCurve->computeLogP();
    _rvCurve->setLogP(logP);
    _logp = logP;
}

std::shared_ptr<Photometry> Star::getPhotometry()
{
    if (!_photometryLoaded && _photometryLoader && !_id.isEmpty()) {
        _photometry = _photometryLoader(_id);
        _photometryLoaded = true;
    }
    return _photometry;
}

std::vector<std::shared_ptr<Spectrum>> Star::getSpectra()
{
    if (!_spectraLoaded && _spectraLoader && !_id.isEmpty()) {
        _spectra = _spectraLoader(_id);
        _spectraLoaded = true;
    }
    tryAttachRVCurve();
    return _spectra;
}

std::shared_ptr<RadialVelocityCurve> Star::getRVCurve()
{
    if (!_RVLoaded && _RVLoader && !_id.isEmpty()) {
        _rvCurve = _RVLoader(_id);
        _RVLoaded = true;
    }
    tryAttachRVCurve();
    return _rvCurve;
}


void Star::addSpectrum(std::shared_ptr<Spectrum> spectrum)
{
    if (!_spectraLoaded && _spectraLoader && !_id.isEmpty()) {
        _spectra = _spectraLoader(_id);
        _spectraLoaded = true;
    }
    _spectra.push_back(spectrum);
    recomputeSpectraMetrics();
    if (_rvCurve && spectrum) {
        std::vector<std::shared_ptr<Spectrum>> one{spectrum};
        _rvCurve->attachToSpectra(one);
    }
}

void Star::removeSpectrum(const QString& spectrumId)
{
    _spectra.erase(
        std::remove_if(_spectra.begin(), _spectra.end(),
            [&spectrumId](const std::shared_ptr<Spectrum>& s) {
                return s && s->getId() == spectrumId;
            }),
        _spectra.end());
    recomputeSpectraMetrics();
}


void Star::computeSummaryMetrics(const SummaryPersistCallback& onChanged)
{
    auto before = captureSummaryValues(*this);

    recomputeRVMetrics();
    recomputeSpectraMetrics();
    recomputePhotometryMetrics();

    if (summaryChanged(before, captureSummaryValues(*this))) {
        if (onChanged) onChanged();           // persist
        if (_summaryChangedCb) _summaryChangedCb();  // notify UI
    }
}

void Star::computeSummaryMetricsFull(const SummaryPersistCallback& onChanged)
{
    // Force lazy loads
    getRVCurve();
    getSpectra();
    getPhotometry();

    computeSummaryMetrics(onChanged);
}

void Star::recomputeRVMetrics()
{
    if (!_rvCurve) return;
    _rvCurve->setLogP(_rvCurve->computeLogP());

    const auto activePts = _rvCurve->getActiveRVPoints();
    _rvNPoints  = static_cast<int>(activePts.size());
    _rvTimespan = _rvCurve->getTimeSpan();
    _rv_avg     = _rvCurve->getMeanRV();
    _rv_med     = _rvCurve->getMedianRV();
    _logp       = _rvCurve->getLogP();
    _deltaRV    = _rvCurve->getRVAmplitude();

    // Uncertainties on the mean/median systemic RV. getStdDevRV() is the sample
    // scatter of the active points and is 0 for a single epoch, so fall back to
    // that lone point's own measurement error. Without this the downstream
    // galactic-orbit code (which requires a *positive* RV uncertainty) rejects
    // every star that only carries RV curve points, e.g. freshly imported ones.
    // Kept consistent with updateRVMetricsFromCurve().
    double rvErr = _rvCurve->getStdDevRV();
    if (!(std::isfinite(rvErr) && rvErr > 0.0) && activePts.size() == 1) {
        const double pe = activePts.front() ? activePts.front()->getRVError()
                                            : std::numeric_limits<double>::quiet_NaN();
        if (std::isfinite(pe) && pe > 0.0)
            rvErr = pe;
    }
    _e_rv_avg = rvErr;
    _e_rv_med = rvErr;

    // Reset fit fields first
    _rvK = 0; _rvEK = 0;
    _rvPeriod = 0; _rvEPeriod = 0;
    _rvGamma = 0; _rvEGamma = 0;
    _rvEKUp = AsymErr::unset; _rvEKDown = AsymErr::unset;
    _rvK2 = AsymErr::unset; _rvEK2 = AsymErr::unset;
    _rvEK2Up = AsymErr::unset; _rvEK2Down = AsymErr::unset;
    _rvEPeriodUp = AsymErr::unset; _rvEPeriodDown = AsymErr::unset;
    _rvEGammaUp = AsymErr::unset; _rvEGammaDown = AsymErr::unset;
    _rvEcc = 0; _rvPhi = 0; _rvT0 = 0;
    _rvChi2 = 0; _rvRms = 0;

    auto bestFit = _rvCurve->getBestFit();
    if (bestFit) {
        _rvK       = bestFit->getK();
        _rvEK      = bestFit->getKError();
        _rvEKUp    = bestFit->getKErrorUp();
        _rvEKDown  = bestFit->getKErrorDown();
        if (bestFit->hasK2()) {
            _rvK2      = bestFit->getK2();
            _rvEK2     = bestFit->getK2Error();
            _rvEK2Up   = bestFit->getK2ErrorUp();
            _rvEK2Down = bestFit->getK2ErrorDown();
        }
        _rvPeriod  = bestFit->getPeriod();
        _rvEPeriod = bestFit->getPeriodError();
        _rvEPeriodUp   = bestFit->getPeriodErrorUp();
        _rvEPeriodDown = bestFit->getPeriodErrorDown();
        _rvGamma   = bestFit->getGamma();
        _rvEGamma  = bestFit->getGammaError();
        _rvEGammaUp   = bestFit->getGammaErrorUp();
        _rvEGammaDown = bestFit->getGammaErrorDown();
        _rvEcc     = bestFit->getEccentricity();
        _rvPhi     = bestFit->getPhi();
        _rvT0      = bestFit->getT0();
        _rvChi2    = bestFit->getChi2();
        _rvRms     = bestFit->getRms();
    }
}

void Star::recomputeSpectraMetrics()
{
    _nSpectra = static_cast<int>(_spectra.size());
    _nFitSpectra = 0;

    // Reset atmospheric - will be set from best fit below
    _teff = 0; _e_teff = 0;
    _logg = 0; _e_logg = 0;
    _he = 0;   _e_he = 0;
    _e_teff_up = AsymErr::unset; _e_teff_down = AsymErr::unset;
    _e_logg_up = AsymErr::unset; _e_logg_down = AsymErr::unset;
    _e_he_up   = AsymErr::unset; _e_he_down   = AsymErr::unset;
    clearAbundances();

    for (const auto& spec : _spectra) {
        if (!spec) continue;
        auto fit = spec->getBestFit();
        if (!fit) continue;
        ++_nFitSpectra;

        // Use first valid best fit for atmospheric params
        if (_teff == 0 && fit->teff > 0) {
            _teff   = fit->teff;
            _e_teff = fit->teffError;
            _logg   = fit->logg;
            _e_logg = fit->loggError;
            _he     = fit->he;
            _e_he   = fit->heError;
            _e_teff_up   = fit->teffErrorUp;
            _e_teff_down = fit->teffErrorDown;
            _e_logg_up   = fit->loggErrorUp;
            _e_logg_down = fit->loggErrorDown;
            _e_he_up     = fit->heErrorUp;
            _e_he_down   = fit->heErrorDown;

            // The star reports component 1, so only that component's
            // abundances land on it. Elements the grid resolves but ASTRA has
            // no column for stay on the fit, and one that was switched off in
            // the model is not a measurement at all.
            for (auto it = fit->abundances.constBegin();
                 it != fit->abundances.constEnd(); ++it) {
                const int ei = astra::elements::indexOfSymbol(it.key());
                if (ei < 0 || !it->isSet() ||
                    astra::elements::isSwitchedOff(it->value))
                    continue;
                setAbundance(ei, it->value);
                setEAbundance(ei, it->error);
                setAbundanceLimit(ei, it->limitSide);
            }
        }
    }
}

void Star::recomputePhotometryMetrics()
{
    _hasTess = false;
    _hasGaia = false;
    _hasZtf = false;
    _hasAtlas = false;
    _hasBlackgem = false;
    _sedMass1 = 0; _sedEMass1 = 0;
    _sedRadius1 = 0; _sedERadius1 = 0;
    _sedLum1 = 0; _sedELum1 = 0;
    _sedMass2 = 0; _sedEMass2 = 0;
    _sedRadius2 = 0; _sedERadius2 = 0;
    _sedLum2 = 0; _sedELum2 = 0;
    _sedEMass1Up = AsymErr::unset; _sedEMass1Down = AsymErr::unset;
    _sedERadius1Up = AsymErr::unset; _sedERadius1Down = AsymErr::unset;
    _sedELum1Up = AsymErr::unset; _sedELum1Down = AsymErr::unset;
    _sedEMass2Up = AsymErr::unset; _sedEMass2Down = AsymErr::unset;
    _sedERadius2Up = AsymErr::unset; _sedERadius2Down = AsymErr::unset;
    _sedELum2Up = AsymErr::unset; _sedELum2Down = AsymErr::unset;

    if (!_photometry) return;

    // Dataset availability - uses getLightcurveSources()
    auto lcSources = _photometry->getLightcurveSources();
    LOG_INFO("Star", QString("recomputePhotometryMetrics [%1]: %2 lightcurve source(s)")
                         .arg(getId()).arg(lcSources.size()));
    for (const auto& source : lcSources) {
        QString src = source.toLower();
        LOG_INFO("Star", QString("  LC source: '%1' (%2 pts)")
                             .arg(source)
                             .arg(_photometry->getLightcurve(source).size()));
        if (src.contains("tess"))                      _hasTess = true;
        if (src.contains("gaia"))                      _hasGaia = true;
        if (src.contains("ztf"))                       _hasZtf = true;
        if (src.contains("atlas"))                     _hasAtlas = true;
        if (src.contains("blackgem") || src == "bg")   _hasBlackgem = true;
    }

    // SED best fit - components is a public member. Errors go through the
    // storage merge rule: near-symmetric up/down collapse to a single
    // symmetric error, genuinely asymmetric ones keep both sides.
    auto bestSed = _photometry->getBestSEDModel();
    if (bestSed) {
        if (bestSed->components.size() >= 1) {
            const auto& c = bestSed->components[0];
            _sedMass1    = c.mass.value;
            const auto m = AsymErr::toStorage(c.mass.errUp, c.mass.errDown);
            _sedEMass1 = m.sym; _sedEMass1Up = m.up; _sedEMass1Down = m.down;
            _sedRadius1  = c.radius.value;
            const auto r = AsymErr::toStorage(c.radius.errUp, c.radius.errDown);
            _sedERadius1 = r.sym; _sedERadius1Up = r.up; _sedERadius1Down = r.down;
            _sedLum1     = c.luminosity.value;
            const auto l = AsymErr::toStorage(c.luminosity.errUp,
                                              c.luminosity.errDown);
            _sedELum1 = l.sym; _sedELum1Up = l.up; _sedELum1Down = l.down;
        }
        if (bestSed->components.size() >= 2) {
            const auto& c = bestSed->components[1];
            _sedMass2    = c.mass.value;
            const auto m = AsymErr::toStorage(c.mass.errUp, c.mass.errDown);
            _sedEMass2 = m.sym; _sedEMass2Up = m.up; _sedEMass2Down = m.down;
            _sedRadius2  = c.radius.value;
            const auto r = AsymErr::toStorage(c.radius.errUp, c.radius.errDown);
            _sedERadius2 = r.sym; _sedERadius2Up = r.up; _sedERadius2Down = r.down;
            _sedLum2     = c.luminosity.value;
            const auto l = AsymErr::toStorage(c.luminosity.errUp,
                                              c.luminosity.errDown);
            _sedELum2 = l.sym; _sedELum2Up = l.up; _sedELum2Down = l.down;
        }
    }
}

void Star::setRVCurve(std::shared_ptr<RadialVelocityCurve> curve)
{
    // Detach from previous curve, if any.
    if (_rvCurve && _rvChangeToken != RadialVelocityCurve::kInvalidToken) {
        _rvCurve->removeChangeListener(_rvChangeToken);
    }
    _rvChangeToken = RadialVelocityCurve::kInvalidToken;
    _rvAttached    = false;

    _rvCurve = std::move(curve);

    if (_rvCurve) {
        _rvChangeToken = _rvCurve->addChangeListener(
            [this]{ markSummaryDirty(); });
        // Note: _rvAttached is set true by tryAttachRVCurve()/ensureRVCurveSynced()
        // once spectra are also available; we only track listener registration here.
    }
    recomputeRVMetrics();
}


void Star::setPhotometry(std::shared_ptr<Photometry> photometry)
{
    _photometry = photometry;
    recomputePhotometryMetrics();
}


void Star::setSpectra(const std::vector<std::shared_ptr<Spectrum>>& spectra)
{
    _spectra = spectra;
    _spectraLoaded = true;
    recomputeSpectraMetrics();

    // Replacing the spectrum objects severs the RV curve's per-spectrum
    // callbacks (they live on the old objects). Re-attach so best-fit and
    // flag changes on the new objects keep propagating to the RV points.
    if (_rvCurve)
        _rvCurve->attachToSpectra(_spectra);
}

void Star::markSummaryDirty()
{
    computeSummaryMetrics(_summaryPersistCb);
}

void Star::tryAttachRVCurve()
{
    if (_rvAttached) return;
    if (!_rvCurve || !_spectraLoaded) return;

    _rvCurve->attachToSpectra(_spectra);

    if (_rvChangeToken == RadialVelocityCurve::kInvalidToken) {
        _rvChangeToken = _rvCurve->addChangeListener(
            [this]{ markSummaryDirty(); });
    }

    _rvCurve->reconcileWithSpectra(_spectra);
    _rvAttached = true;
}


void Star::ensureRVCurveSynced()
{
    (void)getRVCurve();     // lazy-loads curve + tryAttachRVCurve
    (void)getSpectra();     // lazy-loads spectra

    if (!_rvCurve && _RVCurveFactory && !_id.isEmpty()) {
        _rvCurve       = _RVCurveFactory(_id);
        _RVLoaded      = true;
        _rvChangeToken = RadialVelocityCurve::kInvalidToken;  // new curve, no listener yet
        _rvAttached    = false;                                // force re-attach below
    }
    if (!_rvCurve) return;

    _rvCurve->attachToSpectra(_spectra);   // idempotent

    if (_rvChangeToken == RadialVelocityCurve::kInvalidToken) {
        _rvChangeToken = _rvCurve->addChangeListener(
            [this]{ markSummaryDirty(); });
    }
    _rvAttached = true;

    _rvCurve->reconcileWithSpectra(_spectra);
}