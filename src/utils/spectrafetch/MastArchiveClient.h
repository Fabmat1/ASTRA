// src/utils/spectrafetch/MastArchiveClient.h
//
// MAST reduced spectra (HST, IUE, FUSE, ...). Discovery via the MAST CAOM
// TAP service (ivoa.obscore) with chunked OR-chain positional crossmatches
// (the classic archive.stsci.edu SSAP endpoint no longer returns rows);
// downloads are the archive's calibrated FITS products, preferring the
// VO-normalized "*_vo.fits" spectra where offered.

#ifndef MASTARCHIVECLIENT_H
#define MASTARCHIVECLIENT_H

#include "SpectrumArchiveClient.h"

class MastArchiveClient : public SpectrumArchiveClient {
public:
    SpecFetch::Archive archive() const override {
        return SpecFetch::Archive::MastSSAP;
    }
    QString displayName() const override {
        return QStringLiteral("MAST (HST/IUE/FUSE)");
    }
    QString hostKey() const override {
        return QStringLiteral("archive.stsci.edu");
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

    // UV and HST products are on vacuum wavelengths throughout.
    bool deliversVacuumWavelengths() const override { return true; }
    bool deliversBarycentric(const SpecFetch::RemoteSpectrum& r) const override {
        // HST pipeline spectra are heliocentric; IUE/FUSE are not.
        return r.collection.compare(QStringLiteral("HST"),
                                    Qt::CaseInsensitive) == 0;
    }

    /// Missions offered in the setup UI (obs_collection values).
    static QStringList knownMissions();
};

#endif   // MASTARCHIVECLIENT_H
