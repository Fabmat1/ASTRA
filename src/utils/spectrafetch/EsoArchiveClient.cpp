// src/utils/spectrafetch/EsoArchiveClient.cpp

#include "EsoArchiveClient.h"

#include "TapHelpers.h"
#include "VoTableReader.h"
#include "models/Spectrum.h"
#include "utils/CdsTapClient.h"
#include "utils/Logger.h"
#include "utils/SpectrumReader.h"

#include <QFileInfo>

#include <fitsio.h>

#include <cmath>
#include <utility>
#include <vector>

namespace {

constexpr char kTapSyncUrl[] = "https://archive.eso.org/tap_obs/sync";
constexpr char kDataPortalFileUrl[] =
    "https://dataportal.eso.org/dataPortal/file/";
// The anonymous ESO TAP endpoint offers no uploadMethod (verified against
// /tap_obs/capabilities), so the batched crossmatch is an OR-chain of
// CONTAINS circles instead of a TAP_UPLOAD join.
//
// The binding limit is not the request size but ESO's 120 s server-side query
// budget: each extra circle costs roughly 12 s (one circle ~19 s, two ~31 s,
// 21 time out), and the cost varies with how crowded the fields are. So this
// is a starting size, not a safe size - a chunk that comes back "The request
// timed out" is halved and retried, down to a single star, which keeps a
// dense field from stalling the whole run.
constexpr int kInitialChunk  = 5;
constexpr int kTapTimeoutMs  = 300000;
constexpr int kLinkTimeoutMs = 60000;

// Small-angle separation in degrees; plenty for arcsecond-scale radii.
double angularSepDeg(double ra1, double dec1, double ra2, double dec2) {
    const double d2r  = M_PI / 180.0;
    const double dra  = (ra1 - ra2) * std::cos(0.5 * (dec1 + dec2) * d2r);
    const double ddec = dec1 - dec2;
    return std::sqrt(dra * dra + ddec * ddec);
}

double toDoubleOr(const QString& s, double fallback) {
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : fallback;
}

// Wavelength unit -> Angstrom factor. Phase-3 products use nm (XSHOOTER),
// Angstrom (UVES, FEROS, HARPS), rarely um.
double unitToAngstrom(const QString& unit) {
    const QString u = unit.trimmed().toLower();
    if (u.isEmpty()) return 1.0;
    if (u.contains(QStringLiteral("angstrom")) || u == QLatin1String("aa") ||
        u == QLatin1String("a"))
        return 1.0;
    if (u.contains(QStringLiteral("nm")) || u.contains(QStringLiteral("nanomet")))
        return 10.0;
    if (u.contains(QStringLiteral("um")) || u.contains(QStringLiteral("micron")))
        return 1.0e4;
    if (u == QLatin1String("m") || u.contains(QStringLiteral("metre")) ||
        u.contains(QStringLiteral("meter")))
        return 1.0e10;
    return 1.0;
}

bool nameMatches(const QString& name, const QStringList& candidates) {
    for (const QString& c : candidates)
        if (name.compare(c, Qt::CaseInsensitive) == 0) return true;
    return false;
}

}   // namespace

QStringList EsoArchiveClient::knownCollections() {
    // obs_collection values of the spectroscopic Phase-3 streams.
    return {
        QStringLiteral("XSHOOTER"), QStringLiteral("UVES"),
        QStringLiteral("HARPS"),    QStringLiteral("FEROS"),
        QStringLiteral("GIRAFFE"),  QStringLiteral("MUSE"),
        QStringLiteral("ESPRESSO"), QStringLiteral("CRIRES"),
        QStringLiteral("SINFONI"),  QStringLiteral("KMOS"),
    };
}

bool EsoArchiveClient::deliversBarycentric(
    const SpecFetch::RemoteSpectrum& r) const {
    // HARPS and ESPRESSO 1D products are wavelength-calibrated in the solar
    // system barycentric frame; the other Phase-3 streams are topocentric.
    return r.collection.contains(QStringLiteral("HARPS"), Qt::CaseInsensitive) ||
           r.collection.contains(QStringLiteral("ESPRESSO"),
                                 Qt::CaseInsensitive);
}

QList<SpecFetch::RemoteSpectrum> EsoArchiveClient::discover(
    const std::vector<SpecFetch::StarQuery>& stars,
    const SpecFetch::ArchiveOptions& opt, QNetworkAccessManager* nam,
    const std::function<void(int, int)>& progress,
    const std::atomic<bool>& cancel, QString* error) {
    QList<SpecFetch::RemoteSpectrum> out;
    if (error) error->clear();

    const double radiusDeg = opt.radiusArcsec / 3600.0;

    QString collectionFilter;
    if (!opt.collections.isEmpty()) {
        QStringList quoted;
        for (const QString& c : opt.collections)
            quoted << QStringLiteral("'%1'").arg(QString(c).replace('\'', ""));
        // No table alias in the FROM clause below, so the column must be
        // named bare: "o.obs_collection" is rejected as an unknown column.
        collectionFilter = QStringLiteral(" AND obs_collection IN (%1)")
                               .arg(quoted.join(','));
    }

    // Ranges of `stars` still to query, as a stack so a chunk that has to be
    // split is retried before the run moves on. Pushed back-to-front so the
    // first chunk pops first.
    std::vector<std::pair<size_t, size_t>> pending;
    for (size_t end = stars.size(); end > 0;) {
        const size_t begin =
            end > size_t(kInitialChunk) ? end - size_t(kInitialChunk) : 0;
        pending.emplace_back(begin, end);
        end = begin;
    }

    int starsDone = 0;

    while (!pending.empty()) {
        if (cancel.load()) break;

        const auto [chunkBegin, chunkEnd] = pending.back();
        pending.pop_back();
        const size_t chunkSize = chunkEnd - chunkBegin;

        QStringList circles;
        for (size_t k = chunkBegin; k < chunkEnd; ++k)
            circles << QStringLiteral(
                           "1=CONTAINS(POINT('ICRS', s_ra, s_dec), "
                           "CIRCLE('ICRS', %1, %2, %3))")
                           .arg(stars[k].ra, 0, 'f', 8)
                           .arg(stars[k].dec, 0, 'f', 8)
                           .arg(radiusDeg, 0, 'f', 8);

        const QString adql =
            QStringLiteral(
                "SELECT dp_id, obs_collection, instrument_name, "
                "t_min, t_exptime, em_res_power, snr, access_url, "
                "access_estsize, s_ra, s_dec "
                "FROM ivoa.ObsCore "
                "WHERE dataproduct_type = 'spectrum' "
                "AND (%1)%2")
                .arg(circles.join(QStringLiteral(" OR ")))
                .arg(collectionFilter);

        QString    chunkError;
        const QByteArray body =
            SpecFetch::tapQuery(nam, QString::fromLatin1(kTapSyncUrl), adql,
                                QStringLiteral("csv"), kTapTimeoutMs,
                                &chunkError);

        if (!chunkError.isEmpty()) {
            // ESO answers an over-budget query with QUERY_STATUS=ERROR
            // "The request timed out". Half the stars usually fit.
            if (chunkSize > 1 &&
                chunkError.contains(QStringLiteral("timed out"),
                                    Qt::CaseInsensitive)) {
                const size_t mid = chunkBegin + chunkSize / 2;
                pending.emplace_back(mid, chunkEnd);
                pending.emplace_back(chunkBegin, mid);
                LOG_INFO("SpecFetch",
                         QStringLiteral("ESO TAP chunk of %1 timed out, "
                                        "splitting").arg(chunkSize));
                continue;
            }
            if (error) *error = chunkError;
            LOG_WARNING("SpecFetch",
                        QStringLiteral("ESO TAP chunk failed: %1").arg(chunkError));
            break;
        }

        const SpecFetch::Csv csv = SpecFetch::parseCsv(body);
        for (int i = 0; i < csv.rows.size(); ++i) {
            const QString dpId = csv.value(i, QStringLiteral("dp_id"));
            if (dpId.isEmpty()) continue;

            // Rows carry no upload index: map each product back to the
            // nearest chunk star.
            const double rowRa =
                toDoubleOr(csv.value(i, QStringLiteral("s_ra")), NAN);
            const double rowDec =
                toDoubleOr(csv.value(i, QStringLiteral("s_dec")), NAN);
            size_t idx     = chunkEnd;
            double bestSep = 2.0 * radiusDeg;
            if (!std::isnan(rowRa) && !std::isnan(rowDec)) {
                for (size_t k = chunkBegin; k < chunkEnd; ++k) {
                    const double sep = angularSepDeg(rowRa, rowDec,
                                                     stars[k].ra, stars[k].dec);
                    if (sep < bestSep) { bestSep = sep; idx = k; }
                }
            } else if (chunkSize == 1) {
                idx = chunkBegin;
            }
            if (idx >= chunkEnd) continue;

            SpecFetch::RemoteSpectrum r;
            r.archive        = SpecFetch::Archive::EsoPhase3;
            r.archiveLabel   = displayName();
            r.originId       = QStringLiteral("eso:") + dpId;
            r.starId         = stars[idx].starId;
            r.collection     = csv.value(i, QStringLiteral("obs_collection"));
            r.instrumentHint = csv.value(i, QStringLiteral("instrument_name"));
            if (r.instrumentHint.isEmpty()) r.instrumentHint = r.collection;
            r.mjd = toDoubleOr(csv.value(i, QStringLiteral("t_min")), NAN);
            r.ra  = toDoubleOr(csv.value(i, QStringLiteral("s_ra")), NAN);
            r.dec = toDoubleOr(csv.value(i, QStringLiteral("s_dec")), NAN);
            r.resolution =
                toDoubleOr(csv.value(i, QStringLiteral("em_res_power")), NAN);
            r.snr = toDoubleOr(csv.value(i, QStringLiteral("snr")), NAN);
            const double estKb =
                toDoubleOr(csv.value(i, QStringLiteral("access_estsize")), -1);
            r.sizeBytes = estKb > 0 ? qint64(estKb * 1024) : -1;
            r.isCoadd   = true;   // Phase-3 products are what they are

            const QString accessUrl =
                csv.value(i, QStringLiteral("access_url"));
            r.downloadUrl =
                accessUrl.isEmpty()
                    ? QUrl(QString::fromLatin1(kDataPortalFileUrl) + dpId)
                    : QUrl(accessUrl);
            r.fileName = dpId + QStringLiteral(".fits");

            out.append(r);
        }

        starsDone += int(chunkSize);
        if (progress) progress(starsDone, int(stars.size()));
    }

    return out;
}

QUrl EsoArchiveClient::resolveDownloadUrl(const SpecFetch::RemoteSpectrum& r,
                                          QNetworkAccessManager* nam,
                                          QString* error) {
    if (error) error->clear();

    const QString urlStr = r.downloadUrl.toString();
    if (!urlStr.contains(QStringLiteral("datalink"), Qt::CaseInsensitive))
        return r.downloadUrl;

    // access_url is a DataLink document: fetch it and pick the science file
    // (semantics "#this").
    const CdsTap::Response resp = CdsTap::get(nam, urlStr, kLinkTimeoutMs);
    if (!resp.ok()) {
        if (error) *error = resp.error;
        return QUrl();
    }

    const VoTable::Document doc = VoTable::parse(resp.body);
    if (const VoTable::Table* t = doc.firstTable()) {
        const int semCol = t->columnByName(QStringLiteral("semantics"));
        const int urlCol = t->columnByName(QStringLiteral("access_url"));
        if (semCol >= 0 && urlCol >= 0) {
            for (int i = 0; i < t->rows.size(); ++i) {
                if (t->value(i, semCol).compare(QStringLiteral("#this"),
                                                Qt::CaseInsensitive) == 0) {
                    const QString u = t->value(i, urlCol);
                    if (!u.isEmpty()) return QUrl(u);
                }
            }
        }
    }

    // DataLink document without a usable #this row: fall back to the
    // dataportal file endpoint keyed by dp_id.
    const QString dpId = r.originId.section(':', 1);
    if (!dpId.isEmpty())
        return QUrl(QString::fromLatin1(kDataPortalFileUrl) + dpId);

    if (error) *error = doc.error.isEmpty()
                            ? QStringLiteral("no download link in DataLink")
                            : doc.error;
    return QUrl();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parsing
// ─────────────────────────────────────────────────────────────────────────────

// ESO SDP 1D spectra: a BINTABLE with one row whose cells are the full
// arrays (WAVE / FLUX / ERR), units in TUNITn. The default FITS reader
// expects one value per row, so this layout needs its own path.
static bool parsePhase3Bintable(const QString& path,
                                std::vector<double>& wl,
                                std::vector<double>& flux,
                                std::vector<double>& err,
                                double* mjdOut, double* expOut,
                                QString* error) {
    fitsfile* fptr  = nullptr;
    int       status = 0;

    if (fits_open_file(&fptr, path.toUtf8().constData(), READONLY, &status)) {
        char msg[FLEN_ERRMSG];
        fits_read_errmsg(msg);
        if (error) *error = QString::fromLatin1(msg);
        return false;
    }

    // Epoch / exposure from the primary header, where the SDP keywords live.
    {
        int st = 0;
        double v = 0;
        if (fits_read_key(fptr, TDOUBLE, "MJD-OBS", &v, nullptr, &st) == 0 &&
            mjdOut)
            *mjdOut = v;
        st = 0;
        if (fits_read_key(fptr, TDOUBLE, "EXPTIME", &v, nullptr, &st) == 0 &&
            expOut)
            *expOut = v;
        else {
            st = 0;
            if (fits_read_key(fptr, TDOUBLE, "TEXPTIME", &v, nullptr, &st) == 0 &&
                expOut)
                *expOut = v;
        }
    }

    static const QStringList kWaveNames = {
        QStringLiteral("WAVE"), QStringLiteral("WAVELENGTH"),
        QStringLiteral("LAMBDA"), QStringLiteral("AWAV"),
        QStringLiteral("WAVE_AIR"),
    };
    static const QStringList kFluxNames = {
        QStringLiteral("FLUX"), QStringLiteral("FLUX_REDUCED"),
        QStringLiteral("FLUX_CAL"), QStringLiteral("BGFLUX"),
    };
    static const QStringList kErrNames = {
        QStringLiteral("ERR"), QStringLiteral("ERROR"),
        QStringLiteral("SIGMA"), QStringLiteral("ERR_REDUCED"),
        QStringLiteral("FLUX_ERROR"), QStringLiteral("ERRS"),
    };

    int numHdus = 0;
    fits_get_num_hdus(fptr, &numHdus, &status);

    bool found = false;
    for (int hdu = 2; hdu <= numHdus && !found; ++hdu) {
        int hduType = ANY_HDU;
        status      = 0;
        if (fits_movabs_hdu(fptr, hdu, &hduType, &status) != 0 ||
            hduType != BINARY_TBL)
            continue;

        int ncols = 0;
        fits_get_num_cols(fptr, &ncols, &status);
        long nrows = 0;
        fits_get_num_rows(fptr, &nrows, &status);
        if (status != 0 || ncols <= 0 || nrows != 1) { status = 0; continue; }

        int waveCol = -1, fluxCol = -1, errCol = -1;
        double waveScale = 1.0;
        for (int c = 1; c <= ncols; ++c) {
            char name[FLEN_VALUE] = {0};
            int  st               = 0;
            QString key = QStringLiteral("TTYPE%1").arg(c);
            if (fits_read_key(fptr, TSTRING, key.toLatin1().constData(), name,
                              nullptr, &st) != 0)
                continue;
            const QString colName = QString::fromLatin1(name).trimmed();
            if (waveCol < 0 && nameMatches(colName, kWaveNames)) {
                waveCol = c;
                char unit[FLEN_VALUE] = {0};
                st = 0;
                const QString ukey = QStringLiteral("TUNIT%1").arg(c);
                if (fits_read_key(fptr, TSTRING, ukey.toLatin1().constData(),
                                  unit, nullptr, &st) == 0)
                    waveScale = unitToAngstrom(QString::fromLatin1(unit));
            } else if (fluxCol < 0 && nameMatches(colName, kFluxNames)) {
                fluxCol = c;
            } else if (errCol < 0 && nameMatches(colName, kErrNames)) {
                errCol = c;
            }
        }
        if (waveCol < 0 || fluxCol < 0)
            continue;

        auto readArrayCol = [&](int col, std::vector<double>& outVec) -> bool {
            int      typecode = 0;
            long     repeat = 0, width = 0;
            int      st = 0;
            if (fits_get_coltype(fptr, col, &typecode, &repeat, &width, &st) !=
                    0 ||
                repeat < 2)
                return false;
            outVec.resize(size_t(repeat));
            int anynul = 0;
            return fits_read_col(fptr, TDOUBLE, col, 1, 1, repeat, nullptr,
                                 outVec.data(), &anynul, &st) == 0;
        };

        if (!readArrayCol(waveCol, wl) || !readArrayCol(fluxCol, flux))
            continue;
        if (errCol > 0) {
            if (!readArrayCol(errCol, err)) err.clear();
        }
        if (wl.size() != flux.size()) { wl.clear(); flux.clear(); continue; }
        if (!err.empty() && err.size() != wl.size()) err.clear();

        if (waveScale != 1.0)
            for (double& w : wl) w *= waveScale;

        found = true;
    }

    fits_close_file(fptr, &status);

    if (!found && error && error->isEmpty())
        *error = QStringLiteral("no Phase-3 spectral table found");
    return found;
}

std::vector<SpecFetch::ParsedSpectrum> EsoArchiveClient::parse(
    const QString& localPath, const SpecFetch::RemoteSpectrum& r,
    const SpecFetch::ArchiveOptions& opt, QString* error) {
    Q_UNUSED(opt);
    std::vector<SpecFetch::ParsedSpectrum> out;
    if (error) error->clear();

    std::vector<double> wl, flux, err;
    double mjd = NAN, exp = NAN;
    QString p3err;

    auto spectrum = std::make_shared<Spectrum>();

    if (parsePhase3Bintable(localPath, wl, flux, err, &mjd, &exp, &p3err)) {
        if (err.empty()) err.assign(wl.size(), 0.0);
        spectrum->setData(wl, flux, err);
    } else {
        // Not the single-row-arrays layout: let the generic FITS reader try.
        auto reader =
            SpectrumReaderRegistry::instance().getReaderForFile(localPath);
        if (!reader) {
            if (error) *error = p3err;
            return out;
        }
        SpectrumReadResult res = reader->readSpectrum(localPath);
        if (!res.success || !res.spectrum || !res.spectrum->hasData()) {
            if (error)
                *error = res.errorMessage.isEmpty() ? p3err : res.errorMessage;
            return out;
        }
        spectrum = res.spectrum;
        if (res.metadata.mjd) mjd = *res.metadata.mjd;
        if (res.metadata.exposureTime) exp = *res.metadata.exposureTime;
    }

    if (std::isnan(mjd) && !std::isnan(r.mjd)) mjd = r.mjd;
    if (!std::isnan(mjd)) spectrum->setMJD(mjd);
    if (!std::isnan(exp) && exp > 0) spectrum->setExposureTime(exp);
    spectrum->setFile(localPath);
    if (spectrum->getInstrument().isEmpty())
        spectrum->setInstrument(r.instrumentHint);

    SpecFetch::ParsedSpectrum ps;
    ps.spectrum       = spectrum;
    ps.originId       = r.originId;
    ps.isCoadd        = r.isCoadd;
    ps.instrumentHint = r.instrumentHint;
    out.push_back(std::move(ps));
    return out;
}
