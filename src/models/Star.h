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

    // Metadata
    std::vector<QString> getBibcodes() const { return _bibcodes; }
    void setBibcodes(const std::vector<QString>& bibcodes) { _bibcodes = bibcodes; }
    void addBibcode(const QString& bibcode) { _bibcodes.push_back(bibcode); }

    // Photometry and Spectroscopy
    std::shared_ptr<Photometry> getPhotometry() const { return _photometry; }
    void setPhotometry(std::shared_ptr<Photometry> photometry) { 
        _photometry = photometry; 
        _photometryLoaded = true; 
    }

    // Lazy loading support
    bool hasPhotometryLoaded() const { return _photometryLoaded; }
    bool hasSpectraLoaded() const { return _spectraLoaded; }
    
    void setPhotometryLoader(std::function<std::shared_ptr<Photometry>(const QString&)> loader) {
        _photometryLoader = loader;
    }
    void setSpectraLoader(std::function<std::vector<std::shared_ptr<Spectrum>>(const QString&)> loader) {
        _spectraLoader = loader;
    }

    std::shared_ptr<Photometry> getPhotometry();  
    std::vector<std::shared_ptr<Spectrum>> getSpectra();  

    void addSpectrum(std::shared_ptr<Spectrum> spectrum);
    void removeSpectrum(const QString& spectrumId);

    // Fast field access using function pointers
    using FieldGetter = std::function<QVariant(const Star*)>;
    static const std::unordered_map<QString, FieldGetter>& getFieldMap();
    
    // Generic field access for table display
    QVariant getFieldValue(const QString& fieldName) const;

    std::shared_ptr<RadialVelocityCurve> getRVCurve() const { return _rvCurve; }
    void setRVCurve(std::shared_ptr<RadialVelocityCurve> curve) { _rvCurve = curve; }
    void updateRVMetricsFromCurve();

private:
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
    std::function<std::shared_ptr<Photometry>(const QString&)> _photometryLoader;
    std::function<std::vector<std::shared_ptr<Spectrum>>(const QString&)> _spectraLoader;
};

#endif // STAR_H