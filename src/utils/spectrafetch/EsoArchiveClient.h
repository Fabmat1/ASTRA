// src/utils/spectrafetch/EsoArchiveClient.h
//
// ESO Science Archive Phase-3 reduced 1D spectra (XSHOOTER, UVES, HARPS,
// FEROS, GIRAFFE, ...). Download via the archive's DataLink / dataportal
// endpoints.
//
// Discovery takes one of two routes against ivoa.ObsCore, because the
// anonymous ESO TAP endpoint offers no TAP_UPLOAD and so cannot be handed a
// star list:
//
//   * a few stars, and no mirror on disk: an OR-chain of per-star RA/Dec
//     boxes submitted as an asynchronous job, for the reasons set out at the
//     top of the .cpp;
//   * a project-sized list, or a mirror that is already there: EsoObsCoreIndex
//     pulls the whole spectrum slice of ObsCore once and the crossmatch runs
//     locally. See that header for why the whole table is the cheap option.

#ifndef ESOARCHIVECLIENT_H
#define ESOARCHIVECLIENT_H

#include "EsoObsCoreIndex.h"
#include "SpectrumArchiveClient.h"

#include <QMutex>

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

    /// Star count at or above which discovery mirrors ObsCore instead of
    /// asking ESO per star. Below it, a handful of boxes is both faster for
    /// the user and lighter on the archive than pulling the index.
    static constexpr int kIndexStarThreshold = 250;

private:
    QList<SpecFetch::RemoteSpectrum> discoverViaIndex(
        const std::vector<SpecFetch::StarQuery>& stars,
        const SpecFetch::ArchiveOptions& opt, QNetworkAccessManager* nam,
        const std::function<void(int, int)>& progress,
        const std::atomic<bool>& cancel, QString* error);

    QList<SpecFetch::RemoteSpectrum> discoverViaBoxes(
        const std::vector<SpecFetch::StarQuery>& stars,
        const SpecFetch::ArchiveOptions& opt, QNetworkAccessManager* nam,
        const std::function<void(int, int)>& progress,
        const std::atomic<bool>& cancel, QString* error);

    // The registry hands out one client instance for every session, and
    // discovery runs on worker threads, so two sessions can be in here at
    // once. Serialising them is also what stops two of them building the
    // same mirror in parallel.
    QMutex                    _indexMutex;
    SpecFetch::EsoObsCoreIndex _index;
};

#endif   // ESOARCHIVECLIENT_H
