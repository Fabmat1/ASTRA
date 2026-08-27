// src/utils/spectrafetch/SdssOpticalArchiveClient.cpp

#include "SdssOpticalArchiveClient.h"

#include "TapHelpers.h"
#include "WavelengthConvert.h"
#include "models/Spectrum.h"
#include "utils/CdsTapClient.h"
#include "utils/Logger.h"

#include <QUrl>
#include <QUrlQuery>

#include <fitsio.h>

#include <cmath>

namespace {

// VALUES rows per SkyServer query; ~90 chars each keeps the statement far
// below the request limits while still batching usefully.
constexpr int kChunkSize    = 40;
constexpr int kSqlTimeoutMs = 120000;

QString skyServerUrl(const QString& dr) {
    return QStringLiteral("https://skyserver.sdss.org/%1/SkyServerWS/"
                          "SearchTools/SqlSearch")
        .arg(dr.toLower());
}

double toDoubleOr(const QString& s, double fallback) {
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : fallback;
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
    const double radiusArcmin = opt.radiusArcsec / 60.0;

    const auto chunks = SpecFetch::chunked(stars, size_t(kChunkSize));
    int starsDone = 0;

    for (const auto& chunk : chunks) {
        if (cancel.load()) break;

        QStringList values;
        for (int k = 0; k < int(chunk.size()); ++k)
            values << QStringLiteral("(%1, %2, %3)")
                          .arg(k)
                          .arg(chunk[k].ra, 0, 'f', 8)
                          .arg(chunk[k].dec, 0, 'f', 8);

        const QString sql =
            QStringLiteral(
                "SELECT p.idx, s.specObjID, s.plate, s.mjd, s.fiberID, "
                "s.run2d, s.survey, s.instrument, s.snMedian "
                "FROM (VALUES %1) AS p(idx, ra, dec) "
                "CROSS APPLY dbo.fGetNearbySpecObjEq(p.ra, p.dec, %2) n "
                "JOIN specObjAll s ON s.specObjID = n.specObjID")
                .arg(values.join(QStringLiteral(", ")))
                .arg(radiusArcmin, 0, 'f', 6);

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
        for (int i = 0; i < csv.rows.size(); ++i) {
            bool okIdx = false;
            const int idx = csv.value(i, QStringLiteral("idx")).toInt(&okIdx);
            if (!okIdx || idx < 0 || idx >= int(chunk.size())) continue;

            const QString specObjId =
                csv.value(i, QStringLiteral("specobjid"));
            const int plate = csv.value(i, QStringLiteral("plate")).toInt();
            const int mjd   = csv.value(i, QStringLiteral("mjd")).toInt();
            const int fiber = csv.value(i, QStringLiteral("fiberid")).toInt();
            const QString run2d = csv.value(i, QStringLiteral("run2d"));
            if (specObjId.isEmpty() || plate <= 0 || mjd <= 0 || fiber <= 0)
                continue;

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

            double expMjd = r.mjd, expTime = NAN;
            {
                int st = 0;
                double tai = 0;
                if (fits_read_key(fptr, TDOUBLE, "TAI-BEG", &tai, nullptr,
                                  &st) == 0 &&
                    tai > 0)
                    expMjd = tai / 86400.0;
                st = 0;
                double e = 0;
                if (fits_read_key(fptr, TDOUBLE, "EXPTIME", &e, nullptr, &st) ==
                        0 &&
                    e > 0)
                    expTime = e;
            }

            auto spec = makeSpectrum(wl, flux, err);
            spec->setMJD(expMjd);
            if (!std::isnan(expTime)) spec->setExposureTime(expTime);

            SpecFetch::ParsedSpectrum ps;
            ps.spectrum       = spec;
            ps.originId       = r.originId + QStringLiteral("#") + name;
            ps.isCoadd        = false;
            ps.instrumentHint = r.instrumentHint;
            out.push_back(std::move(ps));
        }
    }

    // Coadd HDU (EXTNAME "COADD", conventionally the first extension).
    if (out.empty()) {
        for (int hdu = 2; hdu <= numHdus; ++hdu) {
            if (extname(hdu) != QLatin1String("COADD")) continue;
            std::vector<double> wl, flux, err;
            if (readLoglamTable(fptr, wl, flux, err)) {
                auto spec = makeSpectrum(wl, flux, err);
                spec->setMJD(r.mjd);
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
