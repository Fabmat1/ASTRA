// ─────────────────────────────────────────────────────────────────────────────
// Spectral reference frame detection and the barycentric wavelength shift.
//
// Writes small FITS files with the headers real archive products carry and
// checks what SpecFetch::readFrameInfo() makes of them, then checks the shift
// itself against the velocity that goes into it.
//
// The velocity model is validated separately, against astropy, in
// test_barycentric.
//
// Run: ctest --test-dir build -R spectrum_frame
// ─────────────────────────────────────────────────────────────────────────────
#include "utils/spectrafetch/SpectrumFrame.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <fitsio.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int gFailures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str());
    if (!ok) ++gFailures;
}

void checkClose(double got, double want, double tol, const std::string& what) {
    const bool ok = std::isfinite(got) && std::fabs(got - want) <= tol;
    std::printf("%s  %s (got %.6f, want %.6f +- %.6f)\n",
                ok ? "[ ok ]" : "[FAIL]", what.c_str(), got, want, tol);
    if (!ok) ++gFailures;
}

struct Key {
    const char* name;
    int         type;      // TSTRING or TDOUBLE
    const char* str;
    double      num;
};

Key str(const char* name, const char* value) {
    return { name, TSTRING, value, 0.0 };
}
Key num(const char* name, double value) {
    return { name, TDOUBLE, nullptr, value };
}

// A one-row spectral file with the given primary-header keywords.
bool writeFits(const QString& path, const std::vector<Key>& keys) {
    fitsfile* f      = nullptr;
    int       status = 0;
    const QString spec = QStringLiteral("!") + path;   // '!' overwrites
    if (fits_create_file(&f, spec.toUtf8().constData(), &status)) return false;

    long naxes[1] = {0};
    fits_create_img(f, DOUBLE_IMG, 0, naxes, &status);

    for (const Key& k : keys) {
        if (k.type == TSTRING) {
            char buf[FLEN_VALUE];
            std::snprintf(buf, sizeof(buf), "%s", k.str);
            fits_write_key(f, TSTRING, k.name, buf, nullptr, &status);
        } else {
            double v = k.num;
            fits_write_key(f, TDOUBLE, k.name, &v, nullptr, &status);
        }
    }

    fits_close_file(f, &status);
    return status == 0;
}

}   // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        std::fprintf(stderr, "cannot create a temporary directory\n");
        return 1;
    }
    auto path = [&tmp](const char* name) {
        return tmp.filePath(QString::fromLatin1(name));
    };

    // ── ESO UVES / X-shooter: topocentric, correction computed not applied ──
    {
        const QString p = path("eso_topo.fits");
        check(writeFits(p, {
                  str("SPECSYS", "TOPOCENT"),
                  num("ESO QC VRAD BARYCOR", -12.3456),
                  num("ESO TEL GEOLAT", -24.6272),
                  num("ESO TEL GEOLON", -70.4042),
                  num("ESO TEL GEOELEV", 2635.0),
              }),
              "ESO topocentric: file written");

        const SpecFetch::FrameInfo fi = SpecFetch::readFrameInfo(p);
        check(fi.frame == SpecFetch::Frame::Topocentric,
              "ESO topocentric: SPECSYS read as topocentric");
        check(!SpecFetch::frameIsCorrected(fi.frame),
              "ESO topocentric: counts as uncorrected");
        checkClose(fi.statedCorrectionKms, -12.3456, 1e-6,
                   "ESO topocentric: QC VRAD BARYCOR read");
        check(fi.site.known, "ESO topocentric: site found");
        checkClose(fi.site.latDeg, -24.6272, 1e-6, "ESO topocentric: latitude");
        checkClose(fi.site.lonDeg, -70.4042, 1e-6, "ESO topocentric: longitude");
        checkClose(fi.site.altM, 2635.0, 1e-3, "ESO topocentric: altitude");
    }

    // ── HARPS / ESPRESSO / FEROS: already barycentric ───────────────────────
    {
        const QString p = path("eso_bary.fits");
        writeFits(p, { str("SPECSYS", "BARYCENT"), num("ESO DRS BERV", 21.5) });
        const SpecFetch::FrameInfo fi = SpecFetch::readFrameInfo(p);
        check(fi.frame == SpecFetch::Frame::Barycentric,
              "ESO barycentric: SPECSYS read as barycentric");
        check(SpecFetch::frameIsCorrected(fi.frame),
              "ESO barycentric: counts as corrected");
    }

    // ── HST: the calibration switch stands in for SPECSYS ───────────────────
    {
        const QString p = path("hst_done.fits");
        writeFits(p, { str("HELCORR", "COMPLETE"), num("V_HELIO", 8.75) });
        const SpecFetch::FrameInfo fi = SpecFetch::readFrameInfo(p);
        check(fi.frame == SpecFetch::Frame::Heliocentric,
              "HST HELCORR=COMPLETE: heliocentric");
        check(SpecFetch::frameIsCorrected(fi.frame),
              "HST HELCORR=COMPLETE: counts as corrected");

        const QString q = path("hst_omit.fits");
        writeFits(q, { str("HELCORR", "OMIT"), num("V_HELIO", 8.75) });
        const SpecFetch::FrameInfo fq = SpecFetch::readFrameInfo(q);
        check(fq.frame == SpecFetch::Frame::Geocentric,
              "HST HELCORR=OMIT: geocentric");
        check(!SpecFetch::frameIsCorrected(fq.frame),
              "HST HELCORR=OMIT: counts as uncorrected");
    }

    // ── A file that says nothing stays unknown ──────────────────────────────
    {
        const QString p = path("silent.fits");
        writeFits(p, { str("INSTRUME", "SOMETHING") });
        const SpecFetch::FrameInfo fi = SpecFetch::readFrameInfo(p);
        check(fi.frame == SpecFetch::Frame::Unknown,
              "silent file: frame unknown");
        check(!fi.site.known, "silent file: no site");
        check(std::isnan(fi.statedCorrectionKms),
              "silent file: no stated correction");
    }

    // ── OBSGEO-X/Y/Z round trip ─────────────────────────────────────────────
    // Paranal in ITRF metres; the geodetic position has to come back out.
    {
        const QString p = path("obsgeo.fits");
        writeFits(p, {
                      str("SPECSYS", "TOPOCENT"),
                      // Paranal (-24.6275, -70.4044, 2635 m) on WGS-84.
                      num("OBSGEO-X", 1946449.04),
                      num("OBSGEO-Y", -5467592.60),
                      num("OBSGEO-Z", -2642720.14),
                  });
        const SpecFetch::FrameInfo fi = SpecFetch::readFrameInfo(p);
        check(fi.site.known, "OBSGEO: site found");
        checkClose(fi.site.latDeg, -24.6275, 1e-5, "OBSGEO: latitude");
        checkClose(fi.site.lonDeg, -70.4044, 1e-5, "OBSGEO: longitude");
        checkClose(fi.site.altM, 2635.0, 0.05, "OBSGEO: altitude");
    }

    // ── A missing file must not throw or invent an answer ───────────────────
    {
        const SpecFetch::FrameInfo fi =
            SpecFetch::readFrameInfo(path("does_not_exist.fits"));
        check(fi.frame == SpecFetch::Frame::Unknown,
              "missing file: frame unknown");
    }

    // ── The shift itself ────────────────────────────────────────────────────
    {
        constexpr double c = 299792.458;
        std::vector<double> wl = {4000.0, 5000.0, 6562.8};
        const std::vector<double> original = wl;

        SpecFetch::applyRadialVelocityShift(wl, 30.0);
        checkClose(wl[2], 6562.8 * (1.0 + 30.0 / c), 1e-9,
                   "shift: +30 km/s moves H-alpha redward by v/c");
        check(wl[0] > original[0] && wl[1] > original[1],
              "shift: a positive correction lengthens every wavelength");

        // Undoing it lands back within (v/c)^2, the classical Doppler
        // factor's own asymmetry - 7e-5 A at 30 km/s, three orders below the
        // pixel of any spectrograph this pipeline fetches from.
        SpecFetch::applyRadialVelocityShift(wl, -30.0);
        checkClose(wl[2], 6562.8, 1e-3, "shift: +30 then -30 returns");

        std::vector<double> untouched = original;
        SpecFetch::applyRadialVelocityShift(untouched, std::nan(""));
        check(untouched == original, "shift: NaN velocity is a no-op");
        SpecFetch::applyRadialVelocityShift(untouched, 0.0);
        check(untouched == original, "shift: zero velocity is a no-op");
    }

    // ── bervKms plumbing ────────────────────────────────────────────────────
    {
        SpecFetch::ObserverSite paranal;
        paranal.latDeg = -24.6272;
        paranal.lonDeg = -70.4042;
        paranal.altM   = 2635.0;
        paranal.known  = true;

        // MJD 58748.0, the Galactic centre from Paranal: astropy puts this at
        // -29.63706 km/s (see tests/reference_bjd.json, autumnal_equinox/
        // galactic_centre/paranal).
        const double v =
            SpecFetch::bervKms(58748.0, 266.4168, -29.0078, paranal);
        checkClose(v, -29.63706, 0.05, "bervKms: matches the astropy case");

        // The geocentre differs from the observatory by Earth's rotation at
        // most, which is 0.465 km/s at the equator.
        const SpecFetch::ObserverSite geocentre;
        const double vGeo =
            SpecFetch::bervKms(58748.0, 266.4168, -29.0078, geocentre);
        check(std::fabs(vGeo - v) < 0.47,
              "bervKms: geocentre within Earth's rotation speed");

        check(std::isnan(SpecFetch::bervKms(0.0, 10.0, 10.0, paranal)),
              "bervKms: no epoch gives NaN");
        check(std::isnan(
                  SpecFetch::bervKms(58748.0, std::nan(""), 10.0, paranal)),
              "bervKms: no position gives NaN");
    }

    // ── The provenance key the fit reads back ───────────────────────────────
    {
        checkClose(SpecFetch::barycorrFromOriginMeta(
                       QStringLiteral(R"({"archive":"ESO","barycorrKms":23.744})")),
                   23.744, 1e-9, "originMeta: value read back");
        check(std::isnan(SpecFetch::barycorrFromOriginMeta(
                  QStringLiteral(R"({"archive":"ESO"})"))),
              "originMeta: absent key gives NaN");
        check(std::isnan(SpecFetch::barycorrFromOriginMeta(QString())),
              "originMeta: empty blob gives NaN");
        check(std::isnan(SpecFetch::barycorrFromOriginMeta(
                  QStringLiteral("not json at all"))),
              "originMeta: malformed blob gives NaN");
        check(std::isnan(SpecFetch::barycorrFromOriginMeta(
                  QStringLiteral(R"({"barycorrKms":"twelve"})"))),
              "originMeta: non-numeric value gives NaN");
    }

    std::printf("\n%s: %d failure(s)\n",
                gFailures == 0 ? "PASS" : "FAIL", gFailures);
    return gFailures == 0 ? 0 : 1;
}
