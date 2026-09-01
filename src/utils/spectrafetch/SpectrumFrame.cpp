// src/utils/spectrafetch/SpectrumFrame.cpp

#include "SpectrumFrame.h"

#include "models/BarycentricCorrection.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <fitsio.h>

#include <algorithm>

namespace {

constexpr double kCkmPerSec = 299792.458;

// WGS-84, for the ITRF cartesian position ESO and HST write as OBSGEO-X/Y/Z.
constexpr double kWgs84A  = 6378137.0;
constexpr double kWgs84F  = 1.0 / 298.257223563;
constexpr double kWgs84E2 = 2.0 * kWgs84F - kWgs84F * kWgs84F;

// Reads a floating-point keyword from the current HDU. cfitsio resolves the
// HIERARCH form from the plain name, so "ESO QC VRAD BARYCOR" finds
// "HIERARCH ESO QC VRAD BARYCOR".
bool readDouble(fitsfile* f, const char* key, double* out) {
    int    status = 0;
    double v      = 0.0;
    if (fits_read_key(f, TDOUBLE, key, &v, nullptr, &status) != 0) return false;
    if (std::isnan(v)) return false;
    *out = v;
    return true;
}

bool readString(fitsfile* f, const char* key, QString* out) {
    int  status           = 0;
    char v[FLEN_VALUE]    = {0};
    if (fits_read_key(f, TSTRING, key, v, nullptr, &status) != 0) return false;
    *out = QString::fromLatin1(v).trimmed();
    return !out->isEmpty();
}

SpecFetch::Frame frameFromSpecsys(const QString& raw) {
    const QString s = raw.toUpper();
    if (s.startsWith(QLatin1String("BARYCENT"))) return SpecFetch::Frame::Barycentric;
    if (s.startsWith(QLatin1String("HELIOCEN"))) return SpecFetch::Frame::Heliocentric;
    if (s.startsWith(QLatin1String("TOPOCENT"))) return SpecFetch::Frame::Topocentric;
    if (s.startsWith(QLatin1String("GEOCENTR"))) return SpecFetch::Frame::Geocentric;
    // LSRK, GALACTOC, CMBDIPOL, SOURCE: frames no archive spectrum in this
    // pipeline uses, and none of them is a barycentric scale in disguise.
    return SpecFetch::Frame::Unknown;
}

// ITRF cartesian [m] -> geodetic. Bowring's method, one iteration, which is
// millimetre-accurate at observatory altitudes.
void ecefToGeodetic(double x, double y, double z, SpecFetch::ObserverSite* site) {
    const double b  = kWgs84A * (1.0 - kWgs84F);
    const double ep2 = (kWgs84A * kWgs84A - b * b) / (b * b);
    const double p  = std::sqrt(x * x + y * y);
    const double th = std::atan2(kWgs84A * z, b * p);
    const double lat =
        std::atan2(z + ep2 * b * std::pow(std::sin(th), 3),
                   p - kWgs84E2 * kWgs84A * std::pow(std::cos(th), 3));
    const double n = kWgs84A / std::sqrt(1.0 - kWgs84E2 * std::sin(lat) * std::sin(lat));

    site->latDeg = lat * 180.0 / M_PI;
    site->lonDeg = std::atan2(y, x) * 180.0 / M_PI;
    site->altM   = p / std::cos(lat) - n;
}

// One HDU's contribution. Later HDUs only fill gaps: the primary header is
// where the ESO SDP and HST products put all of this, but generic products
// sometimes carry SPECSYS on the table extension instead.
void scanHdu(fitsfile* f, SpecFetch::FrameInfo* info) {
    QString specsys;
    if (info->frame == SpecFetch::Frame::Unknown &&
        readString(f, "SPECSYS", &specsys))
        info->frame = frameFromSpecsys(specsys);

    // HST: the calibration switch reads COMPLETE once calstis/calcos has moved
    // the wavelengths onto the heliocentric scale, OMIT when it has not.
    QString helcorr;
    if (info->frame == SpecFetch::Frame::Unknown &&
        readString(f, "HELCORR", &helcorr)) {
        const QString h = helcorr.toUpper();
        if (h.startsWith(QLatin1String("COMPLETE")))
            info->frame = SpecFetch::Frame::Heliocentric;
        else if (h.startsWith(QLatin1String("OMIT")) ||
                 h.startsWith(QLatin1String("SKIPPED")))
            info->frame = SpecFetch::Frame::Geocentric;
    }

    // The velocity the pipeline computed for this exposure, whether or not it
    // applied it. Only ever used as a cross-check on our own number.
    if (std::isnan(info->statedCorrectionKms)) {
        static const char* kVelocityKeys[] = {
            "ESO QC VRAD BARYCOR",   // UVES / X-shooter / GIRAFFE / CRIRES+
            "ESO DRS BARYCORR",      // FEROS
            "ESO QC BERV",           // HARPS
            "ESO DRS BERV",          // HARPS
            "V_HELIO",               // HST
            "HELIO_RV",              // SDSS
            "VHELIO",
        };
        for (const char* key : kVelocityKeys) {
            double v = 0.0;
            if (readDouble(f, key, &v)) {
                info->statedCorrectionKms = v;
                info->statedCorrectionKey = QString::fromLatin1(key);
                break;
            }
        }
    }

    if (std::isnan(info->targetRaDeg)) {
        double ra = 0.0, dec = 0.0;
        if (readDouble(f, "RA", &ra) && readDouble(f, "DEC", &dec)) {
            info->targetRaDeg  = ra;
            info->targetDecDeg = dec;
        }
    }

    if (info->site.known) return;

    double x = 0.0, y = 0.0, z = 0.0;
    if (readDouble(f, "OBSGEO-X", &x) && readDouble(f, "OBSGEO-Y", &y) &&
        readDouble(f, "OBSGEO-Z", &z) && (x != 0.0 || y != 0.0 || z != 0.0)) {
        ecefToGeodetic(x, y, z, &info->site);
        info->site.known  = true;
        info->site.source = QStringLiteral("OBSGEO-X/Y/Z");
        return;
    }

    // ESO numbers the keyword after the VLT unit telescope on the instruments
    // that live on one - ESPRESSO writes ESO TEL3 GEOLAT, the La Silla
    // instruments write ESO TEL GEOLAT - so every form has to be tried.
    struct SiteKeys { const char* lat; const char* lon; const char* alt;
                      const char* label; };
    static const SiteKeys kSiteKeys[] = {
        { "ESO TEL GEOLAT", "ESO TEL GEOLON", "ESO TEL GEOELEV",
          "HIERARCH ESO TEL GEO*" },
        { "ESO TEL1 GEOLAT", "ESO TEL1 GEOLON", "ESO TEL1 GEOELEV",
          "HIERARCH ESO TEL1 GEO*" },
        { "ESO TEL2 GEOLAT", "ESO TEL2 GEOLON", "ESO TEL2 GEOELEV",
          "HIERARCH ESO TEL2 GEO*" },
        { "ESO TEL3 GEOLAT", "ESO TEL3 GEOLON", "ESO TEL3 GEOELEV",
          "HIERARCH ESO TEL3 GEO*" },
        { "ESO TEL4 GEOLAT", "ESO TEL4 GEOLON", "ESO TEL4 GEOELEV",
          "HIERARCH ESO TEL4 GEO*" },
        { "GEOLAT", "GEOLON", "GEOELEV", "GEOLAT/GEOLON/GEOELEV" },
        { "SITELAT", "SITELONG", "SITEALT", "SITELAT/SITELONG/SITEALT" },
        { "OBS-LAT", "OBS-LONG", "OBS-ELEV", "OBS-LAT/OBS-LONG/OBS-ELEV" },
        { "LATITUDE", "LONGITUD", "HEIGHT", "LATITUDE/LONGITUD/HEIGHT" },
    };
    for (const SiteKeys& k : kSiteKeys) {
        double lat = 0.0, lon = 0.0, alt = 0.0;
        if (!readDouble(f, k.lat, &lat) || !readDouble(f, k.lon, &lon))
            continue;
        if (lat == 0.0 && lon == 0.0) continue;
        readDouble(f, k.alt, &alt);
        info->site.latDeg = lat;
        info->site.lonDeg = lon;
        info->site.altM   = alt;
        info->site.known  = true;
        info->site.source = QString::fromLatin1(k.label);
        return;
    }
}

}   // namespace

namespace SpecFetch {

QString frameName(Frame f) {
    switch (f) {
        case Frame::Topocentric:  return QStringLiteral("topocentric");
        case Frame::Geocentric:   return QStringLiteral("geocentric");
        case Frame::Heliocentric: return QStringLiteral("heliocentric");
        case Frame::Barycentric:  return QStringLiteral("barycentric");
        case Frame::Unknown:      break;
    }
    return QStringLiteral("unknown");
}

FrameInfo readFrameInfo(const QString& fitsPath) {
    FrameInfo info;

    fitsfile* fptr   = nullptr;
    int       status = 0;
    if (fits_open_file(&fptr, fitsPath.toUtf8().constData(), READONLY, &status))
        return info;

    int numHdus = 0;
    fits_get_num_hdus(fptr, &numHdus, &status);
    if (status != 0) numHdus = 1;

    // The keywords live in the first couple of headers; walking a 60-order
    // echelle file to the end buys nothing.
    const int lastHdu = std::min(numHdus, 4);
    for (int hdu = 1; hdu <= lastHdu; ++hdu) {
        int st = 0;
        if (fits_movabs_hdu(fptr, hdu, nullptr, &st) != 0) break;
        scanHdu(fptr, &info);
    }

    status = 0;
    fits_close_file(fptr, &status);
    return info;
}

double bervKms(double mjdUtc, double raDeg, double decDeg,
               const ObserverSite& site) {
    if (!(mjdUtc > 0.0) || std::isnan(raDeg) || std::isnan(decDeg))
        return std::nan("");
    return BarycentricCorrection::barycentricVelocity(
        mjdUtc, raDeg, decDeg, site.lonDeg, site.latDeg, site.altM);
}

double barycorrFromOriginMeta(const QString& originMetaJson) {
    if (originMetaJson.isEmpty()) return std::nan("");
    const QJsonDocument doc =
        QJsonDocument::fromJson(originMetaJson.toUtf8());
    if (!doc.isObject()) return std::nan("");
    const QJsonValue v =
        doc.object().value(QLatin1String(kBarycorrMetaKey));
    if (!v.isDouble()) return std::nan("");
    return v.toDouble();
}

void applyRadialVelocityShift(std::vector<double>& wavelengths, double vKms) {
    if (!std::isfinite(vKms) || vKms == 0.0) return;
    const double factor = 1.0 + vKms / kCkmPerSec;
    for (double& w : wavelengths) w *= factor;
}

}   // namespace SpecFetch
