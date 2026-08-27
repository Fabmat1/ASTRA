// src/utils/spectrafetch/LamostArchiveClient.cpp

#include "LamostArchiveClient.h"

#include "VoTableReader.h"
#include "WavelengthConvert.h"
#include "models/Spectrum.h"
#include "utils/CdsTapClient.h"
#include "utils/Logger.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTemporaryFile>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>

#include <fitsio.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace {

constexpr int kConeTimeoutMs = 60000;
constexpr int kHeadTimeoutMs = 20000;

struct DrEndpoints {
    const char* key;        // "DR11"
    const char* lrsCone;    // %1 ra, %2 dec, %3 sr [deg]
    const char* lrsFits;    // %1 obsid
    const char* mrsCone;    // may be null (MRS starts with DR7)
    const char* mrsFits;
};

// dr4..dr7 run their own hosts; DR8+ live under www.lamost.org. All of them
// answer the conesearch and fits endpoints anonymously (verified Aug 2026);
// a token, when configured, is still appended to every request.
const DrEndpoints kEndpoints[] = {
    {"DR11",
     "https://www.lamost.org/dr11/v2.0/voservice/conesearch?ra=%1&dec=%2&sr=%3",
     "https://www.lamost.org/dr11/v2.0/spectrum/fits/%1",
     "https://www.lamost.org/dr11/v2.0/medvoservice/conesearch?ra=%1&dec=%2&sr=%3",
     "https://www.lamost.org/dr11/v2.0/medspectrum/fits/%1"},
    {"DR10",
     "https://www.lamost.org/dr10/v2.0/voservice/conesearch?ra=%1&dec=%2&sr=%3",
     "https://www.lamost.org/dr10/v2.0/spectrum/fits/%1",
     "https://www.lamost.org/dr10/v2.0/medvoservice/conesearch?ra=%1&dec=%2&sr=%3",
     "https://www.lamost.org/dr10/v2.0/medspectrum/fits/%1"},
    {"DR9",
     "https://www.lamost.org/dr9/v2.0/voservice/conesearch?ra=%1&dec=%2&sr=%3",
     "https://www.lamost.org/dr9/v2.0/spectrum/fits/%1",
     "https://www.lamost.org/dr9/v2.0/medvoservice/conesearch?ra=%1&dec=%2&sr=%3",
     "https://www.lamost.org/dr9/v2.0/medspectrum/fits/%1"},
    {"DR8",
     "https://www.lamost.org/dr8/v2.0/voservice/conesearch?ra=%1&dec=%2&sr=%3",
     "https://www.lamost.org/dr8/v2.0/spectrum/fits/%1",
     "https://www.lamost.org/dr8/v2.0/medvoservice/conesearch?ra=%1&dec=%2&sr=%3",
     "https://www.lamost.org/dr8/v2.0/medspectrum/fits/%1"},
    {"DR7", "http://dr7.lamost.org/voservice/conesearch?ra=%1&dec=%2&sr=%3",
     "http://dr7.lamost.org/spectrum/fits/%1",
     "http://dr7.lamost.org/medvoservice/conesearch?ra=%1&dec=%2&sr=%3",
     "http://dr7.lamost.org/medspectrum/fits/%1"},
    {"DR6", "http://dr6.lamost.org/voservice/conesearch?ra=%1&dec=%2&sr=%3",
     "http://dr6.lamost.org/spectrum/fits/%1", nullptr, nullptr},
    {"DR5", "http://dr5.lamost.org/voservice/conesearch?ra=%1&dec=%2&sr=%3",
     "http://dr5.lamost.org/spectrum/fits/%1", nullptr, nullptr},
    {"DR4", "http://dr4.lamost.org/voservice/conesearch?ra=%1&dec=%2&sr=%3",
     "http://dr4.lamost.org/spectrum/fits/%1", nullptr, nullptr},
};

// LRS single-exposure release (Bai et al. 2021): a fixed dataset covering the
// observations of Oct 2011 - Jun 2017, keyed by UT date / plan / spectrograph
// / fiber rather than by obsid or data release.
constexpr char   kSexpFitsBase[] =
    "https://casdc.china-vo.org/lamost/vac/sedr5/fits/";
constexpr double kSexpMjdMin = 55800.0;
constexpr double kSexpMjdMax = 57950.0;
constexpr char   kSexpOriginPrefix[] = "lamost-sexp-lrs:";

const DrEndpoints* endpointsFor(const QString& dr) {
    for (const DrEndpoints& e : kEndpoints)
        if (dr.compare(QLatin1String(e.key), Qt::CaseInsensitive) == 0)
            return &e;
    return nullptr;
}

// Cone-search columns are prefixed on DR4-DR9 ("catalogue_obsid",
// "med_catalogue_obsid") and unprefixed on DR10+ ("obsid"): accept an exact
// name first, then the "_name" suffix.
int columnFor(const VoTable::Table& t, const QString& name) {
    for (int i = 0; i < t.fields.size(); ++i)
        if (t.fields.at(i).name.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    const QString suffix = QLatin1Char('_') + name;
    for (int i = 0; i < t.fields.size(); ++i)
        if (t.fields.at(i).name.endsWith(suffix, Qt::CaseInsensitive))
            return i;
    return -1;
}

double toDoubleOr(const QString& s, double fallback) {
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : fallback;
}

double mjdFromDateObs(fitsfile* fptr) {
    char val[FLEN_VALUE] = {0};
    int  st              = 0;
    if (fits_read_key(fptr, TSTRING, "DATE-OBS", val, nullptr, &st) != 0)
        return NAN;
    const QString raw = QString::fromLatin1(val).trimmed();

    QDateTime dt = QDateTime::fromString(raw, Qt::ISODate);
    if (!dt.isValid()) {
        // LAMOST also writes single-digit time fields ("...T11:30:0.000"),
        // which strict ISO parsing rejects: take the pieces apart instead.
        const QStringList parts = raw.split(QLatin1Char('T'));
        if (parts.size() == 2) {
            const QDate d =
                QDate::fromString(parts[0], QStringLiteral("yyyy-MM-dd"));
            const QStringList hms = parts[1].split(QLatin1Char(':'));
            if (d.isValid() && hms.size() == 3) {
                const int    h = hms[0].toInt();
                const int    m = hms[1].toInt();
                const double s = hms[2].toDouble();
                dt = QDateTime(d, QTime(h, m, 0), QTimeZone::UTC);
                dt = dt.addMSecs(qint64(s * 1000.0));
            }
        }
    }
    if (!dt.isValid()) return NAN;
    QDateTime utc = dt;
    utc.setTimeZone(QTimeZone::UTC);
    return utc.toMSecsSinceEpoch() / 86400000.0 + 40587.0;
}

// Synchronous existence probe; fills *size from Content-Length when present.
bool headOk(QNetworkAccessManager* nam, const QString& url, qint64* size) {
    QNetworkRequest req{QUrl(url)};
    req.setRawHeader("User-Agent", "ASTRA/1.0");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    // The NADC nginx stalls Qt's HTTP/2 negotiation on a fresh connection
    // (plain HTTP/1.1 answers instantly).
    req.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    QNetworkReply* reply = nam->head(req);

    QEventLoop loop;
    QTimer     timer;
    timer.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(kHeadTimeoutMs);
    loop.exec();

    if (reply->isRunning()) reply->abort();
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok = reply->error() == QNetworkReply::NoError && status == 200;
    if (ok && size) {
        bool haveLen = false;
        const qint64 len =
            reply->header(QNetworkRequest::ContentLengthHeader)
                .toLongLong(&haveLen);
        if (haveLen && len > 0) *size = len;
    }
    reply->deleteLater();
    return ok;
}

QString sexpFileUrl(const QString& obsDate, const QString& planId, int spId,
                    int fiberId) {
    const QString date8 = QString(obsDate).remove(QLatin1Char('-'));
    if (date8.size() != 8 || planId.isEmpty() || spId <= 0 || fiberId <= 0)
        return QString();
    return QString::fromLatin1(kSexpFitsBase) + date8 + QLatin1Char('/') +
           planId + QLatin1Char('/') +
           QStringLiteral("%1_%2_%3_%4.fit")
               .arg(date8, planId,
                    QString::number(spId).rightJustified(2, QLatin1Char('0')),
                    QString::number(fiberId).rightJustified(3,
                                                            QLatin1Char('0')));
}

}   // namespace

QStringList LamostArchiveClient::knownDataReleases(bool mrs) {
    QStringList out;
    for (const DrEndpoints& e : kEndpoints)
        if (!mrs || e.mrsCone)
            out << QString::fromLatin1(e.key);
    return out;
}

QString LamostArchiveClient::defaultDataRelease() {
    return QString::fromLatin1(kEndpoints[0].key);
}

QList<SpecFetch::RemoteSpectrum> LamostArchiveClient::discover(
    const std::vector<SpecFetch::StarQuery>& stars,
    const SpecFetch::ArchiveOptions& opt, QNetworkAccessManager* nam,
    const std::function<void(int, int)>& progress,
    const std::atomic<bool>& cancel, QString* error) {
    QList<SpecFetch::RemoteSpectrum> out;
    if (error) error->clear();

    const QString drName =
        opt.dataRelease.isEmpty() ? defaultDataRelease() : opt.dataRelease;
    const DrEndpoints* ep = endpointsFor(drName);
    if (!ep || (_mrs && !ep->mrsCone)) {
        if (error)
            *error = QStringLiteral("%1 is not available for %2")
                         .arg(drName, displayName());
        return out;
    }

    const QString coneFmt =
        QString::fromLatin1(_mrs ? ep->mrsCone : ep->lrsCone);
    const QString fitsFmt =
        QString::fromLatin1(_mrs ? ep->mrsFits : ep->lrsFits);
    const QString drKey = drName.toLower();
    const double  srDeg = opt.radiusArcsec / 3600.0;

    // No batch API: one cone search per star. The service's per-host throttle
    // does not apply to discovery, so keep the pace gentle here.
    int starsDone = 0;
    for (const auto& q : stars) {
        if (cancel.load()) break;

        QString url = coneFmt.arg(q.ra, 0, 'f', 8)
                          .arg(q.dec, 0, 'f', 8)
                          .arg(srDeg, 0, 'f', 8);
        if (!opt.token.isEmpty())
            url += QStringLiteral("&token=%1").arg(opt.token);

        CdsTap::Request request(kConeTimeoutMs);
        request.cancel = &cancel;
        const CdsTap::Response resp = CdsTap::get(nam, url, request);
        ++starsDone;
        if (progress) progress(starsDone, int(stars.size()));

        if (!resp.ok()) {
            if (error && error->isEmpty()) *error = resp.error;
            continue;   // one unreachable star should not kill the sweep
        }

        const VoTable::Document doc = VoTable::parse(resp.body);
        const VoTable::Table*   t   = doc.firstTable();
        if (!t || t->rows.isEmpty()) continue;

        const int obsidCol   = columnFor(*t, QStringLiteral("obsid"));
        const int mjdCol     = columnFor(*t, QStringLiteral("mjd"));
        const int snrgCol    = columnFor(*t, QStringLiteral("snrg"));
        const int obsdateCol = columnFor(*t, QStringLiteral("obsdate"));
        const int planidCol  = columnFor(*t, QStringLiteral("planid"));
        const int spidCol    = columnFor(*t, QStringLiteral("spid"));
        const int fiberidCol = columnFor(*t, QStringLiteral("fiberid"));
        if (obsidCol < 0) continue;

        // MRS cone searches return one row per band and exposure of the same
        // obsid; collapse to one product per obsid.
        QSet<QString> seen;
        for (int i = 0; i < t->rows.size(); ++i) {
            const QString obsid = t->value(i, obsidCol);
            if (obsid.isEmpty() || seen.contains(obsid)) continue;
            seen.insert(obsid);

            SpecFetch::RemoteSpectrum r;
            r.archive      = archive();
            r.archiveLabel = QStringLiteral("LAMOST %1 %2")
                                 .arg(drName.toUpper(),
                                      _mrs ? QStringLiteral("MRS")
                                           : QStringLiteral("LRS"));
            // The obsid is stable across data releases (the DR only picks the
            // server), so the dedup key must not embed it.
            r.originId = QStringLiteral("lamost-%1:%2")
                             .arg(_mrs ? QStringLiteral("mrs")
                                       : QStringLiteral("lrs"),
                                  obsid);
            r.starId         = q.starId;
            r.instrumentHint = _mrs ? QStringLiteral("LAMOST MRS")
                                    : QStringLiteral("LAMOST LRS");
            r.collection     = r.archiveLabel;
            if (mjdCol >= 0)
                r.mjd = toDoubleOr(t->value(i, mjdCol), NAN);
            if (snrgCol >= 0)
                r.snr = toDoubleOr(t->value(i, snrgCol), NAN);
            r.resolution = _mrs ? 7500.0 : 1800.0;
            r.isCoadd    = true;

            QString dl = fitsFmt.arg(obsid);
            if (!opt.token.isEmpty())
                dl += QStringLiteral("?token=%1").arg(opt.token);
            r.downloadUrl = QUrl(dl);
            r.fileName    = QStringLiteral("lamost-%1-%2-%3.fits.gz")
                             .arg(drKey,
                                  _mrs ? QStringLiteral("mrs")
                                       : QStringLiteral("lrs"),
                                  obsid);

            // MRS products bundle their exposures; with the exposures option
            // on, parse() imports those instead of the coadd arms.
            if (_mrs) r.isCoadd = !opt.fetchExposures;

            // Either/or for LRS: the individual exposures live in the
            // separate single-exposure release, and when the option is on
            // and that release has the product, its row replaces the coadd.
            const bool inSexpWindow =
                std::isnan(r.mjd) ||
                (r.mjd >= kSexpMjdMin && r.mjd <= kSexpMjdMax);
            if (!_mrs && opt.fetchExposures && inSexpWindow) {
                const QString sexpUrl = sexpFileUrl(
                    obsdateCol >= 0 ? t->value(i, obsdateCol) : QString(),
                    planidCol >= 0 ? t->value(i, planidCol) : QString(),
                    spidCol >= 0 ? int(toDoubleOr(t->value(i, spidCol), -1))
                                 : -1,
                    fiberidCol >= 0
                        ? int(toDoubleOr(t->value(i, fiberidCol), -1))
                        : -1);
                qint64 size = -1;
                if (!sexpUrl.isEmpty() && headOk(nam, sexpUrl, &size)) {
                    SpecFetch::RemoteSpectrum rx = r;
                    rx.originId =
                        QString::fromLatin1(kSexpOriginPrefix) + obsid;
                    rx.collection = QStringLiteral(
                        "LAMOST LRS single exposures (DR5 sexp)");
                    rx.isCoadd     = false;
                    rx.sizeBytes   = size;
                    rx.downloadUrl = QUrl(sexpUrl);
                    rx.fileName =
                        QStringLiteral("lamost-sexp-%1.fit").arg(obsid);
                    // parse() re-anchors each exposure to the coadd's flux
                    // calibration: point it at the sibling coadd download
                    // (and its URL, for when the coadd was never fetched).
                    rx.extras.insert(QStringLiteral("coaddFileName"),
                                     r.fileName);
                    rx.extras.insert(QStringLiteral("coaddUrl"),
                                     r.downloadUrl.toString());
                    out.append(rx);
                    continue;   // the exposures replace the coadd row
                }
            }

            out.append(r);
        }
    }

    return out;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Parsing
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// LRS DR7 and older: primary image, NAXIS2 rows = flux, ivar, wavelength,
// andmask, ormask.
bool parseLrsImage(fitsfile* fptr, std::vector<double>& wl,
                   std::vector<double>& flux, std::vector<double>& err) {
    int  status = 0;
    int  naxis  = 0;
    long naxes[4] = {0};
    if (fits_movabs_hdu(fptr, 1, nullptr, &status) != 0) return false;
    if (fits_get_img_dim(fptr, &naxis, &status) != 0 || naxis != 2)
        return false;
    if (fits_get_img_size(fptr, 4, naxes, &status) != 0) return false;
    const long npix = naxes[0], nrows = naxes[1];
    if (npix < 2 || nrows < 3) return false;

    auto readRow = [&](long row, std::vector<double>& outVec) -> bool {
        outVec.resize(size_t(npix));
        long first[2] = {1, row};
        int  anynul   = 0;
        int  st       = 0;
        return fits_read_pix(fptr, TDOUBLE, first, npix, nullptr,
                             outVec.data(), &anynul, &st) == 0;
    };

    std::vector<double> ivar;
    if (!readRow(1, flux) || !readRow(2, ivar) || !readRow(3, wl))
        return false;

    err.assign(wl.size(), 0.0);
    for (size_t i = 0; i < ivar.size() && i < err.size(); ++i)
        if (ivar[i] > 0) err[i] = 1.0 / std::sqrt(ivar[i]);
    return true;
}

// Read one vector cell (a whole-array column of a single-row bintable).
bool readVectorCell(fitsfile* fptr, const char* name,
                    std::vector<double>& outVec) {
    int col = 0, st = 0;
    if (fits_get_colnum(fptr, CASEINSEN, const_cast<char*>(name), &col, &st) !=
        0)
        return false;
    int  typecode = 0;
    long repeat = 0, width = 0;
    st = 0;
    if (fits_get_coltype(fptr, col, &typecode, &repeat, &width, &st) != 0 ||
        repeat < 2)
        return false;
    outVec.resize(size_t(repeat));
    int anynul = 0;
    st         = 0;
    return fits_read_col(fptr, TDOUBLE, col, 1, 1, repeat, nullptr,
                         outVec.data(), &anynul, &st) == 0;
}

// LRS DR8+: a COADD bintable of vector columns FLUX / IVAR / WAVELENGTH
// (linear Angstroms), one row.
bool parseLrsCoaddTable(fitsfile* fptr, std::vector<double>& wl,
                        std::vector<double>& flux, std::vector<double>& err) {
    int numHdus = 0, status = 0;
    fits_get_num_hdus(fptr, &numHdus, &status);

    for (int hdu = 2; hdu <= numHdus; ++hdu) {
        int hduType = ANY_HDU;
        status      = 0;
        if (fits_movabs_hdu(fptr, hdu, &hduType, &status) != 0 ||
            hduType != BINARY_TBL)
            continue;
        long nrows = 0;
        if (fits_get_num_rows(fptr, &nrows, &status) != 0 || nrows != 1)
            continue;

        std::vector<double> ivar;
        if (!readVectorCell(fptr, "FLUX", flux)) continue;
        if (!readVectorCell(fptr, "WAVELENGTH", wl) &&
            !readVectorCell(fptr, "LOGLAM", wl))
            continue;
        if (wl.size() != flux.size()) continue;
        // A LOGLAM grid announces itself by its magnitude.
        if (!wl.empty() && wl.front() < 100.0)
            for (double& w : wl) w = std::pow(10.0, w);

        err.assign(wl.size(), 0.0);
        if (readVectorCell(fptr, "IVAR", ivar) && ivar.size() == wl.size())
            for (size_t i = 0; i < ivar.size(); ++i)
                if (ivar[i] > 0) err[i] = 1.0 / std::sqrt(ivar[i]);
        return true;
    }
    return false;
}

bool readLrsCoadd(fitsfile* fptr, std::vector<double>& wl,
                  std::vector<double>& flux, std::vector<double>& err) {
    if (parseLrsImage(fptr, wl, flux, err)) return true;
    wl.clear(); flux.clear(); err.clear();
    return parseLrsCoaddTable(fptr, wl, flux, err);
}

// MRS bintable HDU (COADD_B/COADD_R or a single exposure). Two layouts are
// in the wild: one row per pixel with scalar FLUX/IVAR/LOGLAM columns (the
// DR7-era files) and a single row of vector FLUX/IVAR/WAVELENGTH cells
// (DR10/DR11).
bool readMrsTable(fitsfile* fptr, std::vector<double>& wl,
                  std::vector<double>& flux, std::vector<double>& err) {
    int  status = 0;
    long nrows  = 0;
    if (fits_get_num_rows(fptr, &nrows, &status) != 0 || nrows < 1)
        return false;

    std::vector<double> ivar;
    if (nrows == 1) {
        if (!readVectorCell(fptr, "FLUX", flux)) return false;
        if (!readVectorCell(fptr, "WAVELENGTH", wl) &&
            !readVectorCell(fptr, "LOGLAM", wl))
            return false;
        if (wl.size() != flux.size() || wl.size() < 2) return false;
        readVectorCell(fptr, "IVAR", ivar);
    } else {
        auto readCol = [&](const char* name,
                           std::vector<double>& outVec) -> bool {
            int col = 0, st = 0;
            if (fits_get_colnum(fptr, CASEINSEN, const_cast<char*>(name), &col,
                                &st) != 0)
                return false;
            outVec.resize(size_t(nrows));
            int anynul = 0;
            return fits_read_col(fptr, TDOUBLE, col, 1, 1, nrows, nullptr,
                                 outVec.data(), &anynul, &st) == 0;
        };
        if (!readCol("FLUX", flux)) return false;
        if (!readCol("WAVELENGTH", wl) && !readCol("LOGLAM", wl))
            return false;
        readCol("IVAR", ivar);
    }

    // A log grid announces itself by its magnitude.
    if (!wl.empty() && wl.front() < 100.0)
        for (double& w : wl) w = std::pow(10.0, w);

    err.assign(wl.size(), 0.0);
    if (ivar.size() == wl.size())
        for (size_t i = 0; i < ivar.size(); ++i)
            if (ivar[i] > 0) err[i] = 1.0 / std::sqrt(ivar[i]);
    return true;
}

double interpAt(const std::vector<double>& x, const std::vector<double>& y,
                double xi) {
    if (x.empty()) return NAN;
    if (xi <= x.front()) return y.front();
    if (xi >= x.back()) return y.back();
    const auto it = std::upper_bound(x.begin(), x.end(), xi);
    const size_t hi = size_t(it - x.begin());
    const size_t lo = hi - 1;
    const double t  = (xi - x[lo]) / (x[hi] - x[lo]);
    return y[lo] + t * (y[hi] - y[lo]);
}

// Transfer the coadd's flux calibration onto one single-exposure arm as a
// smooth multiplicative correction. The exposure and the coadd are the same
// star, so every stellar feature (Balmer wings, rotation profiles) cancels in
// their ratio and only the instrumental throughput difference survives; the
// wide median windows additionally reject the narrow residuals that
// epoch-to-epoch RV shifts leave at line cores. Fit-relevant spectral shapes
// are therefore preserved - the exposures simply inherit the continuum of the
// coadd they came from. Returns per-pixel factors; empty means "no anchor".
std::vector<double> smoothRatioToCoadd(const std::vector<double>& wl,
                                       const std::vector<double>& flux,
                                       const std::vector<double>& coaddWl,
                                       const std::vector<double>& coaddFlux) {
    constexpr double kWindow  = 150.0;   // Angstrom
    constexpr double kStep    = 75.0;
    constexpr int    kMinPix  = 30;

    if (wl.size() < 2 || coaddWl.size() < 2) return {};

    std::vector<double> nodeX, nodeY, ratios;
    for (double a = wl.front(); a < wl.back(); a += kStep) {
        ratios.clear();
        double xSum = 0.0;
        for (size_t i = 0; i < wl.size(); ++i) {
            if (wl[i] < a || wl[i] >= a + kWindow) continue;
            if (!(flux[i] > 0.0)) continue;
            const double c = interpAt(coaddWl, coaddFlux, wl[i]);
            if (!(c > 0.0)) continue;
            ratios.push_back(c / flux[i]);
            xSum += wl[i];
        }
        if (int(ratios.size()) < kMinPix) continue;
        std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2,
                         ratios.end());
        nodeX.push_back(xSum / double(ratios.size()));
        nodeY.push_back(ratios[ratios.size() / 2]);
    }
    if (nodeX.empty()) return {};

    std::vector<double> corr(wl.size());
    for (size_t i = 0; i < wl.size(); ++i)
        corr[i] = interpAt(nodeX, nodeY, wl[i]);
    return corr;
}

struct SexpArrays {
    long npix  = 0;
    long nspec = 0;
    std::vector<double>  flux, ivar, loglam, fluxcorr;   // npix*nspec each
    std::vector<double>  exptime;                        // nspec
    std::vector<QString> mjm, color;                     // nspec
};

bool readSexpArrays(fitsfile* fptr, SexpArrays& a, QString* error) {
    int numHdus = 0, status = 0;
    fits_get_num_hdus(fptr, &numHdus, &status);

    for (int hdu = 2; hdu <= numHdus; ++hdu) {
        int hduType = ANY_HDU;
        status      = 0;
        if (fits_movabs_hdu(fptr, hdu, &hduType, &status) != 0 ||
            hduType != BINARY_TBL)
            continue;

        int fluxCol = 0, st = 0;
        if (fits_get_colnum(fptr, CASEINSEN, const_cast<char*>("FLUX"),
                            &fluxCol, &st) != 0)
            continue;

        // The exposure count and pixel count come from the FLUX cell's TDIM
        // (npix fastest axis); fall back to repeat = npix when TDIM is absent.
        int  naxis    = 0;
        long naxes[2] = {0, 0};
        st            = 0;
        if (fits_read_tdim(fptr, fluxCol, 2, &naxis, naxes, &st) != 0)
            continue;
        if (naxis == 2) {
            a.npix  = naxes[0];
            a.nspec = naxes[1];
        } else {
            a.npix  = naxes[0];
            a.nspec = 1;
        }
        if (a.npix < 2 || a.nspec < 1) continue;
        const long total = a.npix * a.nspec;

        auto readFlat = [&](const char* name, std::vector<double>& outVec,
                            bool required) -> bool {
            int col = 0, s2 = 0;
            if (fits_get_colnum(fptr, CASEINSEN, const_cast<char*>(name), &col,
                                &s2) != 0) {
                outVec.clear();
                return !required;
            }
            outVec.resize(size_t(total));
            int anynul = 0;
            s2         = 0;
            if (fits_read_col(fptr, TDOUBLE, col, 1, 1, total, nullptr,
                              outVec.data(), &anynul, &s2) != 0) {
                outVec.clear();
                return !required;
            }
            return true;
        };
        auto readStrings = [&](const char* name,
                               std::vector<QString>& outVec) {
            outVec.clear();
            int col = 0, s2 = 0;
            if (fits_get_colnum(fptr, CASEINSEN, const_cast<char*>(name), &col,
                                &s2) != 0)
                return;
            for (long k = 1; k <= a.nspec; ++k) {
                char  buf[72] = {0};
                char* ptr     = buf;
                int   anynul  = 0;
                s2            = 0;
                // One table row; the exposures are the elements of the cell.
                if (fits_read_col_str(fptr, col, 1, k, 1, nullptr, &ptr,
                                      &anynul, &s2) == 0)
                    outVec.push_back(QString::fromLatin1(buf).trimmed());
                else
                    outVec.push_back(QString());
            }
        };

        if (!readFlat("FLUX", a.flux, true) ||
            !readFlat("INVVAR", a.ivar, true) ||
            !readFlat("LOGLAM", a.loglam, true))
            continue;
        readFlat("FLUXCORR", a.fluxcorr, false);

        std::vector<double> expt;
        {
            int col = 0, s2 = 0;
            if (fits_get_colnum(fptr, CASEINSEN, const_cast<char*>("EXPTIME"),
                                &col, &s2) == 0) {
                expt.resize(size_t(a.nspec));
                int anynul = 0;
                s2         = 0;
                if (fits_read_col(fptr, TDOUBLE, col, 1, 1, a.nspec, nullptr,
                                  expt.data(), &anynul, &s2) != 0)
                    expt.clear();
            }
        }
        a.exptime = std::move(expt);
        readStrings("MJM", a.mjm);
        readStrings("COLOR", a.color);
        return true;
    }

    if (error && error->isEmpty())
        *error = QStringLiteral("no single-exposure table found");
    return false;
}

// Load the parent coadd's (vacuum) wavelength/flux arrays for anchoring:
// first from the sibling download, else straight from the archive.
bool loadCoaddForAnchor(const QString& sexpLocalPath,
                        const SpecFetch::RemoteSpectrum& r,
                        std::vector<double>& wl, std::vector<double>& flux) {
    std::vector<double> err;

    const QString coaddName =
        r.extras.value(QStringLiteral("coaddFileName")).toString();
    if (!coaddName.isEmpty()) {
        const QString sibling =
            QFileInfo(sexpLocalPath).dir().filePath(coaddName);
        if (QFileInfo::exists(sibling)) {
            fitsfile* fptr  = nullptr;
            int       status = 0;
            if (fits_open_file(&fptr, sibling.toUtf8().constData(), READONLY,
                               &status) == 0) {
                const bool ok = readLrsCoadd(fptr, wl, flux, err);
                fits_close_file(fptr, &status);
                if (ok) return true;
                wl.clear(); flux.clear();
            }
        }
    }

    const QString coaddUrl =
        r.extras.value(QStringLiteral("coaddUrl")).toString();
    if (coaddUrl.isEmpty()) return false;

    QNetworkAccessManager  nam;
    const CdsTap::Response resp = CdsTap::get(&nam, coaddUrl, kConeTimeoutMs);
    if (!resp.ok() || resp.body.isEmpty()) return false;

    QTemporaryFile tmp(QDir::tempPath() +
                       QStringLiteral("/astra-lamost-XXXXXX.fits.gz"));
    if (!tmp.open()) return false;
    tmp.write(resp.body);
    tmp.flush();

    fitsfile* fptr  = nullptr;
    int       status = 0;
    if (fits_open_file(&fptr, tmp.fileName().toUtf8().constData(), READONLY,
                       &status) != 0)
        return false;
    const bool ok = readLrsCoadd(fptr, wl, flux, err);
    fits_close_file(fptr, &status);
    return ok;
}

}   // namespace

// LRS single-exposure product: one bintable row whose FLUX / INVVAR / LOGLAM
// cells are (npix x 2*NEXP) arrays - the blue and red arm of every exposure.
// Each exposure's arms are put on the coadd's flux system (FLUXCORR first,
// then the smooth coadd ratio) and merged into one spectrum per epoch.
static std::vector<SpecFetch::ParsedSpectrum> parseSexp(
    fitsfile* fptr, const QString& localPath,
    const SpecFetch::RemoteSpectrum& r, const SpecFetch::ArchiveOptions& opt,
    QString* error) {
    std::vector<SpecFetch::ParsedSpectrum> out;

    SexpArrays a;
    if (!readSexpArrays(fptr, a, error)) return out;

    std::vector<double> coaddWl, coaddFlux;
    if (!loadCoaddForAnchor(localPath, r, coaddWl, coaddFlux))
        LOG_WARNING("SpecFetch",
                    QStringLiteral("%1: coadd unavailable, importing single "
                                   "exposures with FLUXCORR calibration only")
                        .arg(r.originId));

    // Group the arm spectra by their epoch (MJM, the modified julian minute),
    // preserving file order so blue comes before red within one exposure.
    std::vector<QString>            epochOrder;
    std::map<QString, std::vector<long>> byEpoch;
    for (long k = 0; k < a.nspec; ++k) {
        const QString key = k < long(a.mjm.size()) && !a.mjm[k].isEmpty()
                                ? a.mjm[k]
                                : QStringLiteral("#%1").arg(k);
        if (byEpoch.find(key) == byEpoch.end()) epochOrder.push_back(key);
        byEpoch[key].push_back(k);
    }

    for (const QString& epoch : epochOrder) {
        std::vector<double> wl, flux, err;

        for (long k : byEpoch[epoch]) {
            std::vector<double> aw, af, ae;
            aw.reserve(size_t(a.npix));
            af.reserve(size_t(a.npix));
            ae.reserve(size_t(a.npix));
            for (long i = 0; i < a.npix; ++i) {
                const size_t idx = size_t(k * a.npix + i);
                const double iv  = a.ivar[idx];
                double       f   = a.flux[idx];
                if (!(iv > 0.0) || !std::isfinite(f)) continue;
                double scale = 1.0;
                if (!a.fluxcorr.empty() && a.fluxcorr[idx] > 0.0)
                    scale = 1.0 / a.fluxcorr[idx];
                aw.push_back(std::pow(10.0, a.loglam[idx]));
                af.push_back(f * scale);
                ae.push_back(scale / std::sqrt(iv));
            }
            if (aw.size() < 50) continue;

            const std::vector<double> corr =
                smoothRatioToCoadd(aw, af, coaddWl, coaddFlux);
            for (size_t i = 0; i < aw.size(); ++i) {
                const double c = corr.empty() ? 1.0 : corr[i];
                wl.push_back(aw[i]);
                flux.push_back(af[i] * c);
                err.push_back(ae[i] * c);
            }
        }
        if (wl.size() < 100) continue;

        // The arms overlap around 5700-5900 A on the same log grid: order the
        // merged points and combine the arms' duplicate pixels by inverse
        // variance (both are on the coadd's flux system by now).
        std::vector<size_t> order(wl.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::sort(order.begin(), order.end(),
                  [&wl](size_t x, size_t y) { return wl[x] < wl[y]; });
        std::vector<double> swl, sflux, serr;
        swl.reserve(wl.size());
        sflux.reserve(wl.size());
        serr.reserve(wl.size());
        for (size_t i = 0; i < order.size();) {
            const double w = wl[order[i]];
            double wSum = 0.0, fSum = 0.0;
            for (; i < order.size() && wl[order[i]] <= w * (1.0 + 1e-9); ++i) {
                const double e  = err[order[i]];
                const double wt = e > 0 ? 1.0 / (e * e) : 1.0;
                wSum += wt;
                fSum += wt * flux[order[i]];
            }
            swl.push_back(w);
            sflux.push_back(fSum / wSum);
            serr.push_back(wSum > 0 ? 1.0 / std::sqrt(wSum) : 0.0);
        }

        if (opt.vacToAir) SpecFetch::vacToAir(swl);

        auto spec = std::make_shared<Spectrum>();
        spec->setData(swl, sflux, serr);
        spec->setFile(localPath);
        spec->setInstrument(r.instrumentHint);

        const long firstArm = byEpoch[epoch].front();
        double     expSec   = 0.0;
        if (firstArm < long(a.exptime.size()) && a.exptime[firstArm] > 0)
            expSec = a.exptime[firstArm];
        // MJM stamps the exposure start; mid-exposure is the epoch RV work
        // expects.
        const double mjmVal = epoch.toDouble();
        if (mjmVal > 0)
            spec->setMJD(mjmVal / 1440.0 + expSec * 0.5 / 86400.0);
        if (expSec > 0) spec->setExposureTime(expSec);

        SpecFetch::ParsedSpectrum ps;
        ps.spectrum       = spec;
        ps.originId       = r.originId + QStringLiteral("#") + epoch;
        ps.isCoadd        = false;
        ps.instrumentHint = r.instrumentHint;
        out.push_back(std::move(ps));
    }

    if (out.empty() && error && error->isEmpty())
        *error = QStringLiteral("no usable exposures in file");
    return out;
}

std::vector<SpecFetch::ParsedSpectrum> LamostArchiveClient::parse(
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

    if (r.originId.startsWith(QLatin1String(kSexpOriginPrefix))) {
        out = parseSexp(fptr, localPath, r, opt, error);
        fits_close_file(fptr, &status);
        return out;
    }

    // Shared primary-header metadata.
    double mjd = NAN, exptime = NAN;
    {
        fits_movabs_hdu(fptr, 1, nullptr, &status);
        mjd = mjdFromDateObs(fptr);
        int st = 0;
        double v = 0;
        if (fits_read_key(fptr, TDOUBLE, "EXPTIME", &v, nullptr, &st) == 0 &&
            v > 0)
            exptime = v;
    }
    if (std::isnan(mjd)) mjd = r.mjd;

    auto makeSpectrum = [&](std::vector<double> wl,
                            const std::vector<double>& flux,
                            const std::vector<double>& err) {
        auto spec = std::make_shared<Spectrum>();
        if (opt.vacToAir) SpecFetch::vacToAir(wl);
        spec->setData(wl, flux, err);
        spec->setFile(localPath);
        spec->setInstrument(r.instrumentHint);
        if (!std::isnan(mjd)) spec->setMJD(mjd);
        return spec;
    };

    if (!_mrs) {
        std::vector<double> wl, flux, err;
        if (readLrsCoadd(fptr, wl, flux, err)) {
            auto spec = makeSpectrum(std::move(wl), flux, err);
            if (!std::isnan(exptime)) spec->setExposureTime(exptime);
            SpecFetch::ParsedSpectrum ps;
            ps.spectrum       = spec;
            ps.originId       = r.originId;
            ps.isCoadd        = true;
            ps.instrumentHint = r.instrumentHint;
            out.push_back(std::move(ps));
        } else if (error) {
            *error = QStringLiteral("unrecognized LAMOST LRS layout");
        }
        fits_close_file(fptr, &status);
        return out;
    }

    // MRS: COADD_B / COADD_R plus per-exposure HDUs ("B-<lmjm>", "R-<lmjm>").
    int numHdus = 0;
    fits_get_num_hdus(fptr, &numHdus, &status);

    std::vector<SpecFetch::ParsedSpectrum> coadds, exposures;

    for (int hdu = 2; hdu <= numHdus; ++hdu) {
        int hduType = ANY_HDU;
        status      = 0;
        if (fits_movabs_hdu(fptr, hdu, &hduType, &status) != 0 ||
            hduType != BINARY_TBL)
            continue;

        char val[FLEN_VALUE] = {0};
        int  st              = 0;
        if (fits_read_key(fptr, TSTRING, "EXTNAME", val, nullptr, &st) != 0)
            continue;
        const QString name = QString::fromLatin1(val).trimmed().toUpper();

        const bool isCoaddArm = name.startsWith(QLatin1String("COADD"));
        const bool isExposure =
            !isCoaddArm && (name.startsWith(QLatin1String("B-")) ||
                            name.startsWith(QLatin1String("R-")));
        if (!isCoaddArm && !isExposure) continue;
        if (isExposure && !opt.fetchExposures) continue;

        std::vector<double> wl, flux, err;
        if (!readMrsTable(fptr, wl, flux, err)) continue;

        auto spec = makeSpectrum(std::move(wl), flux, err);
        if (isExposure) {
            int    st2 = 0;
            double e   = 0;
            if (fits_read_key(fptr, TDOUBLE, "EXPTIME", &e, nullptr, &st2) ==
                    0 &&
                e > 0)
                spec->setExposureTime(e);
            else
                e = 0;
            // Per-exposure epoch: LMJM is the modified julian minute of the
            // exposure start; shift to mid-exposure for RV work.
            st2 = 0;
            char lmjmStr[FLEN_VALUE] = {0};
            if (fits_read_key(fptr, TSTRING, "LMJM", lmjmStr, nullptr, &st2) ==
                0) {
                const double lmjm =
                    QString::fromLatin1(lmjmStr).trimmed().toDouble();
                if (lmjm > 0)
                    spec->setMJD(lmjm / 1440.0 + e * 0.5 / 86400.0);
            }
        } else {
            if (!std::isnan(exptime)) spec->setExposureTime(exptime);
            // Newer files keep DATE-OBS on the arm HDUs rather than in the
            // primary header.
            if (std::isnan(mjd)) {
                const double hduMjd = mjdFromDateObs(fptr);
                if (!std::isnan(hduMjd)) spec->setMJD(hduMjd);
            }
        }

        SpecFetch::ParsedSpectrum ps;
        ps.spectrum = spec;
        ps.originId = r.originId + QStringLiteral("#") +
                      (isCoaddArm ? name.mid(6) /* B or R */ : name);
        ps.isCoadd        = isCoaddArm;
        ps.instrumentHint = r.instrumentHint + QStringLiteral(" ") +
                            (name.startsWith(QLatin1Char('B')) ||
                                     name.endsWith(QLatin1String("_B"))
                                 ? QStringLiteral("blue")
                                 : QStringLiteral("red"));
        (isCoaddArm ? coadds : exposures).push_back(std::move(ps));
    }

    fits_close_file(fptr, &status);

    // Either/or: with the exposures option on, the single exposures replace
    // the coadd arms they were stacked from; the coadds are the fallback for
    // files that carry none.
    out = (opt.fetchExposures && !exposures.empty()) ? std::move(exposures)
                                                     : std::move(coadds);

    if (out.empty() && error && error->isEmpty())
        *error = QStringLiteral("no spectral HDUs found");
    return out;
}
