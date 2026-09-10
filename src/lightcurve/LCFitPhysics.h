#pragma once

#include "lightcurve/ClaretTables.h"

#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <array>
#include <optional>
#include <tuple>
#include <vector>

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

// ── Tying an RV orbit to a light-curve ephemeris ──────────────────
//
// lcurve puts star 1 at the origin, star 2 at (1,0,0), and set_earth() points
// the Earth vector at +x at phase 0, so its ephemeris zero point is the
// conjunction with star 1 BEHIND star 2. Star 1's line-of-sight offset there is
// -mu*sin(i)*cos(2*pi*phi), giving a radial velocity of gamma - K*sin(2*pi*phi):
// at lcurve phase 0 that crosses gamma on the way DOWN, the descending node.
// ASTRA's circular model is gamma + K*sin(2*pi*phi), whose phi = 0 is the
// ascending node. The two conventions are therefore exactly half a cycle apart.
//
// That offset follows from the model's coordinate definition alone. It holds for
// every system the fitter can describe - reflection binaries, eclipsers, CVs
// with a disc and a bright spot, anything - and mentions no morphology.
//
// Returns the phase for a fit referenced to `tRefBJD`, wrapped to [0, 1).
double rvPhaseLockedToLcT0(double t0LcBJD, double tRefBJD, double period);

// How well a fitted light-curve model can tell orbital phase from phase + 0.5.
//
// lcurve's zero point fixes the orbital phase only as far as the model is
// asymmetric under a half-cycle shift. A reflection hump, unequal eclipses, a
// bright spot or Doppler beaming all break that symmetry; a purely ellipsoidal
// curve does not, and one fitted at half the orbital period cannot by
// construction. Rather than enumerate morphologies, ask the fitted model: shift
// it by half an orbit and measure how much worse it fits its own data.
struct HalfCycleEvidence {
  bool usable = false;    ///< the model carries trustworthy phase information
  double deltaChi2 = 0.0; ///< chi2 penalty for shifting the model half a cycle
  double detection = 0.0; ///< chi2 by which the model beats a flat light curve
};

// A model must beat a flat light curve by this much before its ephemeris is
// trusted at all (~5 sigma on one degree of freedom). Below it the curve is
// consistent with no variability and carries no phase information: flickering a
// CV model has no term for, a light curve that is mostly noise, a fit that never
// converged.
inline constexpr double kMinLcDetectionChi2 = 25.0;

// The bar a vote for one branch over the other has to clear before it counts,
// applied identically to the light curve's chi2 penalty and to the RVs' orbit
// detection so the two are judged by the same standard (~3 sigma on one degree
// of freedom). A symmetric curve clears it only on floating-point noise, which
// is precisely what it must not do.
inline constexpr double kMinBranchChi2 = 9.0;

// Below this many usable bins the shifted-model comparison is not meaningful.
inline constexpr int kMinLcPhaseBins = 8;

// `shift` is half an orbit expressed in the MODEL's own phase units: 0.5 when
// the light curve was fitted at the orbital period, and exactly 1.0 when it was
// fitted at half of it. The latter is a deliberate no-op that reports zero
// information, which is the truth: such a curve cannot distinguish the two
// conjunctions even in principle.
//
// Both chi2 differences are divided by the fit's own reduced chi2 whenever that
// exceeds 1, so a model that reproduces the data badly has its vote deflated in
// proportion, and catalogue error bars quoted too small cannot inflate it.
HalfCycleEvidence halfCycleEvidence(const std::vector<double> &phase,
                                    const std::vector<double> &flux,
                                    const std::vector<double> &fluxError,
                                    const std::vector<double> &modelPhase,
                                    const std::vector<double> &modelFlux,
                                    double shift);

// ── Free-parameter catalogue ──────────────────────────────────────
// Every model parameter lcurve is able to fit. buildModelParameters()
// honours ModelInputs::varied for each of these keys; everything else it
// writes is a computational scalar (grid resolution, switches) with no vary
// flag at all, and star spots, which ASTRA does not configure.
struct VaryableParam {
  /// Switch on the Advanced page lcurve needs before it so much as looks at
  /// the parameter - freeing a gated-off one costs nothing and achieves
  /// nothing.
  enum class Gate { None, DiscOn, SpotOn, RadiiOff };

  QString key;
  QString group;
  QString description;
  Gate    gate = Gate::None;
  /// True when no other page of the fit dialog edits this parameter, so its
  /// starting value has to be given wherever it is set free. The defaults
  /// below are legal starting points; lcurve rejects a step outside
  /// [lo, hi] outright, and most of these parameters default to 0, which is
  /// itself illegal for several of them.
  bool   needsStartEditor = false;
  double start = 0.0;
  double range = 0.0; ///< Search half-width offered next to the start value
  double lo = 0.0, hi = 0.0; ///< lcurve's legality bounds
  int    decimals = 4;
};

const QVector<VaryableParam> &varyableParameters();
/// The catalogue entry for `key`, or nullptr when lcurve cannot fit it.
const VaryableParam *varyableParameter(const QString &key);

// "name → 'v eLo eHi'" map for the priors block.
struct PriorInputs {
  std::optional<AsymMeasurement> K1, K2, M1, M2, M2min, Mtotal, q;
  std::optional<AsymMeasurement> R1, R2;
  std::optional<AsymMeasurement> logg1, logg2;
  std::optional<AsymMeasurement> T1, T2;
};
QMap<QString, QString> buildPriors(const PriorInputs &in);


} // namespace LCFitPhysics