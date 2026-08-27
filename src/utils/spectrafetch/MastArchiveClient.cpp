// src/utils/spectrafetch/MastArchiveClient.cpp

#include "MastArchiveClient.h"

#include "TapHelpers.h"
#include "VoTableReader.h"
#include "models/Spectrum.h"
#include "utils/Logger.h"
#include "utils/SpectrumReader.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr char kTapSyncUrl[] =
    "https://mast.stsci.edu/vo-tap/api/v0.1/caom/sync";

// A box query comes back in a second or two, so the timeout only has to cover
// a service under load - and the retry budget must stay small, because MAST
// answers a query it cannot plan with a 504 at the 65 s mark rather than
// failing fast, and the default rotation of 5 attempts would spend five
// minutes learning that.
constexpr int kTapTimeoutMs   = 60000;
constexpr int kTapMaxAttempts = 2;

// A chunk whose query fails is skipped, not fatal - the rest of the batch is
// still worth having. Only a service that fails this many times in a row is
// treated as down, which ends the archive early with what it found.
constexpr int kMaxConsecutiveFailures = 3;

// Stars per query. The cost of a chunk is dominated by the rows it brings
// back, not by the number of boxes in it: 1 star costs ~1.6 s and 50 stars
// ~9 s against the live service (Aug 2026). Kept moderate so a chunk landing
// in crowded fields stays well clear of the row limit below.
constexpr size_t kChunkSize = 20;

// The service's hard output limit (advertised in its capabilities). MAST
// tags a trailing QUERY_STATUS=OVERFLOW onto every response, including a
// COUNT(*) that returns one row, so that INFO cannot be used to detect
// truncation - only the row count can.
constexpr int kServiceRowLimit = 100000;

// The CAOM TAP does not use the spatial index for CONTAINS(POINT, CIRCLE):
// even "SELECT TOP 10 obs_id" with nothing but a 40" cone runs until the
// gateway gives up (504 after 65 s, measured Aug 2026). The identical cone
// written as an s_ra/s_dec range answers in about a second, and several
// OR-ed ranges in one query cost barely more than one.
//
// So discovery asks for bounding boxes and cuts the exact circles out of them
// client-side, which the per-mission tolerance check in discover() was doing
// to the cone's contents anyway. A box is a superset of its circle, so this
// changes only which rows the service has to ship, never which are kept.
QString boxPredicate(double ra, double dec, double radiusDeg) {
    const double decLo = dec - radiusDeg;
    const double decHi = dec + radiusDeg;
    const QString decTerm = QStringLiteral("s_dec BETWEEN %1 AND %2")
                                .arg(std::max(decLo, -90.0), 0, 'f', 8)
                                .arg(std::min(decHi, 90.0), 0, 'f', 8);

    // A box that reaches over a pole covers every right ascension, and the
    // cos(dec) widening below would blow up on the way there.
    if (decLo <= -90.0 || decHi >= 90.0)
        return QStringLiteral("(%1)").arg(decTerm);

    const double d2r = M_PI / 180.0;
    const double cosDec =
        std::cos(std::max(std::fabs(decLo), std::fabs(decHi)) * d2r);
    const double dra = cosDec > 1e-9 ? radiusDeg / cosDec : 360.0;
    if (dra >= 180.0)
        return QStringLiteral("(%1)").arg(decTerm);

    const double raLo = ra - dra;
    const double raHi = ra + dra;
    QString raTerm;
    if (raLo < 0.0)
        raTerm = QStringLiteral("(s_ra >= %1 OR s_ra <= %2)")
                     .arg(raLo + 360.0, 0, 'f', 8)
                     .arg(raHi, 0, 'f', 8);
    else if (raHi > 360.0)
        raTerm = QStringLiteral("(s_ra >= %1 OR s_ra <= %2)")
                     .arg(raLo, 0, 'f', 8)
                     .arg(raHi - 360.0, 0, 'f', 8);
    else
        raTerm = QStringLiteral("s_ra BETWEEN %1 AND %2")
                     .arg(raLo, 0, 'f', 8)
                     .arg(raHi, 0, 'f', 8);

    return QStringLiteral("(%1 AND %2)").arg(raTerm, decTerm);
}

// IUE/FUSE-era pointings are recorded as the aperture position, which drifts
// from the modern catalogue position of the same star: usually under 5" but
// with a long tail (alf Lac: 14.6" and 34.2" across four IUE spectra). So the
// legacy missions need a wider cone than a Gaia-era archive does.
//
// A wide cone is only safe because it is not applied blindly. Measured in the
// 47 Tuc core, widening 3" -> 60" takes HST from 189 to 772 spectra and finds
// 513 legacy-mission products. Everything past the caller's radius therefore
// has to clear the ambiguity gate in discover() below.
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

// One obs_id lists dozens of artifacts: the extracted spectrum, but also
// previews, trailers, raw and intermediate frames, and - for HST - the
// association table that merely names the exposures. Everything but a
// calibrated 1-D spectrum has to be turned away here, because whatever wins
// this ranking is what gets downloaded and handed to the FITS reader.
//
// Verified against the live obscore rows for HD 10250 (Aug 2026): HST offers
// hasp/*_cspec.fits and hsla/*_[ac]spec.fits alongside *_asn.fits and .txt
// trailers, IUE offers *mxlo_vo.fits next to .raw/.lilo/.melo intermediates,
// FUSE offers *nvo4histfcal_vo.fits among ~100 idf/raw/jitter files.
bool isRejectedProduct(const QString& lowerUrl) {
    static const QStringList kSuffixes = {
        QStringLiteral(".gif"),  QStringLiteral(".jpg"),
        QStringLiteral(".jpeg"), QStringLiteral(".png"),
        QStringLiteral(".pdf"),  QStringLiteral(".txt"),
        QStringLiteral(".html"),
    };
    for (const QString& x : kSuffixes)
        if (lowerUrl.endsWith(x)) return true;

    static const QStringList kMarkers = {
        // previews and logs
        QStringLiteral("preview"), QStringLiteral("thumb"),
        QStringLiteral("trailer"), QStringLiteral(".trl"),
        QStringLiteral("_trl"),
        // HST: association tables and everything that is not an extraction
        QStringLiteral("_asn."),   QStringLiteral("_flt."),
        QStringLiteral("_flc."),   QStringLiteral("_crj."),
        QStringLiteral("_ima."),   QStringLiteral("_raw."),
        QStringLiteral("_spt."),   QStringLiteral("_jit"),
        QStringLiteral("_jif."),   QStringLiteral("_epc."),
        QStringLiteral("_wav."),   QStringLiteral("_lrc."),
        QStringLiteral("_metadata"),
        // IUE intermediates (line-by-line, extracted image, raw image)
        QStringLiteral(".raw."),   QStringLiteral(".rilo"),
        QStringLiteral(".silo"),   QStringLiteral(".lilo"),
        QStringLiteral(".melo"),   QStringLiteral(".elbl"),
        // FUSE housekeeping, jitter, raw and intermediate data files
        QStringLiteral("histfraw"), QStringLiteral("histfidf"),
        QStringLiteral("hskpf"),    QStringLiteral("jitrf"),
        QStringLiteral("snapf"),    QStringLiteral("snpaf"),
        QStringLiteral("fesaf"),
    };
    for (const QString& m : kMarkers)
        if (lowerUrl.contains(m)) return true;

    // GHRS and FOS file one observation as a family of single-array products:
    // _c0f holds the wavelengths, _c1f the fluxes, _c2f the errors, _cqf the
    // quality flags, _d0f the raw counts. None of them is a spectrum on its
    // own, and the download queue handles one file per product, so an
    // observation that offers nothing else is skipped. Those targets are
    // still reachable through the HASP/HSLA coadds.
    // Matched against the whole URL, whose file name may sit in a query
    // parameter (the MAST download endpoint) rather than in the path.
    static const QRegularExpression singleArrayProduct(
        QStringLiteral("_[cd][0-9a-z]f\\.(?:fits|fit)(?:\\.gz)?(?:$|[?&#])"));
    return singleArrayProduct.match(lowerUrl).hasMatch();
}

// Rank the survivors; the best-scoring artifact of an obs_id is the one that
// gets queued. Negative means "not a spectrum, skip this observation".
int urlScore(const QString& url) {
    const QString u = url.toLower();
    if (isRejectedProduct(u)) return -1;

    if (u.contains(QStringLiteral("_vo.fits"))) return 100;   // VO-normalized
    if (u.contains(QStringLiteral("cspec")))    return 95;    // HASP/HSLA coadd
    if (u.contains(QStringLiteral("x1d")) ||
        u.contains(QStringLiteral("_sx1")))     return 90;    // HST extraction
    if (u.contains(QStringLiteral("aspec")))    return 85;    // all-grating coadd
    if (u.contains(QStringLiteral("mxlo")) ||
        u.contains(QStringLiteral("mxhi")))     return 80;    // IUE merged
    if (u.contains(QStringLiteral("histfcal"))) return 70;    // FUSE calibrated
    if (u.endsWith(QStringLiteral(".fits")) ||
        u.endsWith(QStringLiteral(".fits.gz")) ||
        u.endsWith(QStringLiteral(".fit")))     return 50;
    if (u.endsWith(QStringLiteral(".fit.gz")))  return 40;
    if (u.endsWith(QStringLiteral(".gz")))      return 10;
    return 0;
}

// MAST serves HST products through a download endpoint whose path is a bare
// "file", with the real name in the uri parameter:
//
//   .../Download/file?uri=mast:HST/product/hasp/hst_..._cspec.fits
//
// Taking QUrl::fileName() there names every HST product of a star "file", so
// they overwrite each other on disk and the "already downloaded" fast path
// then parses whichever one landed first.
QString productFileName(const QUrl& url, const QString& obsId) {
    QString name = QUrlQuery(url).queryItemValue(QStringLiteral("uri"),
                                                 QUrl::FullyDecoded);
    if (!name.isEmpty())
        name = name.section(QLatin1Char('/'), -1);
    if (name.isEmpty() || name.compare(QStringLiteral("file"),
                                       Qt::CaseInsensitive) == 0)
        name = url.fileName();
    if (name.isEmpty() || name.compare(QStringLiteral("file"),
                                       Qt::CaseInsensitive) == 0)
        name = obsId + QStringLiteral(".fits");
    return name;
}

// CAOM names the instrument without its mission ("FUV" for FUSE, "LWR" for
// IUE), which reads as an unidentifiable spectrum once imported. Qualify it,
// the way the other clients report "SDSS/BOSS" or "LAMOST/LRS".
QString qualifiedInstrument(const QString& collection,
                            const QString& instrument) {
    const QString inst = instrument.trimmed();
    const QString coll = collection.trimmed();
    if (inst.isEmpty()) return coll;
    if (coll.isEmpty()) return inst;
    if (inst.startsWith(coll, Qt::CaseInsensitive)) return inst;
    return coll + QLatin1Char('/') + inst;
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
    // Two observations can advertise the same product file (a HASP coadd is
    // listed under each visit it covers). Without this the same file would be
    // downloaded twice and imported as two spectra.
    QHash<QString, QString> urlOwner;   // access_url -> obs_id holding it

    int wideAccepted     = 0;   // taken past the caller's radius
    int ambiguousDropped = 0;   // rejected there because the field is crowded
    int overflowChunks   = 0;   // queries the service truncated
    int failedStars      = 0;   // stars in a chunk that never came back
    QSet<QString> obsWithoutProduct;   // only non-spectrum artifacts offered
    int starsDone        = 0;
    int consecutiveFails = 0;
    bool serviceDown     = false;
    QString lastFailure;

    // Star ranges still to query, as a stack so the chunks are walked in
    // order. A chunk the service truncated is split in half and pushed back
    // rather than half-read, which is the only way a wide cone landing in a
    // crowded field can lose rows silently.
    std::vector<std::pair<size_t, size_t>> work;
    for (size_t end = stars.size(); end > 0;) {
        const size_t begin = end > kChunkSize ? end - kChunkSize : 0;
        work.emplace_back(begin, end);
        end = begin;
    }

    while (!work.empty()) {
        if (cancel.load()) break;
        const auto range = work.back();
        work.pop_back();
        const size_t begin = range.first, end = range.second;
        const int    chunkStars = int(end - begin);

        QStringList boxes;
        for (size_t si = begin; si < end; ++si)
            boxes << boxPredicate(stars[si].ra, stars[si].dec, radiusDeg);

        const QString adql =
            QStringLiteral(
                "SELECT obs_id, obs_collection, instrument_name, t_min, "
                "em_res_power, access_url, access_estsize, "
                "s_ra, s_dec, target_name "
                "FROM ivoa.obscore "
                "WHERE (%1) "
                "AND dataproduct_type = 'spectrum' AND calib_level >= 2 "
                "AND obs_collection IN (%2)")
                .arg(boxes.join(QStringLiteral(" OR ")), quoted.join(','));

        // The MAST TAP always answers VOTable regardless of FORMAT.
        QString chunkError;
        CdsTap::Request request(kTapTimeoutMs);
        request.maxAttempts = kTapMaxAttempts;
        request.cancel      = &cancel;
        const QByteArray body =
            SpecFetch::tapQuery(nam, QString::fromLatin1(kTapSyncUrl), adql,
                                QStringLiteral("votable"), request,
                                &chunkError);

        // One chunk's query failing says nothing about the next one's, so a
        // failure costs those stars and no more. Giving up on the whole
        // archive here used to throw away every star still in the list.
        //
        // A multi-star chunk is halved and retried first, so a single
        // pathological field costs one star rather than twenty. The
        // consecutive-failure guard is what bounds the retrying: a service
        // that is simply down fails the halves too and ends the archive
        // after three tries, not after bisecting the whole list.
        auto failChunk = [&](const QString& why) {
            ++consecutiveFails;
            lastFailure = why;
            if (consecutiveFails >= kMaxConsecutiveFailures) serviceDown = true;

            if (chunkStars > 1 && !serviceDown) {
                const size_t mid = begin + size_t(chunkStars) / 2;
                work.emplace_back(mid, end);
                work.emplace_back(begin, mid);
                LOG_INFO("SpecFetch",
                         QStringLiteral("MAST: %1-star query failed (%2), "
                                        "retrying in halves")
                             .arg(chunkStars)
                             .arg(why));
                return;
            }

            failedStars += chunkStars;
            LOG_WARNING("SpecFetch",
                        QStringLiteral("MAST: query for %1 star(s) failed: %2")
                            .arg(chunkStars)
                            .arg(why));
            starsDone += chunkStars;
            if (progress) progress(starsDone, int(stars.size()));
        };

        if (!chunkError.isEmpty()) {
            if (cancel.load()) break;
            failChunk(chunkError);
            if (serviceDown) break;
            continue;
        }

        const VoTable::Document doc = VoTable::parse(body);
        if (!doc.ok()) {
            failChunk(doc.error);
            if (serviceDown) break;
            continue;
        }
        consecutiveFails = 0;

        const VoTable::Table* t = doc.firstTable();
        if (!t) {
            starsDone += chunkStars;
            if (progress) progress(starsDone, int(stars.size()));
            continue;
        }

        // A truncated answer would look like a clean "nothing more found".
        // Splitting the range halves the rows each half asks for; a single
        // star that still overflows is genuinely beyond the service.
        if (t->rows.size() >= kServiceRowLimit) {
            if (chunkStars > 1) {
                const size_t mid = begin + size_t(chunkStars) / 2;
                work.emplace_back(mid, end);
                work.emplace_back(begin, mid);
                LOG_INFO("SpecFetch",
                         QStringLiteral("MAST: %1-star chunk hit the row "
                                        "limit, splitting")
                             .arg(chunkStars));
                continue;
            }
            ++overflowChunks;
        }

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
            starsDone += chunkStars;
            if (progress) progress(starsDone, int(stars.size()));
            continue;
        }

        // The chunk's rows are the union of its stars' boxes, so each star is
        // matched against all of them and keeps what falls inside its own
        // circle. Rows are pre-scored once here rather than once per star.
        struct Row {
            QString obsId, collection, instrument, url, target;
            double  mjd = NAN, res = NAN, ra = NAN, dec = NAN;
            qint64  size  = -1;
            int     score = -1;
        };
        std::vector<Row> rows;
        rows.reserve(size_t(t->rows.size()));

        for (int i = 0; i < t->rows.size(); ++i) {
            const QString obsId = t->value(i, idCol);
            const QString url   = t->value(i, urlCol);
            if (obsId.isEmpty() || url.isEmpty()) continue;
            const int score = urlScore(url);
            if (score < 0) {
                obsWithoutProduct.insert(obsId);
                continue;
            }

            Row r;
            r.obsId      = obsId;
            r.url        = url;
            r.score      = score;
            r.collection = collCol >= 0 ? t->value(i, collCol) : QString();
            r.instrument = instCol >= 0 ? t->value(i, instCol) : QString();
            r.target     = tgtCol  >= 0 ? t->value(i, tgtCol)  : QString();

            // A calibration frame is never a spectrum of the star, however
            // close its pointing happens to land.
            if (isCalibrationTarget(r.target)) continue;

            r.ra  = raCol  >= 0 ? toDoubleOr(t->value(i, raCol),  NAN) : NAN;
            r.dec = decCol >= 0 ? toDoubleOr(t->value(i, decCol), NAN) : NAN;
            r.mjd = tminCol >= 0 ? toDoubleOr(t->value(i, tminCol), NAN) : NAN;
            r.res = resCol  >= 0 ? toDoubleOr(t->value(i, resCol),  NAN) : NAN;
            if (sizeCol >= 0) {
                const double sz = toDoubleOr(t->value(i, sizeCol), -1);
                r.size = sz > 0 ? qint64(sz) : -1;
            }
            rows.push_back(std::move(r));
        }

        for (size_t si = begin; si < end; ++si) {
            if (cancel.load()) break;
            const SpecFetch::StarQuery& star = stars[si];

            // Collect candidates first: whether a wide match is trustworthy
            // depends on what else this star matched.
            struct Cand {
                const Row* row = nullptr;
                double     sep = 0;
            };
            std::vector<Cand> cands;

            for (const Row& r : rows) {
                const double sep =
                    (std::isnan(r.ra) || std::isnan(r.dec))
                        ? 0.0
                        : angularSepDeg(r.ra, r.dec, star.ra, star.dec) * 3600.0;
                if (sep > toleranceArcsec(r.collection, opt.radiusArcsec))
                    continue;
                cands.push_back({&r, sep});
            }

            // Per mission: anything inside the caller's radius is taken
            // as-is. Beyond it, accept only when the mission found nothing
            // close AND every distant candidate names the same target, which
            // is what separates a badly recorded pointing of this star from a
            // crowded field full of other people's targets.
            QSet<QString> missionsWithTight;
            for (const Cand& c : cands)
                if (c.sep <= opt.radiusArcsec)
                    missionsWithTight.insert(c.row->collection.toUpper());

            QHash<QString, QSet<QString>> wideTargets;
            for (const Cand& c : cands) {
                const QString m = c.row->collection.toUpper();
                if (c.sep > opt.radiusArcsec && !missionsWithTight.contains(m))
                    wideTargets[m].insert(normalizedTarget(c.row->target));
            }

            for (const Cand& c : cands) {
                const Row&    row = *c.row;
                const QString m   = row.collection.toUpper();
                if (c.sep > opt.radiusArcsec) {
                    if (missionsWithTight.contains(m)) continue;
                    const QSet<QString>& names = wideTargets[m];
                    if (names.size() != 1) {
                        ++ambiguousDropped;
                        continue;
                    }
                    ++wideAccepted;
                }

                const QString owner = urlOwner.value(row.url);
                if (!owner.isEmpty() && owner != row.obsId) continue;

                const Pick held       = picks.value(row.obsId);
                const bool closerStar = c.sep < held.sep - 1e-9;
                const bool betterUrl  =
                    c.sep <= held.sep + 1e-9 && row.score > held.score;
                if (!closerStar && !betterUrl) continue;

                SpecFetch::RemoteSpectrum s;
                s.archive        = SpecFetch::Archive::MastSSAP;
                s.collection     = row.collection;
                s.archiveLabel   = QStringLiteral("MAST %1").arg(s.collection);
                s.originId       = QStringLiteral("mast-%1:%2")
                                       .arg(s.collection.toLower(), row.obsId);
                s.starId         = star.starId;
                s.instrumentHint =
                    qualifiedInstrument(row.collection, row.instrument);
                s.mjd            = row.mjd;
                s.resolution     = row.res;
                s.ra             = row.ra;
                s.dec            = row.dec;
                s.sepArcsec      = c.sep;
                s.sizeBytes      = row.size;
                s.isCoadd        = true;
                s.downloadUrl    = QUrl(row.url);
                s.fileName       = productFileName(s.downloadUrl, row.obsId);

                Pick& slot = picks[row.obsId];
                if (slot.index < 0) {
                    slot.index = int(out.size());
                    out.append(s);
                } else {
                    out[slot.index] = s;
                }
                slot.score = row.score;
                slot.sep   = c.sep;
                urlOwner.insert(row.url, row.obsId);
            }
        }

        starsDone += chunkStars;
        if (progress) progress(starsDone, int(stars.size()));
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
    for (auto it = picks.constBegin(); it != picks.constEnd(); ++it)
        obsWithoutProduct.remove(it.key());
    if (!obsWithoutProduct.isEmpty())
        LOG_INFO("SpecFetch",
                 QStringLiteral("MAST: %1 observation(s) skipped - no "
                                "calibrated 1-D spectrum among their products")
                     .arg(obsWithoutProduct.size()));
    if (overflowChunks > 0)
        LOG_WARNING("SpecFetch",
                    QStringLiteral("MAST: %1 star(s) hit the service row limit; "
                                   "results there are incomplete")
                        .arg(overflowChunks));

    // Reported as a session log line, not as a hard failure: `out` still holds
    // everything the stars that did answer offered.
    if (failedStars > 0 && error) {
        int notSearched = 0;
        for (const auto& range : work) notSearched += int(range.second - range.first);
        *error = serviceDown
                     ? QStringLiteral("service unavailable after %1 quer%2 in "
                                      "a row failed, %3 star(s) not searched "
                                      "(%4)")
                           .arg(consecutiveFails)
                           .arg(consecutiveFails == 1 ? "y" : "ies")
                           .arg(notSearched)
                           .arg(lastFailure)
                     : QStringLiteral("%1 of %2 star queries failed (%3)")
                           .arg(failedStars)
                           .arg(stars.size())
                           .arg(lastFailure);
    }

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
    // FUSE files carry INSTRUME='FUV' and IUE ones 'LWR'/'SWP', which say
    // nothing about the mission once the spectrum sits in the star's list.
    // The CAOM name behind the hint is mission-qualified.
    if (!r.instrumentHint.isEmpty())
        spec->setInstrument(r.instrumentHint);

    SpecFetch::ParsedSpectrum ps;
    ps.spectrum       = spec;
    ps.originId       = r.originId;
    ps.isCoadd        = r.isCoadd;
    ps.instrumentHint = r.instrumentHint;
    out.push_back(std::move(ps));
    return out;
}
