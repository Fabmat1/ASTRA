// src/catalog/StarResolver.h
//
// Who is this star? Identifier and position lookups shared by the Add Star and
// Re-identify dialogs, built so that no single archive is a hard dependency.
//
// SIMBAD stays the first choice because one query gives the main id, the Gaia
// DR3 id, the TIC and a position. It is asked on every mirror (CDS, CfA). When
// no SIMBAD answers, the same facts come from elsewhere:
//
//   name     -> position   MAST name lookup, NED, then Sesame on the VizieR
//                          mirrors (IDIA, CfA), which also reach NED and VizieR
//   position -> Gaia DR3   VizieR TAP, else the ESA Gaia archive
//   position -> TIC        MAST (TIC v8.2)
//   TIC      -> position   MAST
//   J-name   -> position   parsed locally (also when embedded: "SDSS J1533...")
//
// What has no stand-in: the SIMBAD main identifier (NED/MAST give their own
// preferred name instead) and the bibliography, which only SIMBAD provides.
//
// Every call is synchronous and spins a local event loop, like the dialogs'
// own lookups, so it can run on the GUI thread or on a worker.

#ifndef STARRESOLVER_H
#define STARRESOLVER_H

#include <QString>

#include <limits>
#include <vector>

class QNetworkAccessManager;

namespace StarResolver {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Match radius for a position typed or resolved to sub-arcsecond precision.
// 3 arcsec is the import wizard's convention; it also absorbs the 2000 -> 2016
// epoch difference for proper motions up to ~200 mas/yr.
inline constexpr double kDefaultMatchArcsec = 3.0;

// The largest bin a J-designation may imply and still be matched to a single
// source automatically. Coarser names ("J1533+3759") need a name resolver.
inline constexpr double kMaxDesignationArcsec = 10.0;

struct Outcome {
    bool    ok = false;
    // No service answered at all, as opposed to "answered: not found".
    bool    unavailable = false;
    QString error;
    QString service;   // who answered (or failed last), for status lines
};

struct SimbadIds {
    QString mainId, sourceId, tic;
    double  ra = kNaN, dec = kNaN;
};

// One sim-script query line ("query id TIC 123", "query coo ..."), on
// whichever SIMBAD mirror answers.
Outcome simbadIdentify(QNetworkAccessManager *nam, const QString &queryLine,
                       SimbadIds &out, int timeoutMs = 30000);

// Bibcodes of the object SIMBAD knows as `ident` ("Gaia DR3 123...").
Outcome simbadBibcodes(QNetworkAccessManager *nam, const QString &ident,
                       std::vector<QString> &out, int timeoutMs = 30000);

struct NamedPosition {
    double  ra = kNaN, dec = kNaN;   // ICRS J2000, degrees
    QString name;                    // the resolver's preferred name
};

// Name -> position without SIMBAD's own front end.
Outcome resolveName(QNetworkAccessManager *nam, const QString &name,
                    NamedPosition &out, int timeoutMs = 20000);

// Position of a J-designation, bare or inside a name, with the radius the
// written digits imply (never below kDefaultMatchArcsec). False when there is
// no designation or its bin is wider than kMaxDesignationArcsec.
bool positionFromDesignation(const QString &text, double &ra, double &dec,
                             double &radiusArcsec);

struct GaiaSource {
    QString sourceId;
    double  ra = kNaN, dec = kNaN, gmag = kNaN, sepArcsec = kNaN;
};

// Gaia DR3 sources within `radiusArcsec`, nearest first.
Outcome gaiaCone(QNetworkAccessManager *nam, double ra, double dec,
                 double radiusArcsec, std::vector<GaiaSource> &out,
                 int timeoutMs = 30000);

struct TicEntry {
    QString tic;
    double  ra = kNaN, dec = kNaN, tmag = kNaN, sepArcsec = kNaN;
};

Outcome ticById(QNetworkAccessManager *nam, const QString &tic, TicEntry &out,
                int timeoutMs = 30000);

// The nearest TIC entry within `radiusArcsec`.
Outcome ticCone(QNetworkAccessManager *nam, double ra, double dec,
                double radiusArcsec, TicEntry &out, int timeoutMs = 30000);

// --- Exposed for tests ---------------------------------------------------

// Parses a sim-script reply framed by the ASTRA begin/end markers.
bool parseSimbadReply(const QString &body, SimbadIds &out, QString &error);
bool parseMastNameReply(const QByteArray &json, NamedPosition &out);
bool parseNedReply(const QByteArray &json, NamedPosition &out);
bool parseSesameReply(const QByteArray &xml, NamedPosition &out);

}   // namespace StarResolver

#endif   // STARRESOLVER_H
