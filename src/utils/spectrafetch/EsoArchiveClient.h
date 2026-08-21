// src/utils/spectrafetch/EsoArchiveClient.h
//
// ESO Science Archive Phase-3 reduced 1D spectra (XSHOOTER, UVES, HARPS,
// FEROS, GIRAFFE, ...). Discovery via the ESO TAP service (ivoa.ObsCore)
// with a TAP_UPLOAD positional crossmatch; download via the archive's
// DataLink / dataportal endpoints.

#ifndef ESOARCHIVECLIENT_H
#define ESOARCHIVECLIENT_H

#include "SpectrumArchiveClient.h"

class EsoArchiveClient : public SpectrumArchiveClient {
public:
    SpecFetch::Archive archive() const override {
        return SpecFetch::Archive::EsoPhase3;
    }
    QString displayName() const override {
        return QStringLiteral("ESO Phase 3");
    }
    QString hostKey() const override {
        return QStringLiteral("archive.eso.org");
    }

    QList<SpecFetch::RemoteSpectrum> discover(
        const std::vector<SpecFetch::StarQuery>& stars,
        const SpecFetch::ArchiveOptions& opt,
        QNetworkAccessManager* nam,
        const std::function<void(int, int)>& progress,
        const std::atomic<bool>& cancel,
        QString* error) override;

    QUrl resolveDownloadUrl(const SpecFetch::RemoteSpectrum& r,
                            QNetworkAccessManager* nam,
                            QString* error) override;

    std::vector<SpecFetch::ParsedSpectrum> parse(
        const QString& localPath,
        const SpecFetch::RemoteSpectrum& r,
        const SpecFetch::ArchiveOptions& opt,
        QString* error) override;

    bool deliversVacuumWavelengths() const override { return false; }
    bool deliversBarycentric(const SpecFetch::RemoteSpectrum& r) const override;

    /// Phase-3 collections offered in the setup UI (obs_collection values).
    static QStringList knownCollections();
};

#endif   // ESOARCHIVECLIENT_H
