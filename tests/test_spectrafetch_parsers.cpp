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
// plausible range, exposures/arms split as expected, originId suffixes, the
// wavelength reference frame the headers state, and - for LAMOST - that the
// epochs are mid-exposure UTC rather than the Beijing-time cards the same
// headers also carry.
// ─────────────────────────────────────────────────────────────────────────────
#include "models/Spectrum.h"
#include "utils/SpectrumReader.h"
#include "utils/spectrafetch/ApogeeArchiveClient.h"
#include "utils/spectrafetch/EsoArchiveClient.h"
#include "utils/spectrafetch/LamostArchiveClient.h"
#include "utils/spectrafetch/MastArchiveClient.h"
#include "utils/spectrafetch/SdssOpticalArchiveClient.h"
#include "utils/spectrafetch/SpectrumFrame.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTime>
#include <QTimeZone>

#include <fitsio.h>

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


// ── LAMOST epoch helpers ─────────────────────────────────────────────────
//
// LAMOST writes both clocks into every header: DATE-OBS and the MJD card are
// UTC, DATE-BEG / DATE-END / LMJD / LMJM / MJM are Beijing time (UTC+8). The
// file therefore checks itself - an epoch built from a local card without the
// shift lands exactly 8 hours (0.3333 d) after the UTC one, and that constant
// offset is invisible in an RV fit while wrecking any phasing against a light
// curve. These read the UTC truth straight out of the file.

constexpr double kBeijingOffsetDays = 8.0 / 24.0;

// Same permissive parse the client uses: LAMOST writes "...T11:30:0.000",
// which strict ISO parsing rejects.
double mjdFromLamostDate(const QString& raw) {
    QDateTime dt = QDateTime::fromString(raw, Qt::ISODate);
    if (!dt.isValid()) {
        const QStringList parts = raw.split(QLatin1Char('T'));
        if (parts.size() == 2) {
            const QDate d =
                QDate::fromString(parts[0], QStringLiteral("yyyy-MM-dd"));
            const QStringList hms = parts[1].split(QLatin1Char(':'));
            if (d.isValid() && hms.size() == 3)
                dt = QDateTime(d, QTime(hms[0].toInt(), hms[1].toInt(), 0),
                               QTimeZone::UTC)
                         .addMSecs(qint64(hms[2].toDouble() * 1000.0));
        }
    }
    if (!dt.isValid()) return NAN;
    dt.setTimeZone(QTimeZone::UTC);
    return dt.toMSecsSinceEpoch() / 86400000.0 + 40587.0;
}

double headerMjd(fitsfile* f, const char* key) {
    char val[FLEN_VALUE] = {0};
    int  st              = 0;
    if (fits_read_key(f, TSTRING, key, val, nullptr, &st) != 0) return NAN;
    return mjdFromLamostDate(QString::fromLatin1(val).trimmed());
}

// One date card, from whichever HDU of `path` carries it first.
double headerMjdOfFile(const QString& path, const char* key) {
    fitsfile* f      = nullptr;
    int       status = 0;
    if (fits_open_file(&f, path.toUtf8().constData(), READONLY, &status) != 0)
        return NAN;
    int nHdus = 0;
    fits_get_num_hdus(f, &nHdus, &status);
    double out = NAN;
    for (int hdu = 1; hdu <= nHdus && std::isnan(out); ++hdu) {
        status = 0;
        if (fits_movabs_hdu(f, hdu, nullptr, &status) != 0) continue;
        out = headerMjd(f, key);
    }
    fits_close_file(f, &status);
    return out;
}

// Mid-exposure UTC MJD of every exposure HDU, from that HDU's own DATE-OBS.
std::vector<double> mrsExposureUtcMjds(const QString& path) {
    std::vector<double> out;
    fitsfile* f      = nullptr;
    int       status = 0;
    if (fits_open_file(&f, path.toUtf8().constData(), READONLY, &status) != 0)
        return out;
    int nHdus = 0;
    fits_get_num_hdus(f, &nHdus, &status);
    for (int hdu = 2; hdu <= nHdus; ++hdu) {
        int type = ANY_HDU;
        status   = 0;
        if (fits_movabs_hdu(f, hdu, &type, &status) != 0) continue;
        char name[FLEN_VALUE] = {0};
        int  st               = 0;
        if (fits_read_key(f, TSTRING, "EXTNAME", name, nullptr, &st) != 0)
            continue;
        const QString ext = QString::fromLatin1(name).trimmed().toUpper();
        if (!ext.startsWith(QLatin1String("B-")) &&
            !ext.startsWith(QLatin1String("R-")))
            continue;
        const double mjd = headerMjd(f, "DATE-OBS");
        if (!std::isnan(mjd)) out.push_back(mjd);
    }
    fits_close_file(f, &status);
    return out;
}

// The plate's observing window in UTC, from the Beijing DATE-BEG / DATE-END
// cards. The sedr5 single-exposure files leave the primary header a stub and
// put all metadata on the first bintable, so every HDU is tried. Returns
// false when the cards are missing.
bool plateUtcWindow(const QString& path, double* begUtc, double* endUtc) {
    fitsfile* f      = nullptr;
    int       status = 0;
    if (fits_open_file(&f, path.toUtf8().constData(), READONLY, &status) != 0)
        return false;
    int nHdus = 0;
    fits_get_num_hdus(f, &nHdus, &status);
    double beg = NAN, end = NAN;
    for (int hdu = 1; hdu <= nHdus && (std::isnan(beg) || std::isnan(end));
         ++hdu) {
        status = 0;
        if (fits_movabs_hdu(f, hdu, nullptr, &status) != 0) continue;
        if (std::isnan(beg)) beg = headerMjd(f, "DATE-BEG");
        if (std::isnan(end)) end = headerMjd(f, "DATE-END");
    }
    fits_close_file(f, &status);
    if (std::isnan(beg) || std::isnan(end) || end <= beg) return false;
    *begUtc = beg - kBeijingOffsetDays;
    *endUtc = end - kBeijingOffsetDays;
    return true;
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
        // The generic reader is what a hand-dropped file goes through, and
        // LAMOST files an integer night under the MJD card (MJD = 56579) next
        // to the real epoch in DATE-OBS. Taking the card at face value would
        // scatter epochs by up to half a day.
        {
            DefaultFitsSpectrumReader reader;
            const SpectrumMetadata meta = reader.readMetadata(path);
            check(meta.mjd.has_value(), "LRS generic reader: found an epoch");
            if (meta.mjd.has_value()) {
                const double v = *meta.mjd;
                check(std::abs(v - std::round(v)) > 1e-6,
                      "LRS generic reader: epoch is not a whole day (" +
                          std::to_string(v) + ")");
                const double fromHeader = headerMjdOfFile(path, "DATE-OBS");
                check(!std::isnan(fromHeader) &&
                          std::abs(v - fromHeader) < 1.0 / 86400.0,
                      "LRS generic reader: epoch equals the file's DATE-OBS");
            }
        }
    }

    // ── LAMOST LRS single exposures (sedr5) ──────────────────────────────    // ── LAMOST LRS single exposures (sedr5) ──────────────────────────────
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

        // The sedr5 files carry no per-exposure UTC card - only the MJM
        // column, which is the *local* modified julian minute. The plate's
        // Beijing DATE-BEG/DATE-END bound the night, so every exposure epoch
        // has to land inside that window once it is shifted to UTC; taking
        // MJM for UTC puts them all 8 h past DATE-END.
        double begUtc = 0.0, endUtc = 0.0;
        if (plateUtcWindow(path, &begUtc, &endUtc)) {
            int inside = 0, total = 0;
            for (const auto& p : parsed) {
                if (!p.spectrum) continue;
                ++total;
                const double mjd = p.spectrum->getMJD();
                if (mjd >= begUtc - 120.0 / 86400.0 &&
                    mjd <= endUtc + 120.0 / 86400.0)
                    ++inside;
            }
            check(total > 0 && inside == total,
                  "sexp epochs: inside the plate's UTC observing window (" +
                      std::to_string(inside) + "/" + std::to_string(total) +
                      ")");
        } else {
            check(false, "sexp epochs: plate DATE-BEG/DATE-END readable");
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

        // Epochs must be mid-exposure UTC. Every exposure HDU states that in
        // its own DATE-OBS, so the file is its own reference; using LMJM (the
        // *local* modified julian minute) unshifted lands each exposure 8 h
        // late, which is what this pins down.
        const std::vector<double> refs = mrsExposureUtcMjds(path);
        check(!refs.empty(), "MRS epochs: file states UTC DATE-OBS per "
                             "exposure (" + std::to_string(refs.size()) + ")");
        if (!refs.empty()) {
            double worstSec = 0.0;
            int    offBy8h  = 0;
            for (const auto& p : parsed) {
                if (!p.spectrum) continue;
                const double mjd = p.spectrum->getMJD();
                double best = 1e9;
                for (const double r : refs)
                    best = std::min(best, std::abs(mjd - r));
                worstSec = std::max(worstSec, best * 86400.0);
                for (const double r : refs)
                    if (std::abs(mjd - r - kBeijingOffsetDays) < 120.0 / 86400.0)
                        ++offBy8h;
            }
            // DATE-OBS is truncated to the whole minute and the client prefers
            // the sub-second DATE-BEG/DATE-END midpoint, so they agree to well
            // under a minute, never to the second.
            check(worstSec < 90.0,
                  "MRS epochs: mid-exposure UTC, worst deviation " +
                      std::to_string(int(worstSec)) + " s");
            check(offBy8h == 0,
                  "MRS epochs: none sitting 8 h late on the Beijing clock (" +
                      std::to_string(offBy8h) + ")");
        }
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
