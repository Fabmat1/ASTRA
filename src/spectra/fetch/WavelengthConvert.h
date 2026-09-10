// src/utils/spectrafetch/WavelengthConvert.h
//
// Vacuum <-> air wavelength conversion. SDSS, LAMOST, and APOGEE publish
// vacuum wavelengths; ASTRA's model grids and most ground-based optical
// products use air wavelengths, so fetched vacuum spectra are converted on
// import (user-toggleable per archive).

#ifndef WAVELENGTHCONVERT_H
#define WAVELENGTHCONVERT_H

#include <vector>

namespace SpecFetch {

// One wavelength [Angstrom], vacuum -> air. Identity below 2000 A, where the
// air convention is not defined (and such UV data never reaches ground-based
// air-calibrated instruments anyway).
double vacToAirAngstrom(double lambdaVac);

// In-place conversion of a wavelength array [Angstrom].
void vacToAir(std::vector<double>& wavelengths);

}   // namespace SpecFetch

#endif   // WAVELENGTHCONVERT_H
