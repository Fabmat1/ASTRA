// ─────────────────────────────────────────────────────────────────────────────
// Archive FITS parser test. Real product files are too large to ship as
// fixtures, so each case reads its input path from an environment variable
// and is skipped when unset:
//
//   ASTRA_TEST_ESO_FITS     an ESO Phase-3 1D spectrum (e.g. XSHOOTER ADP)
//   ASTRA_TEST_SDSS_FULL    a full SDSS spec-PLATE-MJD-FIBER.fits
//   ASTRA_TEST_LAMOST_LRS   a LAMOST LRS FITS (.fits.gz is fine)
//   ASTRA_TEST_LAMOST_SEXP  a LAMOST LRS single-exposure file (sedr5 .fit);
//                           put ASTRA_TEST_LAMOST_LRS in the same directory
//                           to also cover the coadd-anchored calibration
//   ASTRA_TEST_LAMOST_MRS   a LAMOST MRS FITS (.fits.gz is fine)
//   ASTRA_TEST_APSTAR       an APOGEE apStar/asStar file
//   ASTRA_TEST_MAST_HST     an HST coadd (hst_*_cspec.fits or an x1d)
//   ASTRA_TEST_MAST_FUSE    a FUSE NVO spectrum (*nvo4histfcal_vo.fits)
//
// Checks per file: parse succeeds, wavelengths are ascending Angstroms in a
// plausible range, exposures/arms split as expected, originId suffixes, and
// the wavelength reference frame the headers state.
// ─────────────────────────────────────────────────────────────────────────────
#include "models/Spectrum.h"
#include "utils/spectrafetch/ApogeeArchiveClient.h"
#include "utils/spectrafetch/EsoArchiveClient.h"
#include "utils/spectrafetch/LamostArchiveClient.h"
#include "utils/spectrafetch/MastArchiveClient.h"
#include "utils/spectrafetch/SdssOpticalArchiveClient.h"
#include "utils/spectrafetch/SpectrumFrame.h"

#include <QCoreApplication>
#include <QFileInfo>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

int gFailures = 0;
int gRun      = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s  %s\n", ok ? "[ ok ]" : "[FAIL]", what.c_str());
    if (!ok) ++gFailures;
}

bool plausibleSpectrum(const SpecFetch::ParsedSpectrum& p, double wlMin,
                       double wlMax, const std::string& label) {
    if (!p.spectrum || !p.spectrum->hasData()) {
        check(false, label + ": has data");
        return false;
    }
    const auto wl = p.spectrum->getWavelengths();
    const auto fl = p.spectrum->getFluxes();
    check(wl.size() == fl.size() && wl.size() > 100,
          label + ": array sizes (" + std::to_string(wl.size()) + " px)");
    bool ascending = true;
    for (size_t i = 1; i < wl.size(); ++i)
        if (wl[i] <= wl[i - 1]) { ascending = false; break; }
    check(ascending, label + ": wavelengths ascending");
    check(wl.front() > wlMin && wl.back() < wlMax,
          label + ": range " + std::to_string(int(wl.front())) + "-" +
              std::to_string(int(wl.back())) + " A inside [" +
              std::to_string(int(wlMin)) + ", " + std::to_string(int(wlMax)) +
              "]");
    check(p.spectrum->getMJD() > 30000 && p.spectrum->getMJD() < 80000,
          label + ": plausible MJD " +
              std::to_string(p.spectrum->getMJD()));
    return true;
}

// What the file says about its wavelength frame, and - for a topocentric one
// that also carries the pipeline's own correction - whether the velocity ASTRA
// would apply agrees with it. That comparison is the end-to-end check on the
// site, the epoch and the sign, on a real product rather than a fixture.
void reportFrame(const QString& path, const SpecFetch::ParsedSpectrum& p,
                 double raDeg, double decDeg, const std::string& label) {
    // raDeg/decDeg: what the archive record said, NaN when it said nothing.
    const SpecFetch::FrameInfo fi = SpecFetch::readFrameInfo(path);
    std::printf("       %s: frame %s, site %s%s\n", label.c_str(),
                qPrintable(SpecFetch::frameName(fi.frame)),
                fi.site.known ? qPrintable(fi.site.source) : "unstated",
                std::isnan(fi.statedCorrectionKms)
                    ? ""
                    : qPrintable(QStringLiteral(", %1 = %2 km/s")
                                     .arg(fi.statedCorrectionKey)
                                     .arg(fi.statedCorrectionKms, 0, 'f', 4)));

    // A corrected product gets the same check: the import records the shift
    // its wavelengths already carry so the fit can seed the telluric
    // component with it, and that number has to agree with the pipeline's.
    if (fi.frame == SpecFetch::Frame::Unknown) return;
    // Same fallback the fetch service uses when the archive record carried no
    // position: the telescope pointing out of the file.
    if (std::isnan(raDeg))  raDeg  = fi.targetRaDeg;
    if (std::isnan(decDeg)) decDeg = fi.targetDecDeg;

    if (std::isnan(fi.statedCorrectionKms) || !fi.site.known ||
        std::isnan(raDeg) || std::isnan(decDeg) ||
        !p.spectrum || p.spectrum->getMJD() <= 0.0)
        return;

    double epoch = p.spectrum->getMJD();
    if (p.spectrum->getExposureTime() > 0.0)
        epoch += p.spectrum->getExposureTime() / 2.0 / 86400.0;
    const double ours = SpecFetch::bervKms(epoch, raDeg, decDeg, fi.site);

    // The pipeline evaluates its own value at the true mid-exposure with the
    // real ephemeris; 0.1 km/s is a fifth of Earth's rotation speed and far
    // more than either difference can explain, so anything above it means the
    // site, the epoch or the sign is wrong.
    check(std::fabs(ours - fi.statedCorrectionKms) < 0.1,
          label + ": computed BERV " + std::to_string(ours) +
              " km/s matches the header's " +
              std::to_string(fi.statedCorrectionKms) + " km/s");
}

QString envPath(const char* var) {
    const char* v = std::getenv(var);
    return v ? QString::fromLocal8Bit(v) : QString();
}

}   // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    // ── ESO Phase 3 ──────────────────────────────────────────────────────
    if (const QString path = envPath("ASTRA_TEST_ESO_FITS"); !path.isEmpty()) {
        ++gRun;
        EsoArchiveClient client;
        SpecFetch::RemoteSpectrum r;
        r.originId       = "eso:TEST";
        r.instrumentHint = "XSHOOTER";
        SpecFetch::ArchiveOptions opt;
        QString err;
        const auto parsed = client.parse(path, r, opt, &err);
        check(err.isEmpty(), "ESO: no error (" + err.toStdString() + ")");
        check(parsed.size() == 1, "ESO: one spectrum");
        if (!parsed.empty()) {
            plausibleSpectrum(parsed[0], 900.0, 30000.0, "ESO");
            // ESO products carry RA/DEC in the primary header, but the fetch
            // pipeline takes the position from the archive query; pass the
            // target's own if ASTRA_TEST_ESO_RADEC is set as "ra,dec".
            const QString radec = envPath("ASTRA_TEST_ESO_RADEC");
            const QStringList parts = radec.split(QLatin1Char(','));
            if (parts.size() == 2)
                reportFrame(path, parsed[0], parts[0].toDouble(),
                            parts[1].toDouble(), "ESO");
            else
                reportFrame(path, parsed[0], std::nan(""), std::nan(""),
                            "ESO");
        }
    }

    // ── SDSS full spec ───────────────────────────────────────────────────
    if (const QString path = envPath("ASTRA_TEST_SDSS_FULL"); !path.isEmpty()) {
        ++gRun;
        SdssOpticalArchiveClient client;
        SpecFetch::RemoteSpectrum r;
        r.originId       = "sdss-dr17:TEST";
        r.instrumentHint = "SDSS";
        r.mjd            = 52288;
        SpecFetch::ArchiveOptions opt;
        opt.vacToAir       = true;
        opt.fetchExposures = true;
        QString err;
        const auto parsed = client.parse(path, r, opt, &err);
        check(err.isEmpty(), "SDSS: no error (" + err.toStdString() + ")");
        check(!parsed.empty(), "SDSS: spectra parsed");
        int coadds = 0, exposures = 0;
        for (const auto& p : parsed) {
            if (p.isCoadd) ++coadds; else ++exposures;
            if (!p.isCoadd)
                check(p.originId.startsWith("sdss-dr17:TEST#"),
                      "SDSS: exposure originId suffixed (" +
                          p.originId.toStdString() + ")");
        }
        check(coadds == 1, "SDSS: exactly one coadd");
        check(exposures >= 2, "SDSS: several exposures (" +
                                  std::to_string(exposures) + ")");
        plausibleSpectrum(parsed[0], 3000.0, 11000.0, "SDSS coadd");
    }

    // ── LAMOST LRS ───────────────────────────────────────────────────────
    if (const QString path = envPath("ASTRA_TEST_LAMOST_LRS"); !path.isEmpty()) {
        ++gRun;
        LamostArchiveClient client(false);
        SpecFetch::RemoteSpectrum r;
        r.originId       = "lamost-dr7-lrs:TEST";
        r.instrumentHint = "LAMOST LRS";
        SpecFetch::ArchiveOptions opt;
        opt.vacToAir = true;
        QString err;
        const auto parsed = client.parse(path, r, opt, &err);
        check(err.isEmpty(), "LRS: no error (" + err.toStdString() + ")");
        check(parsed.size() == 1, "LRS: one spectrum");
        if (!parsed.empty())
            plausibleSpectrum(parsed[0], 3000.0, 10000.0, "LRS");
    }

    // ── LAMOST LRS single exposures (sedr5) ──────────────────────────────
    // Optional companion: ASTRA_TEST_LAMOST_LRS in the same directory acts
    // as the coadd anchor; without it the FLUXCORR-only fallback is covered.
    if (const QString path = envPath("ASTRA_TEST_LAMOST_SEXP");
        !path.isEmpty()) {
        ++gRun;
        LamostArchiveClient client(false);
        SpecFetch::RemoteSpectrum r;
        r.originId       = "lamost-sexp-lrs:TEST";
        r.instrumentHint = "LAMOST LRS";
        const QString coadd = envPath("ASTRA_TEST_LAMOST_LRS");
        if (!coadd.isEmpty())
            r.extras.insert("coaddFileName", QFileInfo(coadd).fileName());
        SpecFetch::ArchiveOptions opt;
        opt.vacToAir = true;
        QString err;
        const auto parsed = client.parse(path, r, opt, &err);
        check(err.isEmpty(), "sexp: no error (" + err.toStdString() + ")");
        check(parsed.size() >= 2,
              "sexp: several exposures (" + std::to_string(parsed.size()) +
                  ")");
        for (const auto& p : parsed) {
            check(!p.isCoadd, "sexp: flagged as exposure");
            check(p.originId.startsWith("lamost-sexp-lrs:TEST#"),
                  "sexp: originId child suffix (" + p.originId.toStdString() +
                      ")");
        }
        if (!parsed.empty() &&
            plausibleSpectrum(parsed[0], 3400.0, 9400.0, "sexp exposure")) {
            // Both arms merged: the exposure must span blue AND red.
            const auto wl = parsed[0].spectrum->getWavelengths();
            check(wl.front() < 4500.0 && wl.back() > 8000.0,
                  "sexp: arms merged into one spectrum");
        }
    }

    // ── LAMOST MRS ───────────────────────────────────────────────────────
    if (const QString path = envPath("ASTRA_TEST_LAMOST_MRS"); !path.isEmpty()) {
        ++gRun;
        LamostArchiveClient client(true);
        SpecFetch::RemoteSpectrum r;
        r.originId       = "lamost-mrs:TEST";
        r.instrumentHint = "LAMOST MRS";
        SpecFetch::ArchiveOptions opt;
        opt.vacToAir = true;

        // Coadd mode: exactly the two coadd arms.
        opt.fetchExposures = false;
        QString err;
        auto parsed = client.parse(path, r, opt, &err);
        check(err.isEmpty(), "MRS coadds: no error (" + err.toStdString() +
                                 ")");
        int coadds = 0, exposures = 0;
        for (const auto& p : parsed) (p.isCoadd ? coadds : exposures)++;
        check(coadds == 2 && exposures == 0,
              "MRS coadds: two coadd arms, no exposures (" +
                  std::to_string(coadds) + "/" + std::to_string(exposures) +
                  ")");
        if (!parsed.empty())
            plausibleSpectrum(parsed[0], 4800.0, 7000.0, "MRS arm");

        // Exposure mode: the exposures replace the coadd arms.
        opt.fetchExposures = true;
        parsed = client.parse(path, r, opt, &err);
        check(err.isEmpty(), "MRS exposures: no error (" + err.toStdString() +
                                 ")");
        coadds = exposures = 0;
        for (const auto& p : parsed) (p.isCoadd ? coadds : exposures)++;
        check(coadds == 0 && exposures >= 2,
              "MRS exposures: exposures only (" + std::to_string(coadds) +
                  "/" + std::to_string(exposures) + ")");
        if (!parsed.empty())
            plausibleSpectrum(parsed[0], 4800.0, 7000.0, "MRS exposure");
    }

    // ── APOGEE apStar ────────────────────────────────────────────────────
    if (const QString path = envPath("ASTRA_TEST_APSTAR"); !path.isEmpty()) {
        ++gRun;
        ApogeeArchiveClient client;
        SpecFetch::RemoteSpectrum r;
        r.originId       = "apogee-dr17:TEST";
        r.instrumentHint = "APOGEE";
        SpecFetch::ArchiveOptions opt;
        opt.vacToAir       = true;
        opt.fetchExposures = true;
        QString err;
        const auto parsed = client.parse(path, r, opt, &err);
        check(err.isEmpty(), "APOGEE: no error (" + err.toStdString() + ")");
        int coadds = 0, visits = 0;
        for (const auto& p : parsed) (p.isCoadd ? coadds : visits)++;
        check(coadds == 1, "APOGEE: one coadd");
        check(visits >= 1, "APOGEE: visits present (" +
                               std::to_string(visits) + ")");
        if (!parsed.empty())
            plausibleSpectrum(parsed[0], 15000.0, 17100.0, "APOGEE coadd");
    }

    // ── MAST (HST / FUSE) ────────────────────────────────────────────────
    // Both product families store the spectrum as a single table row of
    // N-element vector columns, which the generic FITS reader used to read as
    // one point - the spectra imported empty.
    struct MastCase {
        const char* var;
        const char* label;
        const char* collection;
        const char* instrument;
        double      wlMin, wlMax;
    };
    for (const MastCase& c :
         {MastCase{"ASTRA_TEST_MAST_HST", "MAST HST", "HST", "STIS", 900.0,
                   12000.0},
          MastCase{"ASTRA_TEST_MAST_FUSE", "MAST FUSE", "FUSE", "FUV", 890.0,
                   1200.0}}) {
        const QString path = envPath(c.var);
        if (path.isEmpty()) continue;
        ++gRun;
        MastArchiveClient client;
        SpecFetch::RemoteSpectrum r;
        r.archive    = SpecFetch::Archive::MastSSAP;
        r.collection = QString::fromLatin1(c.collection);
        r.originId   = QStringLiteral("mast-%1:TEST")
                           .arg(r.collection.toLower());
        r.instrumentHint = QStringLiteral("%1/%2").arg(
            QString::fromLatin1(c.collection), QString::fromLatin1(c.instrument));
        SpecFetch::ArchiveOptions opt;
        QString err;
        const auto parsed = client.parse(path, r, opt, &err);
        check(err.isEmpty(),
              std::string(c.label) + ": no error (" + err.toStdString() + ")");
        check(parsed.size() == 1, std::string(c.label) + ": one spectrum");
        if (!parsed.empty()) {
            plausibleSpectrum(parsed[0], c.wlMin, c.wlMax, c.label);
            // The mission-qualified name from the archive, not the bare
            // "FUV"/"STIS" the file header carries.
            check(parsed[0].spectrum &&
                      parsed[0].spectrum->getInstrument() == r.instrumentHint,
                  std::string(c.label) + ": instrument " +
                      (parsed[0].spectrum
                           ? parsed[0].spectrum->getInstrument().toStdString()
                           : std::string("(none)")));
        }
    }

    if (gRun == 0) {
        std::printf("SKIPPED - no ASTRA_TEST_* fixture paths set\n");
        return 0;
    }
    std::printf("%s (%d case group(s), %d failure(s))\n",
                gFailures ? "FAILED" : "PASSED", gRun, gFailures);
    return gFailures ? 1 : 0;
}
