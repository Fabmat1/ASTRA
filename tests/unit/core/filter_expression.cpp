// Unit tests for core/FilterExpression.
//
// This is the little expression language behind the star table's filter box, so
// its failure mode is a filter that silently selects the wrong stars. A parser
// is also the kind of code where a table of cases is worth far more than any
// amount of reading, and it had none.

#include <doctest.h>

#include "core/FilterExpression.h"

#include <QString>
#include <QVariant>

#include <cmath>

namespace {

/// A row with a handful of columns, plus one that is empty.
QVariant column(const QString& key)
{
    if (key == "plx")    return 5.0;
    if (key == "e_plx")  return 0.25;
    if (key == "teff")   return 28000.0;
    if (key == "logg")   return 5.4;
    if (key == "rv_med") return -42.5;
    if (key == "empty")  return QVariant();
    return QVariant();
}

double numeric(const QString& text, bool* ok = nullptr)
{
    QString err;
    const auto expr = FilterExpression::compile(text, &err);
    if (!expr) { if (ok) *ok = false; return std::nan(""); }
    return expr->evaluateNumeric(column, ok);
}

bool boolean(const QString& text, bool* ok = nullptr)
{
    QString err;
    const auto expr = FilterExpression::compile(text, &err);
    if (!expr) { if (ok) *ok = false; return false; }
    return expr->evaluateBool(column, ok);
}

bool compiles(const QString& text)
{
    return FilterExpression::compile(text, nullptr) != nullptr;
}

}   // namespace

TEST_SUITE("core")
{

TEST_CASE("FilterExpression: arithmetic and precedence")
{
    CHECK(numeric("1 + 2") == doctest::Approx(3.0));
    CHECK(numeric("2 * 3 + 4") == doctest::Approx(10.0));
    CHECK(numeric("4 + 2 * 3") == doctest::Approx(10.0));
    CHECK(numeric("(4 + 2) * 3") == doctest::Approx(18.0));
    CHECK(numeric("10 - 2 - 3") == doctest::Approx(5.0));      // left associative
    CHECK(numeric("2 ^ 3 ^ 2") == doctest::Approx(512.0));     // right associative
    CHECK(numeric("-3 + 1") == doctest::Approx(-2.0));
    CHECK(numeric("1e-3") == doctest::Approx(0.001));
    CHECK(numeric("0.5") == doctest::Approx(0.5));
}

TEST_CASE("FilterExpression: column references in every accepted spelling")
{
    CHECK(numeric("plx") == doctest::Approx(5.0));
    CHECK(numeric("{plx}") == doctest::Approx(5.0));
    CHECK(numeric("plx / e_plx") == doctest::Approx(20.0));
    // Case does not matter for a key.
    CHECK(numeric("PLX") == doctest::Approx(5.0));
}

TEST_CASE("FilterExpression: the documented function set")
{
    CHECK(numeric("abs(-3)") == doctest::Approx(3.0));
    CHECK(numeric("sqrt(9)") == doctest::Approx(3.0));
    CHECK(numeric("cbrt(27)") == doctest::Approx(3.0));
    CHECK(numeric("log10(1000)") == doctest::Approx(3.0));
    CHECK(numeric("exp(0)") == doctest::Approx(1.0));
    CHECK(numeric("log(exp(2))") == doctest::Approx(2.0));
    CHECK(numeric("floor(2.7)") == doctest::Approx(2.0));
    CHECK(numeric("ceil(2.1)") == doctest::Approx(3.0));
    CHECK(numeric("round(2.5)") == doctest::Approx(3.0));
    CHECK(numeric("sin(0)") == doctest::Approx(0.0));
    CHECK(numeric("cos(0)") == doctest::Approx(1.0));
    CHECK(numeric("tan(0)") == doctest::Approx(0.0));

    // A real one: distance in parsecs from a parallax in milliarcseconds.
    CHECK(numeric("1000 / plx") == doctest::Approx(200.0));
}

TEST_CASE("FilterExpression: comparisons and their spellings")
{
    bool ok = false;

    CHECK(boolean("plx > 3", &ok));
    CHECK(ok);
    CHECK_FALSE(boolean("plx > 30"));
    CHECK(boolean("plx >= 5"));
    CHECK(boolean("plx <= 5"));
    CHECK(boolean("plx < 10"));
    CHECK(boolean("plx == 5"));
    CHECK(boolean("plx = 5"));         // single equals accepted
    CHECK(boolean("plx != 4"));
    CHECK(boolean("plx <> 4"));        // and the other inequality spelling

    // A comparison over an expression, which is the point of the language.
    CHECK(boolean("plx / e_plx > 10"));
    CHECK_FALSE(boolean("plx / e_plx > 100"));
    CHECK(boolean("log10(teff) > 4"));
}

TEST_CASE("FilterExpression: an arithmetic expression is not a filter")
{
    QString err;
    const auto expr = FilterExpression::compile("plx * 2", &err);
    REQUIRE(expr);
    CHECK_FALSE(expr->hasComparison());

    bool ok = true;
    expr->evaluateBool(column, &ok);
    CHECK_FALSE(ok);          // asking for a truth value is the caller's error

    ok = false;
    CHECK(expr->evaluateNumeric(column, &ok) == doctest::Approx(10.0));
    CHECK(ok);
}

TEST_CASE("FilterExpression: a comparison is a comparison")
{
    QString err;
    const auto expr = FilterExpression::compile("plx > 3", &err);
    REQUIRE(expr);
    CHECK(expr->hasComparison());
}

TEST_CASE("FilterExpression: a missing value never matches")
{
    // The documented rule: a row with no value for a referenced column drops
    // out of the selection rather than being treated as zero, which would
    // quietly include everything with an empty parallax in "plx < 1".
    bool ok = true;
    CHECK_FALSE(boolean("empty > 0", &ok));
    CHECK_FALSE(boolean("empty < 1"));
    CHECK_FALSE(boolean("empty == 0"));
    CHECK_FALSE(boolean("empty + plx > 0"));

    // Arithmetic on a missing value is not a number, and says so.
    ok = true;
    const double v = numeric("empty * 2", &ok);
    CHECK_FALSE(ok);
    CHECK(std::isnan(v));
}

TEST_CASE("FilterExpression: an unknown column is missing, not an error")
{
    CHECK_FALSE(boolean("no_such_column > 0"));
}

TEST_CASE("FilterExpression: malformed input is rejected with a reason")
{
    for (const char* bad : {"", "   ", "1 +", "* 2", "(1 + 2", "1 + 2)",
                            "plx >", "> 3", "abs(", "1 > 2 > 3"}) {
        QString err;
        INFO("input: " << bad);
        CHECK(FilterExpression::compile(QString::fromLatin1(bad), &err) == nullptr);
        CHECK_FALSE(err.isEmpty());
    }
}

TEST_CASE("FilterExpression: whitespace is not significant")
{
    CHECK(compiles("plx>3"));
    CHECK(compiles("  plx   >   3  "));
    CHECK(boolean("plx>3"));
    CHECK(boolean("  plx   >   3  "));
}

TEST_CASE("FilterExpression: division by zero follows IEEE, not the NaN rule")
{
    // Current behaviour, pinned rather than endorsed. A missing value makes a
    // comparison false, but a division by zero gives an infinity, and infinity
    // compares like any other number.
    //
    // The consequence is worth knowing: a filter such as "plx / e_plx > 10"
    // selects a star whose parallax error is exactly zero, because the ratio is
    // infinite rather than unknown. Whether that should instead be treated as
    // missing is a product decision, not something to change underneath a
    // saved filter.
    bool ok = false;
    const double v = numeric("1 / 0", &ok);
    CHECK(ok);
    CHECK(std::isinf(v));
    CHECK(boolean("1 / 0 > 0"));
    CHECK_FALSE(boolean("-1 / 0 > 0"));

    // Zero divided by zero is genuinely undefined, and that does follow the
    // missing-value rule.
    CHECK_FALSE(boolean("0 / 0 > 0"));
    CHECK_FALSE(boolean("0 / 0 < 0"));
}

}   // TEST_SUITE("core")
