// Unit tests for the kinematics module: coordinate transform, potential, and
// orbit integration.
//
// None of this was covered. The coordinate transform is checked against astropy
// with the frame constants pinned to ASTRA's own, so the comparison measures
// the transform rather than a difference of conventions. The potential and the
// integrator are checked against their own defining properties, which is
// stronger than a recorded-output comparison: acceleration must be minus the
// gradient of the potential, and an orbit must conserve energy and the
// z component of angular momentum.

#include <doctest.h>

#include "kinematics/GalacticCoordinates.h"
#include "kinematics/GalacticPotential.h"
#include "kinematics/OrbitIntegrator.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>

using namespace GalKin;

namespace {

QJsonObject loadReference()
{
    QFile f(QStringLiteral(ASTRA_TEST_REFERENCE_DIR) + "/galactic.json");
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

double norm(const Vec3& v)
{
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

}   // namespace

TEST_SUITE("kinematics")
{

TEST_CASE("celestialToGalactic matches astropy")
{
    const QJsonObject ref = loadReference();
    REQUIRE_MESSAGE(!ref.isEmpty(),
                    "galactic.json missing; run tests/unit/generate_references.py");
    CHECK(ref.value("version").toInt() == 1);

    const QJsonObject frameJson = ref.value("frame").toObject();
    FrameParams fp;
    fp.sunGCDistKpc = frameJson.value("sun_gc_kpc").toDouble();
    fp.vlsrKmS      = frameJson.value("vlsr_kms").toDouble();

    for (const auto& entry : ref.value("cases").toArray()) {
        const QJsonObject c = entry.toObject();
        CelestialInput in;
        in.raDeg       = c.value("ra").toDouble();
        in.decDeg      = c.value("dec").toDouble();
        in.distKpc     = c.value("dist").toDouble();
        in.rvKmS       = c.value("rv").toDouble();
        in.pmraMasYr   = c.value("pmra").toDouble();
        in.pmdecMasYr  = c.value("pmdec").toDouble();

        const StateVector got = celestialToGalactic(in, fp);
        const QJsonArray pos = c.value("pos_kpc").toArray();
        const QJsonArray vel = c.value("vel_kms").toArray();

        INFO("star " << c.value("name").toString().toStdString());
        for (int k = 0; k < 3; ++k) {
            // A parsec on a position of order 10 kpc, and 0.1 km/s on a
            // velocity of order 250 km/s: tight enough that a sign slip or a
            // wrong constant shows immediately, loose enough to absorb the
            // difference between two ICRS-to-Galactic rotation matrices.
            CHECK(std::abs(got.pos[k] - pos.at(k).toDouble()) < 1e-3);
            CHECK(std::abs(got.vel[k] - vel.at(k).toDouble()) < 0.2);
        }
    }
}

TEST_CASE("celestialToGalactic places the Sun where the frame says")
{
    FrameParams fp;
    CelestialInput here;          // zero distance: the Sun itself
    here.raDeg = 0.0;
    here.decDeg = 0.0;
    here.distKpc = 0.0;

    const StateVector s = celestialToGalactic(here, fp);
    CHECK(s.pos[0] == doctest::Approx(-fp.sunGCDistKpc));
    CHECK(s.pos[1] == doctest::Approx(0.0));
    CHECK(s.pos[2] == doctest::Approx(0.0));

    // A star at rest relative to the Sun shares the Sun's motion: the LSR speed
    // plus the solar peculiar velocity, towards +y.
    CHECK(s.vel[1] == doctest::Approx(fp.vlsrKmS + fp.vys));
    CHECK(s.vel[0] == doctest::Approx(fp.vxs));
    CHECK(s.vel[2] == doctest::Approx(fp.vzs));
}

TEST_CASE("heliocentricUVW leaves out the solar motion")
{
    CelestialInput in;
    in.raDeg = 90.0; in.decDeg = 10.0; in.distKpc = 0.5;
    in.rvKmS = 25.0; in.pmraMasYr = 5.0; in.pmdecMasYr = -3.0;

    const Vec3 uvw = heliocentricUVW(in);

    // Heliocentric velocities are of order the star's own motion, tens of km/s,
    // not the couple of hundred that galactic rotation adds.
    CHECK(norm(uvw) < 200.0);

    // A star with no proper motion and no radial velocity is not moving
    // relative to the Sun at all.
    CelestialInput still;
    still.raDeg = 90.0; still.decDeg = 10.0; still.distKpc = 0.5;
    const Vec3 zero = heliocentricUVW(still);
    CHECK(norm(zero) < 1e-9);
}

TEST_CASE("galacticVrVphi decomposes the velocity in the plane")
{
    // A star on the negative x axis moving in +y is on a circular orbit there:
    // no radial motion, all of it azimuthal.
    StateVector s;
    s.pos = {-8.4, 0.0, 0.0};
    s.vel = {0.0, 240.0, 0.0};

    double vr = 0.0, vphi = 0.0;
    galacticVrVphi(s, vr, vphi);
    CHECK(vr == doctest::Approx(0.0).epsilon(1e-9));
    CHECK(std::abs(vphi) == doctest::Approx(240.0));

    // Moving straight outwards instead: all radial, no rotation.
    s.vel = {-100.0, 0.0, 0.0};
    galacticVrVphi(s, vr, vphi);
    CHECK(std::abs(vr) == doctest::Approx(100.0));
    CHECK(vphi == doctest::Approx(0.0).epsilon(1e-9));
}

// ── Potential ───────────────────────────────────────────────────────────────

TEST_CASE("GalacticPotential: acceleration is minus the gradient of the potential")
{
    // The defining relation, checked by finite differences. If the two were
    // ever derived inconsistently, orbits would not conserve energy and every
    // integration would drift.
    for (auto model : {GalacticPotential::Model::AS,
                       GalacticPotential::Model::MN_TF,
                       GalacticPotential::Model::MN_NFW}) {
        const GalacticPotential pot(model);

        for (const Vec3& p : {Vec3{8.4, 0.0, 0.0}, Vec3{-8.4, 2.0, 1.0},
                              Vec3{3.0, -4.0, 0.5}, Vec3{0.5, 0.5, 12.0}}) {
            const Vec3 accel = pot.acceleration(p);
            const double h = 1e-5;   // kpc

            for (int k = 0; k < 3; ++k) {
                Vec3 hi = p, lo = p;
                hi[k] += h;
                lo[k] -= h;
                // Potential is km^2/s^2, acceleration is kpc/Myr^2, so the
                // numerical gradient has to be converted the same way the
                // implementation does.
                const double dPhi =
                    (pot.potentialKm2S2(hi) - pot.potentialKm2S2(lo)) / (2.0 * h);
                const double expected =
                    -dPhi / (kKpcPerMyrInKmS * kKpcPerMyrInKmS);
                INFO("model " << int(model) << " axis " << k);
                CHECK(accel[k] == doctest::Approx(expected).epsilon(1e-4));
            }
        }
    }
}

TEST_CASE("GalacticPotential: the potential is a bound well")
{
    for (auto model : {GalacticPotential::Model::AS,
                       GalacticPotential::Model::MN_TF,
                       GalacticPotential::Model::MN_NFW}) {
        const GalacticPotential pot(model);

        // Negative everywhere, and rising towards zero with distance.
        const double nearby = pot.potentialKm2S2({8.4, 0.0, 0.0});
        const double faraway = pot.potentialKm2S2({200.0, 0.0, 0.0});
        CHECK(nearby < 0.0);
        CHECK(faraway < 0.0);
        CHECK(faraway > nearby);

        // Acceleration points back towards the centre.
        const Vec3 a = pot.acceleration({8.4, 0.0, 0.0});
        CHECK(a[0] < 0.0);

        // and its magnitude falls off with distance.
        CHECK(std::abs(pot.acceleration({20.0, 0.0, 0.0})[0])
              < std::abs(pot.acceleration({8.4, 0.0, 0.0})[0]));
    }
}

TEST_CASE("GalacticPotential: the rotation curve is flat and roughly right")
{
    for (auto model : {GalacticPotential::Model::AS,
                       GalacticPotential::Model::MN_TF,
                       GalacticPotential::Model::MN_NFW}) {
        const GalacticPotential pot(model);

        // The local standard of rest is around 220 to 250 km/s in every model
        // fitted to the Milky Way.
        const double vlsr = pot.vlsrKmS();
        INFO("model " << int(model) << " vlsr " << vlsr);
        CHECK(vlsr > 200.0);
        CHECK(vlsr < 260.0);

        // The Sun sits at 8 to 8.5 kpc in all three fits.
        CHECK(pot.sunGCDist() > 7.5);
        CHECK(pot.sunGCDist() < 9.0);

        // A flat rotation curve: the speed at 15 kpc is within a quarter of
        // the speed at 5 kpc, which no Keplerian falloff would manage.
        const double inner = pot.circularVelocityKmS(5.0);
        const double outer = pot.circularVelocityKmS(15.0);
        CHECK(outer > 0.6 * inner);
        CHECK(outer < 1.4 * inner);

        // Escape velocity exceeds the circular speed everywhere, by root two
        // for a point mass and more for an extended one.
        const Vec3 sun{pot.sunGCDist(), 0.0, 0.0};
        CHECK(pot.escapeVelocityKmS(sun) > std::sqrt(2.0) * vlsr * 0.99);
    }
}

TEST_CASE("GalacticPotential: total energy separates bound from unbound")
{
    const GalacticPotential pot;
    const Vec3 sun{8.4, 0.0, 0.0};

    // A star on a circular orbit is bound.
    CHECK(pot.totalEnergyKm2S2(sun, {0.0, pot.vlsrKmS(), 0.0}) < 0.0);
    // One above the escape speed is not.
    const double vesc = pot.escapeVelocityKmS(sun);
    CHECK(pot.totalEnergyKm2S2(sun, {0.0, vesc * 1.01, 0.0}) > 0.0);
    // and exactly at it, marginally bound.
    CHECK(pot.totalEnergyKm2S2(sun, {0.0, vesc, 0.0})
          == doctest::Approx(0.0).epsilon(1e-9).scale(1e5));
}

// ── Orbit integration ───────────────────────────────────────────────────────

TEST_CASE("integrateOrbit conserves energy and angular momentum")
{
    const GalacticPotential pot;
    StateVector start;
    start.pos = {8.4, 0.0, 0.3};
    start.vel = {10.0, 230.0, 15.0};

    OrbitOptions opts;
    opts.tEndMyr = -2000.0;      // two billion years into the past
    opts.tolerance = 1e-9;

    Trajectory traj;
    const OrbitSummary s = integrateOrbit(pot, start, opts, &traj);

    REQUIRE(s.ok);
    CHECK(traj.size() > 100);

    // The summary reports its own drift, which is the integrator's error
    // estimate made visible. Over 2 Gyr it should stay tiny.
    INFO("relative energy drift " << s.energyDriftRel);
    CHECK(s.energyDriftRel < 1e-6);

    // Independently: the z component of angular momentum is conserved exactly
    // in an axisymmetric potential, so it is a check the integrator cannot fake.
    double lzMin = 1e300, lzMax = -1e300;
    for (size_t k = 0; k < traj.size(); ++k) {
        const double lz = traj.x[k] * traj.vy[k] - traj.y[k] * traj.vx[k];
        lzMin = std::min(lzMin, lz);
        lzMax = std::max(lzMax, lz);
    }
    CHECK(std::abs(lzMax - lzMin) / std::abs(s.LzKpcKmS) < 1e-6);

    // And the recorded energies stay put too.
    double eMin = 1e300, eMax = -1e300;
    for (double e : traj.energy) { eMin = std::min(eMin, e); eMax = std::max(eMax, e); }
    CHECK(std::abs(eMax - eMin) / std::abs(s.energyKm2S2) < 1e-6);
}

TEST_CASE("integrateOrbit: a circular orbit stays circular")
{
    const GalacticPotential pot;
    const double r = 8.0;

    StateVector start;
    start.pos = {r, 0.0, 0.0};
    start.vel = {0.0, pot.circularVelocityKmS(r), 0.0};

    OrbitOptions opts;
    opts.tEndMyr = -1000.0;
    Trajectory traj;
    const OrbitSummary s = integrateOrbit(pot, start, opts, &traj);

    REQUIRE(s.ok);
    // The radius should not budge, so the eccentricity is essentially zero.
    CHECK(s.eccentricity() < 1e-6);
    CHECK(s.rMinKpc == doctest::Approx(r).epsilon(1e-6));
    CHECK(s.rMaxKpc == doctest::Approx(r).epsilon(1e-6));
    // and it stays in the plane it started in.
    CHECK(s.zAbsMaxKpc < 1e-9);
}

TEST_CASE("integrateOrbit: an eccentric orbit reports its excursion")
{
    const GalacticPotential pot;
    const double r = 8.0;

    StateVector start;
    start.pos = {r, 0.0, 0.0};
    // Well under the circular speed, so the star falls inwards.
    start.vel = {0.0, 0.5 * pot.circularVelocityKmS(r), 0.0};

    OrbitOptions opts;
    opts.tEndMyr = -3000.0;
    const OrbitSummary s = integrateOrbit(pot, start, opts, nullptr);

    REQUIRE(s.ok);
    CHECK(s.rMinKpc < r);
    CHECK(s.rMaxKpc == doctest::Approx(r).epsilon(0.01));
    CHECK(s.eccentricity() > 0.2);
    CHECK(s.eccentricity() < 1.0);
    CHECK(s.energyDriftRel < 1e-6);
}

TEST_CASE("integrateOrbit: forward and backward are mirror images")
{
    const GalacticPotential pot;
    StateVector start;
    start.pos = {8.4, 0.0, 0.5};
    start.vel = {20.0, 220.0, -30.0};

    OrbitOptions back, fwd;
    back.tEndMyr = -500.0;
    fwd.tEndMyr  = 500.0;

    const OrbitSummary b = integrateOrbit(pot, start, back, nullptr);
    const OrbitSummary f = integrateOrbit(pot, start, fwd, nullptr);

    REQUIRE(b.ok);
    REQUIRE(f.ok);
    // Same conserved quantities either way.
    CHECK(b.energyKm2S2 == doctest::Approx(f.energyKm2S2));
    CHECK(b.LzKpcKmS == doctest::Approx(f.LzKpcKmS));
    CHECK(b.tFinalMyr == doctest::Approx(-f.tFinalMyr));
}

}   // TEST_SUITE("kinematics")
