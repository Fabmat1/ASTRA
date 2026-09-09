#ifndef STARSEARCHQUERY_H
#define STARSEARCHQUERY_H
#pragma once

// Positional interpretation of whatever the user typed into the star search
// box.
//
// A plain substring match over the table cells cannot find the star people
// actually mean. Catalogues and papers abbreviate a J-designation to whatever
// precision the sentence needed, so the same object is "J1533+3759" in a title,
// "J153301.2+375912" in a table, and "J153301.20+375912.3" in our own database.
// None of those three strings contains either of the others. Observers equally
// often paste a position straight from a finding chart or a TAP query
// ("15 33 01.2 +37 59 12.3", "15:33:01 -37:59:12", "233.2550 +37.9867"), and
// the star they mean may carry no J-designation at all.
//
// So the search string is parsed as a *position with a precision*: the digits
// that were supplied fix a bin on the sky, and everything inside that bin is a
// hit. "J1533+3759" bins RA to the minute of time and Dec to the arcminute;
// "J153301.20+375912.3" bins both to their last digit, at which point the
// radius floor (a user setting, 3 arcsec by default) takes over so a position
// that was refined after the designation was minted still matches.
//
// Header-only and QtCore-only, so tests link against nothing else.

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>

namespace StarSearch {

// Default radius floor for a fully-specified position, in arcseconds. Matches
// the import wizard's coordinate-matching convention.
inline constexpr double kDefaultRadiusFloorArcsec = 3.0;

// A parsed sky position plus the half-width of the bin the typed digits imply.
// The tolerances live in coordinate space, not on the sky: a J-designation bins
// RA in units of time, so its RA tolerance must not carry a cos(dec) factor.
// The angular floor is applied at match time, where the star's own declination
// is known.
struct PositionQuery {
    bool   valid     = false;
    double ra        = 0.0; // degrees
    double dec       = 0.0; // degrees
    double raTolDeg  = 0.0; // half-width in RA coordinate degrees
    double decTolDeg = 0.0; // half-width in declination degrees
};

namespace detail {

// Unicode dashes and the degree/quote marks that come with a pasted position.
inline QString canonicalise(const QString &raw) {
    static const QRegularExpression dashRe(
        QStringLiteral("[\\x{2212}\\x{2010}\\x{2011}\\x{2012}\\x{2013}\\x{2014}]"));
    static const QRegularExpression spaceRe(
        QStringLiteral("[\\x{00A0}\\x{202F}\\x{2007}\\x{2009}]"));
    QString s = raw;
    s.replace(dashRe, QStringLiteral("-"));
    s.replace(spaceRe, QStringLiteral(" "));
    return s.trimmed();
}

// Sexagesimal value and bin half-width from a run of digits written without
// separators, as a J-designation does. `digits` is the integer run, `frac` the
// digits after the decimal point (empty when there is none).
//
// The unit is seconds of time for RA and arcseconds for Dec; both are read as
// two digits per field, so the place value of the last supplied digit says how
// coarse the bin is. Four digits ("1533") reach the minutes field, so the bin
// is one minute wide, not one hundred seconds: the resolution has to step
// through the sexagesimal fields rather than powers of ten.
//
// Returns false when the run is too short or too long to be a designation.
inline bool sexagesimalFromDigits(const QString &digits, const QString &frac,
                                  double &value, double &resolution) {
    const int n = digits.size();
    if (n < 2 || n > 6)
        return false;

    QString padded = digits;
    while (padded.size() < 6)
        padded.append(QLatin1Char('0'));

    const double f1 = padded.mid(0, 2).toDouble(); // hours / degrees
    const double f2 = padded.mid(2, 2).toDouble(); // minutes
    const double f3 = padded.mid(4, 2).toDouble(); // seconds

    value = f1 * 3600.0 + f2 * 60.0 + f3;
    if (!frac.isEmpty()) {
        if (n < 6)
            return false; // a fraction only makes sense on the seconds field
        value += QStringLiteral("0.%1").arg(frac).toDouble();
    }

    switch (n) {
    case 2: resolution = 3600.0; break; // whole hours / degrees
    case 3: resolution =  600.0; break; // tens of minutes
    case 4: resolution =   60.0; break; // whole minutes
    case 5: resolution =   10.0; break; // tens of seconds
    default:
        resolution = frac.isEmpty()
                         ? 1.0
                         : std::pow(10.0, -static_cast<double>(frac.size()));
        break;
    }
    return true;
}

// Split a token into its digit run and fractional digits. Rejects anything
// else, so a stray letter cannot be read as a coordinate.
inline bool splitDigits(const QString &token, QString &digits, QString &frac) {
    static const QRegularExpression re(
        QStringLiteral("^(\\d+)(?:\\.(\\d+))?$"));
    const QRegularExpressionMatch m = re.match(token);
    if (!m.hasMatch())
        return false;
    digits = m.captured(1);
    frac   = m.captured(2);
    return true;
}

// The window a value of the given resolution stands for.
//
// A sexagesimal designation is minted by *truncating*, so "J1533" stands for
// everything from 15h33m00s up to (not including) 15h34m00s. Rounding is common
// enough in hand-written positions to be worth covering too, which adds half a
// unit below. The window is therefore [value - res/2, value + res], written as
// a centre and a half-width. Widening it symmetrically to +-res instead would
// make neighbouring bins overlap, so that "J1534+3759" would find the star at
// 15h33m01s.
//
// A decimal value carries no truncation convention, so it gets the plain
// +-res/2.
inline void binWindow(double value, double res, bool truncated, double &centre,
                      double &half) {
    if (truncated) {
        centre = value + 0.25 * res;
        half   = 0.75 * res;
    } else {
        centre = value;
        half   = 0.5 * res;
    }
}

// Assemble a query from RA seconds-of-time and Dec arcseconds, with the
// resolution of each in the same units.
inline PositionQuery build(double raSecOfTime, double raResSec, double decSign,
                           double decArcsec, double decResArcsec,
                           bool truncated = true) {
    double raCentre = 0.0, raHalf = 0.0, decCentre = 0.0, decHalf = 0.0;
    binWindow(raSecOfTime, raResSec, truncated, raCentre, raHalf);
    binWindow(decArcsec, decResArcsec, truncated, decCentre, decHalf);

    PositionQuery q;
    const double raDeg  = raCentre / 3600.0 * 15.0;
    const double decDeg = decSign * decCentre / 3600.0;
    if (raDeg < 0.0 || raDeg >= 360.0 || decDeg < -90.0 || decDeg > 90.0)
        return q;

    q.valid     = true;
    q.ra        = raDeg;
    q.dec       = decDeg;
    q.raTolDeg  = raHalf / 3600.0 * 15.0;
    q.decTolDeg = decHalf / 3600.0;
    return q;
}

// "J153301.20+375912.3", "J1533+3759", "1533-3759", and the same carrying a
// survey prefix, whether or not it is separated: "SDSS J1533+3759",
// "SDSSJ153301.20+375912.3" (the spelling our own catalogue uses), "PG
// 1533+3759".
//
// Two guards keep ordinary names out. The digits must be introduced either by a
// 'J', which may follow letters, or by nothing alphanumeric at all, so the
// variable-star name "V1234+5678" is not read as a position. And at least four
// RA digits are required, since two would match a bare "12+34" anywhere in an
// unrelated string and no real designation is written that way.
inline PositionQuery parseCompact(const QString &s) {
    static const QRegularExpression re(
        QStringLiteral("(?:(?<![0-9.])[Jj]|(?<![0-9A-Za-z.]))"
                       "(\\d{4,6})(?:\\.(\\d+))?"
                       "([+-])"
                       "(\\d{2,6})(?:\\.(\\d+))?"
                       "(?![0-9.])"));
    const QRegularExpressionMatch m = re.match(s);
    if (!m.hasMatch())
        return {};

    double raVal = 0.0, raRes = 0.0, decVal = 0.0, decRes = 0.0;
    if (!sexagesimalFromDigits(m.captured(1), m.captured(2), raVal, raRes))
        return {};
    if (!sexagesimalFromDigits(m.captured(4), m.captured(5), decVal, decRes))
        return {};

    return build(raVal, raRes, m.captured(3) == QLatin1String("-") ? -1.0 : 1.0,
                 decVal, decRes);
}

// Sexagesimal value and bin half-width from separated fields: "15 33 01.2" is
// three fields, "15 33" is two. Unit is seconds of time / arcseconds.
inline bool sexagesimalFromFields(const QStringList &fields, double &value,
                                  double &resolution) {
    if (fields.isEmpty() || fields.size() > 3)
        return false;

    static const double scale[3] = {3600.0, 60.0, 1.0};
    value = 0.0;
    for (int i = 0; i < fields.size(); ++i) {
        bool         ok = false;
        const double v  = fields[i].toDouble(&ok);
        if (!ok || v < 0.0)
            return false;
        if (i > 0 && v >= 60.0)
            return false; // 15 71 00 is not a position
        value += v * scale[i];
    }

    const int    last     = fields.size() - 1;
    const int    dotIndex = fields[last].indexOf(QLatin1Char('.'));
    const int    decimals = dotIndex < 0 ? 0
                                         : fields[last].size() - dotIndex - 1;
    resolution = scale[last] * std::pow(10.0, -static_cast<double>(decimals));
    return true;
}

// "15 33 01.2 +37 59 12.3", "15:33:01.2 -37:59:12", "15h33m01.2s +37d59m12.3s",
// "15 33 +37 59".
inline PositionQuery parseSexagesimal(const QString &s) {
    QString t = s;
    // Unit letters and marks become separators. 'd' is only a separator when it
    // follows a digit, so it cannot chew into a name.
    static const QRegularExpression unitRe(
        QStringLiteral("(?<=[0-9])\\s*[hmsd\\x{00B0}'\":]+"),
        QRegularExpression::CaseInsensitiveOption);
    t.replace(unitRe, QStringLiteral(" "));
    t.replace(QLatin1Char(','), QLatin1Char(' '));
    // Keep a sign glued to the field it introduces.
    static const QRegularExpression signRe(QStringLiteral("([+-])\\s+"));
    t.replace(signRe, QStringLiteral("\\1"));

    static const QRegularExpression wsRe(QStringLiteral("\\s+"));
    const QStringList tokens = t.split(wsRe, Qt::SkipEmptyParts);
    if (tokens.size() < 2 || tokens.size() > 6)
        return {};

    // The declination starts at the signed token, or at the halfway point when
    // the sign was left off and both halves are written out equally.
    int split = -1;
    for (int i = 1; i < tokens.size(); ++i) {
        if (tokens[i].startsWith(QLatin1Char('+')) ||
            tokens[i].startsWith(QLatin1Char('-'))) {
            split = i;
            break;
        }
    }
    double decSign = 1.0;
    if (split < 0) {
        if (tokens.size() % 2 != 0)
            return {};
        split = tokens.size() / 2;
    } else if (tokens[split].startsWith(QLatin1Char('-'))) {
        decSign = -1.0;
    }
    if (split < 1 || split > 3 || tokens.size() - split > 3)
        return {};

    QStringList raFields  = tokens.mid(0, split);
    QStringList decFields = tokens.mid(split);
    if (raFields.first().startsWith(QLatin1Char('+')))
        raFields.first().remove(0, 1);
    if (decFields.first().startsWith(QLatin1Char('+')) ||
        decFields.first().startsWith(QLatin1Char('-')))
        decFields.first().remove(0, 1);

    // A single field on either side is a decimal degree, not a sexagesimal
    // run; parseDegrees() owns that form.
    if (raFields.size() == 1 && decFields.size() == 1)
        return {};

    for (const QString &f : raFields + decFields) {
        QString digits, frac;
        if (!splitDigits(f, digits, frac))
            return {};
    }

    double raVal = 0.0, raRes = 0.0, decVal = 0.0, decRes = 0.0;
    if (!sexagesimalFromFields(raFields, raVal, raRes))
        return {};
    if (!sexagesimalFromFields(decFields, decVal, decRes))
        return {};

    return build(raVal, raRes, decSign, decVal, decRes);
}

// "233.2550 +37.9867", "233.255, 37.9867". Both values are degrees.
//
// At least one of the two must carry a decimal point: without that rule a
// two-word search such as "15 33" would silently become a position query and
// drag in a patch of sky the user never asked about.
inline PositionQuery parseDegrees(const QString &s) {
    QString t = s;
    t.replace(QLatin1Char(','), QLatin1Char(' '));
    static const QRegularExpression signRe(QStringLiteral("([+-])\\s+"));
    t.replace(signRe, QStringLiteral("\\1"));

    static const QRegularExpression wsRe(QStringLiteral("\\s+"));
    const QStringList tokens = t.split(wsRe, Qt::SkipEmptyParts);
    if (tokens.size() != 2)
        return {};
    if (!tokens[0].contains(QLatin1Char('.')) &&
        !tokens[1].contains(QLatin1Char('.')))
        return {};

    static const QRegularExpression numRe(
        QStringLiteral("^([+-]?)(\\d+)(?:\\.(\\d+))?$"));
    const QRegularExpressionMatch ma = numRe.match(tokens[0]);
    const QRegularExpressionMatch md = numRe.match(tokens[1]);
    if (!ma.hasMatch() || !md.hasMatch())
        return {};
    if (ma.captured(1) == QLatin1String("-"))
        return {}; // a negative RA is not a position we can use

    PositionQuery q;
    q.ra  = tokens[0].toDouble();
    q.dec = tokens[1].toDouble();
    if (q.ra < 0.0 || q.ra >= 360.0 || q.dec < -90.0 || q.dec > 90.0)
        return {};

    q.valid     = true;
    q.raTolDeg  = 0.5 * std::pow(10.0, -static_cast<double>(ma.captured(3).size()));
    q.decTolDeg = 0.5 * std::pow(10.0, -static_cast<double>(md.captured(3).size()));
    return q;
}

} // namespace detail

// Read the search string as a sky position, or return an invalid query when it
// is just text. Tried most specific first: a compact designation, then a
// separated sexagesimal pair, then a decimal-degree pair.
inline PositionQuery parsePosition(const QString &raw) {
    const QString s = detail::canonicalise(raw);
    if (s.size() < 4)
        return {};

    PositionQuery q = detail::parseCompact(s);
    if (q.valid)
        return q;
    q = detail::parseSexagesimal(s);
    if (q.valid)
        return q;
    return detail::parseDegrees(s);
}

// Does a star at (raDeg, decDeg) fall inside the queried bin?
//
// The bin is widened to `floorArcsec` in both axes, which is what makes a
// fully-written designation still find a star whose position was refined since
// the designation was minted. The RA floor is divided by cos(dec) so that the
// floor means the same angular distance at every declination, while the bin
// itself stays in coordinate space.
inline bool matchesPosition(const PositionQuery &q, double raDeg, double decDeg,
                            double floorArcsec = kDefaultRadiusFloorArcsec) {
    if (!q.valid || std::isnan(raDeg) || std::isnan(decDeg))
        return false;

    const double floorDeg = std::max(0.0, floorArcsec) / 3600.0;

    const double decTol = std::max(q.decTolDeg, floorDeg);
    if (std::fabs(decDeg - q.dec) > decTol)
        return false;

    const double cosDec = std::max(std::cos(decDeg * M_PI / 180.0), 1.0e-6);
    const double raTol  = std::max(q.raTolDeg, floorDeg / cosDec);

    double dRa = std::fabs(raDeg - q.ra);
    if (dRa > 180.0)
        dRa = 360.0 - dRa; // the 0h wrap
    return dRa <= raTol;
}

// Where a star sits, for search purposes. Stored coordinates win; a star that
// has none is placed by its J-designation, and failing that by a designation
// embedded in its alias ("SDSS J1533+3759"). Without one of the three the star
// simply cannot answer a positional query.
inline bool positionOf(double raDeg, double decDeg, const QString &jname,
                       const QString &alias, double &outRa, double &outDec) {
    if (!std::isnan(raDeg) && !std::isnan(decDeg)) {
        outRa  = raDeg;
        outDec = decDeg;
        return true;
    }
    for (const QString &candidate : {jname, alias}) {
        if (candidate.isEmpty())
            continue;
        const PositionQuery q = detail::parseCompact(detail::canonicalise(candidate));
        if (q.valid) {
            outRa  = q.ra;
            outDec = q.dec;
            return true;
        }
    }
    return false;
}

} // namespace StarSearch

#endif // STARSEARCHQUERY_H
