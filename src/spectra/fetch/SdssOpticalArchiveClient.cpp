// src/utils/spectrafetch/SdssOpticalArchiveClient.cpp

#include "spectra/fetch/SdssOpticalArchiveClient.h"
#include "spectra/fetch/ExposureEpoch.h"

#include "spectra/fetch/TapHelpers.h"
#include "spectra/fetch/WavelengthConvert.h"
#include "core/BarycentricCorrection.h"
#include "spectra/Spectrum.h"
#include "catalog/CdsTapClient.h"
#include "app/Logger.h"

#include <QHash>
#include <QUrl>
#include <QUrlQuery>

#include <fitsio.h>

#include <algorithm>
#include <cmath>

namespace {

// Shared archive helpers, so each client does not carry its own copy.
using SpecFetch::toDoubleOr;

// VALUES rows per SkyServer query; ~90 chars each keeps the statement far
// below the request limits while still batching usefully.
constexpr int kChunkSize    = 40;
constexpr int kSqlTimeoutMs = 120000;

QString skyServerUrl(const QString& dr) {
    return QStringLiteral("https://skyserver.sdss.org/%1/SkyServerWS/"
                          "SearchTools/SqlSearch")
        .arg(dr.toLower());
}

// SDSS-I/II runs (run2d 26/103/104) live under sdss/, everything later under
// eboss/.
bool isLegacyRun2d(const QString& run2d) {
    return run2d == QLatin1String("26") || run2d == QLatin1String("103") ||
           run2d == QLatin1String("104");
}

QString specFileName(int plate, int mjd, int fiber) {
    return QStringLiteral("spec-%1-%2-%3.fits")
        .arg(plate, 4, 10, QLatin1Char('0'))
        .arg(mjd)
        .arg(fiber, 4, 10, QLatin1Char('0'));
}

// Read one scalar double column over all rows.
bool readColumn(fitsfile* fptr, const char* name, long nrows,
                std::vector<double>& out) {
    int col = 0, status = 0;
    if (fits_get_colnum(fptr, CASEINSEN, const_cast<char*>(name), &col,
                        &status) != 0)
        return false;
    out.resize(size_t(nrows));
    int anynul = 0;
    return fits_read_col(fptr, TDOUBLE, col, 1, 1, nrows, nullptr, out.data(),
                         &anynul, &status) == 0;
}

// Mid-exposure epoch of one exposure HDU as MJD(UTC).  The TAI-to-UTC
// arithmetic, and why TAI-BEG + EXPTIME/2 is the only correct midpoint, live
// in ExposureEpoch.h so they can be tested without a FITS fixture.
bool readExposureEpoch(fitsfile* fptr, double& mjdUtc, double& expTime) {
    int    st  = 0;
    double tai = 0;
    if (fits_read_key(fptr, TDOUBLE, "TAI-BEG", &tai, nullptr, &st) != 0 ||
        tai <= 0)
        return false;

    st = 0;
    double e = 0;
    if (fits_read_key(fptr, TDOUBLE, "EXPTIME", &e, nullptr, &st) != 0 || e <= 0)
        e = 0.0;

    mjdUtc  = SpecFetch::sdssMidExposureMjdUtc(tai, e);
    expTime = e;
    return true;
}

// loglam/flux/ivar table (coadd or single exposure) -> spectrum arrays.
bool readLoglamTable(fitsfile* fptr, std::vector<double>& wl,
                     std::vector<double>& flux, std::vector<double>& err) {
    int  status = 0;
    long nrows  = 0;
    if (fits_get_num_rows(fptr, &nrows, &status) != 0 || nrows < 2)
        return false;

    std::vector<double> loglam, ivar;
    if (!readColumn(fptr, "loglam", nrows, loglam) ||
        !readColumn(fptr, "flux", nrows, flux))
        return false;
    readColumn(fptr, "ivar", nrows, ivar);

    wl.resize(loglam.size());
    for (size_t i = 0; i < loglam.size(); ++i)
        wl[i] = std::pow(10.0, loglam[i]);   // vacuum Angstrom

    err.assign(wl.size(), 0.0);
    if (ivar.size() == wl.size())
        for (size_t i = 0; i < ivar.size(); ++i)
            if (ivar[i] > 0) err[i] = 1.0 / std::sqrt(ivar[i]);

    // The SDSS-I/II blue camera reads out red-to-blue, so its per-exposure
    // HDUs are stored in *descending* wavelength (BOSS and every coadd are
    // ascending). Everything downstream assumes ascending.
    if (wl.size() > 1 && wl.front() > wl.back()) {
        std::reverse(wl.begin(), wl.end());
        std::reverse(flux.begin(), flux.end());
        std::reverse(err.begin(), err.end());
    }
    return true;
}

}   // namespace

QStringList SdssOpticalArchiveClient::knownDataReleases() {
    return {QStringLiteral("DR17"), QStringLiteral("DR16")};
}

QList<SpecFetch::RemoteSpectrum> SdssOpticalArchiveClient::discover(
    const std::vector<SpecFetch::StarQuery>& stars,
    const SpecFetch::ArchiveOptions& opt, QNetworkAccessManager* nam,
    const std::function<void(int, int)>& progress,
    const std::atomic<bool>& cancel, QString* error) {
    QList<SpecFetch::RemoteSpectrum> out;
    if (error) error->clear();

    const QString dr =
        opt.dataRelease.isEmpty() ? QStringLiteral("DR17") : opt.dataRelease;
    const double radiusDeg = opt.radiusArcsec / 3600.0;

    const auto chunks = SpecFetch::chunked(stars, size_t(kChunkSize));
    int starsDone = 0;

    for (const auto& chunk : chunks) {
        if (cancel.load()) break;

        // Per-star RA/Dec bounds, joined against specObjAll directly.
        //
        // NOT dbo.fGetNearbySpecObjEq: that function returns only the
        // sciencePrimary spectrum, so a star observed more than once comes
        // back as a single row and every repeat visit silently disappears -
        // joining specObjAll to it cannot bring them back, because the
        // function has already dropped them. Gaia DR3 2500824388329728256 has
        // 21 spectra in DR17 and exactly one is sciencePrimary; the cone
        // returned that one. Repeat visits are the whole point for RV work.
        //
        // A box straddling the RA origin contributes two rows sharing one
        // idx, and the box is a superset of its circle, so the corners are
        // trimmed against the true separation below.
        QStringList values;
        for (int k = 0; k < int(chunk.size()); ++k)
            for (const SpecFetch::RaDecBox& b :
                 SpecFetch::boxesFor(chunk[k].ra, chunk[k].dec, radiusDeg))
                values << QStringLiteral("(%1, %2, %3, %4, %5)")
                              .arg(k)
                              .arg(b.raLo, 0, 'f', 8)
                              .arg(b.raHi, 0, 'f', 8)
                              .arg(b.decLo, 0, 'f', 8)
                              .arg(b.decHi, 0, 'f', 8);

        const QString sql =
            QStringLiteral(
                "SELECT p.idx, s.specObjID, s.plate, s.mjd, s.fiberID, "
                "s.run2d, s.survey, s.instrument, s.snMedian, s.ra, s.dec "
                "FROM (VALUES %1) AS p(idx, ra0, ra1, dec0, dec1) "
                "JOIN specObjAll s ON s.ra BETWEEN p.ra0 AND p.ra1 "
                "AND s.dec BETWEEN p.dec0 AND p.dec1")
                .arg(values.join(QStringLiteral(", ")));

        // SkyServer's SqlSearch 500s on POSTed form bodies (as of 2026-08);
        // the identical query succeeds as a GET, which takes several KB of
        // query string without complaint.
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("cmd"), sql);
        q.addQueryItem(QStringLiteral("format"), QStringLiteral("csv"));
        QUrl url{skyServerUrl(dr)};
        url.setQuery(q);

        CdsTap::Request request(kSqlTimeoutMs);
        request.cancel = &cancel;
        const CdsTap::Response resp =
            CdsTap::get(nam, url.toString(QUrl::FullyEncoded), request);
        if (!resp.ok()) {
            if (error) *error = resp.error;
            LOG_WARNING("SpecFetch", QStringLiteral("SkyServer chunk failed: %1")
                                         .arg(resp.error));
            break;
        }
        if (resp.body.startsWith('{')) {
            // SkyServer reports SQL errors as a JSON blob with HTTP 200/500.
            if (error)
                *error = QString::fromUtf8(resp.body.left(300));
            break;
        }

        const SpecFetch::Csv csv = SpecFetch::parseCsv(resp.body);

        // Trim each box back to the circle the caller asked for, and settle
        // ties: two stars closer together than the radius both match the same
        // spectrum, and the service keys queued downloads on origin_id, so a
        // spectrum has to end up on exactly one star - the nearest, as the ESO
        // client also does.
        QHash<QString, int> bestRow;                  // specObjID -> csv row
        QHash<QString, double> bestSep;
        for (int i = 0; i < csv.rows.size(); ++i) {
            bool okIdx = false;
            const int idx = csv.value(i, QStringLiteral("idx")).toInt(&okIdx);
            if (!okIdx || idx < 0 || idx >= int(chunk.size())) continue;

            const QString specObjId =
                csv.value(i, QStringLiteral("specobjid"));
            if (specObjId.isEmpty()) continue;

            const double rowRa =
                toDoubleOr(csv.value(i, QStringLiteral("ra")), NAN);
            const double rowDec =
                toDoubleOr(csv.value(i, QStringLiteral("dec")), NAN);
            double sep = 0.0;
            if (!std::isnan(rowRa) && !std::isnan(rowDec)) {
                sep = SpecFetch::angularSepDeg(rowRa, rowDec, chunk[idx].ra,
                                               chunk[idx].dec);
                if (sep > radiusDeg) continue;        // a box corner
            }
            const auto it = bestSep.constFind(specObjId);
            if (it != bestSep.constEnd() && it.value() <= sep) continue;
            bestSep[specObjId] = sep;
            bestRow[specObjId] = i;
        }

        // Walk the response in order rather than the hash, so the result is
        // the same on every run.
        for (int i = 0; i < csv.rows.size(); ++i) {
            const QString specObjId =
                csv.value(i, QStringLiteral("specobjid"));
            if (bestRow.value(specObjId, -1) != i) continue;
            const int idx = csv.value(i, QStringLiteral("idx")).toInt();
            const int plate = csv.value(i, QStringLiteral("plate")).toInt();
            const int mjd   = csv.value(i, QStringLiteral("mjd")).toInt();
            const int fiber = csv.value(i, QStringLiteral("fiberid")).toInt();
            const QString run2d = csv.value(i, QStringLiteral("run2d"));
            if (plate <= 0 || mjd <= 0 || fiber <= 0) continue;

            SpecFetch::RemoteSpectrum r;
            r.archive        = SpecFetch::Archive::SdssOptical;
            r.archiveLabel   = QStringLiteral("SDSS %1").arg(dr.toUpper());
            r.originId = QStringLiteral("sdss-%1:%2")
                             .arg(dr.toLower(), specObjId);
            r.starId         = chunk[idx].starId;
            r.collection     = csv.value(i, QStringLiteral("survey"));
            r.instrumentHint =
                csv.value(i, QStringLiteral("instrument")).toUpper();
            if (r.instrumentHint.isEmpty())
                r.instrumentHint = QStringLiteral("SDSS");
            r.mjd = double(mjd);
            r.snr = toDoubleOr(csv.value(i, QStringLiteral("snmedian")), NAN);
            // Full spec files bundle the camera exposures; with the
            // exposures option on, parse() imports those instead of the
            // coadd.
            r.isCoadd = !opt.fetchExposures;

            const QString branch = isLegacyRun2d(run2d)
                                       ? QStringLiteral("sdss")
                                       : QStringLiteral("eboss");
            const QString variant = opt.fetchExposures
                                        ? QStringLiteral("full")
                                        : QStringLiteral("lite");
            r.fileName = specFileName(plate, mjd, fiber);
            r.downloadUrl =
                QUrl(QStringLiteral(
                         "https://data.sdss.org/sas/%1/%2/spectro/redux/%3/"
                         "spectra/%4/%5/%6")
                         .arg(dr.toLower(), branch, run2d, variant)
                         .arg(plate, 4, 10, QLatin1Char('0'))
                         .arg(r.fileName));
            out.append(r);
        }

        starsDone += int(chunk.size());
        if (progress) progress(starsDone, int(stars.size()));
    }

    return out;
}

std::vector<SpecFetch::ParsedSpectrum> SdssOpticalArchiveClient::parse(
    const QString& localPath, const SpecFetch::RemoteSpectrum& r,
    const SpecFetch::ArchiveOptions& opt, QString* error) {
    std::vector<SpecFetch::ParsedSpectrum> out;
    if (error) error->clear();

    fitsfile* fptr  = nullptr;
    int       status = 0;
    if (fits_open_file(&fptr, localPath.toUtf8().constData(), READONLY,
                       &status)) {
        char msg[FLEN_ERRMSG];
        fits_read_errmsg(msg);
        if (error) *error = QString::fromLatin1(msg);
        return out;
    }

    int numHdus = 0;
    fits_get_num_hdus(fptr, &numHdus, &status);

    auto extname = [&fptr](int hdu) -> QString {
        int st = 0;
        if (fits_movabs_hdu(fptr, hdu, nullptr, &st) != 0) return {};
        char val[FLEN_VALUE] = {0};
        st = 0;
        if (fits_read_key(fptr, TSTRING, "EXTNAME", val, nullptr, &st) != 0)
            return {};
        return QString::fromLatin1(val).trimmed().toUpper();
    };

    auto makeSpectrum = [&](const std::vector<double>& wlVac,
                            const std::vector<double>& flux,
                            const std::vector<double>& err) {
        auto spec = std::make_shared<Spectrum>();
        std::vector<double> wl = wlVac;
        if (opt.vacToAir) SpecFetch::vacToAir(wl);
        spec->setData(wl, flux, err);
        spec->setFile(localPath);
        spec->setInstrument(r.instrumentHint);
        return spec;
    };

    // Per-exposure HDUs of a full spec file: EXTNAME like
    // "B1-00012618-00012621-00012622" (blue and red cameras separately).
    // With the exposures option on these replace the coadd; the coadd is
    // the fallback when the file carries none (lite files, odd products).
    if (opt.fetchExposures) {
        for (int hdu = 2; hdu <= numHdus; ++hdu) {
            const QString name = extname(hdu);
            if (name.isEmpty() || name == QLatin1String("COADD") ||
                name == QLatin1String("SPECOBJ") ||
                name == QLatin1String("SPZLINE") ||
                name == QLatin1String("SPALL"))
                continue;
            // Camera-exposure HDUs start with the camera id (b1/b2/r1/r2).
            if (!(name.startsWith(QLatin1Char('B')) ||
                  name.startsWith(QLatin1Char('R'))) ||
                !name.contains(QLatin1Char('-')))
                continue;

            std::vector<double> wl, flux, err;
            if (!readLoglamTable(fptr, wl, flux, err)) continue;

            double expMjd = r.mjd, expTime = 0.0;
            if (!readExposureEpoch(fptr, expMjd, expTime)) expMjd = r.mjd;

            auto spec = makeSpectrum(wl, flux, err);
            spec->setMJD(expMjd);
            if (expTime > 0) spec->setExposureTime(expTime);

            SpecFetch::ParsedSpectrum ps;
            ps.spectrum       = spec;
            ps.originId       = r.originId + QStringLiteral("#") + name;
            ps.isCoadd        = false;
            ps.instrumentHint = r.instrumentHint;
            out.push_back(std::move(ps));
        }
    }

    // Epoch of the coadd: the exposure-weighted mean of its exposure
    // midpoints.  SkyServer's specObj.mjd (r.mjd) is the *night* the plate was
    // observed, an integer with no time of day - on plate 1619 that is 5.6 h
    // away from the actual midpoint, which is useless for RV work.  The full
    // spec files carry the per-exposure HDUs, so the real epoch is right
    // there; only the lite products fall back on the plate MJD.  Blue cameras
    // alone, because each exposure is filed once per camera and the red ones
    // repeat the same TAI-BEG/EXPTIME.
    double coaddMjd = r.mjd, coaddExpTime = 0.0;
    if (out.empty()) {
        double sumWeighted = 0.0;
        for (int hdu = 2; hdu <= numHdus; ++hdu) {
            const QString name = extname(hdu);
            if (!name.startsWith(QLatin1Char('B')) ||
                !name.contains(QLatin1Char('-')))
                continue;
            double mid = 0.0, e = 0.0;
            if (!readExposureEpoch(fptr, mid, e) || e <= 0) continue;
            sumWeighted += mid * e;
            coaddExpTime += e;
        }
        if (coaddExpTime > 0) coaddMjd = sumWeighted / coaddExpTime;
    }

    // Coadd HDU (EXTNAME "COADD", conventionally the first extension).
    if (out.empty()) {
        for (int hdu = 2; hdu <= numHdus; ++hdu) {
            if (extname(hdu) != QLatin1String("COADD")) continue;
            std::vector<double> wl, flux, err;
            if (readLoglamTable(fptr, wl, flux, err)) {
                auto spec = makeSpectrum(wl, flux, err);
                spec->setMJD(coaddMjd);
                if (coaddExpTime > 0) spec->setExposureTime(coaddExpTime);
                SpecFetch::ParsedSpectrum ps;
                ps.spectrum       = spec;
                ps.originId       = r.originId;
                ps.isCoadd        = true;
                ps.instrumentHint = r.instrumentHint;
                out.push_back(std::move(ps));
            }
            break;
        }
    }

    fits_close_file(fptr, &status);

    if (out.empty() && error && error->isEmpty())
        *error = QStringLiteral("no COADD table found");
    return out;
}
