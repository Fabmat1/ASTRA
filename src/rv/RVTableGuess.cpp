#include "rv/RVTableGuess.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QTimeZone>

#include <algorithm>
#include <cmath>

namespace RVTableGuess {

namespace {

/// Lower-cased name split into alphanumeric tokens: "BJD-2450000" gives
/// {"bjd", "2450000"}, "e_RV" gives {"e", "rv"}, "rv_err [km/s]" gives
/// {"rv", "err", "km", "s"}.
QStringList tokens(const QString& name)
{
    static const QRegularExpression sep(QStringLiteral("[^a-z0-9]+"));
    return name.toLower().split(sep, Qt::SkipEmptyParts);
}

bool hasAny(const QStringList& toks, std::initializer_list<const char*> words)
{
    for (const char* w : words)
        if (toks.contains(QLatin1String(w))) return true;
    return false;
}

bool isErrorName(const QStringList& t, const QString& lower)
{
    if (hasAny(t, {"err", "error", "errors", "sigma", "sig", "unc",
                   "uncertainty", "e", "std", "stddev", "dev", "eps"}))
        return true;
    // Glued forms: "erv", "rverr", "ervrad", "sigrv", "dvrad".
    static const QRegularExpression glued(
        QStringLiteral("^(e|err|sig|sigma|d)(rv|vrad|vr|v)$|"
                       "^(rv|vrad|vr|v)(err|error|sig|sigma|e)$"));
    if (glued.match(lower).hasMatch()) return true;
    return lower.contains(QChar(0x03C3));    // σ
}

bool isSysName(const QStringList& t)
{
    return hasAny(t, {"sys", "syst", "systematic", "systematics"});
}

int timeScore(const QStringList& t)
{
    if (hasAny(t, {"bjd", "hjd", "mjd", "jd", "btjd", "bkjd", "tcb", "rjd"}))
        return 3;
    if (hasAny(t, {"time", "epoch", "date", "dateobs", "timestamp", "obstime"}))
        return 2;
    if (hasAny(t, {"t"})) return 1;
    return 0;
}

int rvScore(const QStringList& t)
{
    if (hasAny(t, {"rv", "vrad", "radvel", "vr", "vhelio", "vbary", "vhel",
                   "rv1", "rvel"}))
        return 3;
    if (hasAny(t, {"radial", "velocity", "vel"})) return 2;
    if (hasAny(t, {"v"})) return 1;
    return 0;
}

int instrumentScore(const QStringList& t)
{
    if (hasAny(t, {"instrument", "instr", "inst", "spectrograph"})) return 3;
    if (hasAny(t, {"telescope", "tel", "detector", "setup"})) return 2;
    return 0;
}

int componentScore(const QStringList& t)
{
    return hasAny(t, {"component", "comp"}) ? 3 : 0;
}

/// Share of the non-blank cells of `col` that parse as numbers, and how many
/// non-blank cells there were. Looks at the first `limit` rows only.
struct ColumnStats {
    int    nonBlank = 0;
    int    numeric  = 0;
    int    iso      = 0;
    bool   allPositive = true;
    double median   = std::nan("");
};

ColumnStats statsFor(const std::vector<QStringList>& rows, int col,
                     int limit = 200)
{
    ColumnStats s;
    std::vector<double> vals;
    const int n = std::min<int>(limit, static_cast<int>(rows.size()));
    for (int r = 0; r < n; ++r) {
        if (col >= rows[r].size()) continue;
        const QString cell = rows[r][col].trimmed();
        if (cell.isEmpty()) continue;
        ++s.nonBlank;
        bool ok = false;
        const double v = parseNumber(cell, &ok);
        if (ok) {
            ++s.numeric;
            vals.push_back(v);
            if (!(v > 0.0)) s.allPositive = false;
            continue;
        }
        double mjd;
        if (parseIsoDateMjd(cell, &mjd)) ++s.iso;
    }
    if (!vals.empty()) {
        std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
        s.median = vals[vals.size() / 2];
    }
    return s;
}

bool mostlyNumeric(const ColumnStats& s)
{
    return s.nonBlank > 0 && s.numeric * 2 > s.nonBlank;
}

bool mostlyIso(const ColumnStats& s)
{
    return s.nonBlank > 0 && s.iso * 2 > s.nonBlank;
}

}   // namespace

// ─────────────────────────────────────────────────────────────────────────────

double parseNumber(const QString& cell, bool* ok)
{
    QString s = cell.trimmed();
    // Quoted numbers come out of some spreadsheet exports.
    if (s.size() >= 2 && s.startsWith('"') && s.endsWith('"'))
        s = s.mid(1, s.size() - 2).trimmed();

    bool good = false;
    double v = s.toDouble(&good);
    if (!good && s.count(',') == 1 && !s.contains('.')) {
        QString t = s;
        t.replace(',', '.');
        v = t.toDouble(&good);
    }
    // QString::toDouble accepts "nan" and "inf"; neither is a measurement.
    if (good && !std::isfinite(v)) good = false;
    if (ok) *ok = good;
    return good ? v : std::nan("");
}

bool parseIsoDateMjd(const QString& cell, double* mjd)
{
    const QString s = cell.trimmed();
    // Cheap rejection first: an ISO date starts with four digits and a dash.
    static const QRegularExpression head(QStringLiteral("^\\d{4}-\\d{2}-\\d{2}"));
    if (!head.match(s).hasMatch()) return false;

    QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
    if (!dt.isValid()) {
        // "2023-05-01 03:22:11" with a space instead of the 'T'.
        QString t = s;
        if (t.size() > 10 && t[10] == ' ') t[10] = 'T';
        dt = QDateTime::fromString(t, Qt::ISODateWithMs);
    }
    if (!dt.isValid()) return false;
    // No zone given means UTC here, not local time: observing logs are UTC.
    if (dt.timeSpec() == Qt::LocalTime)
        dt.setTimeZone(QTimeZone::UTC);

    if (mjd) *mjd = static_cast<double>(dt.toMSecsSinceEpoch()) / 86400000.0
                    + 40587.0;    // MJD of 1970-01-01
    return true;
}

bool looksLikeHeader(const QStringList& first,
                     const std::vector<QStringList>& following)
{
    if (first.isEmpty()) return false;

    const int nFollow = std::min<int>(5, static_cast<int>(following.size()));
    for (int c = 0; c < first.size(); ++c) {
        const QString cell = first[c].trimmed();
        if (cell.isEmpty()) continue;
        bool ok = false;
        parseNumber(cell, &ok);
        if (ok) continue;
        double mjd;
        if (parseIsoDateMjd(cell, &mjd)) continue;

        // A text cell. It is a header if the rows below hold numbers there,
        // or if there are no rows below to compare against.
        if (nFollow == 0) return true;
        int numericBelow = 0;
        for (int r = 0; r < nFollow; ++r) {
            if (c >= following[r].size()) continue;
            bool okBelow = false;
            parseNumber(following[r][c], &okBelow);
            double m;
            if (okBelow || parseIsoDateMjd(following[r][c], &m)) ++numericBelow;
        }
        if (numericBelow * 2 > nFollow) return true;
    }
    return false;
}

ColumnGuess guessColumns(const QStringList& names,
                         const std::vector<QStringList>& rows,
                         bool hasHeader)
{
    ColumnGuess g;
    const int n = names.size();
    std::vector<ColumnStats> stats;
    stats.reserve(n);
    for (int c = 0; c < n; ++c) stats.push_back(statsFor(rows, c));

    std::vector<QStringList> toks(n);
    std::vector<QString>     lower(n);
    for (int c = 0; c < n; ++c) {
        if (hasHeader) {
            toks[c]  = tokens(names[c]);
            lower[c] = names[c].toLower().remove(QRegularExpression("[^a-z0-9]"));
        }
    }

    std::vector<bool> used(n, false);
    auto pick = [&](auto score, auto accept) {
        int best = -1, bestScore = 0;
        for (int c = 0; c < n; ++c) {
            if (used[c] || !accept(c)) continue;
            const int s = score(c);
            if (s > bestScore) { bestScore = s; best = c; }
        }
        if (best >= 0) used[best] = true;
        return best;
    };

    // ── By name ─────────────────────────────────────────────────────────────
    if (hasHeader) {
        auto numeric = [&](int c) { return mostlyNumeric(stats[c]); };
        auto plain = [&](int c) {
            return !isErrorName(toks[c], lower[c]) && !isSysName(toks[c]);
        };

        g.time = pick([&](int c) { return timeScore(toks[c]); },
                      [&](int c) {
                          return plain(c) && (numeric(c) || mostlyIso(stats[c]));
                      });
        g.sysError = pick(
            [&](int c) { return isSysName(toks[c]) ? 2 + rvScore(toks[c]) : 0; },
            numeric);
        g.rv = pick([&](int c) { return rvScore(toks[c]); },
                    [&](int c) { return plain(c) && numeric(c); });
        g.error = pick(
            [&](int c) {
                // An error column that names the RV beats a bare "err".
                return isErrorName(toks[c], lower[c]) ? 1 + rvScore(toks[c]) : 0;
            },
            numeric);
        g.instrument = pick([&](int c) { return instrumentScore(toks[c]); },
                            [](int) { return true; });
        g.component = pick([&](int c) { return componentScore(toks[c]); },
                           [](int) { return true; });
    }

    // ── By value, for whatever the names did not settle ────────────────────
    if (g.time < 0) {
        // Dates are large; RVs and their errors are not. A column whose
        // median is at least 1000 is an epoch column in any scale ASTRA reads
        // (reduced JDs included).
        g.time = pick([&](int c) {
                          if (mostlyIso(stats[c])) return 2;
                          return mostlyNumeric(stats[c]) &&
                                 std::fabs(stats[c].median) >= 1000.0 ? 1 : 0;
                      },
                      [](int) { return true; });
    }
    if (g.time < 0 && !hasHeader) {
        // Nothing looks like a date; a headerless table conventionally starts
        // with the epoch anyway.
        g.time = pick([&](int c) { return mostlyNumeric(stats[c]) ? n - c : 0; },
                      [](int) { return true; });
    }
    if (g.rv < 0) {
        // The first unclaimed numeric column, preferring ones after the time.
        g.rv = pick([&](int c) {
                        if (!mostlyNumeric(stats[c])) return 0;
                        if (hasHeader && (isErrorName(toks[c], lower[c]) ||
                                          isSysName(toks[c])))
                            return 0;
                        return (c > g.time ? 2 * n : n) - c;
                    },
                    [](int) { return true; });
    }
    if (g.error < 0 && !hasHeader && g.rv >= 0 && g.rv + 1 < n &&
        !used[g.rv + 1] && mostlyNumeric(stats[g.rv + 1]) &&
        stats[g.rv + 1].allPositive) {
        // "time rv err" is the layout every headerless RV table uses.
        g.error = g.rv + 1;
        used[g.error] = true;
    }
    return g;
}

TimeScale scaleFromName(const QString& columnName)
{
    const QStringList t = tokens(columnName);
    const QString     l = columnName.toLower();
    // Longest names first: "btjd" also contains "jd", and "hjd" has to be
    // recognised before the bare-JD fallback claims it.
    if (t.contains("btjd") || l.contains("btjd")) return TimeScale::BTJD;
    if (t.contains("bkjd") || l.contains("bkjd")) return TimeScale::BKJD;
    if (t.contains("tcb"))                         return TimeScale::GaiaTCB;
    if (l.contains("bjd"))                         return TimeScale::BJD;
    if (l.contains("hjd"))                         return TimeScale::HJD;
    if (l.contains("mjd"))                         return TimeScale::MJD;
    if (t.contains("jd") || t.contains("rjd") || l == "jd") return TimeScale::JD;
    return TimeScale::Unknown;
}

ScaleGuess guessTimeScale(const QString& columnName,
                          const QStringList& metadataLines,
                          const std::vector<QStringList>& rows,
                          int column)
{
    ScaleGuess g;
    const ColumnStats s = statsFor(rows, column);
    const TimeScale named = scaleFromName(columnName);
    const double stated = Time::epochOffsetFor(columnName, metadataLines);

    if (mostlyIso(s) && !mostlyNumeric(s)) {
        g.iso   = true;
        g.scale = TimeScale::MJD;
        g.note  = QStringLiteral("ISO dates read as UTC.");
        return g;
    }

    const bool fullJdScale = named == TimeScale::JD || named == TimeScale::HJD ||
                             named == TimeScale::BJD;

    // No values to judge by: go with the name.
    if (std::isnan(s.median)) {
        g.scale  = named == TimeScale::Unknown ? TimeScale::MJD : named;
        g.offset = stated;
        return g;
    }

    const double v = s.median + stated;

    // The name stands when the values agree with it.
    if (named == TimeScale::BTJD || named == TimeScale::BKJD ||
        named == TimeScale::GaiaTCB ||
        (named != TimeScale::Unknown && Time::isPlausibleFor(v, named) &&
         !(named == TimeScale::MJD && v < 3.0e4))) {
        g.scale  = named;
        g.offset = stated;
        return g;
    }

    const QString label = columnName.trimmed().isEmpty()
                              ? QStringLiteral("The time column")
                              : QString("Column '%1'").arg(columnName.trimmed());

    if (v >= 2.0e6) {
        // Full Julian dates under a name that says MJD, or under no name.
        // Published RV tables are overwhelmingly barycentric.
        g.scale = TimeScale::BJD;
        g.offset = stated;
        g.note = named == TimeScale::MJD
            ? label + " is named MJD but holds full Julian dates; read as BJD."
            : QStringLiteral("Full Julian dates; read as BJD.");
        return g;
    }

    if (v >= 3.0e4 && v < 1.0e6) {
        if (fullJdScale) {
            // "HJD" holding 54813.7: the usual JD - 2400000 reduction.
            g.scale  = named;
            g.offset = stated + 2400000.0;
            g.note   = label + " holds JD - 2400000; offset 2400000 assumed.";
        } else {
            g.scale  = TimeScale::MJD;
            g.offset = stated;
        }
        return g;
    }

    if (v >= 1000.0 && v < 3.0e4) {
        // JD - 2450000 is the reduction RV papers use.
        g.scale  = fullJdScale ? named : TimeScale::BJD;
        g.offset = stated + 2450000.0;
        g.note   = QString("%1 holds reduced Julian dates; read as %2 - 2450000.")
                       .arg(label,
                            g.scale == TimeScale::BJD ? QStringLiteral("BJD")
                          : g.scale == TimeScale::HJD ? QStringLiteral("HJD")
                                                      : QStringLiteral("JD"));
        return g;
    }

    // Too small to be any date ASTRA knows: keep the name (or MJD) and let
    // the dialog's plausibility check say so.
    g.scale  = named == TimeScale::Unknown ? TimeScale::MJD : named;
    g.offset = stated;
    return g;
}

}   // namespace RVTableGuess
