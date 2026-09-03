#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>
#include <QStringList>
#include <QPair>
#include <QMap>
#include <optional>

namespace astra::fitting {

// ────────────────────────────────────────────────────────────────────
// Input config
// ────────────────────────────────────────────────────────────────────

struct StellarComponent {
    QString gridPath;
    double teff   = 25000.0;
    double logg   = 5.5;
    double vsini  = 7.0;
    double he     = -1.0;
    double zeta   = 0.0;     // macroturbulence
    double xi     = 0.0;     // microturbulence
    double z      = 0.0;     // metallicity
    bool freezeTeff  = false;
    bool freezeLogg  = false;
    bool freezeVsini = true;
    bool freezeHe    = false;
    bool freezeZeta  = true;
    bool freezeXi    = true;
    bool freezeZ     = true;

    // Ratio of this component's effective surface area to component 1's.
    // Component 1's is 1 and frozen by definition; only meaningful from the
    // second component onwards.
    double surRatio       = 1.0;
    bool   freezeSurRatio = false;

    // Element abundances keyed by the grid's own species name ("FE", "SI", …),
    // as log10 of the fractional particle number - GAEL's convention, which is
    // ISIS's cN_<ELEMENT>. Every element the grid resolves is *modelled*
    // whether or not it appears here (an absent one is frozen at the middle of
    // its axis); only the ones named here with freeze = false are *fitted*.
    // A value of 10 or more switches the element out of the model entirely.
    QMap<QString, double> abundances;
    QMap<QString, bool>   freezeAbundances;
};

struct IgnoreRegion {
    double wlLow  = 0.0;
    double wlHigh = 0.0;
};

struct ContinuumAnchor {
    double wlLow   = 0.0;
    double wlHigh  = 0.0;
    double spacing = 50.0;     // Å between anchor points
};

// Everything the user configures for one observed spectrum before it becomes
// an Observation in a job. Lives here rather than inside FitSetupWidget so the
// mass fitter can build the same configuration without a widget.
struct SpectrumFitConfig {
    bool   enabled = true;
    double wlMin = 3600.0;
    double wlMax = 5250.0;
    QVector<IgnoreRegion>    ignore;
    QVector<ContinuumAnchor> anchors;
    double resOffset = 0.0;
    double resSlope  = 0.37037;
    bool   inferFromFits = false;

    // Telluric seeds; only used when the job fits a telluric component.
    // Genuinely per-observation, so the "copy to …" buttons leave them be.
    double airmass = 1.0;    // 0 = no tellurics in this spectrum
    double pwv     = 1.0;    // [mm]
};

struct SpectrumFile {
    QString filename;
    QString spectype = "ASCII_with_2_columns";
    double  resOffset = 0.0;
    double  resSlope  = 0.37037;

    // ── Telluric component (only used when the job enables it) ──────────────
    // The spectra handed to the backend are already barycentrically corrected,
    // so `barycorr` does not move the data; it seeds the telluric component's
    // own shift, because the telluric lines sit in the observatory's frame and
    // the correction moved them by exactly this much. The backend fits it from
    // there, so this only has to be close - the value comes from the fetched
    // spectrum's provenance (SpecFetch::kBarycorrMetaKey), and a spectrum that
    // does not record one starts at zero.
    double  airmass     = 1.0;     // 0 switches the component off for this file
    double  pwv         = 1.0;     // precipitable water vapour [mm]
    double  barycorr    = 0.0;     // [km/s]
    bool    fitTelluric = true;

    // DB linkage back to our Spectrum object
    QString spectrumId;

    // Per-file overrides; if not set, the observation's values apply
    std::optional<QPair<double, double>> waveCut;
    std::optional<QVector<IgnoreRegion>> ignore;
    std::optional<QVector<ContinuumAnchor>> anchors;
};

struct Observation {
    QPair<double, double>     waveCut = {3600.0, 5250.0};
    QVector<IgnoreRegion>     ignore;
    QVector<ContinuumAnchor>  anchors;
    QVector<SpectrumFile>     files;
};

struct IsisOptions {
    double  xrange           = 500.0;  // plot panel width
    bool    errorEstimation  = false;  // conf_loop for uncertainties
    bool    autoFreezeVsini  = true;
    bool    addTelluricModel = false;
    bool    applyMask        = false;
    QString saveModel;                 // "", "ascii", or "fits"
    int     xfigIgnore       = -1;
};

struct IsisInteractiveOptions {
    bool    rvCorrection = false;
    QString rvAnchors    = "[[3000:6500:500],[6500:25500:1000]]";
    QString macrobroadening = "r";   // "r" = rotation, "rm" = rotation + macroturbulence
};

struct SpectralFitJob {
    QVector<StellarComponent> components;
    QVector<Observation>      observations;

    // Parameters that should vary per-spectrum (not tied across the group)
    QStringList untiedParams = { "vrad" };

    // Numeric knobs
    double filterSnr       = 5.0;
    double requireBlue     = 0.0;
    int    nitNoiseMax     = 5;
    double outlierSigmaLo  = 3.0;
    double outlierSigmaHi  = 3.0;
    bool   verbose         = true;

    // Fit the Earth's atmosphere as a multiplicative component. Needs the ESO
    // transmission library under <basePath>/telluric/, and does nothing
    // blueward of ~5700 Å where there is nothing to model.
    bool   addTelluricModel = false;

    // Multi-component fits: drop a second component whose surface ratio the
    // converged fit cannot detect. Off by default, so "two components" means
    // "fit two components" unless the user says otherwise.
    bool   autoFreezeSurRatio = false;
    double surRatioThres      = 5.0;
    double c2DetectionThres   = 0.05;

    // Refit K times with jittered continuum anchors and fold the resulting
    // scatter into the reported errors; 0 disables (faster).
    int    contJitterK     = 6;

    // Worker threads the backend may use; 0 = one per logical core. Drives
    // GAEL's internal parallelism and how many jitter refits run at once, so
    // it is also the knob that bounds a fit's peak memory.
    int    workerThreads   = 0;

    // ASTRA-side
    QString outputPath;                  // temp dir for intermediate files
    QStringList basePaths;               // grid search paths (GAEL gs.base_paths)
    QString backend = "GAEL";           // which IFitBackend to use

    // Id of the RemoteHost this fit should run on; empty = run locally.
    // Only the GAEL backend can run remotely.
    QString executionHost;
    IsisOptions            isis;
    IsisInteractiveOptions isisInteractive;   // used only by ISIS (interactive)
};

// The job settings that are *not* per-spectrum, split out so a job can be
// assembled from a plain struct instead of from FitSetupWidget's widgets.
// The defaults are read straight off a default SpectralFitJob: spelling them
// out a second time is how the two would silently drift apart the first time
// one of them is retuned.
inline const SpectralFitJob& jobDefaults()
{
    static const SpectralFitJob d;
    return d;
}

struct JobGlobals {
    QString     backend      = jobDefaults().backend;
    QStringList untiedParams = jobDefaults().untiedParams;

    double filterSnr      = jobDefaults().filterSnr;
    double requireBlue    = jobDefaults().requireBlue;
    int    nitNoiseMax    = jobDefaults().nitNoiseMax;
    double outlierSigmaLo = jobDefaults().outlierSigmaLo;
    double outlierSigmaHi = jobDefaults().outlierSigmaHi;
    bool   verbose        = jobDefaults().verbose;

    bool   addTelluricModel   = jobDefaults().addTelluricModel;
    bool   autoFreezeSurRatio = jobDefaults().autoFreezeSurRatio;
    double surRatioThres      = jobDefaults().surRatioThres;
    double c2DetectionThres   = jobDefaults().c2DetectionThres;
    int    contJitterK        = jobDefaults().contJitterK;
    int    workerThreads      = jobDefaults().workerThreads;

    QStringList basePaths = jobDefaults().basePaths;

    QString executionHost = jobDefaults().executionHost;

    IsisOptions            isis            = jobDefaults().isis;
    IsisInteractiveOptions isisInteractive = jobDefaults().isisInteractive;
};

// ────────────────────────────────────────────────────────────────────
// Output
// ────────────────────────────────────────────────────────────────────

struct FittedParameter {
    double value       = 0.0;
    double error       = 0.0;
    bool   frozen      = false;
    bool   atBoundary  = false;

    // Which side of its allowed range the value is pinned against: -1 at the
    // lower limit, +1 at the upper one, 0 in the interior. For an abundance
    // this is what separates a measurement from a limit: pinned at the bottom
    // of its grid axis means the lines are not detected (an upper limit),
    // pinned at the top means a lower limit. `atBoundary` == (boundarySide != 0).
    int    boundarySide = 0;
};

struct FittedComponent {
    // Tied params have size 1; untied have size N_spectra.
    QVector<FittedParameter> teff;
    QVector<FittedParameter> logg;
    QVector<FittedParameter> vsini;
    QVector<FittedParameter> he;
    QVector<FittedParameter> zeta;
    QVector<FittedParameter> xi;
    QVector<FittedParameter> z;
    QVector<FittedParameter> vrad;
    QVector<FittedParameter> surRatio;

    // Element abundances keyed by the grid's species name ("FE", "SI", …);
    // present for every element the grid resolves, frozen ones included.
    QMap<QString, QVector<FittedParameter>> abundances;
};

struct FittedSpectrum {
    QString             spectrumId;   // back-reference
    QVector<double>     lambda;
    QVector<double>     flux;
    QVector<double>     sigma;
    QVector<double>     model;
    QVector<double>     continuum;
    QVector<uint8_t>    ignoreFlag;   // 1 = used, 0 = masked
    QVector<double>     contX;        // continuum anchor X
    QVector<double>     contY;        // continuum anchor Y

    // Each component's own model on the same lambda grid, in component order:
    // the fitted continuum (and telluric, when one was fitted) times that
    // component's normalised flux alone - what the spectrum would look like if
    // that component were the only star in it, so its lines appear at full
    // depth rather than diluted by the other's light. One entry per component;
    // for a single-component fit the entry equals `model`.
    QVector<QVector<double>> componentModels;

    // Fitted telluric transmission on the same lambda grid; empty when this
    // spectrum had no telluric component. `model` already includes it.
    QVector<double>     telluric;
    FittedParameter     tellAirmass;
    FittedParameter     tellPwv;
    FittedParameter     tellBarycorr;
    bool                hasTelluric = false;
};

// One progress update from a backend. `fraction` < 0 means "indeterminate":
// the backend cannot say how far along it is and the bar should go busy.
// `etaSeconds` < 0 means the backend has no estimate; elapsed time is the
// dialog's own business, so it is deliberately not carried here.
struct FitProgressInfo {
    QString stage;        // "Stage 4 · first full fit"
    QString detail;       // "LM iteration 37/200  |  29 free parameters  |  …"
    double  fraction   = -1.0;
    double  etaSeconds = -1.0;
};

struct SpectralFitResult {
    bool   success          = false;
    // Distinguishes "the user pressed Abort" from a genuine failure: both
    // leave success == false, but only one deserves an error banner.
    bool   aborted          = false;
    QString errorMessage;

    double finalChi2        = 0.0;
    int    iterations       = 0;
    int    nFreeParameters  = 0;
    int    nDataPoints      = 0;
    bool   converged        = false;

    QVector<FittedComponent> components;
    QVector<FittedSpectrum>  spectra;
    QStringList              rejectedFiles;
};

} // namespace astra::fitting

Q_DECLARE_METATYPE(astra::fitting::SpectralFitResult)
Q_DECLARE_METATYPE(astra::fitting::SpectralFitJob)
Q_DECLARE_METATYPE(astra::fitting::FitProgressInfo)