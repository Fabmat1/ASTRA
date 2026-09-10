// Unit tests for the RV orbit model in rv/RadialVelocity.
//
// This is the arithmetic every radial-velocity result rests on, and it has been
// corrected repeatedly: the eccentric model turned out to fold with the
// opposite phase sign to the circular one, and the lightcurve alignment was
// half a cycle out.  Nothing here was covered by a test.
//
// The Kepler solver is checked against a scipy-generated table (Brent's method
// on a bracketing interval, so the reference does not share a failure mode with
// the Newton iteration under test).  The rest is checked against the closed
// forms the model is defined by.

#include <doctest.h>

#include "rv/RadialVelocity.h"
#include "core/Time.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kPi = 3.14159265358979323846;

/// A circular SB1 fit with a reference epoch bound, as the curve loader leaves it.
RVFit makeCircularFit(double period = 3.5, double k = 42.0,
                      double gamma = -11.0, double phi = 0.2,
                      double tRef = 2458000.0)
{
    RVFit fit;
    fit.setPeriod(period);
    fit.setK(k);
    fit.setGamma(gamma);
    fit.setPhi(phi);
    fit.setReferenceTime(tRef, tRef - 2400000.5);
    return fit;
}

RVFit makeEccentricFit(double e = 0.4, double omegaDeg = 75.0,
                       double period = 3.5, double k = 42.0,
                       double gamma = -11.0, double phi = 0.2,
                       double tRef = 2458000.0)
{
    RVFit fit = makeCircularFit(period, k, gamma, phi, tRef);
    fit.setEccentricity(e);       // also flips the fit into eccentric mode
    fit.setOmega(omegaDeg);
    return fit;
}

/// Closed-form eccentric radial velocity, written out independently of the
/// implementation: v = gamma + K (cos(nu + w) + e cos w).
double referenceEccentricRV(double phase, double e, double omegaDeg,
                            double k, double gamma)
{
    const double M = 2.0 * kPi * phase;
    // Solve Kepler by bisection: slow, but shares nothing with the code above.
    double lo = M - 1.0 - e, hi = M + 1.0 + e;
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        ((mid - e * std::sin(mid) - M) < 0.0 ? lo : hi) = mid;
    }
    const double E  = 0.5 * (lo + hi);
    const double nu = 2.0 * std::atan2(std::sqrt(1.0 + e) * std::sin(0.5 * E),
                                       std::sqrt(1.0 - e) * std::cos(0.5 * E));
    const double w  = omegaDeg * kPi / 180.0;
    return gamma + k * (std::cos(nu + w) + e * std::cos(w));
}

QJsonObject loadReference(const QString& name)
{
    QFile f(QStringLiteral(ASTRA_TEST_REFERENCE_DIR) + QLatin1Char('/') + name);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

}   // namespace

TEST_SUITE("rv")
{

TEST_CASE("RVFit::solveKepler matches a scipy-generated reference")
{
    const QJsonObject ref = loadReference("kepler.json");
    REQUIRE_MESSAGE(!ref.isEmpty(),
                    "kepler.json missing; run tests/unit/generate_references.py");
    CHECK(ref.value("version").toInt() == 1);

    const QJsonArray cases = ref.value("cases").toArray();
    REQUIRE(cases.size() > 100);

    double worst = 0.0;
    for (const auto& entry : cases) {
        const QJsonObject c = entry.toObject();
        const double e = c.value("e").toDouble();
        const double m = c.value("M").toDouble();
        const double expected = c.value("E").toDouble();

        // The reference itself must be at least as good as we demand.
        REQUIRE(std::abs(c.value("residual").toDouble()) < 1e-12);

        const double got = RVFit::solveKepler(m, e);
        const double err = std::abs(got - expected);
        worst = std::max(worst, err);
        // Absolute, not relative: E is an angle in radians, so a relative
        // tolerance would be meaninglessly loose near E = pi and impossibly
        // tight near E = 0.
        CHECK(err < 1e-9);
    }
    // Newton with the Murray and Dermott starting guess should reach machine
    // precision everywhere in the table, including e = 0.99.
    CHECK(worst < 1e-9);
}

TEST_CASE("RVFit::solveKepler satisfies Kepler's equation directly")
{
    for (double e : {0.0, 0.2, 0.6, 0.9, 0.99}) {
        for (double m = -6.0; m <= 6.0; m += 0.37) {
            const double E = RVFit::solveKepler(m, e);
            // M is reduced to [-pi, pi] internally, so compare the residual
            // modulo a full turn rather than against the raw M.
            double residual = E - e * std::sin(E) - m;
            residual = std::fmod(residual, 2.0 * kPi);
            if (residual >  kPi) residual -= 2.0 * kPi;
            if (residual < -kPi) residual += 2.0 * kPi;
            CHECK(std::abs(residual) < 1e-10);
        }
    }
}

TEST_CASE("RVFit::solveKepler is the identity for a circular orbit")
{
    for (double m : {-3.0, -1.0, 0.0, 0.5, 2.0, 3.1}) {
        CHECK(RVFit::solveKepler(m, 0.0) == doctest::Approx(m));
    }
}

TEST_CASE("RVFit: the circular model peaks a quarter cycle after phase zero")
{
    const RVFit fit = makeCircularFit(3.5, 42.0, -11.0, 0.0);

    // Historical convention, relied on by the plotted curve: v = gamma + K sin(2 pi phase).
    CHECK(fit.calculateRVAtPhase(0.00) == doctest::Approx(-11.0));
    CHECK(fit.calculateRVAtPhase(0.25) == doctest::Approx(-11.0 + 42.0));
    CHECK(fit.calculateRVAtPhase(0.50) == doctest::Approx(-11.0));
    CHECK(fit.calculateRVAtPhase(0.75) == doctest::Approx(-11.0 - 42.0));

    for (double p = 0.0; p < 1.0; p += 0.05)
        CHECK(fit.calculateRVAtPhase(p) == doctest::Approx(-11.0 + 42.0 * std::sin(2.0 * kPi * p)));
}

TEST_CASE("RVFit: the model is periodic in phase")
{
    const RVFit circ = makeCircularFit();
    const RVFit ecc  = makeEccentricFit();
    for (double p = 0.0; p < 1.0; p += 0.1) {
        CHECK(circ.calculateRVAtPhase(p + 1.0) == doctest::Approx(circ.calculateRVAtPhase(p)));
        CHECK(ecc.calculateRVAtPhase(p + 3.0) == doctest::Approx(ecc.calculateRVAtPhase(p)));
    }
}

TEST_CASE("RVFit: the eccentric model matches the closed form")
{
    const double e = 0.4, omega = 75.0, k = 42.0, gamma = -11.0;
    const RVFit fit = makeEccentricFit(e, omega, 3.5, k, gamma);
    REQUIRE(fit.isEccentric());

    for (double p = 0.0; p < 1.0; p += 0.02)
        CHECK(fit.calculateRVAtPhase(p)
              == doctest::Approx(referenceEccentricRV(p, e, omega, k, gamma)).epsilon(1e-9));
}

TEST_CASE("RVFit: the eccentric model degenerates to the circular one at e = 0")
{
    // setEccentricity(0) leaves the fit circular, so force the flag to check
    // that the two branches agree in the limit.
    RVFit ecc = makeCircularFit(3.5, 42.0, -11.0, 0.0);
    ecc.setEccentricity(1e-12);
    ecc.setOmega(-90.0);      // cos(nu - 90 deg) = sin(nu)
    REQUIRE(ecc.isEccentric());

    const RVFit circ = makeCircularFit(3.5, 42.0, -11.0, 0.0);
    for (double p = 0.0; p < 1.0; p += 0.05)
        CHECK(ecc.calculateRVAtPhase(p) == doctest::Approx(circ.calculateRVAtPhase(p)).epsilon(1e-6));
}

TEST_CASE("RVFit: the secondary is the primary mirrored about gamma")
{
    RVFit fit = makeCircularFit(3.5, 42.0, -11.0, 0.0);
    fit.setK2(21.0);
    REQUIRE(fit.hasK2());

    for (double p = 0.0; p < 1.0; p += 0.05) {
        const double primary   = fit.calculateRVAtPhase(p, 1);
        const double secondary = fit.calculateRVAtPhase(p, 2);
        // Amplitudes differ, but both cross gamma together and move opposite.
        CHECK((primary - (-11.0)) * 21.0 == doctest::Approx(-(secondary - (-11.0)) * 42.0));
    }

    // Mass ratio q = M2/M1 = K1/K2.
    CHECK(fit.massRatio() == doctest::Approx(42.0 / 21.0));
}

TEST_CASE("RVFit::computePhase folds on the reference epoch")
{
    const double period = 3.5, tRef = 2458000.0, phi = 0.2;
    const RVFit fit = makeCircularFit(period, 42.0, -11.0, phi, tRef);

    // At the reference epoch the phase is phi itself.
    Time atRef;
    atRef.setBJD(tRef);
    CHECK(fit.computePhase(atRef) == doctest::Approx(phi));

    // One full period later it is back to the same phase.
    Time oneLater;
    oneLater.setBJD(tRef + period);
    CHECK(fit.computePhase(oneLater) == doctest::Approx(phi));

    // A quarter period on advances the phase by a quarter.
    Time quarter;
    quarter.setBJD(tRef + 0.25 * period);
    CHECK(fit.computePhase(quarter) == doctest::Approx(phi + 0.25));

    // The result is always wrapped into [0, 1).
    for (double dt = -5.0 * period; dt < 5.0 * period; dt += 0.31) {
        Time t;
        t.setBJD(tRef + dt);
        const double p = fit.computePhase(t);
        CHECK(p >= 0.0);
        CHECK(p < 1.0);
    }
}

TEST_CASE("RVFit: circular and eccentric fits fold with opposite phase signs")
{
    // The two models rv_mcmc fits use opposite conventions: the circular model
    // is sin(2 pi (theta + phi)), the eccentric one defines M = 2 pi (theta - phi).
    // getT0BJD carries that sign, and getting it wrong shifts a fitted curve
    // against its own data.
    const double period = 3.5, tRef = 2458000.0, phi = 0.2;

    const RVFit circ = makeCircularFit(period, 42.0, -11.0, phi, tRef);
    CHECK(circ.getT0BJD() == doctest::Approx(tRef - phi * period));

    const RVFit ecc = makeEccentricFit(0.4, 75.0, period, 42.0, -11.0, phi, tRef);
    CHECK(ecc.getT0BJD() == doctest::Approx(tRef + phi * period));

    // The same sign has to reach computePhase, or the curve and the epoch
    // disagree with each other.
    Time atRef;
    atRef.setBJD(tRef);
    CHECK(circ.computePhase(atRef) == doctest::Approx(phi));
    CHECK(ecc.computePhase(atRef) == doctest::Approx(1.0 - phi));

    // Phase 0 by definition falls on T0 for both models. Compare on the
    // circle: phase is wrapped into [0, 1), so an epoch exactly on T0 lands
    // arbitrarily close to either end depending on the last bit of rounding.
    auto distanceFromZero = [](double phase) {
        return std::min(phase, 1.0 - phase);
    };
    Time circT0, eccT0;
    circT0.setBJD(circ.getT0BJD());
    eccT0.setBJD(ecc.getT0BJD());
    // A full BJD near 2.458e6 has an ulp of about 5e-10 days, so folding one
    // costs roughly 1e-10 in phase however carefully it is done. That is some
    // 30 microseconds on a 3.5 day period: far below anything observable, but
    // well above the 1e-16 an exact cancellation would give.
    CHECK(distanceFromZero(circ.computePhase(circT0)) < 1e-9);
    CHECK(distanceFromZero(ecc.computePhase(eccT0)) < 1e-9);
}

TEST_CASE("RVFit: T0 is undefined without a reference epoch")
{
    RVFit fit;
    fit.setPeriod(3.5);
    fit.setPhi(0.2);
    CHECK(std::isnan(fit.getT0BJD()));

    // foldEpochBJD falls back to the phase-equivalent epoch counted from BJD 0,
    // so callers always get something foldable and never rebuild the sign rule.
    CHECK(fit.foldEpochBJD() == doctest::Approx(-0.2 * 3.5));

    fit.setReferenceTime(2458000.0, 2457999.5);
    CHECK(fit.foldEpochBJD() == doctest::Approx(fit.getT0BJD()));

    // A period of zero cannot be folded at all.
    RVFit noPeriod;
    CHECK(noPeriod.foldEpochBJD() == doctest::Approx(0.0));
}

TEST_CASE("RVFit::calculateRV agrees with calculateRVAtPhase")
{
    const double period = 3.5, tRef = 2458000.0;
    RVFit fit = makeEccentricFit(0.4, 75.0, period, 42.0, -11.0, 0.2, tRef);
    fit.setK2(21.0);

    for (double dt = 0.0; dt < 2.0 * period; dt += 0.19) {
        Time t;
        t.setBJD(tRef + dt);
        for (int component : {1, 2}) {
            CHECK(fit.calculateRV(t, component)
                  == doctest::Approx(fit.calculateRVAtPhase(fit.computePhase(t), component)));
        }
    }
}

TEST_CASE("RVFit: a period of zero yields phase zero rather than a division by zero")
{
    RVFit fit;
    fit.setReferenceTime(2458000.0, 2457999.5);
    Time t;
    t.setBJD(2458001.0);
    CHECK(fit.computePhase(t) == doctest::Approx(0.0));
    CHECK(std::isfinite(fit.calculateRV(t)));
}

}   // TEST_SUITE("rv")
