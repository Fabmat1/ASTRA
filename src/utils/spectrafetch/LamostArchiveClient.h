// src/utils/spectrafetch/LamostArchiveClient.h
//
// LAMOST low- and medium-resolution spectra. Discovery via the per-DR
// anonymous cone-search services (dr4..dr7.lamost.org for the old releases,
// www.lamost.org/dr8..dr11 for the newer ones - all verified to answer
// without a token; one can still be supplied and is appended to every URL).
// Downloads are gzipped FITS by obsid (cfitsio reads .fits.gz natively).
//
// LRS coadds come in two layouts: a 5-row image (DR7 and older: flux, ivar,
// wavelength, andmask, ormask) or a COADD bintable of vector columns (DR8+).
// MRS products bundle COADD_B/COADD_R plus the individual exposures as extra
// bintable HDUs, so "individual exposures" is native there.
//
// The exposures option is an either/or: when set, individual exposures are
// fetched instead of the coadd wherever they exist, with the coadd as the
// fallback. For MRS the exposures come from the product file itself; LRS
// exposures are not part of the regular releases but live in the separate
// single-exposure data release (Bai et al. 2021, RAA 21, 249) hosted by
// NADC, covering observations from Oct 2011 to Jun 2017 - discovery swaps
// the coadd row for the single-exposure product when that release has it,
// and parse() splits the b/r arms per exposure, transfers the coadd's flux
// calibration, and merges each exposure into one spectrum.

#ifndef LAMOSTARCHIVECLIENT_H
#define LAMOSTARCHIVECLIENT_H

#include "SpectrumArchiveClient.h"

class LamostArchiveClient : public SpectrumArchiveClient {
public:
    explicit LamostArchiveClient(bool mrs) : _mrs(mrs) {}

    SpecFetch::Archive archive() const override {
        return _mrs ? SpecFetch::Archive::LamostMRS
                    : SpecFetch::Archive::LamostLRS;
    }
    QString displayName() const override {
        return _mrs ? QStringLiteral("LAMOST MRS")
                    : QStringLiteral("LAMOST LRS");
    }
    QString hostKey() const override {
        return QStringLiteral("lamost.org");
    }

    QList<SpecFetch::RemoteSpectrum> discover(
        const std::vector<SpecFetch::StarQuery>& stars,
        const SpecFetch::ArchiveOptions& opt,
        QNetworkAccessManager* nam,
        const std::function<void(int, int)>& progress,
        const std::atomic<bool>& cancel,
        QString* error) override;

    std::vector<SpecFetch::ParsedSpectrum> parse(
        const QString& localPath,
        const SpecFetch::RemoteSpectrum& r,
        const SpecFetch::ArchiveOptions& opt,
        QString* error) override;

    bool deliversVacuumWavelengths() const override { return true; }
    SpecFetch::Frame declaredFrame(
        const SpecFetch::RemoteSpectrum& r) const override {
        // LAMOST pipelines correct the wavelength solution to heliocentric
        // (single exposures included).
        Q_UNUSED(r);
        return SpecFetch::Frame::Heliocentric;
    }

    // The epochs this client sets already have half the exposure added.
    bool reportsExposureStart() const override { return false; }

    /// Data releases offered in the setup UI, newest first.
    static QStringList knownDataReleases(bool mrs);
    /// The release searched when none is picked (the newest LRS one).
    static QString defaultDataRelease();

private:
    bool _mrs = false;
};

#endif   // LAMOSTARCHIVECLIENT_H
