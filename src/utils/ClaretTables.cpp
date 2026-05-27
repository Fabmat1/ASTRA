#include "ClaretTables.h"

#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QTextStream>

#include <cmath>
#include <limits>

namespace {

struct TableSpec {
  QString resourcePath;
  int teffCol = 0;
  int loggCol = -1;
  QVector<int> targetCols;
  int filterCol = -2; // -2 = no filter, -1 = last column
  QString filterValue;
  bool teffIsLog = false;
};

QString filter2011(const QString &band) {
  static const QHash<QString, QString> m = {
      {"Kepler", "Kp"},   {"SDSS-u", "u'"},   {"SDSS-g", "g'"},
      {"SDSS-r", "r'"},   {"SDSS-i", "i'"},   {"SDSS-z", "z'"},
      {"Johnson-U", "U"}, {"Johnson-B", "B"}, {"Johnson-V", "V"},
      {"Johnson-R", "R"}, {"Johnson-I", "I"},
  };
  return m.value(band);
}
QString filter2020sd(const QString &band) {
  static const QHash<QString, QString> m = {
      {"TESS", "Te"},     {"Kepler", "Ke"},   {"SDSS-u", "u'"},
      {"SDSS-g", "g'"},   {"SDSS-r", "r'"},   {"SDSS-i", "i'"},
      {"SDSS-z", "z'"},   {"Johnson-U", "U"}, {"Johnson-B", "B"},
      {"Johnson-V", "V"}, {"Johnson-R", "R"}, {"Johnson-I", "I"},
  };
  return m.value(band);
}
QString filter2020beam(const QString &band) {
  static const QHash<QString, QString> m = {
      {"TESS", "Tes"},    {"Kepler", "Ke"},   {"SDSS-u", "u'"},
      {"SDSS-g", "g'"},   {"SDSS-r", "r'"},   {"SDSS-i", "i'"},
      {"SDSS-z", "z'"},   {"Johnson-U", "U"}, {"Johnson-B", "B"},
      {"Johnson-V", "V"}, {"Johnson-R", "R"}, {"Johnson-I", "I"},
  };
  return m.value(band);
}
QString gdcBandFallback(const QString &band) {
  if (band == "TESS")
    return "Kepler";
  if (band == "SDSS-z")
    return "Johnson-I";
  return {};
}

std::optional<TableSpec> ldcSpec(ClaretTables::StarType type,
                                 const QString &band) {
  using ClaretTables::StarType;
  if (type == StarType::MS) {
    if (band.compare("TESS", Qt::CaseInsensitive) == 0)
      return TableSpec{":/claret_tables/Claret2018_MS_TESS_LDC.dat",
                       1,
                       3,
                       {4, 5, 6, 7},
                       -2,
                       {},
                       false};
    if (band.compare("Kepler", Qt::CaseInsensitive) == 0)
      return TableSpec{":/claret_tables/Claret2018_MS_KEPLER_LDC.dat",
                       1,
                       3,
                       {4, 5, 6, 7},
                       -2,
                       {},
                       false};
    const QString f = filter2011(band);
    if (f.isEmpty())
      return std::nullopt;
    return TableSpec{":/claret_tables/Claret2011_MS_multifilter_LDC.dat",
                     1,
                     3,
                     {4, 5, 6, 7},
                     8,
                     f,
                     false};
  }
  // SD and WD share the sd tables
  const QString f = filter2020sd(band);
  if (f.isEmpty())
    return std::nullopt;
  return TableSpec{":/claret_tables/Claret2020_sd_LDC.dat",
                   2,
                   1,
                   {4, 5, 6, 7},
                   -1,
                   f,
                   false};
}

std::optional<TableSpec> gdcSpec(ClaretTables::StarType type,
                                 const QString &band) {
  using ClaretTables::StarType;
  if (type == StarType::MS) {
    QString lookup = band;
    QString f = filter2011(lookup);
    if (f.isEmpty()) {
      const QString fb = gdcBandFallback(band);
      if (!fb.isEmpty()) {
        f = filter2011(fb);
        lookup = fb;
      }
    }
    if (f.isEmpty())
      return std::nullopt;
    return TableSpec{":/claret_tables/Claret2011_GDC_MS.dat",
                     1,
                     3,
                     {4},
                     5,
                     f,
                     /*teffIsLog*/ true};
  }
  const QString f = filter2020sd(band);
  if (f.isEmpty())
    return std::nullopt;
  return TableSpec{
      ":/claret_tables/Claret2020_sd_GDC.dat", 2, 1, {4}, -1, f, false};
}

std::optional<TableSpec> beamingSpec(const QString &band) {
  const QString f = filter2020beam(band);
  if (f.isEmpty())
    return std::nullopt;
  return TableSpec{
      ":/claret_tables/Claret2020_beaming.dat", 3, 2, {5}, 1, f, false};
}

std::optional<std::tuple<QVector<double>, double, std::optional<double>>>
queryTable(const TableSpec &spec, double T, std::optional<double> logg) {
  QFile f(spec.resourcePath);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return std::nullopt;

  static const QRegularExpression splitter(R"(\s+)");

  double bestD = std::numeric_limits<double>::infinity();
  QVector<double> bestVals;
  double bestT = 0.0;
  std::optional<double> bestG;

  QTextStream s(&f);
  while (!s.atEnd()) {
    const QString line = s.readLine().trimmed();
    if (line.isEmpty() || line.startsWith('#') || line.startsWith('!'))
      continue;
    const QStringList F = line.split(splitter, Qt::SkipEmptyParts);

    // Optional filter check
    if (spec.filterCol != -2 && !spec.filterValue.isEmpty()) {
      int fc = spec.filterCol;
      if (fc < 0)
        fc = F.size() + fc;
      if (fc < 0 || fc >= F.size() || F[fc] != spec.filterValue)
        continue;
    }
    if (spec.teffCol >= F.size())
      continue;

    bool ok = false;
    const double traw = F[spec.teffCol].toDouble(&ok);
    if (!ok)
      continue;
    const double rowT = spec.teffIsLog ? std::pow(10.0, traw) : traw;

    std::optional<double> rowG;
    if (spec.loggCol >= 0 && spec.loggCol < F.size()) {
      const double g = F[spec.loggCol].toDouble(&ok);
      if (ok)
        rowG = g;
    }

    double d = std::abs(rowT - T) / std::max(T, 1.0);
    if (logg && rowG)
      d += std::abs(*rowG - *logg) / 5.0;
    if (d >= bestD)
      continue;

    QVector<double> vals;
    vals.reserve(spec.targetCols.size());
    bool good = true;
    for (int ci : spec.targetCols) {
      if (ci >= F.size()) {
        good = false;
        break;
      }
      const double v = F[ci].toDouble(&ok);
      if (!ok) {
        good = false;
        break;
      }
      vals.append(v);
    }
    if (good) {
      bestD = d;
      bestVals = vals;
      bestT = rowT;
      bestG = rowG;
    }
  }
  if (bestVals.isEmpty())
    return std::nullopt;
  return std::make_tuple(bestVals, bestT, bestG);
}

std::array<double, 4> defaultLdc(double T) {
  if (T > 20000)
    return {0.26, 0.12, -0.10, 0.03};
  if (T > 10000)
    return {0.38, 0.10, -0.05, 0.01};
  if (T > 6000)
    return {0.45, 0.15, -0.08, 0.02};
  return {0.55, 0.20, -0.10, 0.03};
}

double analyticBeaming(double T, double wlNm) {
  const double x = 1.4388e7 / (T * wlNm);
  if (x > 500.0)
    return 5.0;
  if (x < 0.01)
    return 3.0;
  const double ex = std::exp(x);
  return std::max(0.1, 6.0 - x * ex / (ex - 1.0));
}

QString fileNameOf(const QString &path) { return QFileInfo(path).fileName(); }

} // namespace

namespace ClaretTables {

StarType parseStarType(const QString &s) {
  const QString l = s.toLower();
  if (l == "sd")
    return StarType::SD;
  if (l == "wd")
    return StarType::WD;
  return StarType::MS;
}
QString starTypeToString(StarType t) {
  switch (t) {
  case StarType::SD:
    return "sd";
  case StarType::WD:
    return "wd";
  default:
    return "ms";
  }
}

double bandWavelengthNm(const QString &band) {
  static const QHash<QString, double> w = {
      {"TESS", 786.5},      {"Kepler", 640.0},    {"SDSS-u", 355.1},
      {"SDSS-g", 468.6},    {"SDSS-r", 616.5},    {"SDSS-i", 748.1},
      {"SDSS-z", 893.1},    {"Johnson-U", 365.0}, {"Johnson-B", 440.0},
      {"Johnson-V", 547.7}, {"Johnson-R", 640.0}, {"Johnson-I", 790.0},
  };
  return w.value(band, 786.5);
}

QStringList availableBands() {
  return {"TESS",      "Kepler",    "SDSS-u",    "SDSS-g",
          "SDSS-r",    "SDSS-i",    "SDSS-z",    "Johnson-U",
          "Johnson-B", "Johnson-V", "Johnson-R", "Johnson-I"};
}

LdcResult queryLdc(double T, std::optional<double> logg, StarType type,
                   const QString &band) {
  LdcResult out;
  out.coefficients = defaultLdc(T);

  const auto spec = ldcSpec(type, band);
  if (!spec) {
    out.usedFallback = true;
    out.diagnostic = QString("No LDC table for type/band → generic defaults");
    return out;
  }
  const auto res = queryTable(*spec, T, logg);
  if (!res) {
    out.usedFallback = true;
    out.diagnostic = QString("No row in %1 → generic defaults")
                         .arg(fileNameOf(spec->resourcePath));
    return out;
  }
  const auto &[vals, mT, mG] = *res;
  for (int i = 0; i < 4 && i < vals.size(); ++i)
    out.coefficients[i] = vals[i];
  out.diagnostic =
      QString("matched Teff=%1%2 in %3")
          .arg(mT, 0, 'f', 0)
          .arg(mG ? QString(", logg=%1").arg(*mG, 0, 'f', 1) : QString())
          .arg(fileNameOf(spec->resourcePath));
  return out;
}

ScalarResult queryGdc(double T, std::optional<double> logg, StarType type,
                      const QString &band) {
  ScalarResult out;
  out.value = (T > 7500) ? 0.25 : 0.08;

  const auto spec = gdcSpec(type, band);
  if (!spec) {
    out.usedFallback = true;
    out.diagnostic = QString("No GDC table → theoretical β=%1").arg(out.value);
    return out;
  }
  const auto res = queryTable(*spec, T, logg);
  if (!res) {
    out.usedFallback = true;
    out.diagnostic = QString("No row in %1 → theoretical β=%2")
                         .arg(fileNameOf(spec->resourcePath))
                         .arg(out.value);
    return out;
  }
  const auto &[vals, mT, mG] = *res;
  out.value = vals.value(0, out.value);
  out.diagnostic =
      QString("matched Teff=%1%2 in %3")
          .arg(mT, 0, 'f', 0)
          .arg(mG ? QString(", logg=%1").arg(*mG, 0, 'f', 1) : QString())
          .arg(fileNameOf(spec->resourcePath));
  return out;
}

ScalarResult queryBeaming(double T, std::optional<double> logg,
                          const QString &band) {
  ScalarResult out;
  const auto spec = beamingSpec(band);
  if (spec) {
    const auto res = queryTable(*spec, T, logg);
    if (res) {
      const auto &[vals, mT, mG] = *res;
      out.value = vals.value(0, 1.0);
      out.diagnostic =
          QString("matched Teff=%1%2 in %3")
              .arg(mT, 0, 'f', 0)
              .arg(mG ? QString(", logg=%1").arg(*mG, 0, 'f', 1) : QString())
              .arg(fileNameOf(spec->resourcePath));
      return out;
    }
  }
  out.value = analyticBeaming(T, bandWavelengthNm(band));
  out.usedFallback = true;
  out.diagnostic = QString("analytic B(T=%1 K, λ=%2 nm) = %3")
                       .arg(T, 0, 'f', 0)
                       .arg(bandWavelengthNm(band), 0, 'f', 1)
                       .arg(out.value, 0, 'f', 4);
  return out;
}

} // namespace ClaretTables