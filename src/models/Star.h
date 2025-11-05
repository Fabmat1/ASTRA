#ifndef STAR_H
#define STAR_H

#include <QString>
#include <QVariant>
#include <memory>
#include <vector>

class Photometry;
class Spectrum;

class Star
{
public:
    Star();
    ~Star();

    // Identifying fields
    QString getAlias() const { return m_alias; }
    QString getSourceId() const { return m_sourceId; }
    QString getTic() const { return m_tic; }
    QString getJName() const { return m_jname; }

    void setAlias(const QString& alias) { m_alias = alias; }
    void setSourceId(const QString& id) { m_sourceId = id; }
    void setTic(const QString& tic) { m_tic = tic; }
    void setJName(const QString& jname) { m_jname = jname; }

    // Astrometric fields
    double getRa() const { return m_ra; }
    double getDec() const { return m_dec; }
    double getPmra() const { return m_pmra; }
    double getPmdec() const { return m_pmdec; }
    double getEPmra() const { return m_e_pmra; }
    double getEPmdec() const { return m_e_pmdec; }
    double getPlx() const { return m_plx; }
    double getEPlx() const { return m_e_plx; }
    double getPmraPmdecCorr() const { return m_pmra_pmdec_corr; }
    double getPlxPmdecCorr() const { return m_plx_pmdec_corr; }
    double getPlxPmraCorr() const { return m_plx_pmra_corr; }

    void setRa(double ra) { m_ra = ra; }
    void setDec(double dec) { m_dec = dec; }
    void setPmra(double pmra) { m_pmra = pmra; }
    void setPmdec(double pmdec) { m_pmdec = pmdec; }
    void setEPmra(double e_pmra) { m_e_pmra = e_pmra; }
    void setEPmdec(double e_pmdec) { m_e_pmdec = e_pmdec; }
    void setPlx(double plx) { m_plx = plx; }
    void setEPlx(double e_plx) { m_e_plx = e_plx; }
    void setPmraPmdecCorr(double corr) { m_pmra_pmdec_corr = corr; }
    void setPlxPmdecCorr(double corr) { m_plx_pmdec_corr = corr; }
    void setPlxPmraCorr(double corr) { m_plx_pmra_corr = corr; }

    // Photometric fields
    double getGmag() const { return m_gmag; }
    double getEGmag() const { return m_e_gmag; }
    double getBp() const { return m_bp; }
    double getEBp() const { return m_e_bp; }
    double getRp() const { return m_rp; }
    double getERp() const { return m_e_rp; }
    double getBpRp() const { return m_bp_rp; }

    void setGmag(double gmag) { m_gmag = gmag; }
    void setEGmag(double e_gmag) { m_e_gmag = e_gmag; }
    void setBp(double bp) { m_bp = bp; }
    void setEBp(double e_bp) { m_e_bp = e_bp; }
    void setRp(double rp) { m_rp = rp; }
    void setERp(double e_rp) { m_e_rp = e_rp; }
    void setBpRp(double bp_rp) { m_bp_rp = bp_rp; }

    // Spectroscopic fields
    QString getSpecClass() const { return m_spec_class; }
    double getTeff() const { return m_teff; }
    double getETeff() const { return m_e_teff; }
    double getLogg() const { return m_logg; }
    double getELogg() const { return m_e_logg; }
    double getHe() const { return m_he; }
    double getEHe() const { return m_e_he; }

    void setSpecClass(const QString& spec_class) { m_spec_class = spec_class; }
    void setTeff(double teff) { m_teff = teff; }
    void setETeff(double e_teff) { m_e_teff = e_teff; }
    void setLogg(double logg) { m_logg = logg; }
    void setELogg(double e_logg) { m_e_logg = e_logg; }
    void setHe(double he) { m_he = he; }
    void setEHe(double e_he) { m_e_he = e_he; }

    // Radial velocity fields
    double getLogP() const { return m_logp; }
    double getDeltaRV() const { return m_deltaRV; }
    double getEDeltaRV() const { return m_e_deltaRV; }
    double getRVAvg() const { return m_rv_avg; }
    double getERVAvg() const { return m_e_rv_avg; }
    double getRVMed() const { return m_rv_med; }
    double getERVMed() const { return m_e_rv_med; }

    void setLogP(double logp) { m_logp = logp; }
    void setDeltaRV(double deltaRV) { m_deltaRV = deltaRV; }
    void setEDeltaRV(double e_deltaRV) { m_e_deltaRV = e_deltaRV; }
    void setRVAvg(double rv_avg) { m_rv_avg = rv_avg; }
    void setERVAvg(double e_rv_avg) { m_e_rv_avg = e_rv_avg; }
    void setRVMed(double rv_med) { m_rv_med = rv_med; }
    void setERVMed(double e_rv_med) { m_e_rv_med = e_rv_med; }

    // Metadata
    std::vector<QString> getBibcodes() const { return m_bibcodes; }
    void setBibcodes(const std::vector<QString>& bibcodes) { m_bibcodes = bibcodes; }
    void addBibcode(const QString& bibcode) { m_bibcodes.push_back(bibcode); }

    // Photometry and Spectroscopy
    std::shared_ptr<Photometry> getPhotometry() const { return m_photometry; }
    void setPhotometry(std::shared_ptr<Photometry> photometry) { m_photometry = photometry; }

    std::vector<std::shared_ptr<Spectrum>> getSpectra() const { return m_spectra; }
    void addSpectrum(std::shared_ptr<Spectrum> spectrum) { m_spectra.push_back(spectrum); }

    // Generic field access for table display
    QVariant getFieldValue(const QString& fieldName) const;

private:
    // Identifying fields
    QString m_alias;
    QString m_sourceId;
    QString m_tic;
    QString m_jname;

    // Astrometric fields
    double m_ra;
    double m_dec;
    double m_pmra;
    double m_pmdec;
    double m_e_pmra;
    double m_e_pmdec;
    double m_plx;
    double m_e_plx;
    double m_pmra_pmdec_corr;
    double m_plx_pmdec_corr;
    double m_plx_pmra_corr;

    // Photometric fields (Gaia)
    double m_gmag;
    double m_e_gmag;
    double m_bp;
    double m_e_bp;
    double m_rp;
    double m_e_rp;
    double m_bp_rp;

    // Spectroscopic fields
    QString m_spec_class;
    double m_teff;
    double m_e_teff;
    double m_logg;
    double m_e_logg;
    double m_he;
    double m_e_he;

    // Radial velocity fields
    double m_logp;
    double m_deltaRV;
    double m_e_deltaRV;
    double m_rv_avg;
    double m_e_rv_avg;
    double m_rv_med;
    double m_e_rv_med;

    // Metadata
    std::vector<QString> m_bibcodes;

    // Associated data
    std::shared_ptr<Photometry> m_photometry;
    std::vector<std::shared_ptr<Spectrum>> m_spectra;
};

#endif // STAR_H