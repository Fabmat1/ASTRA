// The generic FITS reader on 1-D image spectra: log-sampled wavelength axes,
// the mid-exposure epoch, and pipelines that deliver barycentric wavelengths.
//
// Each case writes a small synthetic file with cfitsio, so nothing depends on
// archive data. The HERMES header mirrors a real Mercator product
// (*_HRF_OBJ_ext_CosmicsRemoved_log_merged_cf.fits): before these cases the
// reader took its ln(lambda) axis for a linear one and returned "wavelengths"
// of 8.23 to 9.11, stored the exposure start as the epoch, took the header's
// BJD (UTC scale) for BJD_TDB, and left the spectrum flagged as topocentric.

#include <doctest.h>

#include "spectra/Spectrum.h"
#include "spectra/SpectrumReader.h"
#include "spectra/fetch/SpectrumFrame.h"

#include <QTemporaryDir>

#include <fitsio.h>

#include <cmath>
#include <functional>
#include <vector>

namespace {

// Writes a 1-D double image with the given header cards.
QString writeImage(const QTemporaryDir& dir, const char* name, long n,
                   const std::function<void(fitsfile*)>& cards)
{
    const QString path = dir.filePath(QString::fromLatin1(name));
    const QByteArray create = ("!" + path).toUtf8();
    fitsfile* f  = nullptr;
    int       st = 0;
    long      naxes[1] = {n};
    fits_create_file(&f, create.constData(), &st);
    fits_create_img(f, DOUBLE_IMG, 1, naxes, &st);
    std::vector<double> flux(size_t(n), 1.0);
    fits_write_img(f, TDOUBLE, 1, n, flux.data(), &st);
    cards(f);
    fits_close_file(f, &st);
    REQUIRE(st == 0);
    return path;
}

void keyD(fitsfile* f, const char* k, double v)
{
    int st = 0;
    fits_update_key(f, TDOUBLE, k, &v, nullptr, &st);
}
void keyS(fitsfile* f, const char* k, const char* v)
{
    int st = 0;
    fits_update_key(f, TSTRING, k, const_cast<char*>(v), nullptr, &st);
}
void keyI(fitsfile* f, const char* k, int v)
{
    int st = 0;
    fits_update_key(f, TINT, k, &v, nullptr, &st);
}

}   // namespace

TEST_SUITE("spectra")
{

TEST_CASE("HERMES merged product: ln wavelength, mid epoch, barycentric")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const double crval = 8.232995942171296, cdelt = 5.20050525665283e-06;
    const QString path = writeImage(dir, "hermes.fits", 1000, [&](fitsfile* f) {
        keyS(f, "TELESCOP", "Mercator");
        keyS(f, "INSTRUME", "HERMES");
        keyS(f, "FIBMODE", "HRF");
        keyS(f, "DATE-OBS", "2026-01-21T00:53:01.818027");
        keyS(f, "DATE-AVG", "2026-01-21T01:05:31.834581");
        keyD(f, "MJD-OBS", 61061.03682659753);   // the start, not the middle
        keyD(f, "BJD", 2461061.5483853);
        keyD(f, "BVCOR", -26.074589);
        keyD(f, "EXPTIME", 1500.0);
        keyD(f, "CRPIX1", 1.0);
        keyD(f, "CRVAL1", crval);
        keyD(f, "CDELT1", cdelt);
        keyS(f, "CTYPE1", "log(wavelength)");
    });

    DefaultFitsSpectrumReader reader;
    const SpectrumReadResult r = reader.readSpectrum(path);
    REQUIRE(r.success);
    const auto wl = r.spectrum->getWavelengths();
    REQUIRE(wl.size() == 1000);
    CHECK(wl.front() == doctest::Approx(std::exp(crval)).epsilon(1e-12));
    CHECK(wl.front() == doctest::Approx(3763.0909).epsilon(1e-7));
    CHECK(wl.back() == doctest::Approx(std::exp(crval + 999 * cdelt)).epsilon(1e-12));

    // DATE-AVG, 12.5 min after MJD-OBS.
    REQUIRE(r.metadata.mjd.has_value());
    const double midMjd = 61061.0 + (1 * 3600 + 5 * 60 + 31.834581) / 86400.0;
    CHECK(std::abs(*r.metadata.mjd - midMjd) * 86400.0 < 0.01);
    // HERMES's BJD card is on the UTC scale (69 s short of TDB): not used.
    CHECK_FALSE(r.metadata.bjd.has_value());

    CHECK(r.spectrum->getInstrument() == QStringLiteral("HERMES HRF"));
    CHECK(r.spectrum->isBarycentricallyCorrected());
    CHECK(SpecFetch::barycorrFromOriginMeta(r.spectrum->getOriginMeta()) ==
          doctest::Approx(-26.074589));
}

TEST_CASE("IRAF DC-FLAG = 1 is log10 sampling")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString path = writeImage(dir, "dcflag.fits", 100, [](fitsfile* f) {
        keyD(f, "CRVAL1", 3.5682);
        keyD(f, "CD1_1", 1e-4);
        keyI(f, "DC-FLAG", 1);
        keyS(f, "CTYPE1", "LINEAR");
    });
    DefaultFitsSpectrumReader reader;
    const SpectrumReadResult r = reader.readSpectrum(path);
    REQUIRE(r.success);
    const auto wl = r.spectrum->getWavelengths();
    CHECK(wl.front() == doctest::Approx(std::pow(10.0, 3.5682)).epsilon(1e-12));
    CHECK(wl[99] == doctest::Approx(std::pow(10.0, 3.5682 + 99e-4)).epsilon(1e-12));
    CHECK_FALSE(r.spectrum->isBarycentricallyCorrected());
}

TEST_CASE("FITS -LOG algorithm and plain linear axes")
{
    QTemporaryDir dir;
    REQUIRE(dir.isValid());
    const QString logPath = writeImage(dir, "wavelog.fits", 10, [](fitsfile* f) {
        keyD(f, "CRVAL1", 5000.0);
        keyD(f, "CDELT1", 0.5);
        keyD(f, "CRPIX1", 1.0);
        keyS(f, "CTYPE1", "AWAV-LOG");
    });
    const QString linPath = writeImage(dir, "linear.fits", 10, [](fitsfile* f) {
        keyD(f, "CRVAL1", 5000.0);
        keyD(f, "CDELT1", 0.5);
        keyD(f, "CRPIX1", 1.0);
        keyS(f, "CTYPE1", "AWAV");
        keyD(f, "MJD-OBS", 60000.25);
    });
    DefaultFitsSpectrumReader reader;

    const auto logWl = reader.readSpectrum(logPath).spectrum->getWavelengths();
    CHECK(logWl[0] == doctest::Approx(5000.0));
    CHECK(logWl[9] == doctest::Approx(5000.0 * std::exp(9 * 0.5 / 5000.0)).epsilon(1e-12));

    const SpectrumReadResult lin = reader.readSpectrum(linPath);
    const auto linWl = lin.spectrum->getWavelengths();
    CHECK(linWl[9] == doctest::Approx(5004.5));
    // No midpoint card: the MJD-OBS value is kept as it was before.
    REQUIRE(lin.metadata.mjd.has_value());
    CHECK(*lin.metadata.mjd == doctest::Approx(60000.25));
}

}
