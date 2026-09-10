// src/utils/spectrafetch/SpectrumArmJoin.h
//
// Joining the spectral arms of one exposure into a single spectrum.
//
// Many instruments split the light of a single exposure over several arms and
// publish one reduced product per arm: X-Shooter (UVB/VIS/NIR), UVES with a
// dichroic (BLUE/RED), LAMOST MRS (B/R), the SDSS spectrograph cameras
// (b1/r1), and so on. Fetched as they come, those arrive as two or three
// separate spectra of the same epoch, each covering a fraction of the range.
//
// This is the archive-independent half of putting them back together:
//
//   1. `groupArms` decides which spectra belong to the same exposure, from
//      the epoch, the exposure time and the wavelength coverage alone. Two
//      spectra are arms of one exposure when they are close in time *and*
//      cover largely different wavelengths; two exposures in the same arm
//      overlap almost completely and are never merged.
//   2. `spliceArms` concatenates a group's samples, cutting each overlap at
//      its midpoint so the result stays strictly increasing and no pixel is
//      counted twice. Fluxes are not rescaled: archival arms of one exposure
//      are published on a common flux system, and silently stretching one of
//      them would be a data change the user never asked for.
//      `armFluxRatioInOverlap` is there to report it when they disagree.
//
// Everything here works on plain vectors so it can be unit tested without the
// database, the network or a Spectrum.

#ifndef SPECTRUMARMJOIN_H
#define SPECTRUMARMJOIN_H

#include <QString>
#include <QStringList>

#include <vector>

namespace SpecFetch {

struct ArmJoinOptions {
    /// Epochs this far apart can still belong to one exposure. Arms of an
    /// exposure are started within seconds of each other, but the recorded
    /// stamp is a start time for one archive and a mid-exposure time for the
    /// next, so the window is a generous one; a wrong pairing is caught by
    /// the wavelength-overlap test rather than by this.
    double maxTimeSeparationSec = 300.0;
    /// Two spectra sharing more than this fraction of the narrower one's
    /// range are the same arm, not two arms.
    double maxOverlapFraction = 0.5;
};

/// One spectrum's metadata, as far as the grouping needs it.
struct ArmMeta {
    /// Star, archive and instrument identity. Only spectra with an equal key
    /// are ever joined.
    QString groupKey;
    double  mjd         = 0.0;   ///< <= 0: unknown, and never joined
    double  exposureSec = 0.0;   ///< <= 0: unknown
    double  wlMin       = 0.0;   ///< Angstrom
    double  wlMax       = 0.0;
};

/// Instrument name with an arm/channel suffix removed, upper case:
/// "LAMOST MRS blue" and "UVES/RED" become "LAMOST MRS" and "UVES", so the
/// arms of one instrument share a group key. Names that consist of nothing
/// but an arm word are returned unchanged.
QString armInstrumentBase(const QString& instrument);

/// Fraction of the narrower wavelength range the two spectra share: 0 for
/// disjoint arms, 1 when one contains the other.
double armOverlapFraction(const ArmMeta& a, const ArmMeta& b);

/// Whether the two epochs can be the same exposure. The window widens with
/// the exposure time, which covers archives stamping mid-exposure against
/// archives stamping the start.
bool armsShareExposure(const ArmMeta& a, const ArmMeta& b,
                       const ArmJoinOptions& opt);

/// Partition `metas` into groups of arms. Every input ends up in exactly one
/// group; groups of one are spectra that have nothing to be joined with.
/// Groups and the indices inside them are returned in input order.
std::vector<std::vector<int>> groupArms(const std::vector<ArmMeta>& metas,
                                        const ArmJoinOptions&       opt);

/// One arm's samples. `errors` may be empty or the same length as
/// `wavelengths`.
struct ArmSegment {
    std::vector<double> wavelengths;   ///< Angstrom
    std::vector<double> fluxes;
    std::vector<double> errors;
};

/// Splice `segments` into one spectrum, cutting overlaps at their midpoint.
/// Segments are ordered by wavelength internally, so the caller may pass them
/// in any order. Returns false when fewer than two usable segments are given
/// or the result would be empty.
bool spliceArms(std::vector<ArmSegment> segments, ArmSegment* out);

/// Median flux ratio of `b` over `a` where the two overlap, computed on the
/// nearest samples of `a`. Returns NaN when they do not overlap in enough
/// pixels for the number to mean anything. A value far from 1 means the two
/// arms are not on the same flux system.
double armFluxRatioInOverlap(const ArmSegment& a, const ArmSegment& b);

// ── Origin ids of joined spectra ─────────────────────────────────────────────
//
// A joined spectrum replaces the products it was made of, so its origin id
// has to stand in for all of them: it is their ids concatenated with '+'.
// That keeps the dedup key stable and lets a later fetch recognize every
// member product as already imported.

/// "eso:A+eso:B" for the members, in the order given. A single member is
/// returned unchanged.
QString joinedOriginId(const QStringList& memberOriginIds);

/// Whether `existingId`, as stored on an imported spectrum, accounts for the
/// product `productId`: as the id itself, as one of its children
/// ("<productId>#<part>"), or as a member of a joined id.
bool originIdCovers(const QString& existingId, const QString& productId);

}   // namespace SpecFetch

#endif   // SPECTRUMARMJOIN_H
