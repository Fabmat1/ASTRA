#pragma once

#include <optional>
#include <memory>
#include <QString>
#include <QStringList>
#include <QDataStream>

class Instrument;

// ─── Unified time‑scale enum ────────────────────────────────────────────────
enum class TimeScale
{
    JD,         // Julian Date
    MJD,        // Modified Julian Date  (JD − 2 400 000.5)
    BJD,        // Barycentric Julian Date (TDB)
    HJD,        // Heliocentric Julian Date
    BTJD,       // TESS Barycentric JD   (BJD − 2 457 000.0)
    BKJD,       // Kepler Barycentric JD (BJD − 2 454 833.0)
    GaiaTCB,    // Gaia TCB              (BJD − 2 455 197.5)
    Unknown
};

// ─── Time ────────────────────────────────────────────────────────────────────
class Time
{
public:
    // ── Construction ────────────────────────────────────────────────────────
    Time();
    explicit Time(double value, TimeScale scale);
    Time(double value, TimeScale scale, double exposureTimeSec);

    /// Build from separate MJD + BJD (legacy DB / import path)
    static Time fromMjdBjd(double mjd, double bjd,
                           double exposureTimeSec = -1.0);

    // ── Primary query ───────────────────────────────────────────────────────
    TimeScale nativeScale() const { return _nativeScale; }
    double    nativeValue() const { return _nativeValue; }
    bool      isValid()     const { return _nativeScale != TimeScale::Unknown; }

    // ── Direct accessors (return std::nullopt if not yet available) ─────────
    /// MJD/JD accessors with lazy auto‑conversion: a Time that carries only an
    /// HJD can be undone back to UTC once an instrument link + coordinates are
    /// available, so the caller sees the observed epoch rather than nothing.
    std::optional<double> jd()  const;
    std::optional<double> mjd() const;

    /// BJD accessor with lazy auto‑conversion.
    /// If BJD has not been set but an instrument link + coordinates are
    /// available, the conversion is performed transparently on first access.
    std::optional<double> bjd() const;

    /// HJD accessor, likewise lazily derived from MJD when it can be.
    std::optional<double> hjd() const;

    /// Returns true if a BJD value has been explicitly set or already computed.
    bool hasBjd() const { return _bjd.has_value(); }

    /// Returns true if an HJD value has been explicitly set or already computed.
    bool hasHjd() const { return _hjd.has_value(); }

    // ── Convenience accessors that fall back ────────────────────────────────
    double mjdOr(double fallback = 0.0) const {
        auto v = mjd();  // triggers lazy computation
        return v.value_or(fallback);
    }
    double bjdOr(double fallback = 0.0) const {
        auto v = bjd();  // triggers lazy computation
        return v.value_or(fallback);
    }

    // ── Setters ─────────────────────────────────────────────────────────────
    void setMJD(double v);
    void setBJD(double v);

    /// Records a heliocentric timestamp. Unlike setMJD() this leaves the other
    /// scales alone: undoing the heliocentric light travel needs the target
    /// coordinates, so the derivation happens in computeMJD() / the lazy
    /// accessors once those are known, and only where nothing is stored yet.
    void setHJD(double v);

    // ── Lazy conversion link ────────────────────────────────────────────────
    /// Store the instrument and target coordinates so that BJD can be
    /// computed on demand when bjd() is called.
    void setAutoConvertInfo(std::shared_ptr<const Instrument> inst,
                            double raDeg, double decDeg);

    /// Forget the auto‑convert link (e.g. when coordinates change).

    /// Whether an auto‑convert link is configured.
    // ── Explicit coordinate‑dependent conversions ───────────────────────────
    void computeBJD(const Instrument& inst, double raDeg, double decDeg);
    void computeHJD(const Instrument& inst, double raDeg, double decDeg);

    /// HJD → MJD/JD: undoes the heliocentric light travel.
    ///
    /// `inst` may be null. The site shifts the heliocentric correction by at
    /// most 21 ms, so the geocentre is a fair stand‑in for an import that knows
    /// the star but not yet the telescope - which is the usual case for a
    /// third‑party RV table.
    void computeMJD(const Instrument* inst, double raDeg, double decDeg);

    /// Fill in every scale that follows from what is already known, in the one
    /// order that works: HJD → MJD → BJD. Import paths call this instead of
    /// picking the right compute*() by hand for the scale the user chose.
    void resolveScales(const Instrument* inst, double raDeg, double decDeg);

    // ── Exposure time ───────────────────────────────────────────────────────
    bool   hasExposureTime()  const { return _exposureSec >= 0.0; }
    double exposureTimeSec()  const { return _exposureSec; }
    void   setExposureTime(double sec) { _exposureSec = sec; }

    // ── Comparison ──────────────────────────────────────────────────────────
    double sortValue() const;
    bool operator<(const Time& o)  const { return sortValue() < o.sortValue(); }
    bool operator==(const Time& o) const;

    // ── Serialisation ───────────────────────────────────────────────────────
    friend QDataStream& operator<<(QDataStream& s, const Time& t);
    friend QDataStream& operator>>(QDataStream& s, Time& t);

    // ── Pretty‑printing & scale string conversion ───────────────────────────
    QString toString() const;

    static QString    scaleToString(TimeScale ts);
    static TimeScale  stringToScale(const QString& str);

    /// Whether `ts` is already referred to the solar-system barycentre, and so
    /// needs no light-travel correction of its own - BJD and the mission scales
    /// that are a constant offset on it.
    static bool isBarycentric(TimeScale ts);

    // ── Reduced (offset) Julian dates ───────────────────────────────────────
    //
    // Catalogues routinely tabulate a Julian date with its leading digits
    // stripped - "HJD-2450000.0" is the near-universal form in the white-dwarf
    // literature, and VizieR states it in the column description. The value is
    // then ~4 digits rather than ~7, and putting it through a conversion as if
    // it were a full JD is silently catastrophic: the constant offset does not
    // cancel, and the resulting MJD lands thousands of years off.
    //
    // ASTRA models the mission scales (BTJD/BKJD/Gaia TCB) as fixed offsets on
    // BJD because those are standardised. A survey's own reduction is not, so
    // it is handled as data - a number added to the column before the scale
    // conversion - rather than as another enum value.

    /// The offset stated in `text`, as the amount to ADD to a tabulated value
    /// to recover the full Julian date. "HJD-2450000.0" therefore yields
    /// +2450000.0, since the table holds HJD minus that. Returns 0 when the
    /// text states no offset.
    static double parseEpochOffset(const QString& text);

    /// The offset for one column, looked up first in the header cell itself
    /// ("HJD-2450000") and then in the file's '#' metadata lines, which is
    /// where VizieR puts it. Returns 0 when nothing states one.
    static double epochOffsetFor(const QString& columnLabel,
                                 const QStringList& metadataLines);

    /// Whether `value` can be a timestamp on `scale` as it stands.
    ///
    /// A full Julian date is ~2.4-2.5 million; anything much smaller labelled
    /// JD, HJD or BJD is a reduced epoch whose offset has gone missing. The
    /// converse catches the same mix-up the other way round: an MJD in the
    /// millions is a Julian date wearing the wrong label. The scales that are
    /// themselves reduced (BTJD and friends) are exempt - being small is what
    /// they are for.
    static bool isPlausibleFor(double value, TimeScale scale);

    // ── Scale‑guessing helpers ──────────────────────────────────────────────
    static TimeScale guessScaleFromInstrument(const QString& instrument);
    static TimeScale guessScaleFromValue(double firstTime);

    // ── Offset constants ────────────────────────────────────────────────────
    static constexpr double MJD_OFFSET  = 2400000.5;
    static constexpr double BTJD_OFFSET = 2457000.0;
    static constexpr double BKJD_OFFSET = 2454833.0;
    static constexpr double GAIA_OFFSET = 2455197.5;

private:
    void propagateOffsets();

    /// Derive MJD/JD from a stored HJD using the auto‑convert link, if that is
    /// the only thing standing between the caller and an epoch. No‑op when the
    /// MJD is already known or no link is configured.
    void ensureMjdFromHjd() const;

    /// Claim `scale` as the native scale when none is known yet, so that a Time
    /// filled in through the setters reports isValid() == true.
    void adoptNativeScale(TimeScale scale, double v);

    TimeScale _nativeScale  = TimeScale::Unknown;
    double    _nativeValue  = 0.0;

    // All four are mutable: every one of them can be filled in lazily from
    // another once the auto‑convert link supplies the coordinates.
    mutable std::optional<double> _jd;
    mutable std::optional<double> _mjd;
    mutable std::optional<double> _bjd;
    mutable std::optional<double> _hjd;

    double _exposureSec = -1.0;

    // ── Lazy BJD conversion link ────────────────────────────────────────────
    std::shared_ptr<const Instrument> _autoInst;
    double _autoRA  = 0.0;    // degrees, J2000
    double _autoDec = 0.0;    // degrees, J2000
};