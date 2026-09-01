// src/utils/spectrafetch/SpectrumFrame.h
//
// Spectral reference frame of a downloaded archive product, and the velocity
// correction that moves a topocentric wavelength scale onto the solar system
// barycentre.
//
// Archives disagree about this: HARPS, ESPRESSO, FEROS, SDSS, LAMOST, APOGEE
// and the HST/FUSE/IUE pipelines publish a corrected wavelength scale, while
// the ESO UVES, X-shooter, GIRAFFE and CRIRES+ Phase-3 streams publish a
// topocentric one and leave the correction to the user. A spectrum that skips
// it carries up to +-30 km/s of Earth's orbital motion, which is a fit-level
// error for anything measuring radial velocities.
//
// The frame is read from the file itself wherever the product states it
// (SPECSYS is mandatory in the ESO Science Data Product standard, HST writes
// the HELCORR calibration switch), and only falls back to what the archive
// client declares when the headers say nothing.

#ifndef SPECTRUMFRAME_H
#define SPECTRUMFRAME_H

#include <QString>

#include <cmath>
#include <vector>

namespace SpecFetch {

// Values follow the FITS WCS SPECSYS keyword.
enum class Frame {
    Unknown,
    Topocentric,     // observatory frame - needs correcting
    Geocentric,      // Earth centre - needs correcting (orbital motion left in)
    Heliocentric,    // Sun centre - within ~15 m/s of barycentric, left alone
    Barycentric,     // solar system barycentre - the target frame
};

/// True for the frames a fit can use as-is.
inline bool frameIsCorrected(Frame f) {
    return f == Frame::Heliocentric || f == Frame::Barycentric;
}

QString frameName(Frame f);

/// Where the observation was made. An unknown site falls back to the
/// geocentre, which costs at most Earth's rotation speed (0.46 km/s).
struct ObserverSite {
    double lonDeg = 0.0;    // degrees east
    double latDeg = 0.0;    // geodetic degrees
    double altM   = 0.0;    // metres above the WGS-84 ellipsoid
    bool   known  = false;  // false: the geocentre is being used as a stand-in

    QString source;         // where the coordinates came from, for logs
};

/// What the file says about its own wavelength scale.
struct FrameInfo {
    Frame  frame = Frame::Unknown;

    // The correction the pipeline computed for this exposure [km/s], when it
    // wrote one down. Used only to cross-check our own number - whether it was
    // applied is what `frame` says, and the sign convention of these keywords
    // is not uniformly documented.
    double statedCorrectionKms = std::nan("");
    QString statedCorrectionKey;

    // Telescope pointing from the file [deg, J2000], for products whose
    // archive record carries no position. Arcminute accuracy is plenty: the
    // correction changes by ~9 m/s per arcminute of line of sight.
    double targetRaDeg  = std::nan("");
    double targetDecDeg = std::nan("");

    ObserverSite site;
};

/// Reads SPECSYS, the pipeline correction keywords and the observatory
/// position out of a FITS product. Every field is optional; a file that says
/// nothing yields a default-constructed FrameInfo.
FrameInfo readFrameInfo(const QString& fitsPath);

/// Barycentric correction [km/s] for one exposure, positive when the observer
/// moves towards the target: `RV_bary = RV_obs + berv`. NaN when the epoch or
/// the target position is missing.
double bervKms(double mjdUtc, double raDeg, double decDeg,
               const ObserverSite& site);

/// Moves a wavelength array [Angstrom] into the barycentric frame:
/// lambda *= (1 + v/c).
void applyRadialVelocityShift(std::vector<double>& wavelengths, double vKms);

/// Key the import writes into a fetched spectrum's provenance blob for the
/// barycentric correction its wavelengths carry.
constexpr char kBarycorrMetaKey[] = "barycorrKms";

/// Reads that key back out of a Spectrum's origin metadata [km/s]. NaN for a
/// spectrum that was not fetched, or one whose frame was never established.
double barycorrFromOriginMeta(const QString& originMetaJson);

}   // namespace SpecFetch

#endif   // SPECTRUMFRAME_H
