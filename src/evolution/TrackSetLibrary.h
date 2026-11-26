#pragma once

// The evolutionary track sets available on this machine, and the subsets a fit uses.
//
// A tracks folder (EvolutionSettings::tracksFolder()) holds files written by
// scripts/export_tracks.py (<family>.sdtracks.fits.gz) and, for Han, the original
// multi-extension FITS file (han*_extended*.fits.gz) with an optional
// <stem>.scipy_tri.txt next to it.  The tracks are unpublished; nothing derived from
// them is bundled or committed.
//
// Subsets follow sdfit's track selection, per family:
//   Cores      Han: tick core masses (>= 2, exact grid nodes)
//   MassRange  Nicolas, Driebe (and Dorman files without composition labels): a mass
//              range, snapped to the tracks
//   Groups     Dorman with composition labels: compositions + the N thinnest
//              envelopes of each (sdfit's default: all compositions, 3 envelopes)
// A 2-D subset is re-triangulated (sdage::delaunay) exactly as sdfit triangulates
// whatever it loads.

#include "evolution/sdage/fit.hpp"

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace astra::evolution {

enum class SubsetKind { Cores, MassRange, Groups };

/// One track family found in the folder.
struct TrackFamilyInfo {
    QString key;           ///< "han", "nicolas", "dorman", "driebe", ... (stored with fits)
    QString displayName;   ///< "Han", ...
    QString description;   ///< one line for the UI
    QString path;          ///< the track file
    QString fileName;      ///< its name (stored with fits)
    QString triangulationPath;  ///< Han FITS only: scipy's triangulation, if present
    QString sha1;          ///< of the track file
    QString error;         ///< non-empty: the file is unusable, and why

    int nTracks = 0;
    int nEep = 0;
    int ndim = 2;
    bool hasHelium = false;
    bool rectangular = false;    ///< Han FITS grid (bilinear or scipy-triangulated)
    SubsetKind subsetKind = SubsetKind::MassRange;
    QVector<double> coreMasses;  ///< distinct, sorted (total masses for a 1-D family)
    QVector<double> envMasses;   ///< distinct, sorted
    QStringList groups;          ///< composition labels, ordered by [Fe/H]
    QVector<double> groupFeh;    ///< [Fe/H] per entry of groups (NaN if unknown)
    int maxEnvelopesPerGroup = 0;

    bool usable() const { return error.isEmpty(); }
    /// "Han: 27 tracks, 9 core masses 0.35-0.75 Msun, ..." for lists and tool tips.
    QString summary() const;
};

/// Which tracks of a family a fit uses.  Unused members keep their defaults.
struct TrackSubset {
    QVector<double> cores;  ///< Cores: the core masses kept; empty = all
    double massLo = std::numeric_limits<double>::quiet_NaN();  ///< MassRange; NaN = open
    double massHi = std::numeric_limits<double>::quiet_NaN();
    QStringList groups;     ///< Groups: compositions kept; empty = all
    int envelopes = 3;      ///< Groups: thinnest envelopes per composition; 0 = all

    QJsonObject toJson(SubsetKind kind) const;
    static TrackSubset fromJson(const QJsonObject& o);
    bool operator==(const TrackSubset& o) const;
};

/// Prior on the EEP axis (sdfit: none / --age-prior / --sdB_Prior / --sdO_Prior).
enum class EepPriorChoice { None, Age, SdB, SdO };

/// Sampler and model settings of a run (stored as config_json with every fit).
struct TrackModelSettings {
    EepPriorChoice eepPrior = EepPriorChoice::None;
    bool massPrior = false;        ///< Gaussian prior on the core mass from the SED mass
    int walkers = 1000;            ///< sdfit's defaults
    int burn = 800;
    int steps = 1000;
    quint64 seed = 1;              ///< run seed; star i of a run uses mix_seed(seed, i)
    bool hanScipyInterpolation = false;  ///< Han FITS: scipy's triangles instead of bilinear

    QJsonObject toJson() const;
    static TrackModelSettings fromJson(const QJsonObject& o);
    sdage::FitConfig toFitConfig() const;
    static QString eepPriorName(EepPriorChoice p);  ///< "none", "age", "sdb", "sdo"
};

/// A family with its subset applied, ready for sdage::Fitter.
struct PreparedTrackSet {
    TrackFamilyInfo family;
    TrackSubset subset;                          ///< normalised (snapped to the tracks)
    std::shared_ptr<const sdage::TrackGrid> full;  ///< every track of the file
    std::shared_ptr<const sdage::TrackGrid> grid;  ///< the subset (may be == full)
    std::vector<bool> keep;                      ///< per node of `full`
    QString interpolation;                       ///< "bilinear", "scipy triangles", "Delaunay"
    QString summary;                             ///< e.g. "Han, core masses 0.45-0.60 (4 tracks)"
};

/// Track lines for plotting: every track of the family, flagged when in the subset.
struct TrackCurves {
    QVector<QVector<QPointF>> lines;  ///< x = log10 Teff, y = log10 L or log g
    QVector<QVector<double>> ages;    ///< age [Myr] at each point of `lines`
    QVector<bool> inSubset;
    QVector<double> core, env;
    QStringList group;
};

enum class CurvePlane { HRD, Kiel };  ///< (log Teff, log L) or (log Teff, log g)

class TrackSetLibrary {
public:
    explicit TrackSetLibrary(QString folder = QString());

    /// The library of EvolutionSettings::tracksFolder(); follows changes of the folder.
    static TrackSetLibrary& shared();

    void setFolder(const QString& folder);
    QString folder() const;

    /// Families in the folder (cached; files that changed on disk are re-read).
    /// Unusable files are listed with TrackFamilyInfo::error set.
    QVector<TrackFamilyInfo> families();
    /// One family by key; nullopt-like: key empty in the result if not found.
    TrackFamilyInfo family(const QString& key);

    /// sdfit's default subset of a family (all tracks; Dorman: 3 envelopes).
    static TrackSubset defaultSubset(const TrackFamilyInfo& f);

    /// Apply `subset` (and, for Han, the interpolation choice).  Returns null and sets
    /// `whyNot` (with a suggestion where there is one) if the subset gives no usable grid.
    std::shared_ptr<const PreparedTrackSet> prepare(const QString& familyKey, const TrackSubset& subset,
                                                    const TrackModelSettings& model, QString* whyNot);

    static TrackCurves curves(const PreparedTrackSet& p, CurvePlane plane);

    /// Display name of a family key that may not be present on this machine.
    static QString displayName(const QString& key);

private:
    struct CacheEntry {
        qint64 size = -1;
        qint64 mtimeMs = 0;
        QString sha1;
        QString error;
        std::shared_ptr<const sdage::TrackGrid> grid;
    };
    // Loads (or returns the cached) grid of a file.
    CacheEntry loadFile(const QString& path);
    TrackFamilyInfo describe(const QString& key, const QString& path, const QString& tri,
                             const CacheEntry& e) const;

    mutable QMutex _mutex;
    QString _folder;
    QHash<QString, CacheEntry> _cache;  // by absolute path
};

}  // namespace astra::evolution
