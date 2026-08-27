#ifndef STAR_H
#define STAR_H

#include <QString>
#include <QVariant>
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>
#include "AsymmetricErrors.h"
#include "RadialVelocity.h"

class Photometry;
class Spectrum;

class Star
{
public:
    Star();
    ~Star();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // Identifying fields
    QString getAlias() const { return _alias; }
    QString getSourceId() const { return _sourceId; }
    QString getTic() const { return _tic; }
    QString getJName() const { return _jname; }

    void setAlias(const QString& alias) { _alias = alias; }
    void setSourceId(const QString& id) { _sourceId = id; }
    void setTic(const QString& tic) { _tic = tic; }
    void setJName(const QString& jname) { _jname = jname; }

    // Astrometric fields
    double getRa() const { return _ra; }
    double getDec() const { return _dec; }
    double getPmra() const { return _pmra; }
    double getPmdec() const { return _pmdec; }
    double getEPmra() const { return _e_pmra; }
    double getEPmdec() const { return _e_pmdec; }
    double getPlx() const { return _plx; }
    double getEPlx() const { return _e_plx; }
    double getPmraPmdecCorr() const { return _pmra_pmdec_corr; }
    double getPlxPmdecCorr() const { return _plx_pmdec_corr; }
    double getPlxPmraCorr() const { return _plx_pmra_corr; }

    void setRa(double ra) { _ra = ra; }
    void setDec(double dec) { _dec = dec; }
    void setPmra(double pmra) { _pmra = pmra; }
    void setPmdec(double pmdec) { _pmdec = pmdec; }
    void setEPmra(double e_pmra) { _e_pmra = e_pmra; }
    void setEPmdec(double e_pmdec) { _e_pmdec = e_pmdec; }
    void setPlx(double plx) { _plx = plx; }
    void setEPlx(double e_plx) { _e_plx = e_plx; }
    void setPmraPmdecCorr(double corr) { _pmra_pmdec_corr = corr; }
    void setPlxPmdecCorr(double corr) { _plx_pmdec_corr = corr; }
    void setPlxPmraCorr(double corr) { _plx_pmra_corr = corr; }

    // Photometric fields
    double getGmag() const { return _gmag; }
    double getEGmag() const { return _e_gmag; }
    double getBp() const { return _bp; }
    double getEBp() const { return _e_bp; }
    double getRp() const { return _rp; }
    double getERp() const { return _e_rp; }
    double getBpRp() const { return _bp_rp; }

    void setGmag(double gmag) { _gmag = gmag; }
    void setEGmag(double e_gmag) { _e_gmag = e_gmag; }
    void setBp(double bp) { _bp = bp; }
    void setEBp(double e_bp) { _e_bp = e_bp; }
    void setRp(double rp) { _rp = rp; }
    void setERp(double e_rp) { _e_rp = e_rp; }
    void setBpRp(double bp_rp) { _bp_rp = bp_rp; }

    // Spectroscopic fields
    QString getSpecClass() const { return _spec_class; }
    double getTeff() const { return _teff; }
    double getETeff() const { return _e_teff; }
    double getLogg() const { return _logg; }
    double getELogg() const { return _e_logg; }
    double getHe() const { return _he; }
    double getEHe() const { return _e_he; }

    void setSpecClass(const QString& spec_class) { _spec_class = spec_class; }
    void setTeff(double teff) { _teff = teff; }
    void setETeff(double e_teff) { _e_teff = e_teff; }
    void setLogg(double logg) { _logg = logg; }
    void setELogg(double e_logg) { _e_logg = e_logg; }
    void setHe(double he) { _he = he; }
    void setEHe(double e_he) { _e_he = e_he; }

    // Optional asymmetric atmospheric errors (NaN = unset → symmetric applies)
    double getETeffUp() const   { return _e_teff_up; }
    double getETeffDown() const { return _e_teff_down; }
    void setETeffUp(double e)   { _e_teff_up = e; }
    void setETeffDown(double e) { _e_teff_down = e; }
    double getELoggUp() const   { return _e_logg_up; }
    double getELoggDown() const { return _e_logg_down; }
    void setELoggUp(double e)   { _e_logg_up = e; }
    void setELoggDown(double e) { _e_logg_down = e; }
    double getEHeUp() const   { return _e_he_up; }
    double getEHeDown() const { return _e_he_down; }
    void setEHeUp(double e)   { _e_he_up = e; }
    void setEHeDown(double e) { _e_he_down = e; }

    // ── Element abundances ──────────────────────────────────────────────────
    // One slot per element in astra::elements::all(), indexed the same way;
    // taken from component 1 of the star's best spectral fit. The value is
    // log10 of the fractional particle number (GAEL's convention). `limit` is
    // -1 when the fit pinned the abundance at the bottom of its grid axis (an
    // upper limit), +1 at the top (a lower limit) and 0 for a measurement.
    // NaN value = the star has no abundance for that element.
    double getAbundance(int elementIndex) const;
    void   setAbundance(int elementIndex, double v);
    double getEAbundance(int elementIndex) const;
    void   setEAbundance(int elementIndex, double v);
    int    getAbundanceLimit(int elementIndex) const;
    void   setAbundanceLimit(int elementIndex, int side);

    // Same, addressed by grid species name ("FE"); a no-op / NaN for an
    // element ASTRA does not carry a column for.
    double getAbundanceBySymbol(const QString& symbol) const;
    void   setAbundanceBySymbol(const QString& symbol, double v);

    /// Drop every element abundance back to "unset".
    void   clearAbundances();
    /// True when at least one element has a value.
    bool   hasAbundances() const;

    // Radial velocity fields
    double getLogP() const { return _logp; }
    double getDeltaRV() const { return _deltaRV; }
    double getEDeltaRV() const { return _e_deltaRV; }
    double getRVAvg() const { return _rv_avg; }
    double getERVAvg() const { return _e_rv_avg; }
    double getRVMed() const { return _rv_med; }
    double getERVMed() const { return _e_rv_med; }

    void setLogP(double logp) { _logp = logp; }
    void setDeltaRV(double deltaRV) { _deltaRV = deltaRV; }
    void setEDeltaRV(double e_deltaRV) { _e_deltaRV = e_deltaRV; }
    void setRVAvg(double rv_avg) { _rv_avg = rv_avg; }
    void setERVAvg(double e_rv_avg) { _e_rv_avg = e_rv_avg; }
    void setRVMed(double rv_med) { _rv_med = rv_med; }
    void setERVMed(double e_rv_med) { _e_rv_med = e_rv_med; }

    // ─── In Star.h, add to PUBLIC section ───────────────────────────────────────

    // ── Spectra counts (fast summary; updated on save) ──────────────────────
    int  getNSpectra() const { return _nSpectra; }
    void setNSpectra(int n) { _nSpectra = n; }
    int  getNFitSpectra() const { return _nFitSpectra; }
    void setNFitSpectra(int n) { _nFitSpectra = n; }

    // ── RV curve summary fields (cached from best-fit / curve) ──────────────
    double getRVTimespan() const   { return _rvTimespan; }
    void   setRVTimespan(double v) { _rvTimespan = v; }
    int    getRVNPoints() const    { return _rvNPoints; }
    void   setRVNPoints(int n)     { _rvNPoints = n; }
    double getRVK() const          { return _rvK; }
    void   setRVK(double v)        { _rvK = v; }
    double getRVEK() const         { return _rvEK; }
    void   setRVEK(double v)       { _rvEK = v; }
    double getRVEKUp() const       { return _rvEKUp; }
    void   setRVEKUp(double v)     { _rvEKUp = v; }
    double getRVEKDown() const     { return _rvEKDown; }
    void   setRVEKDown(double v)   { _rvEKDown = v; }

    // Secondary semi-amplitude of the best SB2 fit; NaN for a single-lined one.
    double getRVK2() const          { return _rvK2; }
    void   setRVK2(double v)        { _rvK2 = v; }
    double getRVEK2() const         { return _rvEK2; }
    void   setRVEK2(double v)       { _rvEK2 = v; }
    double getRVEK2Up() const       { return _rvEK2Up; }
    void   setRVEK2Up(double v)     { _rvEK2Up = v; }
    double getRVEK2Down() const     { return _rvEK2Down; }
    void   setRVEK2Down(double v)   { _rvEK2Down = v; }
    double getRVPeriod() const     { return _rvPeriod; }
    void   setRVPeriod(double v)   { _rvPeriod = v; }
    double getRVEPeriod() const    { return _rvEPeriod; }
    void   setRVEPeriod(double v)  { _rvEPeriod = v; }
    double getRVEPeriodUp() const     { return _rvEPeriodUp; }
    void   setRVEPeriodUp(double v)   { _rvEPeriodUp = v; }
    double getRVEPeriodDown() const   { return _rvEPeriodDown; }
    void   setRVEPeriodDown(double v) { _rvEPeriodDown = v; }
    double getRVGamma() const      { return _rvGamma; }
    void   setRVGamma(double v)    { _rvGamma = v; }
    double getRVEGamma() const     { return _rvEGamma; }
    void   setRVEGamma(double v)   { _rvEGamma = v; }
    double getRVEGammaUp() const     { return _rvEGammaUp; }
    void   setRVEGammaUp(double v)   { _rvEGammaUp = v; }
    double getRVEGammaDown() const   { return _rvEGammaDown; }
    void   setRVEGammaDown(double v) { _rvEGammaDown = v; }
    double getRVEcc() const        { return _rvEcc; }
    void   setRVEcc(double v)      { _rvEcc = v; }
    double getRVPhi() const        { return _rvPhi; }
    void   setRVPhi(double v)      { _rvPhi = v; }
    double getRVT0() const         { return _rvT0; }
    void   setRVT0(double v)       { _rvT0 = v; }
    double getRVChi2() const       { return _rvChi2; }
    void   setRVChi2(double v)     { _rvChi2 = v; }
    double getRVRms() const        { return _rvRms; }
    void   setRVRms(double v)      { _rvRms = v; }

    // ── SED parameters ──────────────────────────────────────────────────────
    double getSedMass1() const       { return _sedMass1; }
    void   setSedMass1(double v)     { _sedMass1 = v; }
    double getSedEMass1() const      { return _sedEMass1; }
    void   setSedEMass1(double v)    { _sedEMass1 = v; }
    double getSedRadius1() const     { return _sedRadius1; }
    void   setSedRadius1(double v)   { _sedRadius1 = v; }
    double getSedERadius1() const    { return _sedERadius1; }
    void   setSedERadius1(double v)  { _sedERadius1 = v; }
    double getSedLum1() const        { return _sedLum1; }
    void   setSedLum1(double v)      { _sedLum1 = v; }
    double getSedELum1() const       { return _sedELum1; }
    void   setSedELum1(double v)     { _sedELum1 = v; }
    double getSedMass2() const       { return _sedMass2; }
    void   setSedMass2(double v)     { _sedMass2 = v; }
    double getSedEMass2() const      { return _sedEMass2; }
    void   setSedEMass2(double v)    { _sedEMass2 = v; }
    double getSedRadius2() const     { return _sedRadius2; }
    void   setSedRadius2(double v)   { _sedRadius2 = v; }
    double getSedERadius2() const    { return _sedERadius2; }
    void   setSedERadius2(double v)  { _sedERadius2 = v; }
    double getSedLum2() const        { return _sedLum2; }
    void   setSedLum2(double v)      { _sedLum2 = v; }
    double getSedELum2() const       { return _sedELum2; }
    void   setSedELum2(double v)     { _sedELum2 = v; }

    // Asymmetric SED errors (NaN = unset → symmetric error applies)
    double getSedEMass1Up() const      { return _sedEMass1Up; }
    void   setSedEMass1Up(double v)    { _sedEMass1Up = v; }
    double getSedEMass1Down() const    { return _sedEMass1Down; }
    void   setSedEMass1Down(double v)  { _sedEMass1Down = v; }
    double getSedERadius1Up() const    { return _sedERadius1Up; }
    void   setSedERadius1Up(double v)  { _sedERadius1Up = v; }
    double getSedERadius1Down() const  { return _sedERadius1Down; }
    void   setSedERadius1Down(double v){ _sedERadius1Down = v; }
    double getSedELum1Up() const       { return _sedELum1Up; }
    void   setSedELum1Up(double v)     { _sedELum1Up = v; }
    double getSedELum1Down() const     { return _sedELum1Down; }
    void   setSedELum1Down(double v)   { _sedELum1Down = v; }
    double getSedEMass2Up() const      { return _sedEMass2Up; }
    void   setSedEMass2Up(double v)    { _sedEMass2Up = v; }
    double getSedEMass2Down() const    { return _sedEMass2Down; }
    void   setSedEMass2Down(double v)  { _sedEMass2Down = v; }
    double getSedERadius2Up() const    { return _sedERadius2Up; }
    void   setSedERadius2Up(double v)  { _sedERadius2Up = v; }
    double getSedERadius2Down() const  { return _sedERadius2Down; }
    void   setSedERadius2Down(double v){ _sedERadius2Down = v; }
    double getSedELum2Up() const       { return _sedELum2Up; }
    void   setSedELum2Up(double v)     { _sedELum2Up = v; }
    double getSedELum2Down() const     { return _sedELum2Down; }
    void   setSedELum2Down(double v)   { _sedELum2Down = v; }

    // ── Companion mass ──────────────────────────────────────────────────────
    double getCompMassMin() const { return _compMassMin; }
    void   setCompMassMin(double v) { _compMassMin = v; }
    double getECompMassMin() const { return _eCompMassMin; }
    void   setECompMassMin(double v) { _eCompMassMin = v; }
    double getECompMassMinUp() const     { return _eCompMassMinUp; }
    void   setECompMassMinUp(double v)   { _eCompMassMinUp = v; }
    double getECompMassMinDown() const   { return _eCompMassMinDown; }
    void   setECompMassMinDown(double v) { _eCompMassMinDown = v; }
    double getCompMassTrue() const { return _compMassTrue; }
    void   setCompMassTrue(double v) { _compMassTrue = v; }
    double getECompMassTrue() const { return _eCompMassTrue; }
    void   setECompMassTrue(double v) { _eCompMassTrue = v; }
    double getECompMassTrueUp() const     { return _eCompMassTrueUp; }
    void   setECompMassTrueUp(double v)   { _eCompMassTrueUp = v; }
    double getECompMassTrueDown() const   { return _eCompMassTrueDown; }
    void   setECompMassTrueDown(double v) { _eCompMassTrueDown = v; }

    // ── Photometric light-curve parameters ──────────────────────────────────
    double getPhotPeriod() const     { return _photPeriod; }
    void   setPhotPeriod(double v)   { _photPeriod = v; }
    double getPhotEPeriod() const    { return _photEPeriod; }
    void   setPhotEPeriod(double v)  { _photEPeriod = v; }
    double getPhotEPeriodUp() const     { return _photEPeriodUp; }
    void   setPhotEPeriodUp(double v)   { _photEPeriodUp = v; }
    double getPhotEPeriodDown() const   { return _photEPeriodDown; }
    void   setPhotEPeriodDown(double v) { _photEPeriodDown = v; }
    double getPhotIncl() const       { return _photIncl; }
    void   setPhotIncl(double v)     { _photIncl = v; }
    double getPhotEIncl() const      { return _photEIncl; }
    void   setPhotEIncl(double v)    { _photEIncl = v; }
    double getPhotEInclUp() const     { return _photEInclUp; }
    void   setPhotEInclUp(double v)   { _photEInclUp = v; }
    double getPhotEInclDown() const   { return _photEInclDown; }
    void   setPhotEInclDown(double v) { _photEInclDown = v; }
    double getPhotQ() const          { return _photQ; }
    void   setPhotQ(double v)        { _photQ = v; }
    double getPhotEQ() const         { return _photEQ; }
    void   setPhotEQ(double v)       { _photEQ = v; }
    double getPhotEQUp() const     { return _photEQUp; }
    void   setPhotEQUp(double v)   { _photEQUp = v; }
    double getPhotEQDown() const   { return _photEQDown; }
    void   setPhotEQDown(double v) { _photEQDown = v; }

    // ── Galactic kinematics ─────────────────────────────────────────────────
    // Velocities: heliocentric UVW [km/s], right-handed, U positive toward
    // the Galactic center (no LSR/solar-motion correction applied).
    // Positions: galactocentric cartesian XYZ [kpc].
    // Asymmetric errors follow the AsymErr convention (NaN = unset →
    // symmetric applies).
    double getGalU() const        { return _galU; }
    void   setGalU(double v)      { _galU = v; }
    double getGalEU() const       { return _galEU; }
    void   setGalEU(double v)     { _galEU = v; }
    double getGalEUUp() const     { return _galEUUp; }
    void   setGalEUUp(double v)   { _galEUUp = v; }
    double getGalEUDown() const   { return _galEUDown; }
    void   setGalEUDown(double v) { _galEUDown = v; }
    double getGalV() const        { return _galV; }
    void   setGalV(double v)      { _galV = v; }
    double getGalEV() const       { return _galEV; }
    void   setGalEV(double v)     { _galEV = v; }
    double getGalEVUp() const     { return _galEVUp; }
    void   setGalEVUp(double v)   { _galEVUp = v; }
    double getGalEVDown() const   { return _galEVDown; }
    void   setGalEVDown(double v) { _galEVDown = v; }
    double getGalW() const        { return _galW; }
    void   setGalW(double v)      { _galW = v; }
    double getGalEW() const       { return _galEW; }
    void   setGalEW(double v)     { _galEW = v; }
    double getGalEWUp() const     { return _galEWUp; }
    void   setGalEWUp(double v)   { _galEWUp = v; }
    double getGalEWDown() const   { return _galEWDown; }
    void   setGalEWDown(double v) { _galEWDown = v; }

    double getGalX() const        { return _galX; }
    void   setGalX(double v)      { _galX = v; }
    double getGalEX() const       { return _galEX; }
    void   setGalEX(double v)     { _galEX = v; }
    double getGalEXUp() const     { return _galEXUp; }
    void   setGalEXUp(double v)   { _galEXUp = v; }
    double getGalEXDown() const   { return _galEXDown; }
    void   setGalEXDown(double v) { _galEXDown = v; }
    double getGalY() const        { return _galY; }
    void   setGalY(double v)      { _galY = v; }
    double getGalEY() const       { return _galEY; }
    void   setGalEY(double v)     { _galEY = v; }
    double getGalEYUp() const     { return _galEYUp; }
    void   setGalEYUp(double v)   { _galEYUp = v; }
    double getGalEYDown() const   { return _galEYDown; }
    void   setGalEYDown(double v) { _galEYDown = v; }
    double getGalZ() const        { return _galZ; }
    void   setGalZ(double v)      { _galZ = v; }
    double getGalEZ() const       { return _galEZ; }
    void   setGalEZ(double v)     { _galEZ = v; }
    double getGalEZUp() const     { return _galEZUp; }
    void   setGalEZUp(double v)   { _galEZUp = v; }
    double getGalEZDown() const   { return _galEZDown; }
    void   setGalEZDown(double v) { _galEZDown = v; }

    // Population membership probabilities (0–1) with symmetric errors.
    double getGalPThin() const       { return _galPThin; }
    void   setGalPThin(double v)     { _galPThin = v; }
    double getGalEPThin() const      { return _galEPThin; }
    void   setGalEPThin(double v)    { _galEPThin = v; }
    double getGalPThick() const      { return _galPThick; }
    void   setGalPThick(double v)    { _galPThick = v; }
    double getGalEPThick() const     { return _galEPThick; }
    void   setGalEPThick(double v)   { _galEPThick = v; }
    double getGalPHalo() const       { return _galPHalo; }
    void   setGalPHalo(double v)     { _galPHalo = v; }
    double getGalEPHalo() const      { return _galEPHalo; }
    void   setGalEPHalo(double v)    { _galEPHalo = v; }

    // Orbit parameters: z angular momentum J_z [kpc km/s] (positive =
    // prograde) and orbital eccentricity, from the MC orbit integration.
    double getGalJz() const        { return _galJz; }
    void   setGalJz(double v)      { _galJz = v; }
    double getGalEJz() const       { return _galEJz; }
    void   setGalEJz(double v)     { _galEJz = v; }
    double getGalEJzUp() const     { return _galEJzUp; }
    void   setGalEJzUp(double v)   { _galEJzUp = v; }
    double getGalEJzDown() const   { return _galEJzDown; }
    void   setGalEJzDown(double v) { _galEJzDown = v; }
    double getGalEcc() const        { return _galEcc; }
    void   setGalEcc(double v)      { _galEcc = v; }
    double getGalEEcc() const       { return _galEEcc; }
    void   setGalEEcc(double v)     { _galEEcc = v; }
    double getGalEEccUp() const     { return _galEEccUp; }
    void   setGalEEccUp(double v)   { _galEEccUp = v; }
    double getGalEEccDown() const   { return _galEEccDown; }
    void   setGalEEccDown(double v) { _galEEccDown = v; }

    // ── Dataset availability flags ──────────────────────────────────────────
    bool getHasTess() const          { return _hasTess; }
    void setHasTess(bool v)          { _hasTess = v; }
    bool getHasGaia() const          { return _hasGaia; }
    void setHasGaia(bool v)          { _hasGaia = v; }
    bool getHasZtf() const           { return _hasZtf; }
    void setHasZtf(bool v)           { _hasZtf = v; }
    bool getHasAtlas() const         { return _hasAtlas; }
    void setHasAtlas(bool v)         { _hasAtlas = v; }
    bool getHasBlackgem() const      { return _hasBlackgem; }
    void setHasBlackgem(bool v)      { _hasBlackgem = v; }

    // ── TESS crowding metric (mean CROWDSAP across used apertures) ──────────
    double getTessCrowdsap() const         { return _tessCrowdsap; }
    void   setTessCrowdsap(double v)       { _tessCrowdsap = v; }

    // Periodogram peaks (LC periods) collected in the Lightcurve Fetch dialog,
    // stored verbatim as JSON. Carried on the model so a full star upsert
    // preserves the column instead of nulling it (it has no dedicated binding
    // otherwise).
    QString getPhotPeaksJson() const           { return _photPeaksJson; }
    void    setPhotPeaksJson(const QString& j) { _photPeaksJson = j; }

    // Metadata
    const std::vector<QString>& getBibcodes() const { return _bibcodes; }
    void setBibcodes(const std::vector<QString>& bibcodes) { _bibcodes = bibcodes; }
    void addBibcode(const QString& bibcode) { _bibcodes.push_back(bibcode); }

    // Photometry and Spectroscopy
    std::shared_ptr<Photometry> getPhotometry() const { return _photometry; }
    void setPhotometry(std::shared_ptr<Photometry> photometry);

    // Lazy loading support
    bool hasPhotometryLoaded() const { return _photometryLoaded; }
    bool hasSpectraLoaded() const { return _spectraLoaded; }
    
    void setPhotometryLoader(std::function<std::shared_ptr<Photometry>(const QString&)> loader) {
        _photometryLoader = loader;
    }
    void setSpectraLoader(std::function<std::vector<std::shared_ptr<Spectrum>>(const QString&)> loader) {
        _spectraLoader = loader;
    }
    void setRVLoader(std::function<std::shared_ptr<RadialVelocityCurve>(const QString&)> loader) {
        _RVLoader = loader;
    }

    std::shared_ptr<Photometry> getPhotometry();  
    std::vector<std::shared_ptr<Spectrum>> getSpectra();  

    void addSpectrum(std::shared_ptr<Spectrum> spectrum);
    void removeSpectrum(const QString& spectrumId);
    void setSpectra(const std::vector<std::shared_ptr<Spectrum>>& spectra);


    // Fast field access using function pointers
    using FieldGetter = std::function<QVariant(const Star*)>;
    static const std::unordered_map<QString, FieldGetter>& getFieldMap();
    
    // Generic field access for table display
    QVariant getFieldValue(const QString& fieldName) const;

    std::shared_ptr<RadialVelocityCurve> getRVCurve();
    // Curve already in memory, without triggering the lazy DB load. Lets bulk
    // callers decide for themselves whether a load is worth it.
    std::shared_ptr<RadialVelocityCurve> rvCurveIfLoaded() const { return _rvCurve; }
    void setRVCurve(std::shared_ptr<RadialVelocityCurve> curve);
    void updateRVMetricsFromCurve();
    // In Star.h, public section:
    static bool isSet(double v) { return !std::isnan(v); }

    using SummaryPersistCallback = std::function<void()>;
    // Compute all summary metrics from already-loaded child objects
    // Does NOT trigger lazy loading - safe to call for large datasets
    void computeSummaryMetrics(const SummaryPersistCallback& onChanged = nullptr);
    
    // Compute summary from child objects, triggering lazy load if needed
    // Only call for individual stars, NOT in bulk
    void computeSummaryMetricsFull(const SummaryPersistCallback& onChanged = nullptr);
    void updateDatasetAvailability();

    void markSummaryDirty();

    // Persist the current summary state to the database immediately, bypassing
    // the change-detection in computeSummaryMetrics(). Used for derived values
    // (e.g. companion masses) that are computed outside the metrics pipeline and
    // mutated directly on the Star before persisting.
    void persistSummary() { if (_summaryPersistCb) _summaryPersistCb(); }

    void setSummaryPersistCallback(SummaryPersistCallback cb)
        { _summaryPersistCb = std::move(cb); }
    using SummaryChangedCallback = std::function<void()>;
    void setSummaryChangedCallback(SummaryChangedCallback cb)
        { _summaryChangedCb = std::move(cb); }
        
    void tryAttachRVCurve();
    void ensureRVCurveSynced();

    using RVCurveFactory =
        std::function<std::shared_ptr<RadialVelocityCurve>(const QString& starId)>;
    void setRVCurveFactory(RVCurveFactory f) { _RVCurveFactory = std::move(f); }

private:
    // Targeted recomputation
    void recomputeRVMetrics();
    void recomputeSpectraMetrics();
    void recomputePhotometryMetrics();

    // Identifying fields
    QString _id;
    QString _alias;
    QString _sourceId;
    QString _tic;
    QString _jname;

    // Astrometric fields
    double _ra;
    double _dec;
    double _pmra;
    double _pmdec;
    double _e_pmra;
    double _e_pmdec;
    double _plx;
    double _e_plx;
    double _pmra_pmdec_corr;
    double _plx_pmdec_corr;
    double _plx_pmra_corr;

    // Photometric fields (Gaia)
    double _gmag;
    double _e_gmag;
    double _bp;
    double _e_bp;
    double _rp;
    double _e_rp;
    double _bp_rp;

    // Spectroscopic fields
    QString _spec_class;
    double _teff;
    double _e_teff;
    double _logg;
    double _e_logg;
    double _he;
    double _e_he;

    // Asymmetric atmospheric errors (NaN = unset → symmetric applies)
    double _e_teff_up   = AsymErr::unset;
    double _e_teff_down = AsymErr::unset;
    double _e_logg_up   = AsymErr::unset;
    double _e_logg_down = AsymErr::unset;
    double _e_he_up     = AsymErr::unset;
    double _e_he_down   = AsymErr::unset;

    // Element abundances, parallel to astra::elements::all(). Sized lazily by
    // ensureAbundanceStorage() so that a Star costs nothing extra until one is
    // actually set; NaN = unset.
    mutable std::vector<double> _abundance;
    mutable std::vector<double> _e_abundance;
    mutable std::vector<int>    _abundanceLimit;
    void ensureAbundanceStorage() const;

    // Radial velocity fields
    double _logp;
    double _deltaRV;
    double _e_deltaRV;
    double _rv_avg;
    double _e_rv_avg;
    double _rv_med;
    double _e_rv_med;

    // Metadata
    std::vector<QString> _bibcodes;

    // Associated data
    std::shared_ptr<Photometry> _photometry;
    std::vector<std::shared_ptr<Spectrum>> _spectra;
    std::shared_ptr<RadialVelocityCurve> _rvCurve;

    // Lazy loading state
    bool _photometryLoaded = false;
    bool _spectraLoaded = false;
    bool _RVLoaded = false;
    std::function<std::shared_ptr<Photometry>(const QString&)> _photometryLoader;
    std::function<std::vector<std::shared_ptr<Spectrum>>(const QString&)> _spectraLoader;
    std::function<std::shared_ptr<RadialVelocityCurve>(const QString&)> _RVLoader;
    RVCurveFactory _RVCurveFactory;

    // Spectra counts
    int _nSpectra = 0;
    int _nFitSpectra = 0;
    bool _rvAttached = false;

    SummaryPersistCallback _summaryPersistCb;
    SummaryChangedCallback _summaryChangedCb;

    RadialVelocityCurve::ListenerToken _rvChangeToken = RadialVelocityCurve::kInvalidToken;

    // RV curve summary
    double _rvTimespan = std::numeric_limits<double>::quiet_NaN();
    int    _rvNPoints  = 0;
    double _rvK        = std::numeric_limits<double>::quiet_NaN();
    double _rvEK       = std::numeric_limits<double>::quiet_NaN();
    double _rvEKUp     = AsymErr::unset;
    double _rvEKDown   = AsymErr::unset;
    double _rvK2       = AsymErr::unset;   // NaN = single-lined best fit
    double _rvEK2      = AsymErr::unset;
    double _rvEK2Up    = AsymErr::unset;
    double _rvEK2Down  = AsymErr::unset;
    double _rvPeriod   = std::numeric_limits<double>::quiet_NaN();
    double _rvEPeriod  = std::numeric_limits<double>::quiet_NaN();
    double _rvEPeriodUp   = AsymErr::unset;
    double _rvEPeriodDown = AsymErr::unset;
    double _rvGamma    = std::numeric_limits<double>::quiet_NaN();
    double _rvEGamma   = std::numeric_limits<double>::quiet_NaN();
    double _rvEGammaUp   = AsymErr::unset;
    double _rvEGammaDown = AsymErr::unset;
    double _rvEcc      = std::numeric_limits<double>::quiet_NaN();
    double _rvPhi      = std::numeric_limits<double>::quiet_NaN();
    double _rvT0       = std::numeric_limits<double>::quiet_NaN();
    double _rvChi2     = std::numeric_limits<double>::quiet_NaN();
    double _rvRms      = std::numeric_limits<double>::quiet_NaN();

    // SED parameters
    double _sedMass1    = std::numeric_limits<double>::quiet_NaN();
    double _sedEMass1   = std::numeric_limits<double>::quiet_NaN();
    double _sedRadius1  = std::numeric_limits<double>::quiet_NaN();
    double _sedERadius1 = std::numeric_limits<double>::quiet_NaN();
    double _sedLum1     = std::numeric_limits<double>::quiet_NaN();
    double _sedELum1    = std::numeric_limits<double>::quiet_NaN();
    double _sedMass2    = std::numeric_limits<double>::quiet_NaN();
    double _sedEMass2   = std::numeric_limits<double>::quiet_NaN();
    double _sedRadius2  = std::numeric_limits<double>::quiet_NaN();
    double _sedERadius2 = std::numeric_limits<double>::quiet_NaN();
    double _sedLum2     = std::numeric_limits<double>::quiet_NaN();
    double _sedELum2    = std::numeric_limits<double>::quiet_NaN();

    double _sedEMass1Up     = AsymErr::unset;
    double _sedEMass1Down   = AsymErr::unset;
    double _sedERadius1Up   = AsymErr::unset;
    double _sedERadius1Down = AsymErr::unset;
    double _sedELum1Up      = AsymErr::unset;
    double _sedELum1Down    = AsymErr::unset;
    double _sedEMass2Up     = AsymErr::unset;
    double _sedEMass2Down   = AsymErr::unset;
    double _sedERadius2Up   = AsymErr::unset;
    double _sedERadius2Down = AsymErr::unset;
    double _sedELum2Up      = AsymErr::unset;
    double _sedELum2Down    = AsymErr::unset;

    // Companion mass
    double _compMassMin   = std::numeric_limits<double>::quiet_NaN();
    double _eCompMassMin  = std::numeric_limits<double>::quiet_NaN();
    double _eCompMassMinUp   = AsymErr::unset;
    double _eCompMassMinDown = AsymErr::unset;
    double _compMassTrue  = std::numeric_limits<double>::quiet_NaN();
    double _eCompMassTrue = std::numeric_limits<double>::quiet_NaN();
    double _eCompMassTrueUp   = AsymErr::unset;
    double _eCompMassTrueDown = AsymErr::unset;

    // Photometric light-curve
    double _photPeriod  = std::numeric_limits<double>::quiet_NaN();
    double _photEPeriod = std::numeric_limits<double>::quiet_NaN();
    double _photEPeriodUp   = AsymErr::unset;
    double _photEPeriodDown = AsymErr::unset;
    double _photIncl    = std::numeric_limits<double>::quiet_NaN();
    double _photEIncl   = std::numeric_limits<double>::quiet_NaN();
    double _photEInclUp   = AsymErr::unset;
    double _photEInclDown = AsymErr::unset;
    double _photQ       = std::numeric_limits<double>::quiet_NaN();
    double _photEQ      = std::numeric_limits<double>::quiet_NaN();
    double _photEQUp    = AsymErr::unset;
    double _photEQDown  = AsymErr::unset;

    // Galactic kinematics (heliocentric UVW, galactocentric XYZ; NaN = unset)
    double _galU  = std::numeric_limits<double>::quiet_NaN();
    double _galEU = std::numeric_limits<double>::quiet_NaN();
    double _galEUUp   = AsymErr::unset;
    double _galEUDown = AsymErr::unset;
    double _galV  = std::numeric_limits<double>::quiet_NaN();
    double _galEV = std::numeric_limits<double>::quiet_NaN();
    double _galEVUp   = AsymErr::unset;
    double _galEVDown = AsymErr::unset;
    double _galW  = std::numeric_limits<double>::quiet_NaN();
    double _galEW = std::numeric_limits<double>::quiet_NaN();
    double _galEWUp   = AsymErr::unset;
    double _galEWDown = AsymErr::unset;
    double _galX  = std::numeric_limits<double>::quiet_NaN();
    double _galEX = std::numeric_limits<double>::quiet_NaN();
    double _galEXUp   = AsymErr::unset;
    double _galEXDown = AsymErr::unset;
    double _galY  = std::numeric_limits<double>::quiet_NaN();
    double _galEY = std::numeric_limits<double>::quiet_NaN();
    double _galEYUp   = AsymErr::unset;
    double _galEYDown = AsymErr::unset;
    double _galZ  = std::numeric_limits<double>::quiet_NaN();
    double _galEZ = std::numeric_limits<double>::quiet_NaN();
    double _galEZUp   = AsymErr::unset;
    double _galEZDown = AsymErr::unset;
    double _galPThin   = std::numeric_limits<double>::quiet_NaN();
    double _galEPThin  = std::numeric_limits<double>::quiet_NaN();
    double _galPThick  = std::numeric_limits<double>::quiet_NaN();
    double _galEPThick = std::numeric_limits<double>::quiet_NaN();
    double _galPHalo   = std::numeric_limits<double>::quiet_NaN();
    double _galEPHalo  = std::numeric_limits<double>::quiet_NaN();
    // Orbit parameters: J_z [kpc km/s] (positive = prograde) and
    // eccentricity from the MC orbit integration
    double _galJz  = std::numeric_limits<double>::quiet_NaN();
    double _galEJz = std::numeric_limits<double>::quiet_NaN();
    double _galEJzUp   = AsymErr::unset;
    double _galEJzDown = AsymErr::unset;
    double _galEcc  = std::numeric_limits<double>::quiet_NaN();
    double _galEEcc = std::numeric_limits<double>::quiet_NaN();
    double _galEEccUp   = AsymErr::unset;
    double _galEEccDown = AsymErr::unset;

    // Dataset availability
    bool _hasTess     = false;
    bool _hasGaia     = false;
    bool _hasZtf      = false;
    bool _hasAtlas    = false;
    bool _hasBlackgem = false;

    // TESS crowding metric (set from lightcurvequery's tess_crowdsap.txt)
    double _tessCrowdsap = std::numeric_limits<double>::quiet_NaN();

    // Periodogram peaks (LC periods) as JSON; see get/setPhotPeaksJson().
    QString _photPeaksJson;
};

#endif // STAR_H