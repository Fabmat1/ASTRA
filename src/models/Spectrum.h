#ifndef SPECTRUM_H
#define SPECTRUM_H

#include <QString>
#include <QVector>
#include <QMap>
#include <QDateTime>
#include <vector>
#include <memory>
#include <functional>

#include "Time.h"
#include "AsymmetricErrors.h"

// ─────────────────────────────────────────────────────────────────────────────
// One element's fitted abundance, as log10 of the fractional particle number
// (GAEL's convention; see models/ElementAbundances.h).
// ─────────────────────────────────────────────────────────────────────────────
struct FittedAbundance
{
    double value = AsymErr::unset;
    double error = 0.0;
    bool   frozen = false;
    /// -1 = pinned at the low edge of its grid axis → the lines are not
    /// detected and the value is an *upper limit*; +1 = pinned at the high
    /// edge → a *lower limit*; 0 = a measurement.
    int    limitSide = 0;

    bool isSet() const { return !std::isnan(value); }
    bool isUpperLimit() const { return limitSide < 0; }
    bool isLowerLimit() const { return limitSide > 0; }
};

// ─────────────────────────────────────────────────────────────────────────────
// Spectral model fit
//
// A fit may have one or two stellar components. The flat parameter fields
// (teff, logg, …) always describe *component 1*, which is what the rest of
// ASTRA reports for the star; component 2's live in the `*2` fields and are
// only meaningful when nComponents == 2.
// ─────────────────────────────────────────────────────────────────────────────
class SpectralFit
{
public:
    SpectralFit();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // File operations for model data
    void setModelDataFile(const QString& file) { _modelDataFile = file; }
    QString getModelDataFile() const { return _modelDataFile; }
    bool saveDataToFile(const QString& filepath);
    bool loadDataFromFile(const QString& filepath);

    void setFlagged(bool f) { isFlagged = f; }   // ← delete from Spectrum

    bool hasData() const {
        return !modelWavelengths.empty() || !modelFluxes.empty() ||
               !rebinnedFluxes.empty() || !rebinnedSigmas.empty() ||
               !modelSplines.empty() || !modelIgnore.empty() ||
               !modelFluxesComp1.empty() || !modelFluxesComp2.empty() ||
               !telluricTransmission.empty();
    }

    QDateTime creationDate;
    QString modelId;
    bool isBestFit;
    bool isFlagged;

    // Model data - not loaded by default
    std::vector<double> modelWavelengths;
    std::vector<double> modelFluxes;          ///< combined model of all components
    std::vector<double> rebinnedFluxes;
    std::vector<double> rebinnedSigmas;
    std::vector<double> modelSplines;
    std::vector<uint8_t> modelIgnore;

    // Per-component models on the same wavelength grid: what the spectrum
    // would look like if that component were the only star in it (lines at
    // full depth, undiluted by the other component's light). Empty for fits
    // that predate this, and comp2 is empty for a one-component fit; for a
    // one-component fit comp1 is the same curve as modelFluxes.
    std::vector<double> modelFluxesComp1;
    std::vector<double> modelFluxesComp2;

    // Fitted telluric transmission on the same grid; empty when the fit had no
    // telluric component. modelFluxes already includes it.
    std::vector<double> telluricTransmission;

    // Fitted parameters (component 1)
    double teff;
    double teffError;
    double logg;
    double loggError;
    double he;
    double heError;
    double vsini;
    double vsiniError;
    double radialVelocity;
    double radialVelocityError;

    double chi2;
    double metallicity;
    double metallicityError;
    double macroturbulence;
    double macroturbulenceError;
    double microturbulence;
    double microturbulenceError;

    // Optional asymmetric 1σ errors (value +up/−down, positive magnitudes).
    // NaN = unset → the symmetric *Error above applies (see AsymmetricErrors.h).
    double teffErrorUp            = AsymErr::unset;
    double teffErrorDown          = AsymErr::unset;
    double loggErrorUp            = AsymErr::unset;
    double loggErrorDown          = AsymErr::unset;
    double heErrorUp              = AsymErr::unset;
    double heErrorDown            = AsymErr::unset;
    double vsiniErrorUp           = AsymErr::unset;
    double vsiniErrorDown         = AsymErr::unset;
    double radialVelocityErrorUp   = AsymErr::unset;
    double radialVelocityErrorDown = AsymErr::unset;
    double metallicityErrorUp     = AsymErr::unset;
    double metallicityErrorDown   = AsymErr::unset;
    double macroturbulenceErrorUp   = AsymErr::unset;
    double macroturbulenceErrorDown = AsymErr::unset;
    double microturbulenceErrorUp   = AsymErr::unset;
    double microturbulenceErrorDown = AsymErr::unset;

    // ── Second stellar component ────────────────────────────────────────────
    // Only meaningful when nComponents == 2. Kept in its own fields rather
    // than replacing the flat ones so that everything reading a fit today
    // keeps seeing component 1, which is the star's reported solution.
    int    nComponents = 1;

    double teff2                = AsymErr::unset;
    double teff2Error           = 0.0;
    double logg2                = AsymErr::unset;
    double logg2Error           = 0.0;
    double he2                  = AsymErr::unset;
    double he2Error             = 0.0;
    double vsini2               = AsymErr::unset;
    double vsini2Error          = 0.0;
    double radialVelocity2      = AsymErr::unset;
    double radialVelocity2Error = 0.0;
    double metallicity2         = AsymErr::unset;
    double metallicity2Error    = 0.0;
    double macroturbulence2     = AsymErr::unset;
    double macroturbulence2Error = 0.0;
    double microturbulence2     = AsymErr::unset;
    double microturbulence2Error = 0.0;

    /// Component 2's effective surface area relative to component 1's
    /// (component 1's is 1 by definition). NaN for a one-component fit.
    double surRatio      = AsymErr::unset;
    double surRatioError = 0.0;

    // ── Element abundances, keyed by grid species name ("FE", "SI", …) ──────
    // Present for every element the component's grid resolves, frozen ones
    // included; empty for a fit on a grid without element axes.
    QMap<QString, FittedAbundance> abundances;    ///< component 1
    QMap<QString, FittedAbundance> abundances2;   ///< component 2

    // ── Fitted telluric component (per spectrum) ────────────────────────────
    bool   hasTelluric        = false;
    double telluricAirmass    = AsymErr::unset;
    double telluricAirmassError = 0.0;
    double telluricPwv        = AsymErr::unset;   ///< [mm]
    double telluricPwvError   = 0.0;
    double telluricBarycorr   = AsymErr::unset;   ///< [km/s]

    /// True when this fit has a second stellar component worth reporting.
    bool hasSecondComponent() const {
        return nComponents >= 2 && !std::isnan(teff2);
    }

private:
    QString _id;
    QString _modelDataFile;
};

struct SpectrumIndexRow {
    QString starId;
    QString spectrumId;
    QString file;
};

// ─────────────────────────────────────────────────────────────────────────────
// Main spectrum class
// ─────────────────────────────────────────────────────────────────────────────
class Spectrum
{
public:
    Spectrum();
    ~Spectrum();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // File operations for spectral data
    void setDataFile(const QString& file) { _dataFile = file; }
    QString getDataFile() const { return _dataFile; }
    bool saveDataToFile(const QString& filepath);
    bool loadDataFromFile(const QString& filepath);

    QString getFile() const { return _file; }
    void setFile(const QString& file) { _file = file; }

    // ── Time (new API) ──────────────────────────────────────────────────────
    const Time& time() const        { return _time; }
    Time&       time()              { return _time; }
    void        setTime(const Time& t) { _time = t; }

    // ── Time (legacy wrappers - delegate to Time) ───────────────────────────
    // These keep every existing call-site compiling.  Migrate at leisure,
    // then remove.
    double getMJD() const           { return _time.mjdOr(0.0); }
    void   setMJD(double mjd)      { _time.setMJD(mjd); }

    double getBJD() const           { return _time.bjdOr(0.0); }
    void   setBJD(double bjd)      { _time.setBJD(bjd); }

    double getExposureTime() const  { return _time.hasExposureTime()
                                             ? _time.exposureTimeSec() : 0.0; }
    void   setExposureTime(double s){ _time.setExposureTime(s); }

    // ── Instrument ──────────────────────────────────────────────────────────
    QString getInstrument() const { return _instrument; }
    void setInstrument(const QString& instrument) { _instrument = instrument; }
    QString getInstrumentId() const      { return _instrumentId; }
    void    setInstrumentId(const QString& id) { _instrumentId = id; }

    QString getModeKey() const           { return _modeKey; }
    void    setModeKey(const QString& k) { _modeKey = k; }

    // Barycentric correction status
    bool isBarycentricallyCorrected() const { return _isBarycentricallyCorrected; }
    void setBarycentricallyCorrected(bool corrected) { _isBarycentricallyCorrected = corrected; }

    // ── Archive origin (empty for locally imported spectra) ────────────────
    QString getOrigin() const               { return _origin; }
    void    setOrigin(const QString& o)     { _origin = o; }

    // Stable external identifier for de-duplication across fetches
    // (e.g. "eso:<dp_id>", "lamost-dr8-lrs:<obsid>", with a "#expN" suffix
    // for individual exposures extracted from a multi-exposure product).
    QString getOriginId() const             { return _originId; }
    void    setOriginId(const QString& id)  { _originId = id; }

    // False only for products that are a single exposure of a coadd
    bool isCoadd() const                    { return _isCoadd; }
    void setIsCoadd(bool c)                 { _isCoadd = c; }

    // Small JSON blob with archive metadata (url, snr, R, collection)
    QString getOriginMeta() const           { return _originMeta; }
    void    setOriginMeta(const QString& m) { _originMeta = m; }

    bool isFetched() const                  { return !_origin.isEmpty(); }

    // ── Spectral data ───────────────────────────────────────────────────────
    void setData(const std::vector<double>& wavelengths,
                 const std::vector<double>& fluxes,
                 const std::vector<double>& errors);

    std::vector<double> getWavelengths() const { return _wavelengths; }
    std::vector<double> getFluxes() const { return _fluxes; }
    std::vector<double> getFluxErrors() const { return _fluxErrors; }

    // ── Model fits ──────────────────────────────────────────────────────────
    void addSpectralFit(std::shared_ptr<SpectralFit> fit);
    void removeSpectralFit(const QString& fitId);
    std::vector<std::shared_ptr<SpectralFit>> getSpectralFits() const;
    std::shared_ptr<SpectralFit> getBestFit() const;
    using BestFitChangedCallback =
        std::function<void(Spectrum*, std::shared_ptr<SpectralFit>)>;
    void setBestFitChangedCallback(BestFitChangedCallback cb)
        { _bestFitChangedCb = std::move(cb); }
    using FitChangedCallback =
        std::function<void(Spectrum*, std::shared_ptr<SpectralFit>)>;
    void setFitChangedCallback(FitChangedCallback cb)
        { _fitChangedCb = std::move(cb); }

    void notifyFitChanged(const std::shared_ptr<SpectralFit>& fit);

    // ── Utilities ───────────────────────────────────────────────────────────
    bool loadFromFile(const QString& filepath);
    bool hasData() const { return !_wavelengths.empty(); }

    bool isFlagged() const { return _flagged; }
    void setFlagged(bool f) { _flagged = f; }

    // Set which fit is the best one. Pass empty string to clear.
    // Unsets isBestFit on all other fits for this spectrum.
    void setBestFitById(const QString& fitId);

private:
    // File and metadata
    QString _id;
    QString _dataFile;
    QString _file;
    QString _instrument;
    QString _instrumentId;      // UUID into Instrument table
    QString _modeKey;           // InstrumentMode key within that instrument

    Time _time;  

    bool _flagged = false;

    bool _isBarycentricallyCorrected;

    // Archive origin metadata (empty/default for local imports)
    QString _origin;
    QString _originId;
    bool    _isCoadd = true;
    QString _originMeta;

    // Spectral data
    std::vector<double> _wavelengths;  // in Angstroms
    std::vector<double> _fluxes;
    std::vector<double> _fluxErrors;

    // Model fits
    std::vector<std::shared_ptr<SpectralFit>> _spectralFits;
    BestFitChangedCallback _bestFitChangedCb;
    FitChangedCallback _fitChangedCb;

    void notifyBestFitChanged();

};

#endif // SPECTRUM_H