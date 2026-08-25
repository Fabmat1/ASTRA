// src/utils/spectrafetch/MastArchiveClient.cpp

#include "MastArchiveClient.h"

#include "TapHelpers.h"
#include "VoTableReader.h"
#include "models/Spectrum.h"
#include "utils/Logger.h"
#include "utils/SpectrumReader.h"

#include <QHash>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr char kTapSyncUrl[] =
    "https://mast.stsci.edu/vo-tap/api/v0.1/caom/sync";
constexpr int kTapTimeoutMs = 180000;

// The MAST CAOM TAP accepts exactly one CONTAINS predicate per query, and
// rejects an "obs_collection IN (...)" term placed ahead of it (verified
// against the live service Aug 2026). So there is no OR-chained batching to
// be had here: discovery walks the stars one query at a time, spatial term
// first. Each query costs roughly 40 s server-side regardless of filters.

// IUE/FUSE-era pointings are recorded as the aperture position, which drifts
// from the modern catalogue position of the same star: usually under 5" but
// with a long tail (alf Lac: 14.6" and 34.2" across four IUE spectra). So the
// legacy missions need a wider cone than a Gaia-era archive does.
//
// A wide cone is only safe because it is not applied blindly. Measured in the
// 47 Tuc core, widening 3" -> 60" takes HST from 189 to 772 spectra (and the
// service answers QUERY_STATUS=OVERFLOW, silently truncating), and finds 513
// legacy-mission products. Everything past the caller's radius therefore has
// to clear the ambiguity gate in acceptWideMatches() below.
constexpr double kLegacyRadiusArcsec = 40.0;

bool isLegacyMission(const QString& collection) {
    static const QStringList kLegacy = {
        QStringLiteral("IUE"), QStringLiteral("FUSE"),
        QStringLiteral("HUT"), QStringLiteral("EUVE"),
    };
    for (const QString& m : kLegacy)
        if (collection.compare(m, Qt::CaseInsensitive) == 0) return true;
    return false;
}

// How far a product of this mission may sit from the star. Modern pointings
// are astrometric, so they get no slack beyond what the caller asked for.
double toleranceArcsec(const QString& collection, double userRadius) {
    return isLegacyMission(collection)
               ? std::max(userRadius, kLegacyRadiusArcsec)
               : userRadius;
}

// Calibration exposures carry a pointing but are not spectra of anything.
// IUE in particular files WAVECAL/TFLOOD frames at calib_level 2, and they
// turned up as matches for real project stars.
bool isCalibrationTarget(const QString& targetName) {
    const QString t = targetName.toUpper().remove(' ').remove('-');
    if (t.isEmpty() || t == QLatin1String("NULL") || t == QLatin1String("NONE"))
        return true;
    static const QStringList kBad = {
        QStringLiteral("WAVECAL"),  QStringLiteral("WAVCAL"),
        QStringLiteral("TFLOOD"),   QStringLiteral("FLATFIELD"),
        QStringLiteral("INTFLAT"),  QStringLiteral("DARK"),
        QStringLiteral("BIAS"),     QStringLiteral("TUNGSTEN"),
        QStringLiteral("PLATINUM"), QStringLiteral("LAMP"),
    };
    for (const QString& b : kBad)
        if (t.contains(b)) return true;
    return false;
}

QString normalizedTarget(const QString& targetName) {
    return targetName.toUpper().remove(' ').remove('-').remove('_');
}

double toDoubleOr(const QString& s, double fallback) {
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : fallback;
}

double angularSepDeg(double ra1, double dec1, double ra2, double dec2) {
    const double d2r  = M_PI / 180.0;
    const double dra  = (ra1 - ra2) * std::cos(0.5 * (dec1 + dec2) * d2r);
    const double ddec = dec1 - dec2;
    return std::sqrt(dra * dra + ddec * ddec);
}

// Every obs_id lists several artifacts (science file, previews, raw). Rank
// candidate science URLs; previews and raw data never win.
int urlScore(const QString& url) {
    const QString u = url.toLower();
    if (u.contains(QStringLiteral("preview")) ||
        u.endsWith(QStringLiteral(".gif")) ||
        u.endsWith(QStringLiteral(".jpg")) ||
        u.endsWith(QStringLiteral(".png")) ||
        u.contains(QStringLiteral(".raw.")))
        return -1;
    if (u.contains(QStringLiteral("_vo.fits"))) return 100;
    if (u.contains(QStringLiteral("x1d")))      return 90;
    if (u.contains(QStringLiteral("mxlo")) ||
        u.contains(QStringLiteral("mxhi")))     return 80;
    if (u.endsWith(QStringLiteral(".fits")) ||
        u.endsWith(QStringLiteral(".fits.gz")) ||
        u.endsWith(QStringLiteral(".fit")))     return 50;
    if (u.endsWith(QStringLiteral(".gz")))      return 10;
    return 0;
}

}   // namespace

QStringList MastArchiveClient::knownMissions() {
    return {QStringLiteral("HST"), QStringLiteral("IUE"),
            QStringLiteral("FUSE"), QStringLiteral("HUT"),
            QStringLiteral("EUVE")};
}

double MastArchiveClient::searchRadiusArcsec(
    const SpecFetch::ArchiveOptions& opt) const {
    // The cone has to cover the widest mission enabled; per-mission
    // tolerances and the ambiguity gate then trim what comes back.
    const QStringList missions =
        opt.collections.isEmpty() ? knownMissions() : opt.collections;
    double r = opt.radiusArcsec;
    for (const QString& m : missions)
        r = std::max(r, toleranceArcsec(m, opt.radiusArcsec));
    return r;
}

QList<SpecFetch::RemoteSpectrum> MastArchiveClient::discover(
    const std::vector<SpecFetch::StarQuery>& stars,
    const SpecFetch::ArchiveOptions& opt, QNetworkAccessManager* nam,
    const std::function<void(int, int)>& progress,
    const std::atomic<bool>& cancel, QString* error) {
    QList<SpecFetch::RemoteSpectrum> out;
    if (error) error->clear();

    const double radiusDeg = searchRadiusArcsec(opt) / 3600.0;

    QStringList missions = opt.collections;
    if (missions.isEmpty()) missions = knownMissions();
    QStringList quoted;
    for (const QString& m : missions)
        quoted << QStringLiteral("'%1'").arg(QString(m).replace('\'', ""));

    // One observation can sit inside two stars' cones at this radius. Keep
    // each obs_id once, attributed to the nearest star, and within that star
    // to the best-ranked science artifact.
    struct Pick {
        int    index = -1;   // position in `out`
        int    score = -1;
        double sep   = std::numeric_limits<double>::max();
    };
    QHash<QString, Pick> picks;

    int wideAccepted     = 0;   // taken past the caller's radius
    int ambiguousDropped = 0;   // rejected there because the field is crowded
    int overflowStars    = 0;   // queries the service truncated

    for (size_t si = 0; si < stars.size(); ++si) {
        if (cancel.load()) break;
        const SpecFetch::StarQuery& star = stars[si];

        // Spatial term first: the service rejects the query otherwise.
        const QString adql =
            QStringLiteral(
                "SELECT obs_id, obs_collection, instrument_name, t_min, "
                "t_exptime, em_res_power, access_url, access_estsize, "
                "s_ra, s_dec, target_name "
                "FROM ivoa.obscore "
                "WHERE CONTAINS(POINT('ICRS', s_ra, s_dec), "
                "CIRCLE('ICRS', %1, %2, %3)) = 1 "
                "AND dataproduct_type = 'spectrum' AND calib_level >= 2 "
                "AND obs_collection IN (%4)")
                .arg(star.ra, 0, 'f', 8)
                .arg(star.dec, 0, 'f', 8)
                .arg(radiusDeg, 0, 'f', 8)
                .arg(quoted.join(','));

        // The MAST TAP always answers VOTable regardless of FORMAT.
        QString chunkError;
        const QByteArray body =
            SpecFetch::tapQuery(nam, QString::fromLatin1(kTapSyncUrl), adql,
                                QStringLiteral("votable"), kTapTimeoutMs,
                                &chunkError);
        if (!chunkError.isEmpty()) {
            if (error) *error = chunkError;
            break;
        }

        const VoTable::Document doc = VoTable::parse(body);
        if (!doc.ok()) {
            if (error) *error = doc.error;
            break;
        }
        // A truncated answer would look like a clean "nothing more found".
        if (body.contains("QUERY_STATUS") && body.contains("OVERFLOW"))
            ++overflowStars;

        const VoTable::Table* t = doc.firstTable();
        if (!t) { if (progress) progress(int(si) + 1, int(stars.size())); continue; }

        const int idCol   = t->columnByName(QStringLiteral("obs_id"));
        const int collCol = t->columnByName(QStringLiteral("obs_collection"));
        const int instCol = t->columnByName(QStringLiteral("instrument_name"));
        const int tminCol = t->columnByName(QStringLiteral("t_min"));
        const int resCol  = t->columnByName(QStringLiteral("em_res_power"));
        const int urlCol  = t->columnByName(QStringLiteral("access_url"));
        const int sizeCol = t->columnByName(QStringLiteral("access_estsize"));
        const int raCol   = t->columnByName(QStringLiteral("s_ra"));
        const int decCol  = t->columnByName(QStringLiteral("s_dec"));
        const int tgtCol  = t->columnByName(QStringLiteral("target_name"));
        if (idCol < 0 || urlCol < 0) {
            if (progress) progress(int(si) + 1, int(stars.size()));
            continue;
        }

        // Collect candidates first: whether a wide match is trustworthy
        // depends on what else this star matched.
        struct Cand {
            QString obsId, collection, instrument, url, target;
            double  sep = 0, mjd = NAN, res = NAN, ra = NAN, dec = NAN;
            qint64  size = -1;
            int     score = -1;
        };
        std::vector<Cand> cands;

        for (int i = 0; i < t->rows.size(); ++i) {
            const QString obsId = t->value(i, idCol);
            const QString url   = t->value(i, urlCol);
            if (obsId.isEmpty() || url.isEmpty()) continue;
            const int score = urlScore(url);
            if (score < 0) continue;

            Cand c;
            c.obsId      = obsId;
            c.url        = url;
            c.score      = score;
            c.collection = collCol >= 0 ? t->value(i, collCol) : QString();
            c.instrument = instCol >= 0 ? t->value(i, instCol) : QString();
            c.target     = tgtCol  >= 0 ? t->value(i, tgtCol)  : QString();

            // A calibration frame is never a spectrum of the star, however
            // close its pointing happens to land.
            if (isCalibrationTarget(c.target)) continue;

            c.ra  = raCol  >= 0 ? toDoubleOr(t->value(i, raCol),  NAN) : NAN;
            c.dec = decCol >= 0 ? toDoubleOr(t->value(i, decCol), NAN) : NAN;
            c.sep = (std::isnan(c.ra) || std::isnan(c.dec))
                        ? 0.0
                        : angularSepDeg(c.ra, c.dec, star.ra, star.dec) * 3600.0;
            if (c.sep > toleranceArcsec(c.collection, opt.radiusArcsec)) continue;

            c.mjd  = tminCol >= 0 ? toDoubleOr(t->value(i, tminCol), NAN) : NAN;
            c.res  = resCol  >= 0 ? toDoubleOr(t->value(i, resCol),  NAN) : NAN;
            if (sizeCol >= 0) {
                const double sz = toDoubleOr(t->value(i, sizeCol), -1);
                c.size = sz > 0 ? qint64(sz) : -1;
            }
            cands.push_back(std::move(c));
        }

        // Per mission: anything inside the caller's radius is taken as-is.
        // Beyond it, accept only when the mission found nothing close AND
        // every distant candidate names the same target, which is what
        // separates a badly recorded pointing of this star from a crowded
        // field full of other people's targets.
        QSet<QString> missionsWithTight;
        for (const Cand& c : cands)
            if (c.sep <= opt.radiusArcsec)
                missionsWithTight.insert(c.collection.toUpper());

        QHash<QString, QSet<QString>> wideTargets;
        for (const Cand& c : cands) {
            const QString m = c.collection.toUpper();
            if (c.sep > opt.radiusArcsec && !missionsWithTight.contains(m))
                wideTargets[m].insert(normalizedTarget(c.target));
        }

        for (const Cand& c : cands) {
            const QString m = c.collection.toUpper();
            if (c.sep > opt.radiusArcsec) {
                if (missionsWithTight.contains(m)) continue;
                const QSet<QString>& names = wideTargets[m];
                if (names.size() != 1) {
                    ++ambiguousDropped;
                    continue;
                }
                ++wideAccepted;
            }

            const Pick held       = picks.value(c.obsId);
            const bool closerStar = c.sep < held.sep - 1e-9;
            const bool betterUrl  =
                c.sep <= held.sep + 1e-9 && c.score > held.score;
            if (!closerStar && !betterUrl) continue;

            SpecFetch::RemoteSpectrum r;
            r.archive        = SpecFetch::Archive::MastSSAP;
            r.collection     = c.collection;
            r.archiveLabel   = QStringLiteral("MAST %1").arg(r.collection);
            r.originId       = QStringLiteral("mast-%1:%2")
                                   .arg(r.collection.toLower(), c.obsId);
            r.starId         = star.starId;
            r.instrumentHint = c.instrument.isEmpty() ? c.collection
                                                      : c.instrument;
            r.mjd            = c.mjd;
            r.resolution     = c.res;
            r.ra             = c.ra;
            r.dec            = c.dec;
            r.sepArcsec      = c.sep;
            r.sizeBytes      = c.size;
            r.isCoadd        = true;
            r.downloadUrl    = QUrl(c.url);
            QString name     = QUrl(c.url).fileName();
            if (name.isEmpty()) name = c.obsId + QStringLiteral(".fits");
            r.fileName = name;

            Pick& slot = picks[c.obsId];
            if (slot.index < 0) {
                slot.index = int(out.size());
                out.append(r);
            } else {
                out[slot.index] = r;
            }
            slot.score = c.score;
            slot.sep   = c.sep;
        }

        if (progress) progress(int(si) + 1, int(stars.size()));
    }

    if (wideAccepted > 0)
        LOG_INFO("SpecFetch",
                 QStringLiteral("MAST: %1 match(es) accepted beyond %2\" on a "
                                "single consistent target name")
                     .arg(wideAccepted)
                     .arg(opt.radiusArcsec, 0, 'g', 3));
    if (ambiguousDropped > 0)
        LOG_WARNING("SpecFetch",
                    QStringLiteral("MAST: %1 match(es) past %2\" dropped as "
                                   "ambiguous (several targets in the cone)")
                        .arg(ambiguousDropped)
                        .arg(opt.radiusArcsec, 0, 'g', 3));
    if (overflowStars > 0)
        LOG_WARNING("SpecFetch",
                    QStringLiteral("MAST: %1 star(s) hit the service row limit; "
                                   "results there are incomplete")
                        .arg(overflowStars));

    return out;
}

std::vector<SpecFetch::ParsedSpectrum> MastArchiveClient::parse(
    const QString& localPath, const SpecFetch::RemoteSpectrum& r,
    const SpecFetch::ArchiveOptions& opt, QString* error) {
    Q_UNUSED(opt);
    std::vector<SpecFetch::ParsedSpectrum> out;
    if (error) error->clear();

    // The calibrated products (x1d, *_vo.fits, mxlo) are ordinary FITS
    // bintables with WAVELENGTH/FLUX/ERROR-style columns that the generic
    // reader understands. UV wavelengths sit below 2000 A, so the vacuum-air
    // conversion is a no-op there by construction.
    auto reader = SpectrumReaderRegistry::instance().getReaderForFile(localPath);
    if (!reader) {
        if (error) *error = QStringLiteral("no FITS reader available");
        return out;
    }
    SpectrumReadResult res = reader->readSpectrum(localPath);
    if (!res.success || !res.spectrum || !res.spectrum->hasData()) {
        if (error)
            *error = res.errorMessage.isEmpty()
                         ? QStringLiteral("could not read spectrum")
                         : res.errorMessage;
        return out;
    }

    auto spec = res.spectrum;
    spec->setFile(localPath);
    if (res.metadata.mjd)
        spec->setMJD(*res.metadata.mjd);
    else if (!std::isnan(r.mjd))
        spec->setMJD(r.mjd);
    if (res.metadata.exposureTime && *res.metadata.exposureTime > 0)
        spec->setExposureTime(*res.metadata.exposureTime);
    if (spec->getInstrument().isEmpty())
        spec->setInstrument(r.instrumentHint);

    SpecFetch::ParsedSpectrum ps;
    ps.spectrum       = spec;
    ps.originId       = r.originId;
    ps.isCoadd        = r.isCoadd;
    ps.instrumentHint = r.instrumentHint;
    out.push_back(std::move(ps));
    return out;
}
