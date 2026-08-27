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

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace {

constexpr char kTapAsyncUrl[] = "https://archive.eso.org/tap_obs/async";
constexpr char kDataPortalFileUrl[] =
    "https://dataportal.eso.org/dataPortal/file/";
// The anonymous ESO TAP endpoint offers no uploadMethod (verified against
// /tap_obs/capabilities), so the batched crossmatch is an OR-chain of
// per-star predicates instead of a TAP_UPLOAD join.
//
// Those predicates are RA/Dec boxes, not CONTAINS circles, because ESO's
// optimiser does not use its index for an OR-chain of geometry calls: measured
// 2026-08-25 on the same 20 Ondrejov stars, an OR of 20 CONTAINS circles could
// not finish inside the 120 s /sync budget (nor could five of them), while an
// OR of 20 boxes answered in 20 s. A box is a superset of its circle, so the
// corners are trimmed below against the true angular separation and the
// effective match radius is still exactly the one the caller asked for.
//
// Discovery goes through /async rather than /sync. Not for the convenience:
// /sync hands a query 120 s and nothing more, and an ObsCore crossmatch does
// not fit in that even for a handful of stars (measured 2026-08-25 against
// the Ondrejov list: 5, 20 and 50 circles all died on the wall, the first two
// as QUERY_STATUS=ERROR "The request timed out", the third as a 502 from the
// front end). A UWS job can be given the service's hard executionDuration
// instead, which ESO advertises as 3600 s, so the same crossmatch that cannot
// finish synchronously at any batch size completes comfortably as a job.
constexpr int kAsyncDurationSec = 3000;

// Rows per job. ESO's default MAXREC is 20000 and it truncates silently, which
// on a bulk run would look like "these stars have no spectra".
constexpr int kAsyncMaxRec = 500000;

// Stars per job. With boxes the whole 302-star Ondrejov list runs as a single
// job in 21 s, so this is not a performance ceiling - it is there so that one
// chunk that will not run is not the whole run, so the progress bar has
// something to move on, and so the ADQL stays a sane size (a star costs about
// 95 characters of predicate).
constexpr int kInitialChunk = 100;

// Client-side patience per job, comfortably above the server's own budget so
// that a job which is going to be killed is killed by ESO, with ESO's message,
// rather than abandoned by us first.
constexpr int kJobBudgetMs = 3300000;

// A run where every chunk fails is a broken service or a broken query, not a
// crowded field; stop rather than work through 280 stars of the same error.
constexpr int kMaxConsecutiveFailures = 3;

constexpr int kLinkTimeoutMs = 60000;

// Distinguishes "this chunk was too much work" from "this query is wrong".
// The first is worth retrying on half the stars, the second never is.
bool looksLikeBudgetError(const QString& msg) {
    static const char* kNeedles[] = {"timed out", "time out", "ABORTED",
                                     "execution duration", "still EXECUTING",
                                     "too long", "502", "Proxy Error"};
    for (const char* needle : kNeedles)
        if (msg.contains(QLatin1String(needle), Qt::CaseInsensitive))
            return true;
    return false;
}

// ADQL for "s_ra/s_dec lies in the box of half-height `radiusDeg` around this
// star". The RA half-width is inflated by 1/cos(dec) so the box still contains
// the whole circle, clamped near the poles where that blows up, and split in
// two when it straddles the RA origin - `s_ra BETWEEN 359.9 AND 0.1` is empty,
// not wrapped.
QString boxPredicate(double ra, double dec, double radiusDeg) {
    const double cosDec = std::cos(dec * M_PI / 180.0);
    const double halfRa =
        std::min(180.0, radiusDeg / std::max(std::abs(cosDec), 1.0e-3));

    const QString decTerm = QStringLiteral("s_dec BETWEEN %1 AND %2")
                                .arg(dec - radiusDeg, 0, 'f', 8)
                                .arg(dec + radiusDeg, 0, 'f', 8);

    const double lo = ra - halfRa;
    const double hi = ra + halfRa;

    QString raTerm;
    if (halfRa >= 180.0) {
        raTerm = QStringLiteral("s_ra BETWEEN 0 AND 360");
    } else if (lo < 0.0) {
        raTerm = QStringLiteral("(s_ra BETWEEN %1 AND 360 OR s_ra BETWEEN 0 "
                                "AND %2)")
                     .arg(lo + 360.0, 0, 'f', 8)
                     .arg(hi, 0, 'f', 8);
    } else if (hi > 360.0) {
        raTerm = QStringLiteral("(s_ra BETWEEN %1 AND 360 OR s_ra BETWEEN 0 "
                                "AND %2)")
                     .arg(lo, 0, 'f', 8)
                     .arg(hi - 360.0, 0, 'f', 8);
    } else {
        raTerm = QStringLiteral("s_ra BETWEEN %1 AND %2")
                     .arg(lo, 0, 'f', 8)
                     .arg(hi, 0, 'f', 8);
    }

    return QStringLiteral("(%1 AND %2)").arg(decTerm, raTerm);
}

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

    int     starsDone           = 0;
    int     failedStars         = 0;
    int     consecutiveFailures = 0;
    QString lastError;

    while (!pending.empty()) {
        if (cancel.load()) break;

        const auto [chunkBegin, chunkEnd] = pending.back();
        pending.pop_back();
        const size_t chunkSize = chunkEnd - chunkBegin;

        QStringList boxes;
        for (size_t k = chunkBegin; k < chunkEnd; ++k)
            boxes << boxPredicate(stars[k].ra, stars[k].dec, radiusDeg);

        const QString adql =
            QStringLiteral(
                "SELECT dp_id, obs_collection, instrument_name, "
                "t_min, t_exptime, em_res_power, snr, access_url, "
                "access_estsize, s_ra, s_dec "
                "FROM ivoa.ObsCore "
                "WHERE dataproduct_type = 'spectrum' "
                "AND (%1)%2")
                .arg(boxes.join(QStringLiteral(" OR ")))
                .arg(collectionFilter);

        QString         chunkError;
        CdsTap::Request request(kJobBudgetMs);
        request.cancel = &cancel;
        const QByteArray body = SpecFetch::tapAsyncQuery(
            nam, QString::fromLatin1(kTapAsyncUrl), adql,
            QStringLiteral("csv"), kAsyncDurationSec, kAsyncMaxRec, request,
            &chunkError);

        if (!chunkError.isEmpty()) {
            if (cancel.load()) break;

            // The chunk was more work than ESO would do in one go. Half the
            // stars is half the sky to sweep, so the split is worth a retry.
            if (chunkSize > 1 && looksLikeBudgetError(chunkError)) {
                const size_t mid = chunkBegin + chunkSize / 2;
                pending.emplace_back(mid, chunkEnd);
                pending.emplace_back(chunkBegin, mid);
                LOG_INFO("SpecFetch",
                         QStringLiteral("ESO TAP chunk of %1 did not finish "
                                        "(%2), splitting")
                             .arg(chunkSize)
                             .arg(chunkError));
                continue;
            }

            // One chunk that will not run is not a reason to throw away the
            // stars behind it: skip it and carry on, and only give up once
            // the failures stop looking incidental.
            ++consecutiveFailures;
            lastError = chunkError;
            failedStars += int(chunkSize);
            LOG_WARNING("SpecFetch",
                        QStringLiteral("ESO TAP chunk of %1 star(s) failed: %2")
                            .arg(chunkSize)
                            .arg(chunkError));

            if (consecutiveFailures >= kMaxConsecutiveFailures) {
                if (error)
                    *error = QStringLiteral("gave up after %1 failed queries "
                                            "(%2)")
                                 .arg(consecutiveFailures)
                                 .arg(chunkError);
                break;
            }

            starsDone += int(chunkSize);
            if (progress) progress(starsDone, int(stars.size()));
            continue;
        }
        consecutiveFailures = 0;

        const SpecFetch::Csv csv = SpecFetch::parseCsv(body);
        // MAXREC truncation is silent in CSV output, so a full result set is
        // the only warning we get that rows were dropped.
        if (csv.rows.size() >= kAsyncMaxRec)
            LOG_WARNING("SpecFetch",
                        QStringLiteral("ESO TAP chunk hit the %1-row limit; "
                                       "some products were dropped")
                            .arg(kAsyncMaxRec));

        for (int i = 0; i < csv.rows.size(); ++i) {
            const QString dpId = csv.value(i, QStringLiteral("dp_id"));
            if (dpId.isEmpty()) continue;

            // Rows carry no upload index: map each product back to the
            // nearest chunk star.
            const double rowRa =
                toDoubleOr(csv.value(i, QStringLiteral("s_ra")), NAN);
            const double rowDec =
                toDoubleOr(csv.value(i, QStringLiteral("s_dec")), NAN);
            // The box corners reach past the radius, so this is where the
            // circle is actually cut - and, as before, where a product is
            // attributed to the nearest of the chunk's stars.
            size_t idx     = chunkEnd;
            double bestSep = radiusDeg;
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

    // Skipped stars are reported even when the run otherwise succeeded: a
    // silently short result set is indistinguishable from "no spectra there".
    // A stop is not a failure, though - the session reports that itself.
    if (error && error->isEmpty() && failedStars > 0 && !cancel.load())
        *error = QStringLiteral("%1 of %2 star(s) could not be queried (%3)")
                     .arg(failedStars)
                     .arg(stars.size())
                     .arg(lastError);

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
