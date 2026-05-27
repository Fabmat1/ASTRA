#pragma once

#include "ClaretTables.h"

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <array>
#include <optional>
#include <tuple>

namespace LCFitPhysics {

// ── Constants ─────────────────────────────────────────────────────
inline constexpr double kDeg2Rad = 0.017453292519943295;
inline constexpr double kDay2Sec = 86400.0;
inline constexpr double kRsunKm = 695700.0;
inline constexpr double kGMsun = 1.3271244e11; // km³ s⁻²
inline constexpr double kLoggSun = 4.4380;

// ── Asymmetric value with two-sided error bars ────────────────────
struct AsymMeasurement {
  double value = 0.0;
  double errLo = 0.0;
  double errHi = 0.0;

  bool isValid() const { return errLo > 0.0 || errHi > 0.0; }
  double sigmaFor(double m) const { return m < value ? errLo : errHi; }
  double pull(double m) const {
    double s = sigmaFor(m);
    return s > 0 ? (m - value) / s : 0.0;
  }
  QString toPriorString() const;

  static std::optional<AsymMeasurement>
  parse(const QString &s); // "v [eLo [eHi]]"
};

// ── Derived quantities ────────────────────────────────────────────
struct Implied {
  double K1 = 0, K2 = 0, R1 = 0, M1 = 0, M2 = 0, Mt = 0, aRs = 0, q = 0;
  std::optional<double> R2, logg1, logg2;
};

Implied impliedFromParams(double iDeg, double q, double vs, double r1,
                          double Pdays,
                          std::optional<double> r2 = std::nullopt);

// (q, vs, r1) from (i, K1, M1, R1, P), or empty if no physical solution.
std::optional<std::tuple<double, double, double>>
solveExact(double iDeg, double K1, double M1, double R1Rsun, double Pdays);

double wdRadiusRsun(double Mmsun);
double estimateR2(std::optional<double> M2est, double Pdays, double vs,
                  ClaretTables::StarType type);

// ── Observational constraints fed to the optimiser ────────────────
struct Observables {
  std::optional<AsymMeasurement> K1, K2, M1, M2, R1, Mt, qObs, logg1;
  int count() const;
};

struct StartParams {
  double i = 0, q = 0, vs = 0, r1 = 0;
};
StartParams optimiseStart(double iDegInit, double Pdays, const Observables &obs,
                          bool iFree);

// ── Config generation ─────────────────────────────────────────────
struct ModelInputs {
  double q = 1.0, i = 80.0, r1 = 0.2, r2 = 0.3, vs = 200.0;
  double t1 = 10000.0, t2 = 5000.0;
  std::array<double, 4> ldc1{0.4, 0.15, -0.05, 0.02};
  std::array<double, 4> ldc2{0.4, 0.15, -0.05, 0.02};
  double gd1 = 0.25, gd2 = 0.08;
  double bf1 = 1.0, bf2 = 1.0;
  double t0 = 0.0;
  double period = 1.0;
  double wavelengthNm = 786.5;
  QSet<QString> varied;
};

QMap<QString, QString> buildModelParameters(const ModelInputs &in);

// "name → 'v eLo eHi'" map for the priors block.
struct PriorInputs {
  std::optional<AsymMeasurement> K1, K2, M1, M2, M2min, Mtotal, q;
  std::optional<AsymMeasurement> R1, R2;
  std::optional<AsymMeasurement> logg1, logg2;
  std::optional<AsymMeasurement> T1, T2;
};
QMap<QString, QString> buildPriors(const PriorInputs &in);

int countDataPoints(const QString &filepath);

} // namespace LCFitPhysics