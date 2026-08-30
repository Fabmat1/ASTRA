// src/utils/spectrafetch/EsoObsCoreIndex.h
//
// A local mirror of the spectroscopic slice of ESO's ivoa.ObsCore table, so
// that a bulk crossmatch is a memory lookup instead of a conversation with
// archive.eso.org.
//
// The anonymous ESO TAP endpoint offers no TAP_UPLOAD, so the only way to ask
// "which of my 88000 stars have Phase-3 spectra" server-side is an OR-chain of
// per-star predicates, chunked into hundreds of asynchronous jobs. Measured on
// a 88000-star catalogue that is about 40 minutes of wall clock and roughly
// 880 jobs.
//
// The whole spectrum slice, on the other hand, is small: 2.42 million rows as
// of 2026-08-30, against a service that advertises a hard outputLimit of
// 15000000 rows and a hard executionDuration of 3600 s. So it fits in one job
// with room to spare, and the sensible thing is to pull it once and match
// locally. Measured 2026-08-30 against the live service:
//
//   full table, 10 columns   57 s server-side + 57 s transfer, 242 MB CSV
//   88000 stars at 3"        0.6 s to sort, band-search and refine
//
// The mirror is kept in a flat binary file sorted by declination, so a match
// is a binary search for the declination band followed by an angular refine
// over the handful of rows inside it. Refreshes are incremental: ObsCore
// carries last_mod_date, and only a few hundred spectrum rows change in a
// typical week, so a top-up is one small synchronous query.
//
// The file is a cache, not user data. Deleting it costs a rebuild and nothing
// else, and any version or layout mismatch is treated as "not there".

#ifndef SPECFETCH_ESOOBSCOREINDEX_H
#define SPECFETCH_ESOOBSCOREINDEX_H

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>

#include <atomic>
#include <limits>
#include <functional>
#include <vector>

class QNetworkAccessManager;

namespace SpecFetch {

class EsoObsCoreIndex {
public:
    // One ObsCore spectrum product. 72 bytes, no padding, written to disk
    // verbatim - see kFormatVersion before changing anything in here.
    //
    // dp_id has been exactly 27 characters for every one of the 2.42 million
    // rows (checked 2026-08-30), so 32 bytes is a fixed field rather than an
    // offset into a blob. A longer one would be skipped at build time with a
    // warning rather than silently truncated.
    struct Row {
        double  ra   = 0.0;    // deg, ICRS
        double  dec  = 0.0;    // deg
        double  tMin = 0.0;    // MJD of exposure start, NaN if absent
        // Epoch seconds, UTC, when the product leaves its proprietary period.
        // Stored rather than filtered at build time because ESO's release
        // dates roll forward hourly: a mirror built with "public today" baked
        // in would keep hiding products that went public an hour later, and
        // going public does not restamp last_mod_date, so no incremental
        // refresh would ever reveal them. kReleaseUnknown means "treat as
        // public" - not hiding a product because its date would not parse.
        qint64  releasedAtSec = 0;
        float   resPower  = 0.0f;   // NaN if absent
        float   snr       = 0.0f;   // NaN if absent
        qint32  estSizeKb = -1;     // -1 if absent
        quint16 collection = 0;     // index into collectionNames()
        quint16 instrument = 0;     // index into instrumentNames()
        char    dpId[32]   = {};    // NUL-padded, never truncated
    };
    // The Row array is written to the cache verbatim, so its size is part of
    // the file format and is checked on load. The 8-byte members lead so the
    // struct carries no padding.
    static_assert(sizeof(Row) == 80, "Row layout changed; bump kFormatVersion");

    static constexpr qint64 kReleaseUnknown =
        std::numeric_limits<qint64>::min();

    /// Whether the product is out of its proprietary period at `nowSec`.
    /// ASTRA holds no ESO credentials, so a product that is not is one whose
    /// download can only ever come back "Host requires authentication".
    bool isPublic(int i, qint64 nowSec) const {
        const qint64 r = _rows[size_t(i)].releasedAtSec;
        return r == kReleaseUnknown || r <= nowSec;
    }

    /// ISO-8601 UTC timestamp to epoch seconds, for the two forms ESO uses
    /// ("2026-08-30T09:30:36Z" and "...T09:30:36.123Z"). Exposed for testing.
    static qint64 parseIsoUtc(const char* begin, const char* end);

    /// Load the cache file if one is there, without ever going to the
    /// network. Idempotent; returns whether an index is now in memory. Lets a
    /// caller with only a handful of stars find out whether a mirror it can
    /// use already exists before committing to building one.
    bool loadCached();

    /// Rows within `radiusDeg` of the position, as (row index, separation in
    /// degrees). Appends; does not clear. Costs a binary search plus the
    /// declination band, so it is safe to call once per star in a tight loop.
    void matchNear(double ra, double dec, double radiusDeg,
                   std::vector<std::pair<int, double>>& out) const;

    /// Build or refresh the mirror so that it covers `collections` (empty
    /// means every collection ESO publishes). Blocking; call from a worker
    /// thread. `progress` is driven over [0, progressTotal] as the build
    /// advances, because that is where the wall clock goes and the caller's
    /// per-star axis stands still until it is done. Returns false and sets
    /// *error only when there is no usable index afterwards.
    bool ensure(const QStringList& collections, QNetworkAccessManager* nam,
                const std::function<void(int, int)>& progress,
                int progressTotal, const std::atomic<bool>& cancel,
                QString* error);

    int         size() const { return int(_rows.size()); }
    const Row&  row(int i) const { return _rows[size_t(i)]; }
    QString     dpId(int i) const;
    QString     collectionOf(int i) const;
    QString     instrumentOf(int i) const;

    /// Whether a loaded index already covers this collection set, i.e. it was
    /// built either unfiltered or over a superset of `collections`.
    bool covers(const QStringList& collections) const;

    /// Absolute path of the cache file. Public so the UI and tests can report
    /// or drop it.
    static QString cachePath();

    /// How stale a mirror may get before it is rebuilt from scratch rather
    /// than topped up. ObsCore has no tombstones, so an incremental refresh
    /// never learns about withdrawn products; a periodic full rebuild is the
    /// only thing that does.
    static constexpr int kFullRebuildDays = 30;

private:
    bool load(QString* error);
    bool save(QString* error) const;

    bool rebuildFull(const QStringList& collections, QNetworkAccessManager* nam,
                     const std::function<void(int, int)>& progress,
                     int progressTotal, const std::atomic<bool>& cancel,
                     QString* error);
    bool refreshIncremental(QNetworkAccessManager* nam,
                            const std::atomic<bool>& cancel, QString* error);

    // Appends the rows of one TAP CSV body. Parses straight into Row rather
    // than through SpecFetch::parseCsv: the unfiltered body is 242 MB, and a
    // QList<QStringList> of 2.4 million rows would cost gigabytes of QString
    // headers to represent something that fits in 174 MB of Row.
    int appendCsv(const QByteArray& body, QString* error);

    quint16 internCollection(const QByteArray& name);
    quint16 internInstrument(const QByteArray& name);
    void    rebuildInternMaps();

    void sortByDec();
    /// Drops rows whose dp_id appears in `replaced`, so an incremental batch
    /// can be appended without duplicating the products it updates.
    void dropDpIds(const QByteArray* ids, int count);

    std::vector<Row> _rows;            // sorted by dec ascending
    QStringList      _collectionNames;
    QStringList      _instrumentNames;
    // Transient: interning 2.4 million rows through QStringList::indexOf would
    // be tens of millions of string compares and two allocations a row. Not
    // serialised, rebuilt from the name lists on load.
    QHash<QByteArray, quint16> _collectionIds;
    QHash<QByteArray, quint16> _instrumentIds;

    QStringList _covered;      // collection filter this mirror was built with
    QString     _watermark;    // max(last_mod_date) at build time
    qint64      _builtAtMs = 0;
    bool        _loaded    = false;
};

}   // namespace SpecFetch

#endif   // SPECFETCH_ESOOBSCOREINDEX_H
