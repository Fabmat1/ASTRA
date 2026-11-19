// src/catalog/StarResolver.cpp

#include "catalog/StarResolver.h"

#include "app/Logger.h"
#include "catalog/CdsTapClient.h"
#include "catalog/StarSearchQuery.h"
#include "core/DelimitedTable.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>
#include <cmath>
#include <functional>

namespace StarResolver {

namespace {

const QString kBegin = QStringLiteral("==ASTRABEGIN==");
const QString kEnd   = QStringLiteral("==ASTRAEND==");

QString digitsOnly(const QString &s) {
    static const QRegularExpression re(QStringLiteral("(\\d{4,})"));
    const auto m = re.match(s);
    return m.hasMatch() ? m.captured(1) : QString();
}

Outcome fromResponse(const CdsTap::Response &r, const QString &service) {
    Outcome o;
    o.ok          = r.ok();
    o.unavailable = r.unavailable || (!r.ok() && !r.cancelled
                                      && r.error.contains(QStringLiteral("timed out")));
    o.error       = r.error;
    o.service     = service;
    return o;
}

QUrlQuery tapForm(const QString &adql) {
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("REQUEST"), QStringLiteral("doQuery"));
    form.addQueryItem(QStringLiteral("LANG"), QStringLiteral("ADQL"));
    form.addQueryItem(QStringLiteral("FORMAT"), QStringLiteral("csv"));
    form.addQueryItem(QStringLiteral("QUERY"), adql);
    return form;
}

QString mastInvokeUrl(const QJsonObject &request) {
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("request"),
                   QString::fromUtf8(QJsonDocument(request).toJson(QJsonDocument::Compact)));
    QUrl url(QStringLiteral("https://mast.stsci.edu/api/v0/invoke"));
    url.setQuery(q);
    return url.toString(QUrl::FullyEncoded);
}

double jsonNumber(const QJsonValue &v) {
    if (v.isDouble())
        return v.toDouble();
    if (v.isString()) {
        bool         ok = false;
        const double d  = v.toString().toDouble(&ok);
        return ok ? d : kNaN;
    }
    return kNaN;
}

QString jsonId(const QJsonValue &v) {
    if (v.isDouble())
        return QString::number(qint64(v.toDouble()));
    return v.toString().trimmed();
}

double separationArcsec(double ra1, double dec1, double ra2, double dec2) {
    const double d2r = M_PI / 180.0;
    const double sd  = std::sin((dec2 - dec1) * d2r / 2.0);
    const double sr  = std::sin((ra2 - ra1) * d2r / 2.0);
    const double a   = sd * sd + std::cos(dec1 * d2r) * std::cos(dec2 * d2r) * sr * sr;
    return 2.0 * std::asin(std::min(1.0, std::sqrt(a))) / d2r * 3600.0;
}

}   // namespace

// ---------------------------------------------------------------------------
// SIMBAD
// ---------------------------------------------------------------------------

bool parseSimbadReply(const QString &body, SimbadIds &out, QString &error) {
    // SIMBAD echoes the script inside ::script::, markers included, so look
    // only in ::data::.
    const int dataIdx = body.indexOf(QStringLiteral("::data::"));
    const int b       = body.indexOf(kBegin, dataIdx >= 0 ? dataIdx : 0);
    const int e       = b >= 0 ? body.indexOf(kEnd, b + kBegin.size()) : -1;

    if (dataIdx < 0 || b < 0 || e < 0) {
        const int errIdx = body.indexOf(QStringLiteral("::error::"));
        if (errIdx >= 0) {
            QString reason;
            // "[2] 'xyzzy': No known catalog could be found"
            static const QRegularExpression lineNo(QStringLiteral("^\\[\\d+\\]\\s*"));
            for (const QString &l : body.mid(errIdx).split('\n').mid(1)) {
                QString t = l.trimmed();
                if (t.isEmpty() || t.startsWith(QStringLiteral("::")))
                    continue;
                reason = t.remove(lineNo);
                break;
            }
            error = reason.isEmpty() ? QStringLiteral("object not found") : reason;
        } else {
            error = QStringLiteral("no data block in the reply");
        }
        return false;
    }

    const QString block = body.mid(b + kBegin.size(), e - b - kBegin.size());
    for (const QString &raw : block.split('\n', Qt::SkipEmptyParts)) {
        const QString line = raw.trimmed();
        const int     eq   = line.indexOf('=');
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        const QString val = line.mid(eq + 1).trimmed();
        if (val.isEmpty() || val == QLatin1String("~"))
            continue;
        bool ok = false;
        if (key == QLatin1String("MAIN"))
            out.mainId = val;
        else if (key == QLatin1String("GAIA"))
            out.sourceId = digitsOnly(val);
        else if (key == QLatin1String("TIC"))
            out.tic = digitsOnly(val);
        else if (key == QLatin1String("RA")) {
            const double v = val.toDouble(&ok);
            if (ok)
                out.ra = v;
        } else if (key == QLatin1String("DEC")) {
            const double v = val.toDouble(&ok);
            if (ok)
                out.dec = v;
        }
    }
    return true;
}

Outcome simbadIdentify(QNetworkAccessManager *nam, const QString &queryLine,
                       SimbadIds &out, int timeoutMs) {
    // One-line format string: a continuation line can break on CRLF.
    QString script = QStringLiteral("format object f1 \"") + kBegin + "\\n"
                     + "MAIN=%MAIN_ID\\n"
                     + "GAIA=%IDLIST(Gaia DR3)\\n"
                     + "TIC=%IDLIST(TIC)\\n"
                     + "RA=%COO(d;A;ICRS;J2000;2000)\\n"
                     + "DEC=%COO(d;D;ICRS;J2000;2000)\\n"
                     + kEnd + "\"\n" + queryLine + "\n";

    CdsTap::Request req(timeoutMs);
    req.maxAttempts = 3;
    const CdsTap::Response r = CdsTap::postSimbadScript(nam, script, req);
    Outcome o = fromResponse(r, QStringLiteral("SIMBAD"));
    if (!r.ok()) {
        LOG_WARNING("StarResolver", QString("SIMBAD unavailable: %1").arg(r.error));
        return o;
    }
    o.service = QStringLiteral("SIMBAD (%1)").arg(QUrl(r.url).host());

    QString err;
    if (!parseSimbadReply(QString::fromUtf8(r.body), out, err)) {
        o.ok    = false;
        o.error = err;
        LOG_INFO("StarResolver", QString("SIMBAD: %1 for '%2'").arg(err, queryLine));
    }
    return o;
}

Outcome simbadBibcodes(QNetworkAccessManager *nam, const QString &ident,
                       std::vector<QString> &out, int timeoutMs) {
    QString escaped = ident;
    escaped.replace('\'', QStringLiteral("''"));
    const QString adql = QString("SELECT r.bibcode FROM ident AS i "
                                 "JOIN has_ref AS h ON h.oidref = i.oidref "
                                 "JOIN ref AS r ON r.oidbib = h.oidbibref "
                                 "WHERE i.id = '%1'")
                             .arg(escaped);
    CdsTap::Request req(timeoutMs);
    req.maxAttempts = 3;
    const CdsTap::Response r = CdsTap::postSimbadTap(nam, tapForm(adql), req);
    Outcome o = fromResponse(r, QStringLiteral("SIMBAD TAP"));
    if (!r.ok())
        return o;
    const DelimitedTable::Csv csv = DelimitedTable::parseCsv(r.body);
    for (int i = 0; i < csv.rows.size(); ++i) {
        const QString bib = csv.value(i, QStringLiteral("bibcode"));
        if (!bib.isEmpty())
            out.push_back(bib);
    }
    return o;
}

// ---------------------------------------------------------------------------
// Name -> position
// ---------------------------------------------------------------------------

bool parseMastNameReply(const QByteArray &json, NamedPosition &out) {
    const QJsonArray arr =
        QJsonDocument::fromJson(json).object().value(QStringLiteral("resolvedCoordinate")).toArray();
    if (arr.isEmpty())
        return false;
    const QJsonObject o = arr.first().toObject();
    out.ra   = jsonNumber(o.value(QStringLiteral("ra")));
    out.dec  = jsonNumber(o.value(QStringLiteral("decl")));
    out.name = o.value(QStringLiteral("canonicalName")).toString();
    return !std::isnan(out.ra) && !std::isnan(out.dec);
}

bool parseNedReply(const QByteArray &json, NamedPosition &out) {
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    const QJsonObject pref = root.value(QStringLiteral("Preferred")).toObject();
    const QJsonObject pos  = pref.value(QStringLiteral("Position")).toObject();
    if (pos.isEmpty())
        return false;
    out.ra   = jsonNumber(pos.value(QStringLiteral("RA")));
    out.dec  = jsonNumber(pos.value(QStringLiteral("Dec")));
    out.name = pref.value(QStringLiteral("Name")).toString();
    return !std::isnan(out.ra) && !std::isnan(out.dec);
}

bool parseSesameReply(const QByteArray &xml, NamedPosition &out) {
    QXmlStreamReader rd(xml);
    double  ra = kNaN, dec = kNaN;
    QString name;
    while (!rd.atEnd()) {
        rd.readNext();
        if (rd.isEndElement() && rd.name() == QLatin1String("Resolver")) {
            if (!std::isnan(ra) && !std::isnan(dec))
                break;
            ra = dec = kNaN;
            name.clear();
        }
        if (!rd.isStartElement())
            continue;
        const auto tag = rd.name();
        if (tag == QLatin1String("jradeg"))
            ra = rd.readElementText().trimmed().toDouble();
        else if (tag == QLatin1String("jdedeg"))
            dec = rd.readElementText().trimmed().toDouble();
        else if (tag == QLatin1String("oname") && name.isEmpty())
            name = rd.readElementText().trimmed();
    }
    if (std::isnan(ra) || std::isnan(dec) || (ra == 0.0 && dec == 0.0))
        return false;
    out.ra   = ra;
    out.dec  = dec;
    out.name = name;
    return true;
}

Outcome resolveName(QNetworkAccessManager *nam, const QString &name,
                    NamedPosition &out, int timeoutMs) {
    Outcome o;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty()) {
        o.error = QStringLiteral("empty name");
        return o;
    }

    CdsTap::Request req(timeoutMs);
    req.maxAttempts = 1;

    struct Resolver {
        QString                                          label;
        QString                                          url;
        std::function<bool(const QByteArray &, NamedPosition &)> parse;
    };
    const QString enc = QString::fromUtf8(QUrl::toPercentEncoding(trimmed));
    const std::vector<Resolver> resolvers{
        {QStringLiteral("MAST name resolver"),
         mastInvokeUrl(QJsonObject{
             {QStringLiteral("service"), QStringLiteral("Mast.Name.Lookup")},
             {QStringLiteral("params"),
              QJsonObject{{QStringLiteral("input"), trimmed},
                          {QStringLiteral("format"), QStringLiteral("json")}}}}),
         parseMastNameReply},
        {QStringLiteral("NED"),
         QStringLiteral("https://ned.ipac.caltech.edu/srs/ObjectLookup?name=") + enc,
         parseNedReply},
        {QStringLiteral("Sesame (IDIA)"),
         QStringLiteral("https://vizier.idia.ac.za/viz-bin/nph-sesame/-ox/SNV?") + enc,
         parseSesameReply},
        {QStringLiteral("Sesame (CfA)"),
         QStringLiteral("https://vizier.cfa.harvard.edu/viz-bin/nph-sesame/-ox/SNV?") + enc,
         parseSesameReply},
    };

    int     answered = 0;
    QString lastErr;
    for (const Resolver &res : resolvers) {
        const CdsTap::Response r = CdsTap::get(nam, res.url, req);
        if (!r.ok()) {
            lastErr = QString("%1: %2").arg(res.label, r.error);
            LOG_WARNING("StarResolver", lastErr);
            continue;
        }
        ++answered;
        NamedPosition p;
        if (res.parse(r.body, p)) {
            out       = p;
            o.ok      = true;
            o.service = res.label;
            LOG_INFO("StarResolver", QString("'%1' -> %2 %3 via %4 (%5)")
                                         .arg(trimmed)
                                         .arg(p.ra, 0, 'f', 6)
                                         .arg(p.dec, 0, 'f', 6)
                                         .arg(res.label, p.name));
            return o;
        }
    }
    o.unavailable = answered == 0;
    o.error       = answered == 0 ? QString("no name resolver answered (%1)").arg(lastErr)
                                  : QString("'%1' not known to MAST, NED or Sesame").arg(trimmed);
    return o;
}

bool positionFromDesignation(const QString &text, double &ra, double &dec,
                             double &radiusArcsec) {
    const StarSearch::PositionQuery q =
        StarSearch::detail::parseCompact(StarSearch::detail::canonicalise(text));
    if (!q.valid)
        return false;
    const double cosDec = std::max(std::cos(q.dec * M_PI / 180.0), 1.0e-6);
    const double halfRa = q.raTolDeg * cosDec * 3600.0;
    const double halfDe = q.decTolDeg * 3600.0;
    // The corner of the bin, so a source anywhere inside it is in the cone.
    const double r = std::hypot(halfRa, halfDe);
    if (r > kMaxDesignationArcsec)
        return false;
    ra           = q.ra;
    dec          = q.dec;
    radiusArcsec = std::max(r, kDefaultMatchArcsec);
    return true;
}

// ---------------------------------------------------------------------------
// Gaia DR3 cone
// ---------------------------------------------------------------------------

Outcome gaiaCone(QNetworkAccessManager *nam, double ra, double dec,
                 double radiusArcsec, std::vector<GaiaSource> &out, int timeoutMs) {
    const double rDeg = radiusArcsec / 3600.0;
    const auto   fmt  = [](double v) { return QString::number(v, 'f', 9); };

    const QString vizier =
        QString("SELECT Source, RA_ICRS, DE_ICRS, Gmag, "
                "DISTANCE(POINT('ICRS',RA_ICRS,DE_ICRS),POINT('ICRS',%1,%2)) AS dist "
                "FROM \"I/355/gaiadr3\" "
                "WHERE 1=CONTAINS(POINT('ICRS',RA_ICRS,DE_ICRS),CIRCLE('ICRS',%1,%2,%3)) "
                "ORDER BY dist ASC")
            .arg(fmt(ra), fmt(dec), fmt(rDeg));
    const QString esa =
        QString("SELECT source_id AS \"Source\", ra AS \"RA_ICRS\", dec AS \"DE_ICRS\", "
                "phot_g_mean_mag AS \"Gmag\", "
                "DISTANCE(POINT('ICRS',ra,dec),POINT('ICRS',%1,%2)) AS dist "
                "FROM gaiadr3.gaia_source "
                "WHERE 1=CONTAINS(POINT('ICRS',ra,dec),CIRCLE('ICRS',%1,%2,%3)) "
                "ORDER BY dist ASC")
            .arg(fmt(ra), fmt(dec), fmt(rDeg));

    CdsTap::Request req(timeoutMs);
    CdsTap::Response r = CdsTap::postVizierForm(nam, tapForm(vizier), req);
    QString service    = QStringLiteral("VizieR");
    if (!r.ok() && r.unavailable && !r.cancelled) {
        LOG_WARNING("StarResolver",
                    QString("VizieR cone unavailable (%1); asking the ESA Gaia archive")
                        .arg(r.error));
        req.maxAttempts = 2;
        r       = CdsTap::postGaiaArchiveForm(nam, tapForm(esa), req);
        service = QStringLiteral("ESA Gaia archive");
    }
    Outcome o = fromResponse(r, service);
    if (!r.ok())
        return o;

    const DelimitedTable::Csv csv = DelimitedTable::parseCsv(r.body);
    for (int i = 0; i < csv.rows.size(); ++i) {
        GaiaSource s;
        s.sourceId = csv.value(i, QStringLiteral("source"));
        s.ra       = csv.number(i, QStringLiteral("ra_icrs"));
        s.dec      = csv.number(i, QStringLiteral("de_icrs"));
        s.gmag     = csv.number(i, QStringLiteral("gmag"));
        const double d = csv.number(i, QStringLiteral("dist"));
        s.sepArcsec = std::isnan(d) ? kNaN : d * 3600.0;
        if (!s.sourceId.isEmpty())
            out.push_back(s);
    }
    std::stable_sort(out.begin(), out.end(), [](const GaiaSource &a, const GaiaSource &b) {
        return a.sepArcsec < b.sepArcsec;
    });
    return o;
}

// ---------------------------------------------------------------------------
// TIC (MAST)
// ---------------------------------------------------------------------------

namespace {

// Rows of a MAST catalogue reply. Artefacts, split and duplicate entries are
// dropped: TIC v8.2 keeps them with a disposition flag.
std::vector<TicEntry> parseTicRows(const QByteArray &json, double ra, double dec) {
    std::vector<TicEntry> rows;
    const QJsonArray data =
        QJsonDocument::fromJson(json).object().value(QStringLiteral("data")).toArray();
    for (const QJsonValue &v : data) {
        const QJsonObject o    = v.toObject();
        const QString     disp = o.value(QStringLiteral("disposition")).toString();
        if (!disp.isEmpty())
            continue;
        TicEntry e;
        e.tic  = jsonId(o.value(QStringLiteral("ID")));
        e.ra   = jsonNumber(o.value(QStringLiteral("ra")));
        e.dec  = jsonNumber(o.value(QStringLiteral("dec")));
        e.tmag = jsonNumber(o.value(QStringLiteral("Tmag")));
        if (!std::isnan(ra) && !std::isnan(e.ra))
            e.sepArcsec = separationArcsec(ra, dec, e.ra, e.dec);
        if (!e.tic.isEmpty())
            rows.push_back(e);
    }
    return rows;
}

}   // namespace

Outcome ticById(QNetworkAccessManager *nam, const QString &tic, TicEntry &out,
                int timeoutMs) {
    const QString id = digitsOnly(tic).isEmpty() ? tic.trimmed() : digitsOnly(tic);
    const QString url = mastInvokeUrl(QJsonObject{
        {QStringLiteral("service"), QStringLiteral("Mast.Catalogs.Filtered.Tic")},
        {QStringLiteral("format"), QStringLiteral("json")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("columns"), QStringLiteral("*")},
                     {QStringLiteral("filters"),
                      QJsonArray{QJsonObject{{QStringLiteral("paramName"), QStringLiteral("ID")},
                                             {QStringLiteral("values"), QJsonArray{id}}}}}}}});
    CdsTap::Request req(timeoutMs);
    req.maxAttempts = 2;
    const CdsTap::Response r = CdsTap::get(nam, url, req);
    Outcome o = fromResponse(r, QStringLiteral("MAST TIC"));
    if (!r.ok())
        return o;
    // A TIC looked up by number is wanted even if it was later flagged.
    const QJsonArray data =
        QJsonDocument::fromJson(r.body).object().value(QStringLiteral("data")).toArray();
    if (data.isEmpty()) {
        o.ok    = false;
        o.error = QString("TIC %1 not found").arg(id);
        return o;
    }
    const QJsonObject row = data.first().toObject();
    out.tic  = jsonId(row.value(QStringLiteral("ID")));
    out.ra   = jsonNumber(row.value(QStringLiteral("ra")));
    out.dec  = jsonNumber(row.value(QStringLiteral("dec")));
    out.tmag = jsonNumber(row.value(QStringLiteral("Tmag")));
    return o;
}

Outcome ticCone(QNetworkAccessManager *nam, double ra, double dec,
                double radiusArcsec, TicEntry &out, int timeoutMs) {
    const QString url = mastInvokeUrl(QJsonObject{
        {QStringLiteral("service"), QStringLiteral("Mast.Catalogs.Tic.Cone")},
        {QStringLiteral("format"), QStringLiteral("json")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("ra"), ra},
                     {QStringLiteral("dec"), dec},
                     {QStringLiteral("radius"), radiusArcsec / 3600.0}}}});
    CdsTap::Request req(timeoutMs);
    req.maxAttempts = 2;
    const CdsTap::Response r = CdsTap::get(nam, url, req);
    Outcome o = fromResponse(r, QStringLiteral("MAST TIC"));
    if (!r.ok())
        return o;
    std::vector<TicEntry> rows = parseTicRows(r.body, ra, dec);
    if (rows.empty()) {
        o.ok    = false;
        o.error = QString("no TIC entry within %1 arcsec").arg(radiusArcsec);
        return o;
    }
    out = *std::min_element(rows.begin(), rows.end(), [](const TicEntry &a, const TicEntry &b) {
        return a.sepArcsec < b.sepArcsec;
    });
    return o;
}

}   // namespace StarResolver
