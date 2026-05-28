#pragma once

#include <cmath>
#include <vector>

struct ScatterTable {
    std::vector<double> x;
    std::vector<double> y;
};

const ScatterTable &defaultTeffScatter();
const ScatterTable &defaultLoggScatter();
const ScatterTable &defaultHeScatter();

double teffError(double teff, double eTeff, bool heRich,
                 double              instrumentOffset = 0.008,
                 const ScatterTable &scatter          = defaultTeffScatter());

double loggError(double teff, double logg, double eLogg, bool heRich,
                 double              instrumentOffset = 0.04,
                 const ScatterTable &scatter          = defaultLoggScatter());

double heError(double teff, double he, double eHe, bool heRich,
               double              instrumentOffset = 0.03,
               const ScatterTable &scatter          = defaultHeScatter());