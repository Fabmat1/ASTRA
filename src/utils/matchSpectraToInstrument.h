#pragma once

#include "models/Instrument.h"
#include <QString>
#include <memory>
#include <vector>

// One contiguous block of samples (an "arm") within a spectrum.
struct WavelengthSegment {
    double minWl = 0, maxWl = 0;
    int    nPoints    = 0;
    double medianStep = 0; // NEW
    double width() const { return maxWl - minWl; }
};

// Structural description of a spectrum's wavelength sampling.
struct SpectrumShape {
    double wlMin       = 0.0; // global extremes
    double wlMax       = 0.0;
    int    nPoints     = 0;
    double medianStep  = 0.0; // robust per-pixel dispersion
    double coveredSpan = 0.0; // sum of segment widths (EXCLUDES gaps)
    double largestGap  = 0.0; // biggest internal gap, Å
    std::vector<WavelengthSegment> segments;

    bool isMultiArm() const { return segments.size() > 1; }
};

struct InstrumentMatch {
    const Instrument *instrument = nullptr;
    QString           modeKey;
    QString           displayString;
    double            confidence = 0.0;
};

/// An instrument name stripped of its decoration (-, _, /, ., whitespace) and
/// lower-cased. Archives and file headers punctuate the same instrument
/// differently - ESO's ObsCore says "XSHOOTER" where this database says
/// "X-Shooter" - and comparing these keys is what bridges that. Used only as
/// the last resort in name resolution, so it can turn "unresolved" into
/// "resolved" but never redirect a match that already worked.
QString instrumentNameKey(const QString &name);

SpectrumShape analyzeWavelengthShape(const std::vector<double> &wavelengths);

// What the caller already knows about where a spectrum came from.
//
// A file header offers a name and nothing else, so the shape matcher has to
// decide the instrument as well as the mode. An archive is not guessing: it
// states the instrument outright and usually the resolving power too, and
// wavelength coverage alone is weak evidence against that - overlapping
// spectrographs exist, and letting shape outvote the archive is how an ESO
// UVES product ends up filed under a different instrument entirely.
struct InstrumentPrior {
    QString hint;               // free-text name, from a header or an archive
    // When set, the search is restricted to this configured instrument and
    // only the mode is in question. The match then always names it, whatever
    // the shape score, so the caller must not gate on confidence.
    QString knownInstrumentId;
    QString knownModeKey;       // mode the source named outright; used as-is
    double  reportedResolution = 0.0;   // lambda/dlambda, 0 when unknown
};

InstrumentMatch matchSpectrumToInstrument(
    const std::vector<std::shared_ptr<Instrument>> &instruments,
    const QString &instrumentHint, const std::vector<double> &wavelengths);

InstrumentMatch matchSpectrumToInstrument(
    const std::vector<std::shared_ptr<Instrument>> &instruments,
    const InstrumentPrior &prior, const std::vector<double> &wavelengths);