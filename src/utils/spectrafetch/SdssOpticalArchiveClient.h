// src/utils/spectrafetch/SdssOpticalArchiveClient.h
//
// SDSS optical spectra (SDSS-I/II, BOSS, eBOSS). Discovery via the SkyServer
// SqlSearch REST endpoint (batched VALUES + fGetNearbySpecObjEq crossmatch),
// download from the SAS (spec-lite, or full spec files when individual
// exposures are requested).

#ifndef SDSSOPTICALARCHIVECLIENT_H
#define SDSSOPTICALARCHIVECLIENT_H

#include "SpectrumArchiveClient.h"

class SdssOpticalArchiveClient : public SpectrumArchiveClient {
public:
    SpecFetch::Archive archive() const override {
        return SpecFetch::Archive::SdssOptical;
    }
    QString displayName() const override {
        return QStringLiteral("SDSS optical");
    }
    QString hostKey() const override {
        return QStringLiteral("data.sdss.org");
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

    // Vacuum wavelengths, heliocentric frame.
    bool deliversVacuumWavelengths() const override { return true; }
    SpecFetch::Frame declaredFrame(
        const SpecFetch::RemoteSpectrum& r) const override {
        // The idlspec2d pipeline shifts the wavelength solution to the
        // heliocentric frame before coadding, so the per-exposure HDUs of a
        // spec- file are on the same corrected scale as the coadd.
        Q_UNUSED(r);
        return SpecFetch::Frame::Heliocentric;
    }

    // MJD comes from TAI-BEG, the start of the exposure.
    bool reportsExposureStart() const override { return true; }

    static QStringList knownDataReleases();   // for the setup UI
};

#endif   // SDSSOPTICALARCHIVECLIENT_H
