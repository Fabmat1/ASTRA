#ifndef STAR_H
#define STAR_H

#include <QString>
#include <QVariant>
#include <memory>
#include <vector>
#include <functional>
#include <unordered_map>
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
    double getRVPeriod() const     { return _rvPeriod; }
    void   setRVPeriod(double v)   { _rvPeriod = v; }
    double getRVEPeriod() const    { return _rvEPeriod; }
    void   setRVEPeriod(double v)  { _rvEPeriod = v; }
    double getRVGamma() const      { return _rvGamma; }
    void   setRVGamma(double v)    { _rvGamma = v; }
    double getRVEGamma() const     { return _rvEGamma; }
    void   setRVEGamma(double v)   { _rvEGamma = v; }
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

    // ── Photometric light-curve parameters ──────────────────────────────────
    double getPhotPeriod() const     { return _photPeriod; }
    void   setPhotPeriod(double v)   { _photPeriod = v; }
    double getPhotEPeriod() const    { return _photEPeriod; }
    void   setPhotEPeriod(double v)  { _photEPeriod = v; }
    double getPhotIncl() const       { return _photIncl; }
    void   setPhotIncl(double v)     { _photIncl = v; }
    double getPhotEIncl() const      { return _photEIncl; }
    void   setPhotEIncl(double v)    { _photEIncl = v; }
    double getPhotQ() const          { return _photQ; }
    void   setPhotQ(double v)        { _photQ = v; }
    double getPhotEQ() const         { return _photEQ; }
    void   setPhotEQ(double v)       { _photEQ = v; }

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

    // Metadata
    std::vector<QString> getBibcodes() const { return _bibcodes; }
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

    // ─── In Star.h, add to PRIVATE section ──────────────────────────────────────

    // Spectra counts
    int _nSpectra = 0;
    int _nFitSpectra = 0;

    // RV curve summary
    double _rvTimespan = std::numeric_limits<double>::quiet_NaN();
    int    _rvNPoints  = 0;
    double _rvK        = std::numeric_limits<double>::quiet_NaN();
    double _rvEK       = std::numeric_limits<double>::quiet_NaN();
    double _rvPeriod   = std::numeric_limits<double>::quiet_NaN();
    double _rvEPeriod  = std::numeric_limits<double>::quiet_NaN();
    double _rvGamma    = std::numeric_limits<double>::quiet_NaN();
    double _rvEGamma   = std::numeric_limits<double>::quiet_NaN();
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

    // Photometric light-curve
    double _photPeriod  = std::numeric_limits<double>::quiet_NaN();
    double _photEPeriod = std::numeric_limits<double>::quiet_NaN();
    double _photIncl    = std::numeric_limits<double>::quiet_NaN();
    double _photEIncl   = std::numeric_limits<double>::quiet_NaN();
    double _photQ       = std::numeric_limits<double>::quiet_NaN();
    double _photEQ      = std::numeric_limits<double>::quiet_NaN();

    // Dataset availability
    bool _hasTess     = false;
    bool _hasGaia     = false;
    bool _hasZtf      = false;
    bool _hasAtlas    = false;
    bool _hasBlackgem = false;
};

#endif // STAR_H