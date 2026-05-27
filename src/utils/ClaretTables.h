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