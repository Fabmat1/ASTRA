// ─────────────────────────────────────────────────────────────────────────────
// Copy-format regression test for QuantityFormat.
//
// The formatter is what every panel and table hands to the clipboard, so the
// cases pinned here are the ones a wrong answer would silently ship into
// someone's paper: asymmetric versus symmetric intervals, the zero-side
// interval that must stay asymmetric, unit translation, publication rounding
// (two significant digits on the tighter side, value matched to it) and the
// negative-decimals case where the error is larger than one.
// ─────────────────────────────────────────────────────────────────────────────
#include "utils/QuantityFormat.h"

#include <QCoreApplication>

#include <cstdio>
#include <string>

namespace {

int gFailures = 0;

void checkEq(const QString &got, const QString &want, const std::string &what)
{
    const bool ok = got == want;
    std::printf("%s  %s - got \"%s\", want \"%s\"\n", ok ? "[ ok ]" : "[FAIL]",
                what.c_str(), got.toUtf8().constData(),
                want.toUtf8().constData());
    if (!ok)
        ++gFailures;
}

using namespace QuantityFormat;

Prefs basePrefs()
{
    Prefs p;
    p.content          = CopyContent::ValueErrorUnit;
    p.style            = CopyStyle::Latex;
    p.latexWrapMath    = false;
    p.latexIncludeName = false;
    p.roundOnCopy      = true;
    return p;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    setPrefs(basePrefs());

    // ── Asymmetric interval, LaTeX, publication rounding ────────────────
    {
        Quantity q(12.3456, AsymErr::unset, 4, "km/s", 0.5213, 0.3117, "K");
        // Two significant digits on the tighter side (0.3117 -> 0.31) fixes
        // the decimals for the whole expression.
        checkEq(copyText(q), "12.35^{+0.52}_{-0.31}\\,\\mathrm{km\\,s^{-1}}",
                "asymmetric LaTeX with rounding");
        checkEq(plainText(q), "12.3456 +0.5213 \xe2\x88\x92""0.3117 km/s",
                "asymmetric display text keeps the call site's precision");
    }

    // ── Symmetric interval collapses to \pm ─────────────────────────────
    {
        Quantity q(1.2345, 0.0567, 4, "M\xe2\x98\x89");
        checkEq(copyText(q), "1.234 \\pm 0.057\\,M_\\odot", "symmetric LaTeX");
    }

    // ── A zero side is a real result and must survive ───────────────────
    {
        Quantity q(88.72, AsymErr::unset, 2, "\xc2\xb0", 0.0, 32.25, "i");
        // The zero side carries no digits, so the 32.25 side sets the
        // rounding: two significant digits leave whole degrees.
        checkEq(copyText(q), "89^{+0}_{-32}^\\circ",
                "zero-side interval stays asymmetric, degrees attach");
    }

    // ── Errors above one give negative decimals ─────────────────────────
    {
        Quantity q(24973.0, 1200.0, 0, "K", AsymErr::unset, AsymErr::unset);
        checkEq(copyText(q), "25000 \\pm 1200\\,\\mathrm{K}",
                "error > 1 rounds the value to the same power of ten");
    }

    // ── Content levels ──────────────────────────────────────────────────
    {
        Quantity q(12.3456, 0.5, 2, "km/s");
        checkEq(copyText(q, CopyContent::Value), "12.35", "value only");
        checkEq(copyText(q, CopyContent::ValueError), "12.35 \\pm 0.50",
                "value and error, no unit");
    }

    // ── Plain style and the $...$ / name options ────────────────────────
    {
        Prefs p = basePrefs();
        p.style = CopyStyle::Plain;
        setPrefs(p);
        Quantity q(12.3456, AsymErr::unset, 4, "km/s", 0.5213, 0.3117, "K");
        checkEq(copyText(q), "12.35 +0.52 -0.31 km/s", "asymmetric plain");

        p.style            = CopyStyle::Latex;
        p.latexWrapMath    = true;
        p.latexIncludeName = true;
        setPrefs(p);
        checkEq(copyText(q),
                "$K = 12.35^{+0.52}_{-0.31}\\,\\mathrm{km\\,s^{-1}}$",
                "math wrapper and name prefix");

        p.roundOnCopy = false;
        setPrefs(p);
        checkEq(copyText(q),
                "$K = 12.3456^{+0.5213}_{-0.3117}\\,\\mathrm{km\\,s^{-1}}$",
                "rounding off keeps the display precision");
    }

    // ── Unit translation fallback ───────────────────────────────────────
    setPrefs(basePrefs());
    checkEq(latexUnit("kpc km/s"), "\\mathrm{kpc\\,km\\,s^{-1}}",
            "generic unit fallback");
    checkEq(latexUnit("mas/yr"), "\\mathrm{mas\\,yr^{-1}}", "per-year unit");

    std::printf("\n%s (%d failure(s))\n", gFailures ? "FAILED" : "PASSED",
                gFailures);
    return gFailures ? 1 : 0;
}
