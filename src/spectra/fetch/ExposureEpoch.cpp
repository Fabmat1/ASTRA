#include "spectra/fetch/ExposureEpoch.h"

#include "core/BarycentricCorrection.h"

#include <QDate>
#include <QDateTime>
#include <QStringList>
#include <QTime>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <vector>

namespace SpecFetch {

double lamostMjdFromDateString(const QString& raw)
{
    if (raw.isEmpty()) return NAN;

    QDateTime dt = QDateTime::fromString(raw, Qt::ISODate);
    if (!dt.isValid()) {
        // LAMOST also writes single-digit time fields ("...T11:30:0.000"),
        // which strict ISO parsing rejects: take the pieces apart instead.
        const QStringList parts = raw.split(QLatin1Char('T'));
        if (parts.size() == 2) {
            const QDate d =
                QDate::fromString(parts[0], QStringLiteral("yyyy-MM-dd"));
            const QStringList hms = parts[1].split(QLatin1Char(':'));
            if (d.isValid() && hms.size() == 3) {
                const int    h = hms[0].toInt();
                const int    m = hms[1].toInt();
                const double s = hms[2].toDouble();
                dt = QDateTime(d, QTime(h, m, 0), QTimeZone::UTC);
                dt = dt.addMSecs(qint64(s * 1000.0));
            }
        }
    }
    if (!dt.isValid()) return NAN;
    QDateTime utc = dt;
    utc.setTimeZone(QTimeZone::UTC);
    return utc.toMSecsSinceEpoch() / 86400000.0 + 40587.0;
}

double lamostExposureMidMjd(double obsMjdUtc,
                            double begMjdLocal,
                            double endMjdLocal,
                            double localMinuteStamp,
                            double exposureSec)
{
    if (!std::isnan(begMjdLocal) && !std::isnan(endMjdLocal) &&
        endMjdLocal > begMjdLocal) {
        const double mid =
            0.5 * (begMjdLocal + endMjdLocal) - kLamostBeijingOffsetDays;
        const double spanSec = (endMjdLocal - begMjdLocal) * 86400.0;
        const bool spanOk = exposureSec <= 0.0 ||
                            std::abs(spanSec - exposureSec) < kLamostSpanToleranceSec;
        const bool agreesWithObs =
            std::isnan(obsMjdUtc) ||
            std::abs(mid - obsMjdUtc) < kLamostObsToleranceSec / 86400.0;
        if (spanOk && agreesWithObs) return mid;
    }

    if (!std::isnan(obsMjdUtc)) return obsMjdUtc;

    if (localMinuteStamp > 0.0) {
        const double half =
            exposureSec > 0.0 ? exposureSec * 0.5 / 86400.0 : 0.0;
        return localMinuteStamp / 1440.0 - kLamostBeijingOffsetDays + half;
    }
    return NAN;
}

double lamostCoaddMidMjd(const QString& lmjmList,
                         int            nExposures,
                         double         totalExposureSec,
                         double         exptimeSec)
{
    // A long list is cut off at the 68-character card with no CONTINUE card
    // after it, sometimes marked ("...-8600&"), sometimes not ("...-85292"):
    // an entry shorter than the first one is a fragment.
    QString raw = lmjmList.trimmed();
    if (raw.endsWith(QLatin1Char('&'))) raw.chop(1);
    QStringList parts = raw.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    if (parts.size() > 1 && parts.last().trimmed().size() < parts.first().trimmed().size())
        parts.removeLast();
    std::vector<double> starts;
    for (const QString& part : parts) {
        bool ok = false;
        const double m = part.trimmed().toDouble(&ok);
        if (ok && m > 0.0) starts.push_back(m);
    }
    if (starts.empty()) return NAN;
    std::sort(starts.begin(), starts.end());

    const int n = nExposures > 0 ? nExposures : int(starts.size());
    if (int(starts.size()) < n) {
        // The missing starts can only be recovered from a steady cadence.
        if (starts.size() < 2) return NAN;
        double lo = 1e300, hi = -1e300;
        for (size_t i = 1; i < starts.size(); ++i) {
            lo = std::min(lo, starts[i] - starts[i - 1]);
            hi = std::max(hi, starts[i] - starts[i - 1]);
        }
        if (hi - lo > 1.0) return NAN;
        const double step = (starts.back() - starts.front()) / double(starts.size() - 1);
        while (int(starts.size()) < n) starts.push_back(starts.back() + step);
    }
    double perExposureSec = 0.0;
    if (totalExposureSec > 0.0) {
        perExposureSec = totalExposureSec / n;
    } else if (exptimeSec > 0.0) {
        perExposureSec = exptimeSec;
        for (size_t i = 1; i < starts.size(); ++i)
            perExposureSec = std::min(perExposureSec, (starts[i] - starts[i - 1]) * 60.0);
    }

    double sum = 0.0;
    for (const double m : starts) sum += m / 1440.0;
    return sum / double(starts.size()) - kLamostBeijingOffsetDays +
           0.5 * perExposureSec / 86400.0;
}

double sdssMidExposureMjdUtc(double taiBegSeconds, double exposureSec)
{
    if (!(taiBegSeconds > 0.0)) return NAN;

    const double mjdTai = taiBegSeconds / 86400.0;
    const double half   = exposureSec > 0.0 ? exposureSec / (2.0 * 86400.0) : 0.0;
    return mjdTai - BarycentricCorrection::leapSecondsAt(mjdTai) / 86400.0 + half;
}

}   // namespace SpecFetch
