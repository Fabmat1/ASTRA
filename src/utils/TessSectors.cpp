#include "TessSectors.h"

#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QTimeZone>
#include <QMap>

#include <algorithm>
#include <cmath>

namespace {

double jdFromUtc(const QDateTime& dt)
{
    // JD of the Unix epoch is 2440587.5
    return dt.toMSecsSinceEpoch() / 86400000.0 + 2440587.5;
}

QVector<TessSectors::Sector> parseTable()
{
    // One row per spacecraft orbit (two to three per sector); collapse to the
    // full [first orbit start, last orbit end] span of each sector.
    QMap<int, TessSectors::Sector> bySector;

    QFile f(QStringLiteral(":/data/tess_sectors.csv"));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#') ||
                line.startsWith(QStringLiteral("Sector")))
                continue;

            const QStringList cols = line.split(',');
            if (cols.size() < 4) continue;

            bool ok = false;
            const int sec = cols[0].trimmed().toInt(&ok);
            if (!ok || sec <= 0) continue;

            const auto fmt = QStringLiteral("yyyy-MM-dd HH:mm:ss");
            QDateTime start = QDateTime::fromString(cols[2].trimmed(), fmt);
            QDateTime end   = QDateTime::fromString(cols[3].trimmed(), fmt);
            if (!start.isValid() || !end.isValid()) continue;
            start.setTimeZone(QTimeZone::utc());
            end.setTimeZone(QTimeZone::utc());

            auto& s = bySector[sec];
            const double sJd = jdFromUtc(start), eJd = jdFromUtc(end);
            if (s.sector == 0) {
                s.sector  = sec;
                s.startJd = sJd;
                s.endJd   = eJd;
            } else {
                s.startJd = std::min(s.startJd, sJd);
                s.endJd   = std::max(s.endJd, eJd);
            }
        }
    }

    QVector<TessSectors::Sector> out;
    out.reserve(bySector.size());
    for (const auto& s : bySector)   // QMap iterates key-ascending
        out.push_back(s);
    return out;
}

} // anon

namespace TessSectors {

const QVector<Sector>& table()
{
    static const QVector<Sector> tbl = parseTable();
    return tbl;
}

int sectorForJd(double jd)
{
    if (!std::isfinite(jd) || jd <= 0.0) return -1;

    // BJD(TDB) vs the table's UTC differs by ~minutes; also cover the small
    // gap between consecutive orbits of one sector.
    constexpr double tolDays = 0.05;

    const auto& tbl = table();
    auto it = std::upper_bound(tbl.begin(), tbl.end(), jd,
        [](double v, const Sector& s) { return v < s.startJd; });
    if (it == tbl.begin()) return -1;
    --it;
    return (jd <= it->endJd + tolDays) ? it->sector : -1;
}

double ffiCadenceSeconds(int sector)
{
    if (sector <= 26) return 1800.0;
    if (sector <= 55) return 600.0;
    return 200.0;
}

QString cadenceLabel(double medianDtSeconds, int sector)
{
    if (!std::isfinite(medianDtSeconds) || medianDtSeconds <= 0.0)
        return QStringLiteral("unknown cadence");

    if (medianDtSeconds < 60.0)
        return QStringLiteral("20 s short cadence");
    if (medianDtSeconds < 180.0)
        return QStringLiteral("2 min short cadence");

    const double ffi = ffiCadenceSeconds(sector);
    if (std::abs(medianDtSeconds - ffi) / ffi < 0.35) {
        const QString len = ffi >= 1800.0 ? QStringLiteral("30 min")
                          : ffi >= 600.0  ? QStringLiteral("10 min")
                          :                 QStringLiteral("200 s");
        return QStringLiteral("FFI-derived (%1)").arg(len);
    }
    return QStringLiteral("long cadence (~%1 min)")
        .arg(qRound(medianDtSeconds / 60.0));
}

} // namespace TessSectors
