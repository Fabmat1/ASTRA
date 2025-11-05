#include "Star.h"
#include "Photometry.h"
#include "Spectrum.h"
#include <QVariant>

Star::Star()
    : m_ra(0.0)
    , m_dec(0.0)
    , m_pmra(0.0)
    , m_pmdec(0.0)
    , m_e_pmra(0.0)
    , m_e_pmdec(0.0)
    , m_plx(0.0)
    , m_e_plx(0.0)
    , m_pmra_pmdec_corr(0.0)
    , m_plx_pmdec_corr(0.0)
    , m_plx_pmra_corr(0.0)
    , m_gmag(0.0)
    , m_e_gmag(0.0)
    , m_bp(0.0)
    , m_e_bp(0.0)
    , m_rp(0.0)
    , m_e_rp(0.0)
    , m_bp_rp(0.0)
    , m_teff(0.0)
    , m_e_teff(0.0)
    , m_logg(0.0)
    , m_e_logg(0.0)
    , m_he(0.0)
    , m_e_he(0.0)
    , m_logp(0.0)
    , m_deltaRV(0.0)
    , m_e_deltaRV(0.0)
    , m_rv_avg(0.0)
    , m_e_rv_avg(0.0)
    , m_rv_med(0.0)
    , m_e_rv_med(0.0)
{
}

Star::~Star()
{
}

QVariant Star::getFieldValue(const QString& fieldName) const
{
    // Map field names to values for table display
    if (fieldName == "alias") return m_alias;
    if (fieldName == "source_id") return m_sourceId;
    if (fieldName == "tic") return m_tic;
    if (fieldName == "jname") return m_jname;

    if (fieldName == "ra") return m_ra;
    if (fieldName == "dec") return m_dec;
    if (fieldName == "pmra") return m_pmra;
    if (fieldName == "pmdec") return m_pmdec;
    if (fieldName == "e_pmra") return m_e_pmra;
    if (fieldName == "e_pmdec") return m_e_pmdec;
    if (fieldName == "plx") return m_plx;
    if (fieldName == "e_plx") return m_e_plx;

    if (fieldName == "gmag") return m_gmag;
    if (fieldName == "e_gmag") return m_e_gmag;
    if (fieldName == "bp") return m_bp;
    if (fieldName == "e_bp") return m_e_bp;
    if (fieldName == "rp") return m_rp;
    if (fieldName == "e_rp") return m_e_rp;
    if (fieldName == "bp_rp") return m_bp_rp;

    if (fieldName == "spec_class") return m_spec_class;
    if (fieldName == "teff") return m_teff;
    if (fieldName == "e_teff") return m_e_teff;
    if (fieldName == "logg") return m_logg;
    if (fieldName == "e_logg") return m_e_logg;
    if (fieldName == "he") return m_he;
    if (fieldName == "e_he") return m_e_he;

    if (fieldName == "logp") return m_logp;
    if (fieldName == "deltaRV") return m_deltaRV;
    if (fieldName == "e_deltaRV") return m_e_deltaRV;
    if (fieldName == "rv_avg") return m_rv_avg;
    if (fieldName == "e_rv_avg") return m_e_rv_avg;
    if (fieldName == "rv_med") return m_rv_med;
    if (fieldName == "e_rv_med") return m_e_rv_med;

    return QVariant();
}