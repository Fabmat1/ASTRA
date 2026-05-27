#include "LCFitPhysics.h"

#include <QFile>
#include <QRegularExpression>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace LCFitPhysics {

// ── AsymMeasurement helpers ────────────────────────────────────────

QString AsymMeasurement::toPriorString() const {
  return QString::number(value, 'g', 12) + ' ' +
         QString::number(errLo, 'g', 12) + ' ' +
         QString::number(errHi, 'g', 12);
}

std::optional<AsymMeasurement> AsymMeasurement::parse(const QString &s) {
  static const QRegularExpression splitter(R"(\s+)");
  const QString t = s.trimmed();
  if (t.isEmpty())
    return std::nullopt;
  const auto parts = t.split(splitter, Qt::SkipEmptyParts);
  bool ok = false;
  AsymMeasurement m;
  m.value = parts[0].toDouble(&ok);
  if (!ok)
    return std::nullopt;
  if (parts.size() == 1) {
    m.errLo = m.errHi = 0.0;
    return m;
  }
  m.errLo = std::abs(parts[1].toDouble(&ok));
  if (!ok)
    return std::nullopt;
  if (parts.size() == 2) {
    m.errHi = m.errLo;
    return m;
  }
  m.errHi = std::abs(parts[2].toDouble(&ok));
  if (!ok)
    return std::nullopt;
  return m;
}

int Observables::count() const {
  int n = 0;
  for (const auto *p : {&K1, &K2, &M1, &M2, &R1, &Mt, &qObs, &logg1})
    if (p->has_value() && p->value().isValid())
      ++n;
  return n;
}

// ── Physics ────────────────────────────────────────────────────────

Implied impliedFromParams(double iDeg, double q, double vs, double r1,
                          double Pdays, std::optional<double> r2) {
  const double si = std::sin(iDeg * kDeg2Rad);
  const double Psec = Pdays * kDay2Sec;
  const double aKm = vs * Psec / (2.0 * M_PI);
  const double Mt =
      4.0 * M_PI * M_PI * aKm * aKm * aKm / (kGMsun * Psec * Psec);

  Implied r;
  r.q = q;
  r.K1 = vs * si * q / (1.0 + q);
  r.K2 = vs * si / (1.0 + q);
  r.R1 = r1 * aKm / kRsunKm;
  r.M1 = Mt / (1.0 + q);
  r.M2 = q * Mt / (1.0 + q);
  r.Mt = Mt;
  r.aRs = aKm / kRsunKm;

  if (r.R1 > 0 && r.M1 > 0)
    r.logg1 = kLoggSun + std::log10(r.M1) - 2.0 * std::log10(r.R1);
  if (r2 && *r2 > 0 && *r2 < 1) {
    const double R2 = *r2 * aKm / kRsunKm;
    r.R2 = R2;
    if (R2 > 0 && r.M2 > 0)
      r.logg2 = kLoggSun + std::log10(r.M2) - 2.0 * std::log10(R2);
  }
  return r;
}

std::optional<std::tuple<double, double, double>>
solveExact(double iDeg, double K1, double M1, double R1Rsun, double Pdays) {
  const double si = std::sin(iDeg * kDeg2Rad);
  if (si < 0.01)
    return std::nullopt;
  const double Psec = Pdays * kDay2Sec;
  const double rhs =
      2.0 * M_PI * kGMsun * M1 * si * si * si / (K1 * K1 * K1 * Psec);

  // Bisect on log(q): (1+q)²/q³ = rhs (monotonic in q on (0, ∞))
  double lo = std::log(1e-4), hi = std::log(1e4);
  for (int it = 0; it < 200; ++it) {
    const double m = 0.5 * (lo + hi);
    const double q = std::exp(m);
    if ((1.0 + q) * (1.0 + q) / (q * q * q) > rhs)
      lo = m;
    else
      hi = m;
    if (hi - lo < 1e-13)
      break;
  }
  const double q = std::exp(0.5 * (lo + hi));
  const double vs = K1 * (1.0 + q) / (q * si);
  const double aKm = vs * Psec / (2.0 * M_PI);
  const double r1 = R1Rsun * kRsunKm / aKm;
  if (r1 <= 0 || r1 >= 1)
    return std::nullopt;
  return std::make_tuple(q, vs, r1);
}

double wdRadiusRsun(double M) {
  if (M <= 0.0 || M >= 1.44)
    return 0.012;
  const double mu = M / 1.454;
  const double mu23 = std::pow(mu, -2.0 / 3.0);
  const double mu23p = std::pow(mu, 2.0 / 3.0);
  const double R = 0.0114 * std::sqrt(mu23 - mu23p) *
                   std::pow(1.0 + 3.5 * mu23 + 1.0 / mu, -2.0 / 3.0);
  return std::max(R, 0.003);
}

double estimateR2(std::optional<double> M2est, double Pdays, double vs,
                  ClaretTables::StarType type) {
  const double aKm = vs * Pdays * kDay2Sec / (2.0 * M_PI);
  double R2Rsun;
  if (type == ClaretTables::StarType::WD) {
    const double M = (M2est && *M2est > 0) ? *M2est : 0.6;
    R2Rsun = wdRadiusRsun(M);
  } else if (!M2est || *M2est <= 0) {
    R2Rsun = 0.3;
  } else {
    R2Rsun = (*M2est < 1.0) ? std::pow(*M2est, 0.8) : std::pow(*M2est, 0.57);
  }
  return std::clamp(R2Rsun * kRsunKm / aKm, 1e-4, 0.95);
}

// ── Nelder-Mead simplex (4D, with bound clamping) ──────────────────

template <typename F>
static std::vector<double>
nelderMead(F &&f, std::vector<double> x0,
           const std::vector<std::pair<double, double>> &bounds,
           int maxIter = 1500, double xtol = 1e-7) {
  const int n = int(x0.size());
  auto clamp = [&](std::vector<double> &x) {
    for (int i = 0; i < n; ++i)
      x[i] = std::clamp(x[i], bounds[i].first, bounds[i].second);
  };
  clamp(x0);

  std::vector<std::vector<double>> S(n + 1, x0);
  std::vector<double> fS(n + 1);
  fS[0] = f(S[0]);
  for (int i = 0; i < n; ++i) {
    const double step = std::max(0.05 * std::abs(x0[i]), 0.05);
    S[i + 1][i] += step;
    clamp(S[i + 1]);
    fS[i + 1] = f(S[i + 1]);
  }

  std::vector<int> order(n + 1);
  for (int it = 0; it < maxIter; ++it) {
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](int a, int b) { return fS[a] < fS[b]; });

    const int worst = order[n], second = order[n - 1], best = order[0];
    double dx = 0.0;
    for (int i = 0; i < n; ++i)
      dx = std::max(dx, std::abs(S[worst][i] - S[best][i]));
    if (dx < xtol)
      break;

    std::vector<double> c(n, 0.0);
    for (int i = 0; i < n; ++i)
      for (int k = 0; k < n; ++k)
        c[k] += S[order[i]][k];
    for (double &v : c)
      v /= n;

    std::vector<double> xr(n);
    for (int k = 0; k < n; ++k)
      xr[k] = c[k] + (c[k] - S[worst][k]);
    clamp(xr);
    const double fr = f(xr);

    if (fr < fS[best]) {
      std::vector<double> xe(n);
      for (int k = 0; k < n; ++k)
        xe[k] = c[k] + 2.0 * (c[k] - S[worst][k]);
      clamp(xe);
      const double fe = f(xe);
      if (fe < fr) {
        S[worst] = xe;
        fS[worst] = fe;
      } else {
        S[worst] = xr;
        fS[worst] = fr;
      }
    } else if (fr < fS[second]) {
      S[worst] = xr;
      fS[worst] = fr;
    } else {
      std::vector<double> xc(n);
      for (int k = 0; k < n; ++k)
        xc[k] = c[k] + 0.5 * (S[worst][k] - c[k]);
      clamp(xc);
      const double fc = f(xc);
      if (fc < fS[worst]) {
        S[worst] = xc;
        fS[worst] = fc;
      } else {
        for (int i = 1; i <= n; ++i) {
          const int idx = order[i];
          for (int k = 0; k < n; ++k)
            S[idx][k] = S[best][k] + 0.5 * (S[idx][k] - S[best][k]);
          clamp(S[idx]);
          fS[idx] = f(S[idx]);
        }
      }
    }
  }
  int b = 0;
  for (int i = 1; i <= n; ++i)
    if (fS[i] < fS[b])
      b = i;
  return S[b];
}

StartParams optimiseStart(double iDegInit, double Pdays, const Observables &obs,
                          bool iFree) {
  auto chi2 = [&](const std::vector<double> &x) -> double {
    const double iD = iFree ? x[0] : iDegInit;
    const double q = std::exp(x[1]);
    const double vs = std::exp(x[2]);
    const double r1 = 1.0 / (1.0 + std::exp(-x[3]));
    const double si = std::sin(iD * kDeg2Rad);
    if (si < 1e-6)
      return 1e20;

    const auto imp = impliedFromParams(iD, q, vs, r1, Pdays);
    double c2 = 0.0;
    auto add = [&](const std::optional<AsymMeasurement> &m, double mv) {
      if (m && m->isValid()) {
        const double p = m->pull(mv);
        c2 += p * p;
      }
    };
    add(obs.K1, imp.K1);
    add(obs.K2, imp.K2);
    add(obs.M1, imp.M1);
    add(obs.M2, imp.M2);
    add(obs.R1, imp.R1);
    add(obs.Mt, imp.Mt);
    add(obs.qObs, q);
    if (obs.logg1 && obs.logg1->isValid() && imp.logg1) {
      const double p = obs.logg1->pull(*imp.logg1);
      c2 += p * p;
    }
    c2 += 0.01 * std::pow((iD - 80.0) / 20.0, 2);
    if (si > 0)
      c2 -= 0.5 * std::log(si);
    return c2;
  };

  // Initial guess: exact solve if K1/M1/R1 are present, else defaults
  double q0 = 1.0, vs0 = 200.0, r10 = 0.2;
  if (obs.K1 && obs.M1 && obs.R1) {
    if (auto ex = solveExact(iDegInit, obs.K1->value, obs.M1->value,
                             obs.R1->value, Pdays))
      std::tie(q0, vs0, r10) = *ex;
  }
  const std::vector<double> x0 = {
      iDegInit,
      std::log(std::max(q0, 1e-4)),
      std::log(std::max(vs0, 1.0)),
      std::log(r10 / std::max(1e-6, 1.0 - r10)),
  };
  const std::vector<std::pair<double, double>> bounds = {
      iFree ? std::make_pair(5.0, 89.99)
            : std::make_pair(iDegInit - 1e-3, iDegInit + 1e-3),
      {std::log(0.01), std::log(100.0)},
      {std::log(10.0), std::log(3000.0)},
      {-6.0, 6.0},
  };

  const std::vector<double> seeds =
      iFree ? std::vector<double>{iDegInit, 80.0, 60.0, 45.0, 70.0}
            : std::vector<double>{iDegInit};

  std::vector<double> best = x0;
  double bestF = std::numeric_limits<double>::infinity();
  for (double iSeed : seeds) {
    std::vector<double> xt = x0;
    xt[0] = std::clamp(iSeed, bounds[0].first, bounds[0].second);
    auto r = nelderMead(chi2, xt, bounds);
    const double fr = chi2(r);
    if (fr < bestF) {
      bestF = fr;
      best = r;
    }
  }

  StartParams sp;
  sp.i = iFree ? best[0] : iDegInit;
  sp.q = std::exp(best[1]);
  sp.vs = std::exp(best[2]);
  sp.r1 = 1.0 / (1.0 + std::exp(-best[3]));
  return sp;
}

// ── Config string helpers ──────────────────────────────────────────

static QString fmt(double v) { return QString::number(v, 'g', 12); }

static QString lcurveParam(double value, double range, double step, bool vary,
                           bool defined = true) {
  return QString("%1 %2 %3 %4 %5")
      .arg(fmt(value), fmt(range), fmt(step))
      .arg(vary ? 1 : 0)
      .arg(defined ? 1 : 0);
}

QMap<QString, QString> buildModelParameters(const ModelInputs &in) {
  auto V = [&](const QString &k) { return in.varied.contains(k); };
  auto radiusRange = [](double v) {
    if (v < 0.5)
      return std::max(std::min(0.3 * std::max(v, 0.01), 0.5 - v), 0.001);
    return 0.01;
  };

  const double qRange = std::min(2.0 * in.q, 5.0);
  const double iRange = std::min({in.i, 90.0 - in.i, 45.0});
  const double vsRange = std::max(0.5 * in.vs, 10.0);
  const double t1Range = std::max(0.5 * in.t1, 1000.0);
  const double t2Range = std::max(0.5 * in.t2, 1000.0);

  QMap<QString, QString> p;
  p["q"] = lcurveParam(in.q, qRange, std::max(in.q * 0.02, 0.001), V("q"));
  p["iangle"] = lcurveParam(in.i, iRange, 1.0, V("iangle"));
  p["r1"] = lcurveParam(in.r1, radiusRange(in.r1),
                        std::max(in.r1 * 0.010, 1e-5), V("r1"));
  p["r2"] = lcurveParam(in.r2, radiusRange(in.r2),
                        std::max(in.r2 * 0.015, 1e-5), V("r2"));
  p["velocity_scale"] = lcurveParam(in.vs, vsRange, std::max(in.vs * 0.02, 0.1),
                                    V("velocity_scale"));
  p["t1"] = lcurveParam(in.t1, t1Range, std::max(in.t1 * 0.005, 10.0), V("t1"));
  p["t2"] = lcurveParam(in.t2, t2Range, std::max(in.t2 * 0.02, 10.0), V("t2"));

  for (int j = 0; j < 4; ++j) {
    p[QString("ldc1_%1").arg(j + 1)] =
        lcurveParam(in.ldc1[j], 0.5, 0.001, V(QString("ldc1_%1").arg(j + 1)));
    p[QString("ldc2_%1").arg(j + 1)] =
        lcurveParam(in.ldc2[j], 0.5, 0.001, V(QString("ldc2_%1").arg(j + 1)));
  }

  p["beam_factor1"] = lcurveParam(in.bf1, 1.0, 0.01, V("beam_factor1"));
  p["beam_factor2"] = lcurveParam(in.bf2, 1.0, 0.01, V("beam_factor2"));
  p["t0"] = lcurveParam(in.t0, 0.1, 1e-5, V("t0"));
  p["period"] = lcurveParam(1.0, 0.001, 1e-8, V("period"));
  p["pdot"] = lcurveParam(0.0, 0.01, 1e-5, V("pdot"));
  p["deltat"] = lcurveParam(0.0, 0.001, 1e-4, V("deltat"));
  p["gravity_dark1"] = lcurveParam(in.gd1, 0.1, 1e-6, V("gravity_dark1"));
  p["gravity_dark2"] = lcurveParam(in.gd2, 0.1, 1e-6, V("gravity_dark2"));

  // Fixed values
  p["absorb"] = lcurveParam(1.0, 0.5, 0.01, false);
  p["cphi3"] = lcurveParam(0.01, 0.05, 0.01, false);
  p["cphi4"] = lcurveParam(0.055, 0.05, 0.01, false);
  p["spin1"] = lcurveParam(1.0, 0.1, 0.01, false);
  p["spin2"] = lcurveParam(1.0, 0.1, 0.01, false);
  for (const char *k : {"slope", "quad", "cube", "third"})
    p[k] = lcurveParam(0.0, 0.01, 1e-5, false);

  struct PD {
    const char *name;
    double val, rng, step;
  };
  for (const PD &d : std::array<PD, 8>{{
           {"rdisc1", 0, 0.01, 0.001},
           {"rdisc2", 0, 0.01, 0.02},
           {"height_disc", 0, 0.01, 1e-5},
           {"beta_disc", 0, 0.01, 1e-5},
           {"temp_disc", 0, 50, 40},
           {"texp_disc", 0, 0.2, 0.001},
           {"lin_limb_disc", 0, 0.02, 1e-4},
           {"quad_limb_disc", 0, 0.02, 1e-4},
       }})
    p[d.name] = lcurveParam(d.val, d.rng, d.step, false);

  for (const PD &d : std::array<PD, 10>{{
           {"radius_spot", 0, 0.01, 0.01},
           {"length_spot", 0, 0.01, 0.005},
           {"height_spot", 0, 0.01, 1e-5},
           {"expon_spot", 0, 0.2, 0.1},
           {"epow_spot", 0, 0.01, 0.01},
           {"angle_spot", 0, 5, 2},
           {"yaw_spot", 0, 5, 2},
           {"temp_spot", 0, 500, 200},
           {"tilt_spot", 0, 5, 2},
           {"cfrac_spot", 0, 0.05, 0.008},
       }})
    p[d.name] = lcurveParam(d.val, d.rng, d.step, false);

  for (const char *star : {"1", "2"})
    for (const char *attr : {"long", "lat", "fwhm", "tcen"})
      p[QString("stsp%1_1_%2").arg(star, attr)] =
          lcurveParam(0, 0, 0, false, false);

  // Scalar grid / control parameters
  const QMap<QString, QString> scalars = {
      {"delta_phase", "1e-07"},
      {"nlat1f", "50"},
      {"nlat2f", "150"},
      {"nlat1c", "50"},
      {"nlat2c", "150"},
      {"npole", "1"},
      {"nlatfill", "2"},
      {"nlngfill", "2"},
      {"lfudge", "0"},
      {"llo", "90"},
      {"lhi", "-90"},
      {"phase1", "0.1"},
      {"phase2", "0.4"},
      {"roche1", "1"},
      {"roche2", "1"},
      {"eclipse1", "1"},
      {"eclipse2", "1"},
      {"glens1", "0"},
      {"use_radii", "1"},
      {"gdark_bolom1", "1"},
      {"gdark_bolom2", "1"},
      {"mucrit1", "0"},
      {"mucrit2", "0"},
      {"limb1", "Claret"},
      {"limb2", "Claret"},
      {"mirror", "0"},
      {"add_disc", "0"},
      {"nrad", "40"},
      {"opaque", "0"},
      {"add_spot", "0"},
      {"nspot", "0"},
      {"iscale", "0"},
      {"wavelength", fmt(in.wavelengthNm)},
      {"tperiod", fmt(in.period)},
  };
  for (auto it = scalars.cbegin(); it != scalars.cend(); ++it)
    p[it.key()] = it.value();

  return p;
}

QMap<QString, QString> buildPriors(const PriorInputs &in) {
  QMap<QString, QString> out;
  auto add = [&](const std::optional<AsymMeasurement> &m, const char *name) {
    if (m && m->isValid())
      out[name] = m->toPriorString();
  };
  add(in.K1, "K1");
  add(in.K2, "K2");
  add(in.M1, "M1");
  add(in.M2, "M2");
  add(in.M2min, "M2_min");
  add(in.Mtotal, "M_total");
  add(in.q, "q");
  add(in.R1, "R1");
  add(in.R2, "R2");
  add(in.logg1, "logg1");
  add(in.logg2, "logg2");
  add(in.T1, "T1");
  add(in.T2, "T2");
  return out;
}

int countDataPoints(const QString &filepath) {
  QFile f(filepath);
  if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
    return 0;
  int n = 0;
  QTextStream s(&f);
  while (!s.atEnd()) {
    const QString line = s.readLine().trimmed();
    if (!line.isEmpty() && !line.startsWith('#') && !line.startsWith('!'))
      ++n;
  }
  return n;
}

} // namespace LCFitPhysics