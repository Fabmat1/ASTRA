#ifndef STARMATCHING_H
#define STARMATCHING_H
#pragma once

// Shared star identification helpers for the import paths.
//
// Catalogue tables identify the same object in incompatible ways: SIMBAD writes
// "* alf Lac" where an observing log writes "Alf Lac", Gaia ids appear both as
// "Gaia DR3 385485619900166400" and as the bare number, and many tables carry
// nothing but coordinates. These helpers give every import page the same
// normalisation rules, the same column auto-detection, and a cone search that
// stays cheap on large catalogues.

#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "models/Star.h"

namespace StarMatching {

// ── Identifier normalisation ────────────────────────────────────

// "Gaia DR3 385485619900166400" / "GaiaDR2 1234567890" → "385485619900166400".
// Anything without an embedded catalogue number is returned trimmed.
inline QString normalizeSourceId(const QString &raw) {
    const QString clean = raw.trimmed();
    if (clean.isEmpty())
        return QString();

    static const QRegularExpression numRe(QStringLiteral("(\\d{10,})"));
    const QRegularExpressionMatch   m = numRe.match(clean);
    if (m.hasMatch())
        return m.captured(1);
    return clean;
}

// Reduce a star name to a key that survives catalogue formatting differences:
// leading object-type markers ("*", "**", "V*", "NAME"), padding inside HD/BD
// designations, and separator/case noise.
//   "* alf Lac"  → "alflac"      "Alf Lac"  → "alflac"
//   "HD   1185"  → "hd1185"      "HD 1185"  → "hd1185"
//   "* mu. For"  → "mufor"       "60Her"    → "60her"
inline QString normalizeAlias(const QString &raw) {
    QString s = raw.trimmed();
    if (s.isEmpty())
        return QString();

    // Strip leading object-type prefixes, possibly stacked ("V* NAME ...").
    static const QRegularExpression prefixRe(
        QStringLiteral("^(?:\\*\\*|\\*|V\\*|SV\\*|CL\\*|NAME)\\s*"),
        QRegularExpression::CaseInsensitiveOption);
    for (;;) {
        const QString before = s;
        s.remove(prefixRe);
        s = s.trimmed();
        if (s == before)
            break;
    }

    static const QRegularExpression noiseRe(QStringLiteral("[\\s._\\-+]+"));
    s.remove(noiseRe);
    return s.toLower();
}

// ── Column auto-detection ───────────────────────────────────────

// Index of the column whose name best fits `patterns`, or -1.
// Exact names beat prefixes beat suffixes beat substrings, and earlier
// patterns win ties, so callers can order patterns by preference. Columns
// containing any `veto` token are ignored (keeps an error column from being
// picked as the value column). With `exactOnly`, nothing but an exact name
// match counts - the only safe rule for two-letter names like "ra"/"de",
// which appear as substrings of "radial_velocity", "declination_error", ...
inline int bestColumnFor(const QStringList &columns, const QStringList &patterns,
                         const QStringList &veto = {}, bool exactOnly = false) {
    static const QRegularExpression noiseRe(QStringLiteral("[\\s_\\-.]+"));
    auto crush = [](QString s) {
        return s.trimmed().toLower().remove(noiseRe);
    };

    int bestIdx   = -1;
    int bestScore = 0;

    for (int i = 0; i < columns.size(); ++i) {
        const QString col = columns[i].trimmed().toLower();
        if (col.isEmpty())
            continue;

        bool vetoed = false;
        for (const QString &v : veto) {
            if (col.contains(v)) {
                vetoed = true;
                break;
            }
        }
        if (vetoed)
            continue;

        const QString colCrushed = crush(col);

        for (int k = 0; k < patterns.size(); ++k) {
            const QString pat        = patterns[k].toLower();
            const QString patCrushed = crush(pat);

            int score = 0;
            if (col == pat || colCrushed == patCrushed)
                score = 1000;
            else if (exactOnly)
                score = 0;
            else if (colCrushed.startsWith(patCrushed))
                score = 300;
            else if (colCrushed.endsWith(patCrushed))
                score = 200;
            else if (colCrushed.contains(patCrushed))
                score = 100;

            if (score == 0)
                continue;
            score += static_cast<int>(patterns.size()) - k; // preference bonus
            if (score > bestScore) {
                bestScore = score;
                bestIdx   = i;
            }
        }
    }
    return bestIdx;
}

// ── Cone search ─────────────────────────────────────────────────

// Angular separation in arcseconds (small-angle, RA wrap aware).
inline double separationArcsec(double ra1, double dec1, double ra2,
                               double dec2) {
    double dRa = ra1 - ra2;
    if (dRa > 180.0)
        dRa -= 360.0;
    else if (dRa < -180.0)
        dRa += 360.0;
    dRa *= std::cos(0.5 * (dec1 + dec2) * M_PI / 180.0);
    const double dDec = dec1 - dec2;
    return std::sqrt(dRa * dRa + dDec * dDec) * 3600.0;
}

// Positions sorted by declination, so a lookup only touches the rows inside
// the declination band instead of the whole catalogue. Built once per import
// run; lookups are O(log n + k) rather than a full scan per table row.
template <typename PayloadT>
class ConeIndex {
  public:
    void reserve(std::size_t n) { _entries.reserve(n); }

    void add(double ra, double dec, PayloadT payload) {
        if (std::isnan(ra) || std::isnan(dec))
            return;
        _entries.push_back({dec, ra, std::move(payload)});
        _sorted = false;
    }

    void finalize() {
        std::sort(_entries.begin(), _entries.end(),
                  [](const Entry &a, const Entry &b) { return a.dec < b.dec; });
        _sorted = true;
    }

    void clear() {
        _entries.clear();
        _sorted = true;
    }

    bool        isEmpty() const { return _entries.empty(); }
    std::size_t size() const { return _entries.size(); }

    // Closest payload within tolArcsec; a default-constructed PayloadT when
    // nothing is in range.
    PayloadT nearest(double ra, double dec, double tolArcsec,
                     double *sepArcsecOut = nullptr) const {
        if (_entries.empty() || std::isnan(ra) || std::isnan(dec) ||
            tolArcsec <= 0.0 || !_sorted)
            return PayloadT{};

        const double tolDeg = tolArcsec / 3600.0;
        auto         lo =
            std::lower_bound(_entries.begin(), _entries.end(), dec - tolDeg,
                             [](const Entry &e, double v) { return e.dec < v; });

        PayloadT best{};
        double   bestSep = tolArcsec;
        bool     found   = false;
        for (auto it = lo; it != _entries.end() && it->dec <= dec + tolDeg;
             ++it) {
            const double sep = separationArcsec(ra, dec, it->ra, it->dec);
            if (sep <= bestSep) {
                bestSep = sep;
                best    = it->payload;
                found   = true;
            }
        }
        if (found && sepArcsecOut)
            *sepArcsecOut = bestSep;
        return best;
    }

  private:
    struct Entry {
        double   dec;
        double   ra;
        PayloadT payload;
    };
    std::vector<Entry> _entries; // sorted by dec once finalize() ran
    bool               _sorted = true;
};

// Cone index over a star list.
class CoordinateIndex : public ConeIndex<std::shared_ptr<Star>> {
  public:
    void build(const std::vector<std::shared_ptr<Star>> &stars) {
        clear();
        reserve(stars.size());
        for (const auto &star : stars) {
            if (!star || !Star::isSet(star->getRa()) ||
                !Star::isSet(star->getDec()))
                continue;
            add(star->getRa(), star->getDec(), star);
        }
        finalize();
    }
};

} // namespace StarMatching

#endif // STARMATCHING_H
