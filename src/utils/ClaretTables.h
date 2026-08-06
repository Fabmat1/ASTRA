#pragma once

#include <QString>
#include <QStringList>
#include <array>
#include <optional>

namespace ClaretTables {

enum class StarType { MS, SD, WD };

StarType parseStarType(const QString &s); // "ms" / "sd" / "wd"
QString starTypeToString(StarType t);

double bandWavelengthNm(const QString &band);
QStringList availableBands();

// The tabulated band whose effective wavelength sits closest to wavelengthNm.
// Ties break on band name so the result is stable.
QString nearestBand(double wavelengthNm);

// How well a band is served by the shipped tables, so the UI can tell the
// user what a substituted band actually buys them before they pick it.
enum class Coverage {
  Exact,       // a table row exists for this band
  Substituted, // no table for this band; a neighbouring one is read instead
  None,        // nothing tabulated - the query falls back to a generic value
};

struct BandCoverage {
  Coverage kind = Coverage::None;
  QString substitute; // band actually read when kind == Substituted
};

BandCoverage ldcCoverage(StarType type, const QString &band);
BandCoverage gdcCoverage(StarType type, const QString &band);
BandCoverage beamingCoverage(const QString &band);

struct LdcResult {
  std::array<double, 4> coefficients{0.4, 0.15, -0.05, 0.02};
  bool usedFallback = false;
  QString diagnostic;
};

struct ScalarResult {
  double value = 0.0;
  bool usedFallback = false;
  QString diagnostic;
};

LdcResult queryLdc(double Teff, std::optional<double> logg, StarType type,
                   const QString &band);
ScalarResult queryGdc(double Teff, std::optional<double> logg, StarType type,
                      const QString &band);
ScalarResult queryBeaming(double Teff, std::optional<double> logg,
                          const QString &band);

} // namespace ClaretTables