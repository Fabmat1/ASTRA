// Unit tests for core/AsymmetricErrors and core/Quantity.
//
// These headers carry an unusually emphatic warning: a zero side of a credible
// interval is a valid value, not a defect, and must never be repaired,
// mirrored or symmetrized away. That happens when a fitted parameter sits
// against a physical bound, an inclination optimum at 88.7 degrees being the
// worked example, and "fixing" it quotes an edge past 90 degrees.
//
// Nothing tested that invariant. It is exactly the kind of rule that a
// well-meaning simplification breaks silently, so it is pinned here.

#include <doctest.h>

#include "core/AsymmetricErrors.h"
#include "core/Quantity.h"

#include <cmath>
#include <random>

TEST_SUITE("core")
{

TEST_CASE("AsymErr: unset is NaN and nothing else counts as unset")
{
    CHECK(std::isnan(AsymErr::unset));
    CHECK_FALSE(AsymErr::isSet(AsymErr::unset));
    CHECK_FALSE(AsymErr::isSet(std::numeric_limits<double>::infinity()));

    // Zero is a value, not an absence. This is the whole point.
    CHECK(AsymErr::isSet(0.0));
    CHECK(AsymErr::isSet(-0.0));
    CHECK(AsymErr::isSet(1.5));
}

TEST_CASE("AsymErr: an interval with one side set is still asymmetric")
{
    CHECK_FALSE(AsymErr::hasAsymmetric(AsymErr::unset, AsymErr::unset));
    CHECK(AsymErr::hasAsymmetric(1.0, AsymErr::unset));
    CHECK(AsymErr::hasAsymmetric(AsymErr::unset, 1.0));
    CHECK(AsymErr::hasAsymmetric(0.0, 32.25));
}

TEST_CASE("AsymErr: the symmetric error fills in for an unset side")
{
    CHECK(AsymErr::upOr(2.0, 5.0) == doctest::Approx(2.0));
    CHECK(AsymErr::upOr(AsymErr::unset, 5.0) == doctest::Approx(5.0));
    CHECK(AsymErr::downOr(3.0, 5.0) == doctest::Approx(3.0));
    CHECK(AsymErr::downOr(AsymErr::unset, 5.0) == doctest::Approx(5.0));

    // A zero side overrides the symmetric error rather than falling back to it.
    CHECK(AsymErr::upOr(0.0, 5.0) == doctest::Approx(0.0));
}

TEST_CASE("AsymErr: a zero side against a physical bound stays asymmetric")
{
    // The documented case: an inclination optimum at 88.7 degrees whose
    // posterior bulk lies lower. Up is genuinely zero because the interval
    // cannot cross 90 degrees.
    const double up = 0.0, down = 32.25;

    CHECK(AsymErr::hasAsymmetric(up, down));
    CHECK_FALSE(AsymErr::nearlySymmetric(up, down));

    const auto stored = AsymErr::toStorage(up, down);
    // The pair must survive storage: collapsing it to a single 16.1 would put
    // the upper edge at 104.8 degrees, which is not a possible inclination.
    CHECK(AsymErr::isSet(stored.up));
    CHECK(AsymErr::isSet(stored.down));
    CHECK(stored.up == doctest::Approx(0.0));
    CHECK(stored.down == doctest::Approx(32.25));
}

TEST_CASE("AsymErr::nearlySymmetric only collapses sides that agree")
{
    // Within 10 percent of the larger side by default.
    CHECK(AsymErr::nearlySymmetric(1.00, 1.05));
    CHECK(AsymErr::nearlySymmetric(1.00, 0.95));
    CHECK_FALSE(AsymErr::nearlySymmetric(1.00, 1.20));

    // Exactly on the margin.
    CHECK(AsymErr::nearlySymmetric(1.0, 1.1));
    CHECK_FALSE(AsymErr::nearlySymmetric(1.0, 1.12));

    // The tolerance is configurable.
    CHECK(AsymErr::nearlySymmetric(1.0, 1.2, 0.25));

    // Two zeros are symmetric; a zero against anything finite is not.
    CHECK(AsymErr::nearlySymmetric(0.0, 0.0));
    CHECK_FALSE(AsymErr::nearlySymmetric(0.0, 1.0));

    // An unset side cannot be compared, so it is never "nearly symmetric".
    CHECK_FALSE(AsymErr::nearlySymmetric(AsymErr::unset, 1.0));
    CHECK_FALSE(AsymErr::nearlySymmetric(1.0, AsymErr::unset));
}

TEST_CASE("AsymErr::toStorage collapses only what should collapse")
{
    // Sides that agree become one symmetric number and the pair goes away.
    const auto tight = AsymErr::toStorage(1.00, 1.04);
    CHECK(tight.sym == doctest::Approx(1.02));
    CHECK_FALSE(AsymErr::isSet(tight.up));
    CHECK_FALSE(AsymErr::isSet(tight.down));

    // Sides that differ are kept, with the mean as the legacy symmetric value
    // so old readers still see something sensible.
    const auto wide = AsymErr::toStorage(1.0, 3.0);
    CHECK(wide.sym == doctest::Approx(2.0));
    CHECK(wide.up == doctest::Approx(1.0));
    CHECK(wide.down == doctest::Approx(3.0));

    // A one-sided interval keeps the side it has as the symmetric value.
    const auto oneSided = AsymErr::toStorage(2.5, AsymErr::unset);
    CHECK(oneSided.sym == doctest::Approx(2.5));
    CHECK(oneSided.up == doctest::Approx(2.5));
    CHECK_FALSE(AsymErr::isSet(oneSided.down));

    // Nothing at all.
    const auto none = AsymErr::toStorage(AsymErr::unset, AsymErr::unset);
    CHECK(none.sym == doctest::Approx(0.0));
    CHECK_FALSE(AsymErr::isSet(none.up));
}

TEST_CASE("AsymErr::symmetrized averages the two sides")
{
    CHECK(AsymErr::symmetrized(5.0, AsymErr::unset, AsymErr::unset)
          == doctest::Approx(5.0));
    CHECK(AsymErr::symmetrized(5.0, 1.0, 3.0) == doctest::Approx(2.0));
    // An unset side falls back to the symmetric error before averaging.
    CHECK(AsymErr::symmetrized(4.0, 2.0, AsymErr::unset) == doctest::Approx(3.0));
}

TEST_CASE("AsymErr::drawTwoPiece uses the side the draw falls on")
{
    std::mt19937_64 rng(12345);
    std::normal_distribution<double> gauss(0.0, 1.0);

    int above = 0, below = 0;
    double maxAbove = 0.0, maxBelow = 0.0;
    for (int i = 0; i < 20000; ++i) {
        const double x = AsymErr::drawTwoPiece(rng, gauss, 10.0, 1.0, 4.0);
        if (x >= 10.0) { ++above; maxAbove = std::max(maxAbove, x - 10.0); }
        else           { ++below; maxBelow = std::max(maxBelow, 10.0 - x); }
    }

    // Half the draws land on each side, whatever the widths.
    CHECK(above > 9000);
    CHECK(below > 9000);
    // The lower side is four times wider, so its excursions are much larger.
    CHECK(maxBelow > 2.0 * maxAbove);

    // A zero side means the value never moves that way, which is exactly what
    // a parameter sitting on a physical bound should do.
    std::mt19937_64 rng2(999);
    for (int i = 0; i < 500; ++i)
        CHECK(AsymErr::drawTwoPiece(rng2, gauss, 88.7, 0.0, 32.25) <= 88.7);
}

// ── Quantity ────────────────────────────────────────────────────────────────

TEST_CASE("Quantity: a default is empty, not zero")
{
    const Quantity q;
    CHECK_FALSE(q.hasValue());
    CHECK_FALSE(q.hasError());
    CHECK_FALSE(q.isAsymmetric());
}

TEST_CASE("Quantity: the symmetric error applies both ways when unset")
{
    const Quantity q(10.0, 0.5, 2);
    CHECK(q.hasValue());
    CHECK(q.hasError());
    CHECK(q.up() == doctest::Approx(0.5));
    CHECK(q.down() == doctest::Approx(0.5));
    CHECK_FALSE(q.isAsymmetric());
    CHECK(q.symmetricError() == doctest::Approx(0.5));
}

TEST_CASE("Quantity: an explicit interval overrides the symmetric error")
{
    const Quantity q(10.0, 0.5, 2, "km/s", 1.0, 3.0);
    CHECK(q.up() == doctest::Approx(1.0));
    CHECK(q.down() == doctest::Approx(3.0));
    CHECK(q.isAsymmetric());
    CHECK(q.symmetricError() == doctest::Approx(2.0));
    CHECK(q.unit == "km/s");
}

TEST_CASE("Quantity: an interval with equal sides renders symmetrically")
{
    // Explicitly set but identical: there is nothing to stack, so it should be
    // shown as a single plus-or-minus rather than two equal rows.
    const Quantity q(10.0, 0.5, 2, {}, 2.0, 2.0);
    CHECK(q.hasError());
    CHECK_FALSE(q.isAsymmetric());
    CHECK(q.symmetricError() == doctest::Approx(2.0));
}

TEST_CASE("Quantity: a zero side is still an error worth showing")
{
    const Quantity inclination(88.7, 16.1, 2, "deg", 0.0, 32.25);
    CHECK(inclination.hasError());
    CHECK(inclination.isAsymmetric());
    CHECK(inclination.up() == doctest::Approx(0.0));
    CHECK(inclination.down() == doctest::Approx(32.25));

    // Both sides zero is a value with no uncertainty at all.
    const Quantity exact(1.0, 0.0, 2, {}, 0.0, 0.0);
    CHECK_FALSE(exact.hasError());
    CHECK_FALSE(exact.isAsymmetric());
}

TEST_CASE("Quantity: a missing error is not a zero error")
{
    const Quantity noError(10.0, std::numeric_limits<double>::quiet_NaN(), 2);
    CHECK(noError.hasValue());
    CHECK_FALSE(noError.hasError());
    CHECK_FALSE(noError.isAsymmetric());
}

}   // TEST_SUITE("core")
