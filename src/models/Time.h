#pragma once

#include <optional>
#include <QString>
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
    BKJD,       // Kepler Barycentric JD (BJD − 2 454 833.0)       // ← NEW
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
    std::optional<double> jd()  const { return _jd;  }
    std::optional<double> mjd() const { return _mjd; }
    std::optional<double> bjd() const { return _bjd; }
    std::optional<double> hjd() const { return _hjd; }

    // ── Convenience accessors that fall back ────────────────────────────────
    double mjdOr(double fallback = 0.0) const { return _mjd.value_or(fallback); }
    double bjdOr(double fallback = 0.0) const { return _bjd.value_or(fallback); }

    // ── Setters ─────────────────────────────────────────────────────────────
    void setMJD(double v);
    void setBJD(double v);
    void setHJD(double v);

    // ── Coordinate‑dependent conversions ────────────────────────────────────
    void computeBJD(const Instrument& inst, double raDeg, double decDeg);
    void computeHJD(const Instrument& inst, double raDeg, double decDeg);

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

    // ── Scale‑guessing helpers (moved from LightcurveTime) ──────────────── // ← NEW
    /// Guess the time scale from an instrument name (e.g. "TESS" → BTJD).
    static TimeScale guessScaleFromInstrument(const QString& instrument);

    /// Guess the time scale from the magnitude of a raw time value.
    static TimeScale guessScaleFromValue(double firstTime);

    // ── Offset constants ────────────────────────────────────────────────────
    static constexpr double MJD_OFFSET  = 2400000.5;
    static constexpr double BTJD_OFFSET = 2457000.0;
    static constexpr double BKJD_OFFSET = 2454833.0;    // ← NEW
    static constexpr double GAIA_OFFSET = 2455197.5;

private:
    void propagateOffsets();

    TimeScale _nativeScale  = TimeScale::Unknown;
    double    _nativeValue  = 0.0;

    std::optional<double> _jd;
    std::optional<double> _mjd;
    std::optional<double> _bjd;
    std::optional<double> _hjd;

    double _exposureSec = -1.0;
};