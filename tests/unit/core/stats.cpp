// Unit tests for core/Stats.
//
// These helpers were each written two or three times across the codebase with
// quietly different conventions, and the chi-square survival function existed
// as two independent numerical implementations. This file pins the one that
// survived: the medians against their definitions, and logChi2SF against a
// scipy-generated table plus an asymptotic check in the tail where scipy itself
// cannot represent the answer.

#include <doctest.h>

#include "core/Stats.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <vector>

namespace {

QJsonObject loadReference(const QString& name)
{
    QFile f(QStringLiteral(ASTRA_TEST_REFERENCE_DIR) + QLatin1Char('/') + name);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

}   // namespace

TEST_SUITE("core")
{

TEST_CASE("Stats::median averages the central pair for an even sample")
{
    CHECK(Stats::median({3.0, 1.0, 2.0}) == doctest::Approx(2.0));
    CHECK(Stats::median({4.0, 1.0, 3.0, 2.0}) == doctest::Approx(2.5));
    CHECK(Stats::median({1.0}) == doctest::Approx(1.0));

    // Order must not matter.
    CHECK(Stats::median({9.0, -3.0, 0.0, 7.0, 2.0}) == doctest::Approx(2.0));

    // Outliers must not drag it, which is the whole reason it is used.
    CHECK(Stats::median({1.0, 2.0, 3.0, 4.0, 1e9}) == doctest::Approx(3.0));
}

TEST_CASE("Stats::medianUpper takes the upper central value")
{
    // The distinction from median() only shows for an even sample.
    CHECK(Stats::medianUpper({4.0, 1.0, 3.0, 2.0}) == doctest::Approx(3.0));
    CHECK(Stats::median({4.0, 1.0, 3.0, 2.0}) == doctest::Approx(2.5));

    // For an odd sample the two agree.
    CHECK(Stats::medianUpper({3.0, 1.0, 2.0}) == doctest::Approx(2.0));
}

TEST_CASE("Stats: the empty sample yields whatever the caller asked for")
{
    CHECK(std::isnan(Stats::median({})));
    CHECK(std::isnan(Stats::medianUpper({})));
    CHECK(Stats::median({}, 0.0) == doctest::Approx(0.0));
    CHECK(Stats::medianUpper({}, 0.0) == doctest::Approx(0.0));
}

TEST_CASE("Stats::madSigma recovers the standard deviation of normal data")
{
    // The 1.4826 factor is chosen so the estimate matches sigma for a normal
    // sample. A symmetric ramp is close enough to check the scaling.
    std::vector<double> v;
    for (int i = -50; i <= 50; ++i) v.push_back(i * 0.1);
    const double mad = Stats::madSigma(v);
    CHECK(mad == doctest::Approx(1.4826 * 2.5).epsilon(0.02));

    // It ignores an outlier that would wreck a plain standard deviation.
    std::vector<double> withOutlier = v;
    withOutlier.push_back(1e6);
    CHECK(Stats::madSigma(withOutlier) == doctest::Approx(mad).epsilon(0.05));

    // Too few points to estimate anything.
    CHECK(Stats::madSigma({}) == doctest::Approx(0.0));
    CHECK(Stats::madSigma({1.0}) == doctest::Approx(0.0));

    // No spread at all.
    CHECK(Stats::madSigma({2.0, 2.0, 2.0, 2.0}) == doctest::Approx(0.0));
}

TEST_CASE("Stats::logChi2SF matches scipy across both branches")
{
    const QJsonObject ref = loadReference("chi2.json");
    REQUIRE_MESSAGE(!ref.isEmpty(),
                    "chi2.json missing; run tests/unit/generate_references.py");
    CHECK(ref.value("version").toInt() == 1);

    const QJsonArray cases = ref.value("cases").toArray();
    REQUIRE(cases.size() > 50);

    for (const auto& entry : cases) {
        const QJsonObject c = entry.toObject();
        const double x        = c.value("x").toDouble();
        const int    dof      = c.value("dof").toInt();
        const double expected = c.value("log10_sf").toDouble();

        const double got = Stats::logChi2SF(x, dof);
        // Relative on the logarithm: the values span 0 down to about -219, and
        // an absolute tolerance would be either useless or unmeetable.
        CHECK(got == doctest::Approx(expected).epsilon(1e-9));
    }
}

TEST_CASE("Stats::logChi2SF keeps going where the probability underflows")
{
    // scipy's logsf collapses to -inf here, because it forms the survival
    // probability before taking its logarithm. Staying in log space is the
    // whole point of this implementation: a badly fitting RV curve produces
    // chi-square values well past the underflow point.
    const double deep = Stats::logChi2SF(5000.0, 1);
    CHECK(std::isfinite(deep));
    CHECK(deep < -1000.0);

    // For one degree of freedom the tail is sf(x) ~ sqrt(2 / (pi x)) exp(-x/2),
    // so log10 sf approaches -x / (2 ln 10) plus a slowly varying term.
    const double asymptotic = (-5000.0 / 2.0
                               + 0.5 * std::log(2.0 / (M_PI * 5000.0)))
                              / std::log(10.0);
    CHECK(deep == doctest::Approx(asymptotic).epsilon(1e-3));

    // Monotonic all the way down.
    double previous = 0.0;
    for (double x : {10.0, 100.0, 1000.0, 10000.0, 100000.0}) {
        const double v = Stats::logChi2SF(x, 5);
        CHECK(std::isfinite(v));
        CHECK(v < previous);
        previous = v;
    }
}

TEST_CASE("Stats::logChi2SF handles the degenerate arguments")
{
    // x <= 0 means the statistic is at or below its minimum: p = 1.
    CHECK(Stats::logChi2SF(0.0, 5) == doctest::Approx(0.0));
    CHECK(Stats::logChi2SF(-1.0, 5) == doctest::Approx(0.0));

    // No degrees of freedom, no distribution. Both callers guard before this.
    CHECK(std::isnan(Stats::logChi2SF(10.0, 0)));
    CHECK(std::isnan(Stats::logChi2SF(10.0, -3)));
}

TEST_CASE("Stats::logChi2SF: the median of a chi-square sits near p = 0.5")
{
    // A property that holds for every dof and is independent of the reference.
    const double medians[] = {0.4549, 1.3863, 2.3660, 4.3515, 9.3418};
    const int    dofs[]    = {1, 2, 3, 5, 10};
    for (int i = 0; i < 5; ++i)
        CHECK(Stats::logChi2SF(medians[i], dofs[i])
              == doctest::Approx(std::log10(0.5)).epsilon(1e-3));
}

}   // TEST_SUITE("core")
