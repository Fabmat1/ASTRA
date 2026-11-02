// Unit tests for rv/AliasEnvelope: alias grouping in the χ² period landscape.
//
// The landscapes are synthetic but shaped like the real failure: a sparse
// SDSS+LAMOST RV curve whose likelihood is a nested comb (close pairs of
// aliases, pairs repeating at a wider pitch, lobes of those). The envelope has
// to find the wider pitch, not the pairs and not the lobes, or the lobes merge
// and one of them collects the probability of its neighbours.

#include <doctest.h>

#include "rv/AliasEnvelope.h"

#include <QVector>

#include <cmath>
#include <random>

namespace {

constexpr double kChi2Min = 1.0;
constexpr double kScale   = 0.02;   // χ²_min / dof
constexpr int    kPitch   = 40;     // cells between alias pairs
constexpr int    kPair    = 7;      // cells between the two aliases of a pair
constexpr int    kLobe    = 600;    // cells between lobes
constexpr int    kLobes   = 6;

/// χ² from a relative likelihood, floored where the likelihood vanishes.
double chi2Of(double like)
{
    return kChi2Min - 2.0 * kScale * std::log(std::max(like, 1e-30));
}

/// Nested comb: lobes of alias pairs, each alias one or two cells wide, with a
/// little jitter on the alias heights so the comb is not perfectly regular.
QVector<double> nestedComb()
{
    const int n = kLobe * (kLobes + 1);
    QVector<double> like(n, 0.0);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> jit(0.85, 1.0);
    for (int g = kPitch / 2; g + kPair + 1 < n; g += kPitch) {
        // Lobe envelope: Gaussian bumps centred on every kLobe cells, deep
        // valleys in between, strongest lobe in the middle.
        const double lobePos = double(g) / kLobe;
        const int    lobe    = int(std::lround(lobePos));
        if (lobe < 1 || lobe > kLobes) continue;
        const double d   = (lobePos - lobe) * kLobe;
        const double env = std::exp(-0.5 * d * d / (110.0 * 110.0))
                         * std::exp(-0.5 * std::pow((lobe - 3.5) / 2.0, 2));
        like[g]             = std::max(like[g],             env * jit(rng));
        like[g + 1]         = std::max(like[g + 1],         0.3 * env);
        like[g + kPair]     = std::max(like[g + kPair],     0.6 * env * jit(rng));
    }
    QVector<double> chi2(n);
    for (int k = 0; k < n; ++k) chi2[k] = chi2Of(like[k]);
    return chi2;
}

/// Lobe boundaries strictly inside the array (the crests).
QVector<int> crests(const QVector<double>& env)
{
    QVector<int> b = AliasEnvelope::lobeBoundaries(env);
    REQUIRE(b.size() >= 2);
    return b.mid(1, b.size() - 2);
}

} // namespace

TEST_SUITE("rv") {

TEST_CASE("AliasEnvelope: sliding min and mean match the naive definitions")
{
    std::mt19937 rng(3);
    std::uniform_real_distribution<double> u(-5.0, 5.0);
    QVector<double> v(97);
    for (double& x : v) x = u(rng);
    for (int w : {1, 3, 10, 60, 200}) {
        const QVector<double> mn = AliasEnvelope::slidingMin(v, w);
        const QVector<double> me = AliasEnvelope::slidingMean(v, w);
        REQUIRE(mn.size() == v.size());
        REQUIRE(me.size() == v.size());
        for (int i = 0; i < v.size(); ++i) {
            const int lo = std::max(0, i - w);
            const int hi = std::min(int(v.size()) - 1, i + w);
            double m = v[lo], s = 0.0;
            for (int k = lo; k <= hi; ++k) { m = std::min(m, v[k]); s += v[k]; }
            CHECK(mn[i] == doctest::Approx(m));
            CHECK(me[i] == doctest::Approx(s / (hi - lo + 1)));
        }
    }
}

TEST_CASE("AliasEnvelope: pitch of a nested comb is the pair spacing, not the "
          "pairs or the lobes")
{
    const QVector<double> chi2 = nestedComb();
    const int pitch = AliasEnvelope::combPitch(chi2, kChi2Min, kScale);
    CHECK(pitch >= kPitch - 2);
    CHECK(pitch <= kPitch + 2);
}

TEST_CASE("AliasEnvelope: the envelope at the default width keeps every lobe "
          "apart")
{
    const QVector<double> chi2 = nestedComb();
    const int pitch = AliasEnvelope::combPitch(chi2, kChi2Min, kScale);
    REQUIRE(pitch > 0);
    // Default sensitivity: half-width of two pitches.
    const int w = 2 * pitch;
    const QVector<double> env =
        AliasEnvelope::slidingMean(AliasEnvelope::slidingMin(chi2, w), w);
    // One crest between each pair of neighbouring lobes, none inside one: the
    // crests sit in the valleys, half a lobe spacing from the lobe centres.
    const QVector<int> c = crests(env);
    CHECK(c.size() == kLobes - 1);
    for (int k : c) {
        const double offset = std::abs(double(k) / kLobe
                                       - std::floor(double(k) / kLobe) - 0.5);
        CHECK(offset < 0.15);
    }
}

TEST_CASE("AliasEnvelope: twice the real pitch merges lobes (the old failure)")
{
    // What the earlier pitch estimate produced on the real curve. Guards the
    // test above against passing for a reason unrelated to the pitch.
    const QVector<double> chi2 = nestedComb();
    const int w = 2 * 2 * kPitch * 4;
    const QVector<double> env =
        AliasEnvelope::slidingMean(AliasEnvelope::slidingMin(chi2, w), w);
    CHECK(crests(env).size() < kLobes - 1);
}

TEST_CASE("AliasEnvelope: a single clean peak has no comb to group")
{
    const int n = 4000;
    QVector<double> chi2(n);
    for (int k = 0; k < n; ++k) {
        const double d = (k - 1700) / 6.0;
        chi2[k] = chi2Of(std::exp(-0.5 * d * d));
    }
    const int pitch = AliasEnvelope::combPitch(chi2, kChi2Min, kScale);
    const int w = std::max(1, 2 * pitch);
    const QVector<double> env =
        AliasEnvelope::slidingMean(AliasEnvelope::slidingMin(chi2, w), w);
    // Whatever the pitch, grouping must leave the one peak as one lobe: no
    // crest where the likelihood is not negligible (|d| < 5.5σ).
    for (int k : crests(env)) {
        const bool outside = k < 1700 - 33 || k > 1700 + 33;
        CHECK(outside);
    }
}

TEST_CASE("AliasEnvelope: rounding on a flat stretch makes no crest")
{
    // A long flat floor, as a running minimum leaves at the bottom of a lobe,
    // between two slopes, preceded by a million-cell ramp so a plain double
    // prefix sum would be large where the floor is averaged.
    const int n = 1'200'000;
    QVector<double> v(n);
    for (int i = 0; i < n; ++i) {
        if (i < 1'000'000)      v[i] = 350.0 + 1e-4 * (i % 97);
        else if (i < 1'050'000) v[i] = 350.0 - (i - 1'000'000) * 0.007;
        else if (i < 1'150'000) v[i] = 0.0623;
        else                    v[i] = 0.0623 + (i - 1'150'000) * 0.007;
    }
    const QVector<double> env = AliasEnvelope::slidingMean(v, 270);
    int onFloor = 0;            // crests belong in the jittered ramp only
    for (int k : crests(env))
        if (k >= 1'000'000) ++onFloor;
    CHECK(onFloor == 0);
    // Lobe boundaries reach the array ends.
    const QVector<int> b = AliasEnvelope::lobeBoundaries(env);
    CHECK(b.first() == 0);
    CHECK(b.last() == n - 1);
}

TEST_CASE("AliasEnvelope: degenerate input gives no pitch")
{
    CHECK(AliasEnvelope::combPitch({}, 0.0, 1.0) == 0);
    CHECK(AliasEnvelope::combPitch(QVector<double>(50, 3.0), 3.0, 0.0) == 0);
    // One significant cell has nothing to correlate with.
    QVector<double> one(200, 1e6);
    one[100] = 1.0;
    CHECK(AliasEnvelope::combPitch(one, 1.0, 0.1) == 0);
}

} // TEST_SUITE
