#include "QuantityFormat.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace QuantityFormat {

namespace {

Prefs g_prefs;

constexpr const char *kMinusSign = "\xe2\x88\x92"; // U+2212, display only

/// Errors that actually constrain the rounding: finite and non-zero. A zero
/// side is legitimate (the estimate sits at the edge of the interval) but says
/// nothing about how many digits to keep.
void collectErrors(const Quantity &q, double &smallest, bool &any)
{
    smallest = 0.0;
    any      = false;
    auto consider = [&](double e) {
        if (!std::isfinite(e) || e <= 0.0)
            return;
        if (!any || e < smallest)
            smallest = e;
        any = true;
    };
    consider(q.up());
    consider(q.down());
    if (!any)
        consider(q.error);
}

QString stripTrailingSpace(QString s) { return s.trimmed(); }

} // namespace

const Prefs &prefs() { return g_prefs; }
void         setPrefs(const Prefs &p) { g_prefs = p; }

Parts partsFor(CopyContent c)
{
    switch (c) {
        case CopyContent::Value:          return ValuePart;
        case CopyContent::ValueError:     return ValuePart | AllErrParts;
        case CopyContent::ValueErrorUnit: return AllParts;
    }
    return AllParts;
}

QString number(double v, int decimals)
{
    if (!std::isfinite(v))
        return QStringLiteral("-");
    if (decimals >= 0)
        return QString::number(v, 'f', decimals);
    const double step = std::pow(10.0, -decimals);
    return QString::number(std::round(v / step) * step, 'f', 0);
}

int autoDecimals(double value, double err)
{
    if (std::isfinite(err) && err > 0.0)
        return std::clamp(2 - static_cast<int>(std::floor(std::log10(err))), 0,
                          12);
    const double m = std::abs(value);
    if (!std::isfinite(m) || m <= 0.0)
        return 3;
    return std::clamp(5 - static_cast<int>(std::floor(std::log10(m))), 0, 12);
}

int copyDecimals(const Quantity &q)
{
    if (!g_prefs.roundOnCopy)
        return q.precision;
    double smallest = 0.0;
    bool   any      = false;
    collectErrors(q, smallest, any);
    if (!any)
        return q.precision;
    // Two significant digits on the tighter side is the convention papers
    // expect; the value then carries exactly the same decimals.
    int d = 1 - static_cast<int>(std::floor(std::log10(smallest)));
    return std::clamp(d, -6, 12);
}

DisplayParts displayParts(const Quantity &q)
{
    DisplayParts p;
    if (!q.hasValue())
        return p;

    const int prec = q.precision;
    p.value        = number(q.value, prec);
    p.unit         = q.unit;

    if (q.isAsymmetric()) {
        p.asymmetric = true;
        p.errUp      = QStringLiteral("+") + number(q.up(), prec);
        p.errDown    = QString::fromUtf8(kMinusSign) + number(q.down(), prec);
    } else if (q.hasError()) {
        const double e = std::isfinite(q.up()) && q.up() > 0.0 ? q.up() : q.down();
        p.errSym = QStringLiteral("± ") + number(e, prec);
    }
    return p;
}

// ── Units ───────────────────────────────────────────────────────────────

bool latexUnitAttaches(const QString &displayUnit)
{
    const QString u = displayUnit.trimmed();
    return u == QString::fromUtf8("\xc2\xb0") || u == "deg";
}

QString latexUnit(const QString &displayUnit)
{
    const QString u = displayUnit.trimmed();
    if (u.isEmpty())
        return QString();

    static const QHash<QString, QString> exact = {
        {QString::fromUtf8("\xc2\xb0"), "^\\circ"},          // °
        {"deg", "^\\circ"},
        {QString::fromUtf8("M\xe2\x98\x89"), "M_\\odot"},    // M☉
        {QString::fromUtf8("R\xe2\x98\x89"), "R_\\odot"},    // R☉
        {QString::fromUtf8("L\xe2\x98\x89"), "L_\\odot"},    // L☉
        {"Msun", "M_\\odot"},
        {"Rsun", "R_\\odot"},
        {"Lsun", "L_\\odot"},
        {QString::fromUtf8("\xc3\x85"), "\\mathrm{\\AA}"},   // Å
        {"%", "\\%"},
        {"days", "\\mathrm{d}"},
        {"day", "\\mathrm{d}"},
    };
    auto it = exact.find(u);
    if (it != exact.end())
        return *it;

    // Generic fallback: "kpc km/s" -> \mathrm{kpc\,km\,s^{-1}}. Everything
    // before the (single) slash is the numerator, everything after it the
    // denominator, whose factors pick up an exponent of -1.
    const QStringList sides = u.split('/');
    auto factors = [](const QString &side) {
        return side.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    };

    QStringList out;
    for (const QString &f : factors(sides.value(0)))
        out << f;
    for (int i = 1; i < sides.size(); ++i)
        for (const QString &f : factors(sides.at(i)))
            out << f + "^{-1}";
    if (out.isEmpty())
        return QString();
    return "\\mathrm{" + out.join("\\,") + "}";
}

// ── Copy text ───────────────────────────────────────────────────────────

namespace {

QString latexText(const Quantity &q, Parts parts, int dec)
{
    QString body;
    const bool wantValue = parts.testFlag(ValuePart);
    const bool wantErr   = (parts & AllErrParts) != 0;
    const bool asym      = q.isAsymmetric();

    if (wantValue)
        body += number(q.value, dec);

    if (wantErr && q.hasError()) {
        if (asym) {
            const bool up   = parts.testFlag(ErrUpPart);
            const bool down = parts.testFlag(ErrDownPart);
            if (up && down)
                body += QString("^{+%1}_{-%2}")
                            .arg(number(q.up(), dec), number(q.down(), dec));
            else if (up)
                body += (wantValue ? QString("^{+%1}") : QString("+%1"))
                            .arg(number(q.up(), dec));
            else if (down)
                body += (wantValue ? QString("^{-%1}") : QString("-%1"))
                            .arg(number(q.down(), dec));
        } else {
            const double e =
                std::isfinite(q.up()) && q.up() > 0.0 ? q.up() : q.down();
            if (wantValue)
                body += " \\pm " + number(e, dec);
            else
                body += number(e, dec);
        }
    }

    if (parts.testFlag(UnitPart) && !q.unit.isEmpty()) {
        const QString lu = latexUnit(q.unit);
        if (!lu.isEmpty()) {
            if (body.isEmpty())
                body = lu;
            else if (latexUnitAttaches(q.unit))
                body += lu;
            else
                body += "\\," + lu;
        }
    }

    if (body.isEmpty())
        return QString();
    if (g_prefs.latexIncludeName && !q.name.isEmpty() &&
        parts.testFlag(ValuePart))
        body = q.name + " = " + body;
    if (g_prefs.latexWrapMath)
        body = "$" + body + "$";
    return body;
}

QString plainTextImpl(const Quantity &q, Parts parts, int dec)
{
    QStringList out;
    const bool wantValue = parts.testFlag(ValuePart);
    const bool wantErr   = (parts & AllErrParts) != 0;

    if (wantValue)
        out << number(q.value, dec);

    if (wantErr && q.hasError()) {
        if (q.isAsymmetric()) {
            if (parts.testFlag(ErrUpPart))
                out << "+" + number(q.up(), dec);
            if (parts.testFlag(ErrDownPart))
                out << "-" + number(q.down(), dec);
        } else {
            const double e =
                std::isfinite(q.up()) && q.up() > 0.0 ? q.up() : q.down();
            if (wantValue)
                out << QString::fromUtf8("\xc2\xb1") << number(e, dec);
            else
                out << number(e, dec);
        }
    }

    if (parts.testFlag(UnitPart) && !q.unit.isEmpty())
        out << q.unit;

    if (out.isEmpty())
        return QString();
    QString s = out.join(' ');
    if (g_prefs.latexIncludeName && !q.name.isEmpty() &&
        parts.testFlag(ValuePart))
        s = q.name + " = " + s;
    return s;
}

} // namespace

QString copyText(const Quantity &q, Parts parts, CopyStyle style)
{
    if (!q.hasValue() || parts == NoPart)
        return QString();
    const int dec = copyDecimals(q);
    return style == CopyStyle::Latex ? latexText(q, parts, dec)
                                     : plainTextImpl(q, parts, dec);
}

QString copyText(const Quantity &q)
{
    return copyText(q, partsFor(g_prefs.content), g_prefs.style);
}

QString copyText(const Quantity &q, CopyContent content)
{
    return copyText(q, partsFor(content), g_prefs.style);
}

QString plainText(const Quantity &q)
{
    if (!q.hasValue())
        return QStringLiteral("-");
    const DisplayParts p = displayParts(q);
    QStringList        out;
    out << p.value;
    if (p.asymmetric)
        out << p.errUp << p.errDown;
    else if (!p.errSym.isEmpty())
        out << p.errSym;
    if (!p.unit.isEmpty())
        out << p.unit;
    return stripTrailingSpace(out.join(' '));
}

QString richText(const Quantity &q)
{
    if (!q.hasValue())
        return QStringLiteral("-");
    const DisplayParts p = displayParts(q);
    QString            s = p.value;
    if (p.asymmetric)
        s += QString("<sup><small>%1</small></sup>"
                     "<sub><small>%2</small></sub>")
                 .arg(p.errUp, p.errDown);
    else if (!p.errSym.isEmpty())
        s += " " + p.errSym;
    if (!p.unit.isEmpty())
        s += " " + p.unit;
    return s;
}

} // namespace QuantityFormat
