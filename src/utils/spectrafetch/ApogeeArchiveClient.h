// src/utils/spectrafetch/ApogeeArchiveClient.h
//
// SDSS APOGEE H-band spectra (DR17). Discovery via SkyServer SQL
// (fGetNearbyApogeeStarEq + apogeeStar); downloads are apStar/asStar files
// from the SAS. An apStar file carries the coadd in its first image row and
// the individual visits in the rows after the second, so "individual
// exposures" needs no extra downloads.

#ifndef APOGEEARCHIVECLIENT_H
#define APOGEEARCHIVECLIENT_H

#include "SpectrumArchiveClient.h"

class ApogeeArchiveClient : public SpectrumArchiveClient {
public:
    SpecFetch::Archive archive() const override {
        return SpecFetch::Archive::Apogee;
    }
    QString displayName() const override {
        return QStringLiteral("SDSS APOGEE");
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

    // Vacuum wavelengths; visit spectra are shifted to the star's rest
    // heliocentric frame in the apStar combination.
    bool deliversVacuumWavelengths() const override { return true; }
    SpecFetch::Frame declaredFrame(
        const SpecFetch::RemoteSpectrum& r) const override {
        Q_UNUSED(r);
        return SpecFetch::Frame::Barycentric;
    }
};

#endif   // APOGEEARCHIVECLIENT_H
