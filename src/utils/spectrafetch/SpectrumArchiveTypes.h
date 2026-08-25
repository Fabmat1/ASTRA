// src/utils/spectrafetch/SpectrumArchiveTypes.h
//
// Shared value types for the online spectrum archive clients and the
// SpectrumFetchService that drives them.

#ifndef SPECTRUMARCHIVETYPES_H
#define SPECTRUMARCHIVETYPES_H

#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariantMap>

#include <cmath>
#include <memory>

class Spectrum;

namespace SpecFetch {

enum class Archive {
    LamostLRS,
    LamostMRS,
    SdssOptical,
    EsoPhase3,
    MastSSAP,
    Apogee,
};

// Stable key used in settings, origin strings, and download paths.
QString archiveKey(Archive a);
QString archiveDisplayName(Archive a);

// One project star, resolved on the GUI thread before discovery starts.
struct StarQuery {
    QString starId;              // Star::getId() (UUID)
    QString gaiaId;              // Star::getSourceId(), may be empty
    double  ra  = std::nan(""); // deg, ICRS
    double  dec = std::nan("");
    QString label;               // alias or Gaia id, for logs/UI

    bool hasPosition() const { return !std::isnan(ra) && !std::isnan(dec); }
};

// One product an archive offers for download.
struct RemoteSpectrum {
    Archive  archive = Archive::EsoPhase3;
    QString  archiveLabel;       // "ESO Phase 3", "LAMOST DR8 LRS", ...
    QString  originId;           // stable dedup key, e.g. "eso:<dp_id>"
    QString  starId;             // which StarQuery it matched
    QString  instrumentHint;     // "UVES", "SDSS/BOSS", "LAMOST/LRS", ...
    QString  collection;         // ESO obs_collection / SDSS survey / mission
    double   mjd        = std::nan("");
    double   ra         = std::nan("");
    double   dec        = std::nan("");
    double   sepArcsec  = std::nan("");   // match distance from the star
    double   resolution = std::nan("");
    double   snr        = std::nan("");
    qint64   sizeBytes  = -1;    // -1 = unknown
    bool     isCoadd    = true;  // false only when the product IS an exposure
    QUrl     downloadUrl;        // may need resolveDownloadUrl (ESO DataLink)
    QString  fileName;           // suggested local file name
    QVariantMap extras;          // client-private context carried to parse()
};

// One Spectrum extracted from a downloaded file (a file can hold several:
// coadd + individual exposures, or the two LAMOST MRS arms).
struct ParsedSpectrum {
    std::shared_ptr<Spectrum> spectrum;   // wl [Angstrom] / flux / err set
    QString originId;                     // parent originId, "#expN" suffixed
    bool    isCoadd = true;
    QString instrumentHint;
};

// Per-archive knobs from the setup dialog / settings.
struct ArchiveOptions {
    bool        fetchExposures = false;  // also fetch individual exposures
    QString     dataRelease;             // "DR7", "DR17", ... (archive-specific)
    QStringList collections;             // ESO instruments / MAST missions
    double      radiusArcsec = 3.0;
    QString     token;                   // LAMOST token for newest DRs
    bool        vacToAir = true;         // convert vacuum wavelengths to air
};

}   // namespace SpecFetch

#endif   // SPECTRUMARCHIVETYPES_H
