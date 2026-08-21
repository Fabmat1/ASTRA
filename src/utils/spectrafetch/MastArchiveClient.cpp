// src/utils/spectrafetch/MastArchiveClient.cpp

#include "MastArchiveClient.h"

#include "TapHelpers.h"
#include "VoTableReader.h"
#include "models/Spectrum.h"
#include "utils/SpectrumReader.h"

#include <QHash>
#include <QUrl>

#include <cmath>

namespace {

constexpr char kTapSyncUrl[] =
    "https://mast.stsci.edu/vo-tap/api/v0.1/caom/sync";
constexpr int kChunkSize    = 25;
constexpr int kTapTimeoutMs = 180000;

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

QList<SpecFetch::RemoteSpectrum> MastArchiveClient::discover(
    const std::vector<SpecFetch::StarQuery>& stars,
    const SpecFetch::ArchiveOptions& opt, QNetworkAccessManager* nam,
    const std::function<void(int, int)>& progress,
    const std::atomic<bool>& cancel, QString* error) {
    QList<SpecFetch::RemoteSpectrum> out;
    if (error) error->clear();

    const double radiusDeg = opt.radiusArcsec / 3600.0;

    QStringList missions = opt.collections;
    if (missions.isEmpty()) missions = knownMissions();
    QStringList quoted;
    for (const QString& m : missions)
        quoted << QStringLiteral("'%1'").arg(QString(m).replace('\'', ""));

    const auto chunks = SpecFetch::chunked(stars, size_t(kChunkSize));
    int starsDone = 0;

    for (const auto& chunk : chunks) {
        if (cancel.load()) break;

        QStringList circles;
        for (const auto& q : chunk)
            circles << QStringLiteral(
                           "CONTAINS(POINT('ICRS', s_ra, s_dec), "
                           "CIRCLE('ICRS', %1, %2, %3)) = 1")
                           .arg(q.ra, 0, 'f', 8)
                           .arg(q.dec, 0, 'f', 8)
                           .arg(radiusDeg, 0, 'f', 8);

        const QString adql =
            QStringLiteral(
                "SELECT obs_id, obs_collection, instrument_name, t_min, "
                "t_exptime, em_res_power, access_url, access_estsize, "
                "s_ra, s_dec "
                "FROM ivoa.obscore "
                "WHERE dataproduct_type = 'spectrum' AND calib_level >= 2 "
                "AND obs_collection IN (%1) AND (%2)")
                .arg(quoted.join(','))
                .arg(circles.join(QStringLiteral(" OR ")));

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
        const VoTable::Table* t = doc.firstTable();
        if (!t) { starsDone += int(chunk.size()); continue; }

        const int idCol   = t->columnByName(QStringLiteral("obs_id"));
        const int collCol = t->columnByName(QStringLiteral("obs_collection"));
        const int instCol = t->columnByName(QStringLiteral("instrument_name"));
        const int tminCol = t->columnByName(QStringLiteral("t_min"));
        const int resCol  = t->columnByName(QStringLiteral("em_res_power"));
        const int urlCol  = t->columnByName(QStringLiteral("access_url"));
        const int sizeCol = t->columnByName(QStringLiteral("access_estsize"));
        const int raCol   = t->columnByName(QStringLiteral("s_ra"));
        const int decCol  = t->columnByName(QStringLiteral("s_dec"));
        if (idCol < 0 || urlCol < 0) {
            starsDone += int(chunk.size());
            continue;
        }

        // One product per obs_id, keeping the best-ranked science URL.
        QHash<QString, int> bestScore;
        QHash<QString, SpecFetch::RemoteSpectrum> byObsId;

        for (int i = 0; i < t->rows.size(); ++i) {
            const QString obsId = t->value(i, idCol);
            const QString url   = t->value(i, urlCol);
            if (obsId.isEmpty() || url.isEmpty()) continue;
            const int score = urlScore(url);
            if (score < 0) continue;
            if (byObsId.contains(obsId) && score <= bestScore.value(obsId))
                continue;

            const double rowRa  = raCol >= 0
                ? toDoubleOr(t->value(i, raCol), NAN) : NAN;
            const double rowDec = decCol >= 0
                ? toDoubleOr(t->value(i, decCol), NAN) : NAN;
            int    idx     = -1;
            double bestSep = 2.0 * radiusDeg;
            if (!std::isnan(rowRa) && !std::isnan(rowDec)) {
                for (int k = 0; k < int(chunk.size()); ++k) {
                    const double sep = angularSepDeg(rowRa, rowDec,
                                                     chunk[k].ra, chunk[k].dec);
                    if (sep < bestSep) { bestSep = sep; idx = k; }
                }
            } else if (chunk.size() == 1) {
                idx = 0;
            }
            if (idx < 0) continue;

            SpecFetch::RemoteSpectrum r;
            r.archive      = SpecFetch::Archive::MastSSAP;
            r.collection   = collCol >= 0 ? t->value(i, collCol) : QString();
            r.archiveLabel = QStringLiteral("MAST %1").arg(r.collection);
            r.originId     = QStringLiteral("mast-%1:%2")
                             .arg(r.collection.toLower(), obsId);
            r.starId         = chunk[idx].starId;
            r.instrumentHint = instCol >= 0 ? t->value(i, instCol) : QString();
            if (r.instrumentHint.isEmpty()) r.instrumentHint = r.collection;
            r.mjd = tminCol >= 0 ? toDoubleOr(t->value(i, tminCol), NAN) : NAN;
            r.resolution =
                resCol >= 0 ? toDoubleOr(t->value(i, resCol), NAN) : NAN;
            r.ra  = rowRa;
            r.dec = rowDec;
            if (sizeCol >= 0) {
                const double sz = toDoubleOr(t->value(i, sizeCol), -1);
                r.sizeBytes = sz > 0 ? qint64(sz) : -1;
            }
            r.isCoadd     = true;
            r.downloadUrl = QUrl(url);
            QString name  = QUrl(url).fileName();
            if (name.isEmpty()) name = obsId + QStringLiteral(".fits");
            r.fileName = name;

            bestScore.insert(obsId, score);
            byObsId.insert(obsId, r);
        }

        for (const auto& r : byObsId) out.append(r);

        starsDone += int(chunk.size());
        if (progress) progress(starsDone, int(stars.size()));
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
