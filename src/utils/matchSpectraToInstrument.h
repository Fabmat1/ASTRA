#pragma once

#include "models/Instrument.h"
#include <QString>
#include <memory>
#include <vector>

// One contiguous block of samples (an "arm") within a spectrum.
struct WavelengthSegment {
    double minWl   = 0.0;
    double maxWl   = 0.0;
    int    nPoints = 0;
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

SpectrumShape analyzeWavelengthShape(const std::vector<double> &wavelengths);

InstrumentMatch matchSpectrumToInstrument(
    const std::vector<std::shared_ptr<Instrument>> &instruments,
    const QString &instrumentHint, const std::vector<double> &wavelengths);