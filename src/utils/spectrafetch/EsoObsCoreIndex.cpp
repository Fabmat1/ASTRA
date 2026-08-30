// src/utils/spectrafetch/EsoObsCoreIndex.cpp

#include "EsoObsCoreIndex.h"

#include "TapHelpers.h"
#include "utils/AppPaths.h"
#include "utils/CdsTapClient.h"
#include "utils/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QDataStream>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace SpecFetch {

namespace {

constexpr char kLogCat[] = "SpecFetch";

constexpr char kTapAsyncUrl[] = "https://archive.eso.org/tap_obs/async";
constexpr char kTapSyncUrl[]  = "https://archive.eso.org/tap_obs/sync";

// Bump when Row or the file layout changes; an older file then reads as
// absent and is rebuilt.
constexpr quint32 kMagic         = 0x41455343;   // "AESC"
constexpr quint32 kFormatVersion = 2;

// Distinguishes a cache written on this machine's byte order from one copied
// off another. The Row array goes to disk raw, so a mismatch is unreadable
// rather than merely stale.
constexpr quint32 kByteOrderMark = 0x01020304;

// The columns the ESO client actually reads off a discovered product, in the
// order appendCsv() expects to find them. t_exptime is deliberately absent:
// the client selects it today and never looks at it, and at 2.4 million rows
// an unused column is real transfer time.
constexpr char kColumns[] = "dp_id, obs_collection, instrument_name, t_min, "
                            "em_res_power, snr, access_estsize, s_ra, s_dec, "
                            "obs_release_date";

// ESO's default MAXREC is 20000 and it truncates silently, which on a bulk
// build would quietly amputate the mirror. The service's hard limit is
// 15000000; the whole spectrum slice was 2.42 million on 2026-08-30.
constexpr int kAsyncMaxRec = 5000000;

// Server-side budget for a build job. The service's hard executionDuration is
// 3600 s and the full unfiltered table took 57 s to compute, so this is a
// generous ceiling rather than an expected cost.
constexpr int kAsyncDurationSec = 3000;

// Client-side wall clock for one build job, above the server's own budget so
// that a doomed job is killed by ESO with ESO's message rather than abandoned
// here with ours. Covers the result transfer too: 242 MB at the 4.4 MB/s
// measured on 2026-08-30 is about a minute, and a slow link needs more.
constexpr int kBuildBudgetMs = 3600000;

constexpr int kSyncTimeoutMs = 180000;

// An incremental top-up that turns out to be this large is not a top-up. A
// typical week moves a few hundred spectrum rows, so crossing this means
// something restamped a whole collection and a rebuild is both cheaper and
// more correct (it also picks up withdrawals, which last_mod_date cannot).
constexpr int kIncrementalCeiling = 100000;

// Escapes a collection name for an ADQL string literal. The names are ESO's
// own and contain no quotes today, but this is going into a query.
QString adqlLiteral(const QString& s) {
    return QStringLiteral("'%1'").arg(QString(s).remove(QLatin1Char('\'')));
}

QString collectionFilter(const QStringList& collections) {
    if (collections.isEmpty()) return QString();
    QStringList quoted;
    for (const QString& c : collections) quoted << adqlLiteral(c);
    return QStringLiteral(" AND obs_collection IN (%1)").arg(quoted.join(','));
}

QStringList normalised(const QStringList& collections) {
    QStringList out = collections;
    out.removeDuplicates();
    out.sort();
    return out;
}

// strtod over a non-terminated field. Returns NaN for an empty or unparsable
// one, which is exactly what the callers want to store.
double fieldToDouble(const char* begin, const char* end) {
    if (begin >= end) return std::nan("");
    char  buf[64];
    const size_t n = std::min(sizeof(buf) - 1, size_t(end - begin));
    std::memcpy(buf, begin, n);
    buf[n] = '\0';
    char*        stop = nullptr;
    const double v    = std::strtod(buf, &stop);
    return stop == buf ? std::nan("") : v;
}

}   // namespace

// "2026-08-30T09:30:36Z" and "2026-08-30T09:30:36.123Z" are the two forms ESO
// uses for obs_release_date; they agree on the first 19 characters and that is
// all the precision this needs. Hand-rolled rather than QDateTime::fromString
// because it runs on every one of 2.4 million rows.
qint64 EsoObsCoreIndex::parseIsoUtc(const char* begin, const char* end) {
    if (end - begin < 19) return kReleaseUnknown;

    const auto num = [begin](int offset, int digits) {
        int v = 0;
        for (int i = 0; i < digits; ++i) {
            const char c = begin[offset + i];
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    };
    const int year = num(0, 4), month = num(5, 2), day = num(8, 2);
    const int hour = num(11, 2), minute = num(14, 2), second = num(17, 2);
    if (year < 0 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 ||
        second > 60)
        return kReleaseUnknown;

    // days_from_civil (Howard Hinnant's proleptic Gregorian algorithm), which
    // is branch-free and needs no timezone database - the values are UTC.
    const int      y   = year - (month <= 2 ? 1 : 0);
    const int      era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy =
        unsigned((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned doe  = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const qint64   days = qint64(era) * 146097 + qint64(doe) - 719468;

    return days * 86400 + qint64(hour) * 3600 + qint64(minute) * 60 + second;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Accessors
// ─────────────────────────────────────────────────────────────────────────────

QString EsoObsCoreIndex::dpId(int i) const {
    const Row& r = _rows[size_t(i)];
    // Not NUL-terminated when a dp_id is exactly 32 bytes long.
    const size_t n = ::strnlen(r.dpId, sizeof(r.dpId));
    return QString::fromLatin1(r.dpId, int(n));
}

QString EsoObsCoreIndex::collectionOf(int i) const {
    const quint16 k = _rows[size_t(i)].collection;
    return k < _collectionNames.size() ? _collectionNames.at(k) : QString();
}

QString EsoObsCoreIndex::instrumentOf(int i) const {
    const quint16 k = _rows[size_t(i)].instrument;
    return k < _instrumentNames.size() ? _instrumentNames.at(k) : QString();
}

bool EsoObsCoreIndex::covers(const QStringList& collections) const {
    if (!_loaded) return false;
    if (_covered.isEmpty()) return true;    // built unfiltered: covers all
    if (collections.isEmpty()) return false;   // wants all, has a subset
    for (const QString& c : collections)
        if (!_covered.contains(c, Qt::CaseInsensitive)) return false;
    return true;
}

QString EsoObsCoreIndex::cachePath() {
    return AppPaths::root() + QStringLiteral("/cache/eso_obscore_spectra.idx");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Matching
// ─────────────────────────────────────────────────────────────────────────────

void EsoObsCoreIndex::matchNear(double ra, double dec, double radiusDeg,
                                std::vector<std::pair<int, double>>& out) const {
    if (_rows.empty() || std::isnan(ra) || std::isnan(dec)) return;

    // The rows are sorted by declination, so the candidates are a contiguous
    // band and everything outside it is one comparison away from being ruled
    // out. On 88000 stars against 2.4 million rows this is the whole cost.
    const auto byDec = [](const Row& r, double d) { return r.dec < d; };
    const auto lo    = std::lower_bound(_rows.begin(), _rows.end(),
                                        dec - radiusDeg, byDec);
    const auto hi    = std::upper_bound(_rows.begin(), _rows.end(),
                                        dec + radiusDeg,
                                        [](double d, const Row& r) {
                                            return d < r.dec;
                                        });

    const double d2r    = M_PI / 180.0;
    const double r2     = radiusDeg * radiusDeg;
    const double cosDec = std::cos(dec * d2r);

    for (auto it = lo; it != hi; ++it) {
        // Wrapped, so a star at RA 359.999 still matches a product at 0.001.
        double dra = it->ra - ra;
        if (dra > 180.0) dra -= 360.0;
        else if (dra < -180.0) dra += 360.0;

        const double x  = dra * cosDec;
        const double y  = it->dec - dec;
        const double d2 = x * x + y * y;
        if (d2 <= r2)
            out.emplace_back(int(it - _rows.begin()), std::sqrt(d2));
    }
}

void EsoObsCoreIndex::sortByDec() {
    std::sort(_rows.begin(), _rows.end(),
              [](const Row& a, const Row& b) { return a.dec < b.dec; });
}

// ─────────────────────────────────────────────────────────────────────────────
//  CSV -> rows
// ─────────────────────────────────────────────────────────────────────────────

void EsoObsCoreIndex::rebuildInternMaps() {
    _collectionIds.clear();
    _instrumentIds.clear();
    for (int i = 0; i < _collectionNames.size(); ++i)
        _collectionIds.insert(_collectionNames.at(i).toUtf8(), quint16(i));
    for (int i = 0; i < _instrumentNames.size(); ++i)
        _instrumentIds.insert(_instrumentNames.at(i).toUtf8(), quint16(i));
}

quint16 EsoObsCoreIndex::internCollection(const QByteArray& name) {
    const auto it = _collectionIds.constFind(name);
    if (it != _collectionIds.constEnd()) return it.value();
    _collectionNames.append(QString::fromUtf8(name));
    const quint16 id = quint16(_collectionNames.size() - 1);
    _collectionIds.insert(name, id);
    return id;
}

quint16 EsoObsCoreIndex::internInstrument(const QByteArray& name) {
    const auto it = _instrumentIds.constFind(name);
    if (it != _instrumentIds.constEnd()) return it.value();
    _instrumentNames.append(QString::fromUtf8(name));
    const quint16 id = quint16(_instrumentNames.size() - 1);
    _instrumentIds.insert(name, id);
    return id;
}

int EsoObsCoreIndex::appendCsv(const QByteArray& body, QString* error) {
    if (body.isEmpty()) {
        if (error) *error = QStringLiteral("empty TAP response");
        return -1;
    }

    const char* p   = body.constData();
    const char* end = p + body.size();

    // Header, so the column order is read rather than assumed.
    const char* nl = static_cast<const char*>(::memchr(p, '\n', size_t(end - p)));
    if (!nl) {
        if (error) *error = QStringLiteral("TAP response has no header row");
        return -1;
    }
    QList<QByteArray> header =
        QByteArray(p, int(nl - p)).trimmed().toLower().split(',');
    for (QByteArray& h : header) h = h.trimmed();

    const auto colOf = [&header](const char* name) {
        return header.indexOf(QByteArray(name));
    };
    const int cDpId  = colOf("dp_id");
    const int cColl  = colOf("obs_collection");
    const int cInst  = colOf("instrument_name");
    const int cTMin  = colOf("t_min");
    const int cRes   = colOf("em_res_power");
    const int cSnr   = colOf("snr");
    const int cSize  = colOf("access_estsize");
    const int cRa    = colOf("s_ra");
    const int cDec   = colOf("s_dec");
    const int cRel   = colOf("obs_release_date");
    if (cDpId < 0 || cRa < 0 || cDec < 0) {
        if (error)
            *error = QStringLiteral("TAP response is missing dp_id/s_ra/s_dec");
        return -1;
    }
    const int nCols = header.size();

    p = nl + 1;

    // These columns never contain a quoted field or an embedded comma (all
    // 2.42 million rows checked on 2026-08-30: every one split into exactly
    // the selected number of fields), so a plain scan is enough and a general
    // CSV parser would only cost time. A row that does not split cleanly is
    // dropped rather than misread.
    std::vector<const char*> fieldBegin(size_t(nCols) + 1);
    std::vector<const char*> fieldEnd(size_t(nCols) + 1);

    int added = 0, skippedLong = 0, skippedShape = 0;
    _rows.reserve(_rows.size() + size_t(body.size() / 100));

    while (p < end) {
        const char* lineEnd =
            static_cast<const char*>(::memchr(p, '\n', size_t(end - p)));
        if (!lineEnd) lineEnd = end;
        const char* lineStop = lineEnd;
        if (lineStop > p && *(lineStop - 1) == '\r') --lineStop;

        if (lineStop == p) { p = lineEnd + 1; continue; }

        int         nf = 0;
        const char* f  = p;
        for (const char* q = p; q <= lineStop; ++q) {
            if (q == lineStop || *q == ',') {
                if (nf < nCols) {
                    fieldBegin[size_t(nf)] = f;
                    fieldEnd[size_t(nf)]   = q;
                }
                ++nf;
                f = q + 1;
            }
        }
        p = lineEnd + 1;

        if (nf != nCols) { ++skippedShape; continue; }

        const size_t idLen = size_t(fieldEnd[size_t(cDpId)] -
                                    fieldBegin[size_t(cDpId)]);
        if (idLen == 0) { ++skippedShape; continue; }
        if (idLen > sizeof(Row::dpId)) { ++skippedLong; continue; }

        Row r;
        std::memcpy(r.dpId, fieldBegin[size_t(cDpId)], idLen);

        r.ra  = fieldToDouble(fieldBegin[size_t(cRa)], fieldEnd[size_t(cRa)]);
        r.dec = fieldToDouble(fieldBegin[size_t(cDec)], fieldEnd[size_t(cDec)]);
        if (std::isnan(r.ra) || std::isnan(r.dec)) { ++skippedShape; continue; }

        if (cTMin >= 0)
            r.tMin = fieldToDouble(fieldBegin[size_t(cTMin)],
                                   fieldEnd[size_t(cTMin)]);
        if (cRes >= 0)
            r.resPower = float(fieldToDouble(fieldBegin[size_t(cRes)],
                                             fieldEnd[size_t(cRes)]));
        if (cSnr >= 0)
            r.snr = float(fieldToDouble(fieldBegin[size_t(cSnr)],
                                        fieldEnd[size_t(cSnr)]));
        if (cSize >= 0) {
            const double kb = fieldToDouble(fieldBegin[size_t(cSize)],
                                            fieldEnd[size_t(cSize)]);
            r.estSizeKb = std::isnan(kb) || kb < 0 ? -1 : qint32(kb);
        }
        r.releasedAtSec =
            cRel >= 0 ? parseIsoUtc(fieldBegin[size_t(cRel)],
                                    fieldEnd[size_t(cRel)])
                      : kReleaseUnknown;
        if (cColl >= 0)
            r.collection = internCollection(
                QByteArray(fieldBegin[size_t(cColl)],
                           int(fieldEnd[size_t(cColl)] -
                               fieldBegin[size_t(cColl)])));
        if (cInst >= 0)
            r.instrument = internInstrument(
                QByteArray(fieldBegin[size_t(cInst)],
                           int(fieldEnd[size_t(cInst)] -
                               fieldBegin[size_t(cInst)])));

        _rows.push_back(r);
        ++added;
    }

    if (skippedLong > 0)
        LOG_WARNING(kLogCat,
                    QStringLiteral("ESO index: %1 product(s) skipped, dp_id "
                                   "longer than %2 bytes")
                        .arg(skippedLong)
                        .arg(int(sizeof(Row::dpId))));
    if (skippedShape > 0)
        LOG_WARNING(kLogCat,
                    QStringLiteral("ESO index: %1 malformed CSV row(s) skipped")
                        .arg(skippedShape));
    return added;
}

void EsoObsCoreIndex::dropDpIds(const QByteArray* ids, int count) {
    if (count <= 0) return;
    QSet<QByteArray> drop;
    drop.reserve(count);
    for (int i = 0; i < count; ++i) drop.insert(ids[i]);

    // fromRawData rather than a copy: this runs over every row in the mirror
    // to drop a few hundred, and 2.4 million short-lived QByteArrays is the
    // whole cost of an otherwise trivial merge.
    const auto it = std::remove_if(
        _rows.begin(), _rows.end(), [&drop](const Row& r) {
            return drop.contains(QByteArray::fromRawData(
                r.dpId, int(::strnlen(r.dpId, sizeof(r.dpId)))));
        });
    _rows.erase(it, _rows.end());
}

// ─────────────────────────────────────────────────────────────────────────────
//  Cache file
// ─────────────────────────────────────────────────────────────────────────────

bool EsoObsCoreIndex::loadCached() {
    if (_loaded) return true;
    QString ignored;
    return load(&ignored);
}

bool EsoObsCoreIndex::load(QString* error) {
    _loaded = false;
    _rows.clear();
    _collectionNames.clear();
    _instrumentNames.clear();
    _collectionIds.clear();
    _instrumentIds.clear();
    _covered.clear();
    _watermark.clear();
    _builtAtMs = 0;

    QFile f(cachePath());
    if (!f.exists()) {
        if (error) *error = QStringLiteral("no cached index");
        return false;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return false;
    }

    QDataStream s(&f);
    s.setByteOrder(QDataStream::LittleEndian);
    s.setFloatingPointPrecision(QDataStream::DoublePrecision);

    quint32 magic = 0, version = 0, rowSize = 0, byteOrder = 0, rowCount = 0;
    s >> magic >> version >> rowSize >> byteOrder >> rowCount;
    // The Row array is raw bytes, so the layout and the machine's byte order
    // are as much part of the format as the version is.
    if (magic != kMagic || version != kFormatVersion ||
        rowSize != quint32(sizeof(Row)) || byteOrder != kByteOrderMark) {
        // A cache is disposable: a file from an older layout is not an error
        // to report to the user, it is a rebuild.
        if (error) *error = QStringLiteral("cached index has an old layout");
        return false;
    }

    s >> _builtAtMs >> _watermark >> _covered >> _collectionNames
      >> _instrumentNames;
    if (s.status() != QDataStream::Ok) {
        if (error) *error = QStringLiteral("cached index header is truncated");
        return false;
    }

    // A truncated or corrupt header must not be allowed to ask for an
    // arbitrary allocation, so the claim is checked against what is left in
    // the file before a single row is reserved.
    const qint64 want      = qint64(rowCount) * qint64(sizeof(Row));
    const qint64 remaining = f.size() - f.pos();
    if (want > remaining) {
        if (error) *error = QStringLiteral("cached index is truncated");
        return false;
    }

    _rows.resize(rowCount);
    if (rowCount > 0) {
        const qint64 got = s.readRawData(reinterpret_cast<char*>(_rows.data()),
                                         int(want));
        if (got != want) {
            _rows.clear();
            if (error) *error = QStringLiteral("cached index is truncated");
            return false;
        }
    }

    rebuildInternMaps();
    _loaded = true;
    return true;
}

bool EsoObsCoreIndex::save(QString* error) const {
    QDir().mkpath(QFileInfo(cachePath()).absolutePath());

    // QSaveFile so an interrupted write leaves the previous mirror in place
    // rather than a half-written one that reads as corrupt on next start.
    QSaveFile f(cachePath());
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }

    QDataStream s(&f);
    s.setByteOrder(QDataStream::LittleEndian);
    s.setFloatingPointPrecision(QDataStream::DoublePrecision);

    s << kMagic << kFormatVersion << quint32(sizeof(Row)) << kByteOrderMark
      << quint32(_rows.size()) << _builtAtMs
      << _watermark << _covered << _collectionNames << _instrumentNames;
    if (!_rows.empty()) {
        const qint64 want = qint64(_rows.size()) * qint64(sizeof(Row));
        if (s.writeRawData(reinterpret_cast<const char*>(_rows.data()),
                           int(want)) != want) {
            if (error) *error = QStringLiteral("short write");
            return false;
        }
    }

    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Build and refresh
// ─────────────────────────────────────────────────────────────────────────────

bool EsoObsCoreIndex::ensure(const QStringList& collections,
                             QNetworkAccessManager*                 nam,
                             const std::function<void(int, int)>&   progress,
                             int progressTotal, const std::atomic<bool>& cancel,
                             QString* error) {
    if (error) error->clear();
    const QStringList want = normalised(collections);

    QString loadError;
    if (!_loaded) load(&loadError);

    if (_loaded && covers(want)) {
        const qint64 ageMs =
            QDateTime::currentMSecsSinceEpoch() - _builtAtMs;
        const qint64 maxAgeMs = qint64(kFullRebuildDays) * 86400000LL;

        if (ageMs < maxAgeMs) {
            QString refreshError;
            if (refreshIncremental(nam, cancel, &refreshError)) {
                if (progress) progress(progressTotal, progressTotal);
                return true;
            }
            if (cancel.load()) return true;   // stale but usable

            // A top-up that will not run is not a reason to throw away a
            // mirror that is at most kFullRebuildDays old: use it and say so.
            LOG_WARNING(kLogCat,
                        QStringLiteral("ESO index: refresh failed (%1); using "
                                       "the cached mirror as it stands")
                            .arg(refreshError));
            if (progress) progress(progressTotal, progressTotal);
            return true;
        }
        LOG_INFO(kLogCat,
                 QStringLiteral("ESO index: mirror is %1 days old, rebuilding")
                     .arg(ageMs / 86400000LL));
    }

    return rebuildFull(want, nam, progress, progressTotal, cancel, error);
}

bool EsoObsCoreIndex::rebuildFull(const QStringList&     collections,
                                  QNetworkAccessManager* nam,
                                  const std::function<void(int, int)>& progress,
                                  int                      progressTotal,
                                  const std::atomic<bool>& cancel,
                                  QString*                 error) {
    _rows.clear();
    _collectionNames.clear();
    _instrumentNames.clear();
    _collectionIds.clear();
    _instrumentIds.clear();
    _loaded = false;

    // Ask for the watermark before the rows, not after: anything published
    // while the build runs then lands in the next incremental refresh instead
    // of falling into the gap between the two.
    QString watermark;
    {
        CdsTap::Request req(kSyncTimeoutMs);
        req.cancel = &cancel;
        QString          wmError;
        const QByteArray body = tapQuery(
            nam, QString::fromLatin1(kTapSyncUrl),
            QStringLiteral("SELECT max(last_mod_date) AS mx FROM ivoa.ObsCore "
                           "WHERE dataproduct_type = 'spectrum'%1")
                .arg(collectionFilter(collections)),
            QStringLiteral("csv"), req, &wmError);
        const Csv csv = parseCsv(body);
        if (csv.rows.size() == 1)
            watermark = csv.value(0, QStringLiteral("mx")).trimmed();
        if (watermark.isEmpty())
            LOG_WARNING(kLogCat,
                        QStringLiteral("ESO index: no last_mod_date watermark "
                                       "(%1); refreshes will rebuild instead")
                            .arg(wmError));
    }

    // One job per selected collection rather than one job for all of them:
    // the caller's progress bar has nothing else to move on while the build
    // runs, a collection that will not run does not take the rest with it,
    // and it is still a handful of requests against the hundreds the per-star
    // crossmatch used to issue. With no filter it is a single job.
    QStringList jobs = collections;
    if (jobs.isEmpty()) jobs << QString();

    QElapsedTimer timer;
    timer.start();

    int     ok = 0;
    QString lastError;

    for (int j = 0; j < jobs.size(); ++j) {
        if (cancel.load()) break;

        const QString filter =
            jobs.at(j).isEmpty() ? QString()
                                 : QStringLiteral(" AND obs_collection = %1")
                                       .arg(adqlLiteral(jobs.at(j)));
        const QString adql =
            QStringLiteral("SELECT %1 FROM ivoa.ObsCore "
                           "WHERE dataproduct_type = 'spectrum'%2")
                .arg(QString::fromLatin1(kColumns), filter);

        CdsTap::Request req(kBuildBudgetMs);
        req.cancel = &cancel;
        QString          jobError;
        const QByteArray body =
            tapAsyncQuery(nam, QString::fromLatin1(kTapAsyncUrl), adql,
                          QStringLiteral("csv"), kAsyncDurationSec,
                          kAsyncMaxRec, req, &jobError);

        if (!jobError.isEmpty()) {
            if (cancel.load()) break;
            lastError = jobError;
            LOG_WARNING(kLogCat,
                        QStringLiteral("ESO index: %1 could not be mirrored "
                                       "(%2)")
                            .arg(jobs.at(j).isEmpty()
                                     ? QStringLiteral("the spectrum table")
                                     : jobs.at(j),
                                 jobError));
        } else {
            const int added = appendCsv(body, &lastError);
            if (added < 0) {
                LOG_WARNING(kLogCat,
                            QStringLiteral("ESO index: %1 returned an "
                                           "unreadable table (%2)")
                                .arg(jobs.at(j), lastError));
            } else {
                ++ok;
                LOG_INFO(kLogCat,
                         QStringLiteral("ESO index: mirrored %1 product(s) "
                                        "from %2")
                             .arg(added)
                             .arg(jobs.at(j).isEmpty()
                                      ? QStringLiteral("ivoa.ObsCore")
                                      : jobs.at(j)));
                // MAXREC truncation is silent in CSV, and a truncated mirror
                // would read as "these stars have no spectra".
                if (added >= kAsyncMaxRec)
                    LOG_WARNING(kLogCat,
                                QStringLiteral("ESO index: %1 hit the %2-row "
                                               "limit; products were dropped")
                                    .arg(jobs.at(j))
                                    .arg(kAsyncMaxRec));
            }
        }

        if (progress)
            progress(int(qint64(progressTotal) * (j + 1) / jobs.size()),
                     progressTotal);
    }

    if (cancel.load()) {
        if (error) error->clear();
        return false;
    }
    if (ok == 0) {
        if (error)
            *error = lastError.isEmpty()
                         ? QStringLiteral("could not mirror the ESO spectrum "
                                          "index")
                         : lastError;
        return false;
    }

    sortByDec();
    _covered   = normalised(collections);
    _watermark = watermark;
    _builtAtMs = QDateTime::currentMSecsSinceEpoch();
    _loaded    = true;

    QString saveError;
    if (!save(&saveError))
        LOG_WARNING(kLogCat,
                    QStringLiteral("ESO index: could not write the cache (%1); "
                                   "this run works, the next one rebuilds")
                        .arg(saveError));

    LOG_INFO(kLogCat,
             QStringLiteral("ESO index: %1 product(s) over %2 collection(s) in "
                            "%3 s")
                 .arg(_rows.size())
                 .arg(_collectionNames.size())
                 .arg(timer.elapsed() / 1000.0, 0, 'f', 1));

    // A partial mirror is worse than a slow one: the stars behind the missing
    // collection would come back empty and look like a real answer.
    if (ok < jobs.size() && error)
        *error = QStringLiteral("%1 of %2 collection(s) could not be mirrored "
                                "(%3)")
                     .arg(jobs.size() - ok)
                     .arg(jobs.size())
                     .arg(lastError);
    return true;
}

bool EsoObsCoreIndex::refreshIncremental(QNetworkAccessManager*   nam,
                                         const std::atomic<bool>& cancel,
                                         QString*                 error) {
    if (_watermark.isEmpty()) {
        if (error) *error = QStringLiteral("no watermark to refresh from");
        return false;
    }

    const QString filter = collectionFilter(_covered);
    const QString since  = QStringLiteral(" AND last_mod_date > '%1'")
                               .arg(QString(_watermark).remove(QLatin1Char('\'')));

    // How much has moved decides whether this is a top-up at all.
    CdsTap::Request req(kSyncTimeoutMs);
    req.cancel = &cancel;

    int     changed = 0;
    QString countError;
    {
        const QByteArray body = tapQuery(
            nam, QString::fromLatin1(kTapSyncUrl),
            QStringLiteral("SELECT count(*) AS n FROM ivoa.ObsCore WHERE "
                           "dataproduct_type = 'spectrum'%1%2")
                .arg(filter, since),
            QStringLiteral("csv"), req, &countError);
        const Csv csv = parseCsv(body);
        if (csv.rows.size() != 1) {
            if (error)
                *error = countError.isEmpty()
                             ? QStringLiteral("could not count changed rows")
                             : countError;
            return false;
        }
        changed = csv.value(0, QStringLiteral("n")).toInt();
    }

    if (cancel.load()) return false;

    if (changed == 0) {
        LOG_INFO(kLogCat, QStringLiteral("ESO index: up to date"));
        return true;
    }
    if (changed > kIncrementalCeiling) {
        if (error)
            *error = QStringLiteral("%1 changed rows is past the incremental "
                                    "ceiling")
                         .arg(changed);
        return false;
    }

    const QString adql =
        QStringLiteral("SELECT %1 FROM ivoa.ObsCore WHERE "
                       "dataproduct_type = 'spectrum'%2%3")
            .arg(QString::fromLatin1(kColumns), filter, since);

    QString          jobError;
    const QByteArray body =
        tapQuery(nam, QString::fromLatin1(kTapSyncUrl), adql,
                 QStringLiteral("csv"), req, &jobError);
    if (!jobError.isEmpty()) {
        if (error) *error = jobError;
        return false;
    }

    // Append into a scratch index first, so the mirror is only touched once
    // the whole batch has parsed and the dp_ids to replace are known.
    EsoObsCoreIndex batch;
    batch._collectionNames = _collectionNames;
    batch._instrumentNames = _instrumentNames;
    batch.rebuildInternMaps();
    if (batch.appendCsv(body, error) < 0) return false;

    std::vector<QByteArray> ids;
    ids.reserve(batch._rows.size());
    for (const Row& r : batch._rows)
        ids.emplace_back(r.dpId, int(::strnlen(r.dpId, sizeof(r.dpId))));

    // last_mod_date moves for updated products as well as new ones, so the
    // batch replaces rather than adds.
    dropDpIds(ids.data(), int(ids.size()));

    _collectionNames = batch._collectionNames;
    _instrumentNames = batch._instrumentNames;
    rebuildInternMaps();
    _rows.insert(_rows.end(), batch._rows.begin(), batch._rows.end());
    sortByDec();

    // Advance the watermark to what this batch actually carried, so a product
    // stamped between the count and the fetch is picked up next time.
    QString wmError;
    const QByteArray wmBody = tapQuery(
        nam, QString::fromLatin1(kTapSyncUrl),
        QStringLiteral("SELECT max(last_mod_date) AS mx FROM ivoa.ObsCore "
                       "WHERE dataproduct_type = 'spectrum'%1%2")
            .arg(filter, since),
        QStringLiteral("csv"), req, &wmError);
    const Csv wmCsv = parseCsv(wmBody);
    if (wmCsv.rows.size() == 1) {
        const QString mx = wmCsv.value(0, QStringLiteral("mx")).trimmed();
        if (!mx.isEmpty()) _watermark = mx;
    }

    QString saveError;
    if (!save(&saveError))
        LOG_WARNING(kLogCat,
                    QStringLiteral("ESO index: could not write the refreshed "
                                   "cache (%1)")
                        .arg(saveError));

    LOG_INFO(kLogCat,
             QStringLiteral("ESO index: refreshed %1 product(s), %2 total")
                 .arg(batch._rows.size())
                 .arg(_rows.size()));
    return true;
}

}   // namespace SpecFetch
