// src/utils/spectrafetch/ApogeeArchiveClient.cpp

#include "ApogeeArchiveClient.h"

#include "TapHelpers.h"
#include "WavelengthConvert.h"
#include "models/Spectrum.h"
#include "utils/CdsTapClient.h"

#include <QUrl>
#include <QUrlQuery>

#include <fitsio.h>

#include <cmath>

namespace {

// fGetNearbyApogeeStarEq costs roughly 5 s per star (1 star ~8 s, 5 ~29 s,
// 21 ~109 s), so the old chunk of 40 ran ~200 s and blew the timeout below -
// which then cost five retries of a full timeout before failing. 15 keeps a
// chunk near 75 s, comfortably inside the budget. The sibling optical query
// (fGetNearbySpecObjEq) is far cheaper and needs no such limit.
constexpr int kChunkSize    = 15;
constexpr int kSqlTimeoutMs = 120000;
constexpr char kSkyServerUrl[] =
    "https://skyserver.sdss.org/dr17/SkyServerWS/SearchTools/SqlSearch";

double toDoubleOr(const QString& s, double fallback) {
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : fallback;
}

}   // namespace

QList<SpecFetch::RemoteSpectrum> ApogeeArchiveClient::discover(
    const std::vector<SpecFetch::StarQuery>& stars,
    const SpecFetch::ArchiveOptions& opt, QNetworkAccessManager* nam,
    const std::function<void(int, int)>& progress,
    const std::atomic<bool>& cancel, QString* error) {
    QList<SpecFetch::RemoteSpectrum> out;
    if (error) error->clear();

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
                "SELECT p.idx, s.apstar_id, s.apogee_id, s.nvisits, s.snr, "
                "s.telescope, s.field "
                "FROM (VALUES %1) AS p(idx, ra, dec) "
                "CROSS APPLY dbo.fGetNearbyApogeeStarEq(p.ra, p.dec, %2) n "
                "JOIN apogeeStar s ON s.apstar_id = n.apstar_id")
                .arg(values.join(QStringLiteral(", ")))
                .arg(radiusArcmin, 0, 'f', 6);

        // SkyServer's SqlSearch 500s on POSTed form bodies (as of 2026-08);
        // the identical query succeeds as a GET.
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("cmd"), sql);
        q.addQueryItem(QStringLiteral("format"), QStringLiteral("csv"));
        QUrl url{QString::fromLatin1(kSkyServerUrl)};
        url.setQuery(q);

        CdsTap::Request request(kSqlTimeoutMs);
        request.cancel = &cancel;
        const CdsTap::Response resp =
            CdsTap::get(nam, url.toString(QUrl::FullyEncoded), request);
        if (!resp.ok()) {
            if (error) *error = resp.error;
            break;
        }
        if (resp.body.startsWith('{')) {
            if (error) *error = QString::fromUtf8(resp.body.left(300));
            break;
        }

        const SpecFetch::Csv csv = SpecFetch::parseCsv(resp.body);
        for (int i = 0; i < csv.rows.size(); ++i) {
            bool okIdx = false;
            const int idx = csv.value(i, QStringLiteral("idx")).toInt(&okIdx);
            if (!okIdx || idx < 0 || idx >= int(chunk.size())) continue;

            const QString apstarId = csv.value(i, QStringLiteral("apstar_id"));
            const QString apogeeId = csv.value(i, QStringLiteral("apogee_id"));
            const QString telescope =
                csv.value(i, QStringLiteral("telescope"));
            const QString field = csv.value(i, QStringLiteral("field"));
            if (apstarId.isEmpty() || apogeeId.isEmpty() || field.isEmpty())
                continue;

            // South (LCO) products are asStar, everything else apStar.
            const QString prefix =
                telescope.startsWith(QStringLiteral("lco"), Qt::CaseInsensitive)
                    ? QStringLiteral("asStar")
                    : QStringLiteral("apStar");

            SpecFetch::RemoteSpectrum r;
            r.archive        = SpecFetch::Archive::Apogee;
            r.archiveLabel   = QStringLiteral("APOGEE DR17");
            r.originId       = QStringLiteral("apogee-dr17:%1").arg(apstarId);
            r.starId         = chunk[idx].starId;
            r.collection     = telescope;
            r.instrumentHint = QStringLiteral("APOGEE");
            r.snr = toDoubleOr(csv.value(i, QStringLiteral("snr")), NAN);
            r.resolution = 22500.0;
            // apStar files bundle the visits; with the exposures option on,
            // parse() imports those instead of the combined spectrum.
            r.isCoadd    = !opt.fetchExposures;
            r.fileName =
                QStringLiteral("%1-dr17-%2.fits").arg(prefix, apogeeId);
            r.downloadUrl = QUrl(
                QStringLiteral("https://data.sdss.org/sas/dr17/apogee/spectro/"
                               "redux/dr17/stars/%1/%2/%3")
                    .arg(telescope, field, r.fileName));
            out.append(r);
        }

        starsDone += int(chunk.size());
        if (progress) progress(starsDone, int(stars.size()));
    }

    return out;
}

std::vector<SpecFetch::ParsedSpectrum> ApogeeArchiveClient::parse(
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

    // Wavelength grid and visit epochs live in the primary header:
    // wl_i = 10^(CRVAL1 + i*CDELT1) [vacuum Angstrom], JD<n> per visit.
    double crval = NAN, cdelt = NAN;
    int    nvisits = 0;
    std::vector<double> visitMjd;
    {
        int st = 0;
        fits_movabs_hdu(fptr, 1, nullptr, &st);
        st = 0;
        fits_read_key(fptr, TDOUBLE, "CRVAL1", &crval, nullptr, &st);
        st = 0;
        fits_read_key(fptr, TDOUBLE, "CDELT1", &cdelt, nullptr, &st);
        st = 0;
        fits_read_key(fptr, TINT, "NVISITS", &nvisits, nullptr, &st);
        for (int v = 1; v <= std::max(nvisits, 0); ++v) {
            double jd = NAN;
            st        = 0;
            const QByteArray key = QByteArrayLiteral("JD") + QByteArray::number(v);
            if (fits_read_key(fptr, TDOUBLE, key.constData(), &jd, nullptr,
                              &st) == 0 &&
                jd > 0)
                visitMjd.push_back(jd - 2400000.5);
            else
                visitMjd.push_back(NAN);
        }
    }
    if (std::isnan(crval) || std::isnan(cdelt) || cdelt <= 0) {
        if (error) *error = QStringLiteral("no APOGEE wavelength solution");
        fits_close_file(fptr, &status);
        return out;
    }

    // HDU 2 = flux rows, HDU 3 = errors. Row 1 is the pixel-weighted coadd,
    // row 2 the global-weighted one, rows 3.. the individual visits; a
    // single-visit file only has that one row.
    auto readImageRow = [&](int hdu, long row, std::vector<double>& vec,
                            long* npixOut) -> bool {
        int st = 0;
        if (fits_movabs_hdu(fptr, hdu, nullptr, &st) != 0) return false;
        int  naxis    = 0;
        long naxes[4] = {0};
        if (fits_get_img_dim(fptr, &naxis, &st) != 0 || naxis < 1)
            return false;
        if (fits_get_img_size(fptr, 4, naxes, &st) != 0 || naxes[0] < 2)
            return false;
        const long nrows = naxis >= 2 ? naxes[1] : 1;
        if (row > nrows) return false;
        if (npixOut) *npixOut = naxes[0];
        vec.resize(size_t(naxes[0]));
        long first[2] = {1, row};
        int  anynul   = 0;
        return fits_read_pix(fptr, TDOUBLE, first, naxes[0], nullptr,
                             vec.data(), &anynul, &st) == 0;
    };

    long npix   = 0;
    long nrows  = 1;
    {
        int st = 0;
        if (fits_movabs_hdu(fptr, 2, nullptr, &st) == 0) {
            int  naxis    = 0;
            long naxes[4] = {0};
            if (fits_get_img_dim(fptr, &naxis, &st) == 0 &&
                fits_get_img_size(fptr, 4, naxes, &st) == 0) {
                npix  = naxes[0];
                nrows = naxis >= 2 ? std::max<long>(naxes[1], 1) : 1;
            }
        }
    }
    if (npix < 2) {
        if (error) *error = QStringLiteral("no APOGEE flux image");
        fits_close_file(fptr, &status);
        return out;
    }

    std::vector<double> wlGrid(static_cast<size_t>(npix), 0.0);
    for (long i = 0; i < npix; ++i)
        wlGrid[size_t(i)] = std::pow(10.0, crval + double(i) * cdelt);
    if (opt.vacToAir) SpecFetch::vacToAir(wlGrid);

    auto emitRow = [&](long row, const QString& idSuffix, bool isCoadd,
                       double mjd, double meanMjdFallback) {
        std::vector<double> flux, err;
        if (!readImageRow(2, row, flux, nullptr)) return;
        if (!readImageRow(3, row, err, nullptr)) err.assign(flux.size(), 0.0);
        if (flux.size() != wlGrid.size()) return;

        auto spec = std::make_shared<Spectrum>();
        spec->setData(wlGrid, flux, err);
        spec->setFile(localPath);
        spec->setInstrument(r.instrumentHint);
        const double useMjd = !std::isnan(mjd) ? mjd : meanMjdFallback;
        if (!std::isnan(useMjd)) spec->setMJD(useMjd);

        SpecFetch::ParsedSpectrum ps;
        ps.spectrum       = spec;
        ps.originId       = idSuffix.isEmpty() ? r.originId
                                               : r.originId + idSuffix;
        ps.isCoadd        = isCoadd;
        ps.instrumentHint = r.instrumentHint;
        out.push_back(std::move(ps));
    };

    double meanMjd = NAN;
    {
        double sum = 0;
        int    n   = 0;
        for (double m : visitMjd)
            if (!std::isnan(m)) { sum += m; ++n; }
        if (n > 0) meanMjd = sum / n;
    }

    if (nrows >= nvisits + 2 && nvisits >= 2) {
        // Either/or: with the exposures option on, the individual visits
        // replace the combined spectrum; it stays the fallback when no
        // visit row is readable.
        if (opt.fetchExposures) {
            for (int v = 1; v <= nvisits; ++v)
                emitRow(2 + v, QStringLiteral("#visit%1").arg(v), false,
                        v <= int(visitMjd.size()) ? visitMjd[size_t(v - 1)]
                                                  : NAN,
                        meanMjd);
        }
        if (out.empty())
            emitRow(1, QString(), true, meanMjd, NAN);
    } else {
        // Single-visit file: the one row is the spectrum.
        emitRow(1, QString(), true,
                !visitMjd.empty() ? visitMjd.front() : meanMjd, NAN);
    }

    fits_close_file(fptr, &status);

    if (out.empty() && error && error->isEmpty())
        *error = QStringLiteral("no readable APOGEE rows");
    return out;
}
