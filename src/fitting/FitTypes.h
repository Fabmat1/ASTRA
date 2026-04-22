#pragma once

#include <QString>
#include <QVector>
#include <QStringList>
#include <QPair>
#include <optional>

namespace astra::fitting {

// ────────────────────────────────────────────────────────────────────
// Input config
// ────────────────────────────────────────────────────────────────────

struct StellarComponent {
    QString gridPath;
    double teff   = 35000.0;
    double logg   = 5.5;
    double vsini  = 5.0;
    double he     = -1.0;
    double zeta   = 0.0;     // macroturbulence
    double xi     = 0.0;     // microturbulence
    double z      = 0.0;     // metallicity
    bool freezeTeff  = false;
    bool freezeLogg  = false;
    bool freezeVsini = false;
    bool freezeHe    = false;
    bool freezeZeta  = true;
    bool freezeXi    = true;
    bool freezeZ     = true;
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

struct SpectrumFile {
    QString filename;
    QString spectype = "ASCII_with_2_columns";
    double  resOffset = 0.0;
    double  resSlope  = 0.37037;

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

    // ASTRA-side
    QString outputPath;                  // temp dir for intermediate files
    QStringList basePaths;               // grid search paths (DIGGA gs.base_paths)
    QString backend = "DIGGA";           // which IFitBackend to use
};

// ────────────────────────────────────────────────────────────────────
// Output
// ────────────────────────────────────────────────────────────────────

struct FittedParameter {
    double value       = 0.0;
    double error       = 0.0;
    bool   frozen      = false;
    bool   atBoundary  = false;
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
};

struct SpectralFitResult {
    bool   success          = false;
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