// src/utils/spectrafetch/SpectrumArchiveClient.h
//
// Abstract interface for one online spectrum archive. Discovery and parsing
// run synchronously on QtConcurrent worker threads (driven by
// SpectrumFetchService); downloads are handled by the service itself.

#ifndef SPECTRUMARCHIVECLIENT_H
#define SPECTRUMARCHIVECLIENT_H

#include "SpectrumArchiveTypes.h"
#include "SpectrumFrame.h"

#include <QList>

#include <atomic>
#include <functional>
#include <vector>

class QNetworkAccessManager;

class SpectrumArchiveClient {
public:
    virtual ~SpectrumArchiveClient() = default;

    virtual SpecFetch::Archive archive() const = 0;
    virtual QString displayName() const = 0;

    // Throttle bucket for downloads, e.g. "data.sdss.org". Downloads whose
    // URL host differs still count against this bucket so one archive never
    // hogs the queue.
    virtual QString hostKey() const = 0;

    // Batched discovery: find every matching product for the given stars.
    // Runs on a worker thread and may make several chunked synchronous HTTP
    // calls; implementations must poll `cancel` between chunks and report
    // progress(starsDone, starsTotal). On failure returns what it has and
    // sets *error.
    virtual QList<SpecFetch::RemoteSpectrum> discover(
        const std::vector<SpecFetch::StarQuery>& stars,
        const SpecFetch::ArchiveOptions& opt,
        QNetworkAccessManager* nam,
        const std::function<void(int, int)>& progress,
        const std::atomic<bool>& cancel,
        QString* error) = 0;

    // Resolve download indirection (ESO DataLink). Called on a worker thread
    // right before the download is queued. Default: r.downloadUrl unchanged.
    virtual QUrl resolveDownloadUrl(const SpecFetch::RemoteSpectrum& r,
                                    QNetworkAccessManager* nam,
                                    QString* error);

    // Parse one downloaded file into 1..N spectra (coadd, exposures, arms).
    // Runs on a worker thread; uses cfitsio. On failure returns empty and
    // sets *error.
    virtual std::vector<SpecFetch::ParsedSpectrum> parse(
        const QString& localPath,
        const SpecFetch::RemoteSpectrum& r,
        const SpecFetch::ArchiveOptions& opt,
        QString* error) = 0;

    // Positional match radius this archive actually searches with. Defaults
    // to the radius the user asked for; an archive whose recorded positions
    // are coarser than modern astrometry may widen it (see MAST).
    virtual double searchRadiusArcsec(const SpecFetch::ArchiveOptions& opt) const {
        return opt.radiusArcsec;
    }

    // Whether the archive delivers wavelengths in vacuum (candidates for the
    // vacuum-to-air conversion).
    virtual bool deliversVacuumWavelengths() const { return false; }

    // The wavelength reference frame this archive publishes, per product.
    // Only a fallback: the frame is read out of the file itself (SPECSYS, and
    // the pipeline switches that stand in for it) whenever the product states
    // it, and this answers for the ones that do not. Unknown means "leave the
    // wavelengths alone" - a spectrum that is silently corrected twice is
    // worse off than one that was never corrected at all.
    virtual SpecFetch::Frame declaredFrame(
        const SpecFetch::RemoteSpectrum& r) const {
        Q_UNUSED(r);
        return SpecFetch::Frame::Unknown;
    }

    // True when the epoch the client puts on a spectrum is the start of the
    // exposure rather than its midpoint, so the barycentric correction can be
    // evaluated half an exposure later. Worth up to ~60 m/s on a long one.
    virtual bool reportsExposureStart() const { return true; }
};

#endif   // SPECTRUMARCHIVECLIENT_H
