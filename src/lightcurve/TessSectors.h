#pragma once

#include <QString>
#include <QVector>

// ── Local TESS sector ephemeris ─────────────────────────────────────────────
//
// Time ranges of every TESS observing sector, shipped with ASTRA as a
// compiled-in resource (:/data/tess_sectors.csv, derived from the official
// MIT "TESS orbit times" table). No online queries are needed to map a
// lightcurve point onto the sector it was observed in.
//
// The table stores UTC-based Julian dates; TESS lightcurve timestamps are
// barycentric (BJD/TDB), which differs by at most a few minutes. Sector
// boundaries are therefore matched with a small tolerance - irrelevant at
// the multi-day scale of a sector.
namespace TessSectors
{
    struct Sector {
        int    sector   = 0;
        double startJd  = 0.0;   // start of first orbit (UTC-based JD)
        double endJd    = 0.0;   // end of last orbit (UTC-based JD)
    };

    /// Full sector table, ascending in sector number (== ascending in time).
    /// Parsed once on first use from the compiled-in resource.
    const QVector<Sector>& table();

    /// Sector containing the given (B)JD, or -1 if it falls outside every
    /// known sector (including dates beyond the shipped table).
    int sectorForJd(double jd);

    /// Nominal FFI exposure length for a sector, in seconds
    /// (30 min for sectors 1-26, 10 min for 27-55, 200 s from 56 on).
    double ffiCadenceSeconds(int sector);

    /// Human-readable classification of a median point spacing (seconds)
    /// within one sector: 20 s / 2 min short cadence, FFI-derived, or a
    /// generic long-cadence fallback.
    QString cadenceLabel(double medianDtSeconds, int sector);
}
