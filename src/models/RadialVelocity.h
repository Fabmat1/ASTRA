#ifndef RADIALVELOCITY_H
#define RADIALVELOCITY_H

#include <QString>
#include <QStringList>
#include <QSet>
#include <QDateTime>
#include <vector>
#include <memory>
#include <cmath>
#include <limits>

#include "Time.h"
#include "AsymmetricErrors.h"

class Spectrum;
class SpectralFit;
class Instrument;

// ─────────────────────────────────────────────────────────────────────────────
// Individual radial velocity measurement point
// ─────────────────────────────────────────────────────────────────────────────
class RadialVelocityPoint
{
public:
    RadialVelocityPoint();
    RadialVelocityPoint(double rv, double rvError, double mjd, double bjd);
    RadialVelocityPoint(double rv, double rvError, const Time& time);
    ~RadialVelocityPoint();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // Parent curve reference (for DB)
    QString getCurveId() const { return _curveId; }
    void setCurveId(const QString& id) { _curveId = id; }

    // RV measurement
    double getRV() const { return _rv; }
    void setRV(double rv) { _rv = rv; }

    double getRVErrorFormal() const { return _rvErrorFormal; }
    void setRVErrorFormal(double error) { _rvErrorFormal = error; _rvErrorDirty = true; }

    double getRVErrorSystematic() const { return _rvErrorSystematic; }
    void setRVErrorSystematic(double error) { _rvErrorSystematic = error; _rvErrorDirty = true; }

    double getRVError() const {
        if (_rvErrorDirty) {
            _rvError = std::sqrt(_rvErrorFormal * _rvErrorFormal
                               + _rvErrorSystematic * _rvErrorSystematic);
            _rvErrorDirty = false;
        }
        return _rvError;
    }
    void setRVError(double error) { _rvError = error; _rvErrorDirty = false; }

    // ── Time (new API) ──────────────────────────────────────────────────────
    const Time& time() const           { return _time; }
    Time&       time()                 { return _time; }
    void        setTime(const Time& t) { _time = t; }

    // ── Time (legacy wrappers) ──────────────────────────────────────────────
    double getMJD() const   { return _time.mjdOr(0.0); }
    double getBJD() const   { return _time.bjdOr(0.0); }
    void   setMJD(double v) { _time.setMJD(v); }
    void   setBJD(double v) { _time.setBJD(v); }

    // Heliocentric correction (km/s applied to RV, not a time shift)
    double getHeliocentricCorrection() const { return _helioCorrection; }
    void setHeliocentricCorrection(double correction) { _helioCorrection = correction; }
    bool isHeliocentricCorrectionApplied() const { return _helioCorrectionApplied; }
    void setHeliocentricCorrectionApplied(bool applied) { _helioCorrectionApplied = applied; }

    // Instrument reference
    std::shared_ptr<Instrument> getInstrument() const { return _instrument; }
    void setInstrument(std::shared_ptr<Instrument> instrument) { _instrument = instrument; }

    // Source spectrum/fit references (weak to avoid circular dependency)
    std::weak_ptr<Spectrum> getSourceSpectrum() const { return _sourceSpectrum; }
    std::weak_ptr<SpectralFit> getSourceFit() const { return _sourceFit; }
    void setSourceSpectrum(std::weak_ptr<Spectrum> spectrum) { _sourceSpectrum = spectrum; }
    void setSourceFit(std::weak_ptr<SpectralFit> fit) { _sourceFit = fit; }

    // Source IDs for database persistence (since weak_ptrs don't serialize)
    QString getSpectrumId() const { return _spectrumId; }
    void setSpectrumId(const QString& id) { _spectrumId = id; }
    QString getSpectralFitId() const { return _spectralFitId; }
    void setSpectralFitId(const QString& id) { _spectralFitId = id; }

    // Data source description (e.g. filename, "spectral_fit", "table_import")
    QString getSource() const { return _source; }
    void setSource(const QString& source) { _source = source; }

    // Stellar component this RV belongs to: 1 = primary, 2 = secondary.
    int  getComponent() const { return _component; }
    void setComponent(int c) { _component = (c >= 2 ? 2 : 1); }

    // Create from spectral fit. component 2 reads the fit's radialVelocity2
    // fields; returns nullptr when that component has no finite RV.
    static std::shared_ptr<RadialVelocityPoint> createFromSpectralFit(
        std::shared_ptr<SpectralFit> fit,
        std::shared_ptr<Spectrum> spectrum,
        std::shared_ptr<Instrument> instrument = nullptr,
        int component = 1);

    // Flagged: excluded from RV fits; auto-mirrored from source SpectralFit
    bool isFlagged() const { return _flagged; }
    void setFlagged(bool f) { _flagged = f; }

    enum class RVSource { Manual = 0, FromFit = 1 };

    RVSource getRVSource() const { return _rvSource; }
    void     setRVSource(RVSource s) { _rvSource = s; }

    // Manual snapshot (preserved across fit refreshes)
    bool   hasManualValue() const { return !std::isnan(_rvManual); }
    double getRVManual() const { return _rvManual; }
    double getRVManualErrorFormal() const { return _rvManualErrorFormal; }
    double getRVManualErrorSystematic() const { return _rvManualErrorSystematic; }
    void   setRVManual(double v) { _rvManual = v; }
    void   setRVManualErrorFormal(double v) { _rvManualErrorFormal = v; }
    void   setRVManualErrorSystematic(double v) { _rvManualErrorSystematic = v; }

    // Capture current active values as manual snapshot and switch to Manual mode.
    void captureAsManual();

    // Apply values from a SpectralFit. Always refreshes linkage + flag.
    // Active RV/error fields are overwritten only if rvSource == FromFit.
    void mirrorFlagFromFit(const SpectralFit& fit);
    void applyFromFit(const SpectralFit& fit);   // unchanged signature

private:
    QString _id;
    QString _curveId;
    double _rv;                    // km/s
    mutable double _rvError;       // km/s (cached: may be auto-computed)
    double _rvErrorFormal;         // km/s
    double _rvErrorSystematic;     // km/s
    mutable bool _rvErrorDirty;

    Time _time;                    // replaces _mjd, _bjd

    double _helioCorrection;       // km/s
    bool _helioCorrectionApplied;

    std::shared_ptr<Instrument> _instrument;
    std::weak_ptr<Spectrum> _sourceSpectrum;
    std::weak_ptr<SpectralFit> _sourceFit;

    // Serializable IDs for DB round-tripping
    QString _spectrumId;
    QString _spectralFitId;
    QString _source;
    int  _component = 1;
    bool _flagged = false;
    double   _rvManual              = std::numeric_limits<double>::quiet_NaN();
    double   _rvManualErrorFormal   = 0.0;
    double   _rvManualErrorSystematic = 0.0;
    RVSource _rvSource              = RVSource::Manual;

    std::vector<std::shared_ptr<RadialVelocityPoint>> getActiveRVPoints() const;
};


// ─────────────────────────────────────────────────────────────────────────────
// RV orbital fit parameters
// ─────────────────────────────────────────────────────────────────────────────
class RVFit
{
public:
    RVFit();
    ~RVFit();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // Parent curve reference (for DB)
    QString getCurveId() const { return _curveId; }
    void setCurveId(const QString& id) { _curveId = id; }

    // Fit metadata
    QDateTime getCreationDate() const { return _creationDate; }
    void setCreationDate(const QDateTime& date) { _creationDate = date; }

    bool isBestFit() const { return _isBestFit; }
    void setBestFit(bool best) { _isBestFit = best; }

    QString getFitMethod() const { return _fitMethod; }
    void setFitMethod(const QString& method) { _fitMethod = method; }

    // Orbital parameters
    double getK() const { return _K; }
    double getKError() const { return _KError; }
    void setK(double k) { _K = k; }
    void setKError(double error) { _KError = error; }

    // Secondary semi-amplitude (SB2). NaN = unset, i.e. an SB1 fit. The
    // secondary shares P, e, omega, phi and gamma and moves in antiphase:
    // RV2(t) = gamma - K2 * f(t) where RV1(t) = gamma + K * f(t).
    double getK2() const { return _K2; }
    double getK2Error() const { return _K2Error; }
    void setK2(double k2) { _K2 = k2; }
    void setK2Error(double error) { _K2Error = error; }
    bool hasK2() const { return std::isfinite(_K2) && _K2 > 0.0; }

    // Mass ratio q = M2/M1 = K1/K2; NaN unless hasK2().
    double massRatio() const;
    double massRatioError() const;

    double getGamma() const { return _gamma; }
    double getGammaError() const { return _gammaError; }
    void setGamma(double gamma) { _gamma = gamma; }
    void setGammaError(double error) { _gammaError = error; }

    double getPeriod() const { return _period; }
    double getPeriodError() const { return _periodError; }
    void setPeriod(double period) { _period = period; }
    void setPeriodError(double error) { _periodError = error; }

    double getPhi() const { return _phi; }
    double getPhiError() const { return _phiError; }
    void setPhi(double phi) { _phi = phi; }
    void setPhiError(double error) { _phiError = error; }

    double getT0() const { return _t0; }
    double getT0Error() const { return _t0Error; }
    void setT0(double t0) { _t0 = t0; }
    void setT0Error(double error) { _t0Error = error; }

    // Eccentric orbit parameters (optional)
    bool isEccentric() const { return _isEccentric; }
    void setEccentric(bool eccentric) { _isEccentric = eccentric; }

    double getEccentricity() const { return _eccentricity; }
    double getEccentricityError() const { return _eccentricityError; }
    void setEccentricity(double e) { _eccentricity = e; _isEccentric = (e > 0); }
    void setEccentricityError(double error) { _eccentricityError = error; }

    double getOmega() const { return _omega; }
    double getOmegaError() const { return _omegaError; }
    void setOmega(double omega) { _omega = omega; }
    void setOmegaError(double error) { _omegaError = error; }

    // ── Optional asymmetric 1σ errors (value +up/−down, positive magnitudes).
    //    NaN = unset → the symmetric error applies (see AsymmetricErrors.h).
    double getKErrorUp() const   { return _KErrorUp; }
    double getKErrorDown() const { return _KErrorDown; }
    void setKErrorUp(double e)   { _KErrorUp = e; }
    void setKErrorDown(double e) { _KErrorDown = e; }

    double getK2ErrorUp() const   { return _K2ErrorUp; }
    double getK2ErrorDown() const { return _K2ErrorDown; }
    void setK2ErrorUp(double e)   { _K2ErrorUp = e; }
    void setK2ErrorDown(double e) { _K2ErrorDown = e; }

    double getGammaErrorUp() const   { return _gammaErrorUp; }
    double getGammaErrorDown() const { return _gammaErrorDown; }
    void setGammaErrorUp(double e)   { _gammaErrorUp = e; }
    void setGammaErrorDown(double e) { _gammaErrorDown = e; }

    double getPeriodErrorUp() const   { return _periodErrorUp; }
    double getPeriodErrorDown() const { return _periodErrorDown; }
    void setPeriodErrorUp(double e)   { _periodErrorUp = e; }
    void setPeriodErrorDown(double e) { _periodErrorDown = e; }

    double getPhiErrorUp() const   { return _phiErrorUp; }
    double getPhiErrorDown() const { return _phiErrorDown; }
    void setPhiErrorUp(double e)   { _phiErrorUp = e; }
    void setPhiErrorDown(double e) { _phiErrorDown = e; }

    double getT0ErrorUp() const   { return _t0ErrorUp; }
    double getT0ErrorDown() const { return _t0ErrorDown; }
    void setT0ErrorUp(double e)   { _t0ErrorUp = e; }
    void setT0ErrorDown(double e) { _t0ErrorDown = e; }

    double getEccentricityErrorUp() const   { return _eccentricityErrorUp; }
    double getEccentricityErrorDown() const { return _eccentricityErrorDown; }
    void setEccentricityErrorUp(double e)   { _eccentricityErrorUp = e; }
    void setEccentricityErrorDown(double e) { _eccentricityErrorDown = e; }

    double getOmegaErrorUp() const   { return _omegaErrorUp; }
    double getOmegaErrorDown() const { return _omegaErrorDown; }
    void setOmegaErrorUp(double e)   { _omegaErrorUp = e; }
    void setOmegaErrorDown(double e) { _omegaErrorDown = e; }

    // Calculate RV at given time (component 2 = secondary, antiphase)
    double calculateRV(double bjd, int component = 1) const;
    double calculateRV(const Time& t, int component = 1) const;

    // Fit quality metrics
    double getChi2() const { return _chi2; }
    void setChi2(double chi2) { _chi2 = chi2; }

    double getRms() const { return _rms; }
    void setRms(double rms) { _rms = rms; }

    // ── Reference time (runtime only - not persisted; rebound on load) ──
    // Set by RadialVelocityCurve::updateFitReferences() to the first
    // datapoint's BJD/MJD.  _phi is interpreted as the phase at this time.
    void setReferenceTime(double bjd, double mjd) { _tRefBJD = bjd; _tRefMJD = mjd; }
    double getReferenceBJD() const { return _tRefBJD; }
    double getReferenceMJD() const { return _tRefMJD; }

    // Derived time of periapsis (phase 0) just before reference.
    // NaN if reference is unset.
    double getT0BJD() const;
    double getT0MJD() const;

    // Epoch to fold on: getT0BJD(), or - when no reference time is bound (a
    // curve whose points have not been loaded) - the equivalent epoch measured
    // from BJD 0. Callers must not rebuild that fallback themselves: φ enters
    // the fold with the sign phaseSign() carries, which differs between the
    // circular and eccentric models.
    double foldEpochBJD() const;

    // Model evaluation (component 2 = secondary, antiphase)
    double calculateRVAtPhase(double phase, int component = 1) const;
    double computePhase(const Time& t) const;        // [0,1)

    // Update _chi2 and _rms from the supplied points (flagged are skipped).
    void updateStatistics(
        const std::vector<std::shared_ptr<RadialVelocityPoint>>& points);

    // Numerically robust Kepler equation solver (Newton with Murray–Dermott
    // initial guess).  Returns eccentric anomaly E for given M, e.
    static double solveKepler(double M, double e,
                              double tol = 1e-12, int maxIter = 60);

  private:
    // Sign with which φ enters the time→phase fold. The circular and eccentric
    // RV models fitted by rv_mcmc use OPPOSITE phase conventions: the circular
    // model is sin(2π(θ + φ)) (maths.cpp sinusoid()), while the eccentric
    // Keplerian model defines the mean anomaly as M = 2π(θ − φ) (models.cpp
    // rv_curve()). Folding with this sign keeps computePhase()/getT0BJD() (and
    // therefore the plotted curve, χ²/rms and predicted RV) aligned with the
    // data for eccentricity-enabled fits.
    double phaseSign() const { return _isEccentric ? -1.0 : 1.0; }

    QString _id;
    QString _curveId;
    QDateTime _creationDate;
    bool _isBestFit;
    QString _fitMethod;

    double _K;
    double _KError;
    double _gamma;
    double _gammaError;
    double _period;
    double _periodError;
    double _phi;
    double _phiError;
    double _t0;
    double _t0Error;

    bool _isEccentric;
    double _eccentricity;
    double _eccentricityError;
    double _omega;
    double _omegaError;

    // Secondary semi-amplitude; NaN = SB1 fit.
    double _K2      = AsymErr::unset;
    double _K2Error = 0.0;

    // Asymmetric errors; NaN = unset (fall back to the symmetric error).
    double _KErrorUp            = AsymErr::unset;
    double _KErrorDown          = AsymErr::unset;
    double _K2ErrorUp           = AsymErr::unset;
    double _K2ErrorDown         = AsymErr::unset;
    double _gammaErrorUp        = AsymErr::unset;
    double _gammaErrorDown      = AsymErr::unset;
    double _periodErrorUp       = AsymErr::unset;
    double _periodErrorDown     = AsymErr::unset;
    double _phiErrorUp          = AsymErr::unset;
    double _phiErrorDown        = AsymErr::unset;
    double _t0ErrorUp           = AsymErr::unset;
    double _t0ErrorDown         = AsymErr::unset;
    double _eccentricityErrorUp   = AsymErr::unset;
    double _eccentricityErrorDown = AsymErr::unset;
    double _omegaErrorUp        = AsymErr::unset;
    double _omegaErrorDown      = AsymErr::unset;

    double _tRefBJD = 0.0;
    double _tRefMJD = 0.0;

    double _chi2;
    double _rms;
};


// ─────────────────────────────────────────────────────────────────────────────
// Collection of RV points and fits for a star
// ─────────────────────────────────────────────────────────────────────────────
class RadialVelocityCurve
{
public:
    RadialVelocityCurve();
    ~RadialVelocityCurve();

    // UUID for database
    QString getId() const { return _id; }
    void setId(const QString& id) { _id = id; }

    // Parent star reference (for DB)
    QString getStarId() const { return _starId; }
    void setStarId(const QString& id) { _starId = id; }

    // RV Points management
    void addRVPoint(std::shared_ptr<RadialVelocityPoint> point);
    void removeRVPoint(const QString& pointId);
    void clearRVPoints();

    std::vector<std::shared_ptr<RadialVelocityPoint>> getRVPoints() const { return _rvPoints; }
    std::shared_ptr<RadialVelocityPoint> getRVPoint(const QString& pointId) const;

    // Lookup by originating spectrum. findPoint matches one component;
    // findPoints returns every component's point for that spectrum.
    std::shared_ptr<RadialVelocityPoint> findPoint(
        const QString& spectrumId, int component) const;
    std::vector<std::shared_ptr<RadialVelocityPoint>> findPoints(
        const QString& spectrumId) const;

    // True if any point belongs to the secondary component.
    bool hasComponent2() const;

    // Populate from spectra
    void populateFromSpectra(const std::vector<std::shared_ptr<Spectrum>>& spectra);
    void updateFromSpectra(const std::vector<std::shared_ptr<Spectrum>>& spectra);

    // RV Fits management
    void addRVFit(std::shared_ptr<RVFit> fit);
    void removeRVFit(const QString& fitId);
    void clearRVFits();

    std::vector<std::shared_ptr<RVFit>> getRVFits() const { return _rvFits; }
    std::shared_ptr<RVFit> getRVFit(const QString& fitId) const;
    std::shared_ptr<RVFit> getBestFit() const;
    void setBestFit(const QString& fitId);

    // Statistical metrics. These describe the PRIMARY component only
    // (component 1): deltaRV, rv_avg and logP are variability metrics of the
    // star's reported solution, and mixing antiphase secondary points would
    // inflate the amplitude and break the logP semantics.
    double getMinRV() const;
    double getMaxRV() const;
    double getMeanRV() const;
    double getMedianRV() const;
    double getStdDevRV() const;
    double getRVAmplitude() const;

    double getWeightedMeanRV() const;
    double getWeightedStdDevRV() const;

    // ── Time range (legacy - delegates to Time inside each point) ───────────
    double getMinMJD() const;
    double getMaxMJD() const;
    double getTimeSpan() const;   // Max MJD − Min MJD

    size_t getNumPoints() const { return _rvPoints.size(); }
    size_t getNumFits() const { return _rvFits.size(); }

    // ── Log-p variability metric ────────────────────────────────────────────
    double computeLogP() const;

    double getLogP() const { return _logP; }
    void setLogP(double logP) { _logP = logP; }

    // ─────────────────────────────────────────────────────────────────────
    // Change notification - multi-subscriber, token-based.
    //
    // Each subscriber gets an opaque ListenerToken from addChangeListener();
    // call removeChangeListener(token) (typically in your destructor) to
    // detach. Removing a stale or zero token is a no-op.
    //
    // Listeners are invoked synchronously, in registration order, by
    // notifyChanged(). Listeners must not modify the listener list during
    // dispatch (we snapshot before invoking, so adding/removing during a
    // callback is safe but takes effect on the NEXT notification).
    // ─────────────────────────────────────────────────────────────────────
    using ChangeCallback = std::function<void()>;
    using ListenerToken  = std::uint64_t;
    static constexpr ListenerToken kInvalidToken = 0;

    [[nodiscard]] ListenerToken addChangeListener(ChangeCallback cb);
    void removeChangeListener(ListenerToken token);

    // DEPRECATED: replaces all current listeners with a single one. Kept for
    // backwards compatibility while call sites migrate to addChangeListener.
    // Passing a null callback clears the single-slot listener (but leaves
    // listeners installed via addChangeListener intact).
    [[deprecated("Use addChangeListener / removeChangeListener instead")]]
    void setChangeCallback(ChangeCallback cb);

    void attachToSpectra(const std::vector<std::shared_ptr<Spectrum>>& spectra);

    std::vector<std::shared_ptr<RadialVelocityPoint>> getActiveRVPoints() const;
    std::vector<std::shared_ptr<RadialVelocityPoint>> getActiveRVPoints(int component) const;

    using PointPersistCallback =
        std::function<void(const std::shared_ptr<RadialVelocityPoint>&)>;
    void setPointPersistCallback(PointPersistCallback cb)
        { _pointPersistCb = std::move(cb); }

    void persistPoint(const std::shared_ptr<RadialVelocityPoint>& p)
        { if (_pointPersistCb && p) _pointPersistCb(p); }

    // DB-row deletion channel for points the curve drops on its own (e.g. a
    // FromFit secondary point whose best fit lost its second component).
    using PointDeleteCallback = std::function<void(const QString& pointId)>;
    void setPointDeleteCallback(PointDeleteCallback cb)
        { _pointDeleteCb = std::move(cb); }
    void deletePointRow(const QString& pointId)
        { if (_pointDeleteCb && !pointId.isEmpty()) _pointDeleteCb(pointId); }

    // Re-binds every fit's reference time to the earliest point's
    // BJD/MJD.  Called automatically by addRVPoint/addRVFit; call manually
    // after a bulk load (e.g. at end of repository::loadRadialVelocityCurve).
    void updateFitReferences();

    using BjdResolverCallback =
        std::function<void(const std::shared_ptr<RadialVelocityPoint>&)>;
    void setBjdResolverCallback(BjdResolverCallback cb)
        { _bjdResolverCb = std::move(cb); }
    void resolveBjd(const std::shared_ptr<RadialVelocityPoint>& p)
        { if (_bjdResolverCb && p) _bjdResolverCb(p); }

    void reconcileWithSpectra(
        const std::vector<std::shared_ptr<Spectrum>>& spectra);

    /// Drop every point that was derived from one of `spectrumIds`, which the
    /// caller is deleting. Returns the ids of the removed points so the caller
    /// can delete their DB rows; without this the points survive the spectrum
    /// and linger in the curve as orphans. Notifies listeners once, and only
    /// if something was actually removed.
    QStringList removePointsForSpectra(const QSet<QString>& spectrumIds);

    bool computeReferenceEpoch(double &bjdOut, double &mjdOut) const;

    // Announce that points/fits were modified directly (e.g. flags edited on
    // the point objects). Callers doing bulk edits should bracket them with
    // beginBatchUpdate()/endBatchUpdate() so listeners get ONE notification
    // instead of one per row - listeners typically rebuild whole plots.
    void notifyChanged();
    void beginBatchUpdate();
    void endBatchUpdate();

private:
    QString _id;
    QString _starId;
    std::vector<std::shared_ptr<RadialVelocityPoint>> _rvPoints;
    std::vector<std::shared_ptr<RVFit>> _rvFits;
    double _logP;

    // Multi-subscriber change notification.
    struct Listener {
        ListenerToken    token;
        ChangeCallback   cb;
    };
    std::vector<Listener> _listeners;
    ListenerToken         _nextToken     = 1;     // 0 reserved for "invalid"
    ListenerToken         _legacyToken   = kInvalidToken;  // for setChangeCallback shim

    // Batch-update state: while _batchDepth > 0, notifyChanged() is swallowed
    // and replayed once by the outermost endBatchUpdate().
    int  _batchDepth   = 0;
    bool _batchPending = false;

    BjdResolverCallback   _bjdResolverCb;
    PointPersistCallback  _pointPersistCb;
    PointDeleteCallback   _pointDeleteCb;

    double calculateMedian(std::vector<double> values) const;

    // Unflagged component-1 points; the statistics above run over these.
    std::vector<std::shared_ptr<RadialVelocityPoint>> activePrimaryPoints() const;

    void onBestFitChanged(const std::shared_ptr<Spectrum>& spec,
                          const std::shared_ptr<SpectralFit>& newBest);
    void onLinkedFitMetadataChanged(const std::shared_ptr<Spectrum>& spec,
                                    const std::shared_ptr<SpectralFit>& fit);
};

#endif // RADIALVELOCITY_H