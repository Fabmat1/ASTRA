// src/utils/spectrafetch/TapHelpers.h
//
// Shared helpers for the archive clients: TAP_UPLOAD positional crossmatch
// payloads, synchronous and asynchronous (UWS) queries against an explicit
// TAP endpoint, and a small CSV response parser.

#ifndef SPECFETCH_TAPHELPERS_H
#define SPECFETCH_TAPHELPERS_H

#include "spectra/fetch/SpectrumArchiveTypes.h"
#include "core/DelimitedTable.h"

#include "catalog/CdsTapClient.h"

#include <algorithm>
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>

#include <vector>

class QNetworkAccessManager;

namespace SpecFetch {

// VOTable 1.3 with columns idx (int), ra, dec (double, deg) - the payload
// for a TAP_UPLOAD positional crossmatch. idx refers into `stars`.
QByteArray buildPositionVOTable(const std::vector<StarQuery>& stars);

// Synchronous TAP query with a VOTable upload against an explicit endpoint.
// The uploaded table is visible to the ADQL as TAP_UPLOAD.<uploadName>.
// Returns the raw body (`format` picks csv or votable); empty + *error set
// on failure.

// Plain synchronous TAP form query (no upload) against an explicit endpoint.
// `req` carries the timeout and, for discovery loops, the cancel flag.
QByteArray tapQuery(QNetworkAccessManager* nam, const QString& url,
                    const QString& adql, const QString& format,
                    const CdsTap::Request& req, QString* error);

// Asynchronous (UWS) TAP query against an explicit /async endpoint.
//
// The point of this over tapQuery is the server-side time budget: a /sync
// query is capped at what the service is willing to hold a connection open
// for (ESO: 120 s), while a UWS job may be given up to the service's hard
// executionDuration (ESO: 3600 s). Bulk positional crossmatches do not fit in
// the former.
//
// Submits the job, raises its execution duration, runs it, polls until it
// leaves the active phases, and returns the result body. `req.timeoutMs` is
// the client's overall wall-clock budget for the whole job, not a per-request
// one; `req.cancel` is polled between and during the individual calls, and a
// cancelled job is aborted and deleted server-side rather than left running.
//
// `executionDurationSec` <= 0 leaves the job at the service default.
// `maxRec` <= 0 leaves the row limit at the service default.
QByteArray tapAsyncQuery(QNetworkAccessManager* nam, const QString& asyncUrl,
                         const QString& adql, const QString& format,
                         int executionDurationSec, int maxRec,
                         const CdsTap::Request& req, QString* error);

// Minimal CSV parse: header row -> lower-cased name->column map, then rows.
// Handles quoted fields and embedded commas; good enough for TAP/SkyServer
// CSV output.
// The CSV reader lives in core/DelimitedTable so the catalogue code can use it
// too; these aliases keep the SpecFetch:: spelling the archive clients use.
using Csv = DelimitedTable::Csv;
using DelimitedTable::parseCsv;

// ── Positional boxes ───────────────────────────────────────────────────────
//
// Several archives will not use their spatial index for a CONTAINS circle
// (ESO and MAST both time out on one), so the query asks for an RA/Dec box
// instead and the corners are trimmed client-side. A box is a superset of its
// circle, which is what keeps the match radius exact.

// "<raCol>/<decCol> lies in the box of half-height `radiusDeg` around this
// star". The RA half-width is inflated by 1/cos(dec) so the box still holds
// the whole circle, clamped near the poles where that blows up, and split in
// two when it straddles the RA origin - `ra BETWEEN 359.9 AND 0.1` is empty,
// not wrapped.
QString boxPredicate(const QString& raCol, const QString& decCol, double ra,
                     double dec, double radiusDeg);

// The same box as [raLo, raHi] x [decLo, decHi] intervals rather than ADQL,
// for queries that join against per-star bounds instead of OR-ing predicates.
// A box straddling the RA origin yields two entries sharing one star.
struct RaDecBox {
    double raLo, raHi, decLo, decHi;
};
std::vector<RaDecBox> boxesFor(double ra, double dec, double radiusDeg);

// Small-angle separation in degrees; plenty for arcsecond-scale radii.
double angularSepDeg(double ra1, double dec1, double ra2, double dec2);

// Parses a VOTable/CSV cell, falling back when the text is empty or not a
// number. Every archive client needs this, so it lives here rather than being
// re-declared in each of them.
double toDoubleOr(const QString& s, double fallback);

// Split a work list into chunks of at most `chunkSize`.
template <typename T>
std::vector<std::vector<T>> chunked(const std::vector<T>& items,
                                    size_t chunkSize) {
    std::vector<std::vector<T>> out;
    for (size_t i = 0; i < items.size(); i += chunkSize) {
        const size_t end = std::min(items.size(), i + chunkSize);
        out.emplace_back(items.begin() + i, items.begin() + end);
    }
    return out;
}

}   // namespace SpecFetch

#endif   // SPECFETCH_TAPHELPERS_H
