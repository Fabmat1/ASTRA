#include "Time.h"
#include "Instrument.h"

#include <cmath>
#include <QDebug>

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

Time::Time()
    : _nativeScale(TimeScale::Unknown)
    , _nativeValue(0.0)
{}

Time::Time(double value, TimeScale scale)
    : _nativeScale(scale)
    , _nativeValue(value)
{
    switch (scale) {
    case TimeScale::JD:       _jd  = value;                    break;
    case TimeScale::MJD:      _mjd = value;                    break;
    case TimeScale::BJD:      _bjd = value;                    break;
    case TimeScale::HJD:      _hjd = value;                    break;
    case TimeScale::BTJD:     _bjd = value + BTJD_OFFSET;     break;
    case TimeScale::BKJD:     _bjd = value + BKJD_OFFSET;     break;
    case TimeScale::GaiaTCB:  _bjd = value + GAIA_OFFSET;     break;
    case TimeScale::Unknown:  break;
    }
    propagateOffsets();
}

Time::Time(double value, TimeScale scale, double exposureTimeSec)
    : Time(value, scale)
{
    _exposureSec = exposureTimeSec;
}

Time Time::fromMjdBjd(double mjd, double bjd, double exposureTimeSec)
{
    Time t;

    if (bjd != 0.0) {
        t._nativeScale = TimeScale::BJD;
        t._nativeValue = bjd;
        t._bjd = bjd;
    }
    if (mjd != 0.0) {
        if (!t._bjd.has_value()) {
            t._nativeScale = TimeScale::MJD;
            t._nativeValue = mjd;
        }
        t._mjd = mjd;
    }

    if (exposureTimeSec >= 0.0)
        t._exposureSec = exposureTimeSec;

    t.propagateOffsets();
    return t;
}

// ═════════════════════════════════════════════════════════════════════════════
// Offset propagation
// ═════════════════════════════════════════════════════════════════════════════

void Time::propagateOffsets()
{
    // JD ↔ MJD  (pure constant offset, always valid)
    if (_jd.has_value() && !_mjd.has_value())
        _mjd = *_jd - MJD_OFFSET;
    if (_mjd.has_value() && !_jd.has_value())
        _jd = *_mjd + MJD_OFFSET;

    // BJD → MJD is NOT done here (needs barycentric correction in reverse).
    // MJD → BJD is NOT done here (needs sky coordinates → see lazy bjd()).
}

// ═════════════════════════════════════════════════════════════════════════════
// Setters
// ═════════════════════════════════════════════════════════════════════════════

void Time::setMJD(double v)
{
    _mjd = v;
    _jd  = v + MJD_OFFSET;
    // Invalidate cached BJD – the MJD changed, so any previously
    // lazily‑computed BJD is stale.  Explicit setBJD() values are also
    // overridden; the caller should re‑set BJD if they know it.
    _bjd.reset();
}

void Time::setBJD(double v)
{
    _bjd = v;
}

void Time::setHJD(double v)
{
    _hjd = v;
}

// ═════════════════════════════════════════════════════════════════════════════
// Lazy conversion link
// ═════════════════════════════════════════════════════════════════════════════

void Time::setAutoConvertInfo(std::shared_ptr<const Instrument> inst,
                              double raDeg, double decDeg)
{
    _autoInst = std::move(inst);
    _autoRA   = raDeg;
    _autoDec  = decDeg;
}

void Time::clearAutoConvertInfo()
{
    _autoInst.reset();
    _autoRA  = 0.0;
    _autoDec = 0.0;
}

// ═════════════════════════════════════════════════════════════════════════════
// BJD accessor with lazy auto‑conversion
// ═════════════════════════════════════════════════════════════════════════════

std::optional<double> Time::bjd() const
{
    if (_bjd.has_value())
        return _bjd;

    // Attempt lazy conversion: need MJD + instrument + coordinates
    if (!_mjd.has_value() || !_autoInst)
        return std::nullopt;

    // Perform the conversion and cache the result
    _bjd = _autoInst->mjdToBjd(*_mjd, _autoRA, _autoDec);
    return _bjd;
}

// ═════════════════════════════════════════════════════════════════════════════
// Coordinate‑dependent conversions (explicit)
// ═════════════════════════════════════════════════════════════════════════════

void Time::computeBJD(const Instrument& inst, double raDeg, double decDeg)
{
    if (_bjd.has_value() && *_bjd > 0.0) return;
    if (!_mjd.has_value()) {
        qWarning() << "Time::computeBJD: MJD not available – cannot convert.";
        return;
    }
    _bjd = inst.mjdToBjd(*_mjd, raDeg, decDeg);
}

void Time::computeHJD(const Instrument& inst, double raDeg, double decDeg)
{
    if (_hjd.has_value()) return;
    if (!_mjd.has_value()) {
        qWarning() << "Time::computeHJD: MJD not available – cannot convert.";
        return;
    }
    Q_UNUSED(inst); Q_UNUSED(raDeg); Q_UNUSED(decDeg);
    qWarning() << "Time::computeHJD: not yet fully implemented.";
}

// ═════════════════════════════════════════════════════════════════════════════
// Comparison / sorting
// ═════════════════════════════════════════════════════════════════════════════

double Time::sortValue() const
{
    // Use bjd() (not _bjd) so lazy computation kicks in
    auto b = bjd();
    if (b.has_value())         return *b;
    if (_mjd.has_value())      return *_mjd + MJD_OFFSET;
    if (_jd.has_value())       return *_jd;
    return _nativeValue;
}

bool Time::operator==(const Time& o) const
{
    return std::fabs(sortValue() - o.sortValue()) < 1e-9;
}

// ═════════════════════════════════════════════════════════════════════════════
// Serialisation
// ═════════════════════════════════════════════════════════════════════════════

QDataStream& operator<<(QDataStream& s, const Time& t)
{
    s << static_cast<qint32>(t._nativeScale)
      << t._nativeValue;

    auto writeOpt = [&](const std::optional<double>& o) {
        bool has = o.has_value();
        s << has;
        if (has) s << *o;
    };
    writeOpt(t._jd);
    writeOpt(t._mjd);
    writeOpt(t._bjd);
    writeOpt(t._hjd);

    s << t._exposureSec;

    // Persist auto‑convert coordinates (but not the instrument pointer –
    // that is re‑linked on load by the owning object).
    bool hasAuto = (t._autoInst != nullptr);
    s << hasAuto;
    if (hasAuto) {
        s << t._autoRA << t._autoDec;
    }

    return s;
}

QDataStream& operator>>(QDataStream& s, Time& t)
{
    qint32 scaleInt;
    s >> scaleInt >> t._nativeValue;
    t._nativeScale = static_cast<TimeScale>(scaleInt);

    auto readOpt = [&](std::optional<double>& o) {
        bool has;
        s >> has;
        if (has) { double v; s >> v; o = v; }
        else     { o.reset(); }
    };
    readOpt(t._jd);
    readOpt(t._mjd);
    readOpt(t._bjd);
    readOpt(t._hjd);

    s >> t._exposureSec;

    // Read auto‑convert coordinates (instrument pointer re‑linked externally)
    bool hasAuto = false;
    s >> hasAuto;
    if (hasAuto) {
        s >> t._autoRA >> t._autoDec;
    }

    return s;
}

// ═════════════════════════════════════════════════════════════════════════════
// Pretty‑printing & scale string conversion
// ═════════════════════════════════════════════════════════════════════════════

QString Time::toString() const
{
    if (!isValid()) return QStringLiteral("Time(invalid)");

    QString s = QStringLiteral("Time(%1 %2")
                    .arg(_nativeValue, 0, 'f', 6)
                    .arg(scaleToString(_nativeScale));
    if (_exposureSec >= 0.0)
        s += QStringLiteral(", exp=%1s").arg(_exposureSec, 0, 'f', 1);
    if (_autoInst)
        s += QStringLiteral(", auto‑BJD");
    s += ')';
    return s;
}

QString Time::scaleToString(TimeScale ts)
{
    switch (ts) {
    case TimeScale::JD:       return QStringLiteral("JD");
    case TimeScale::MJD:      return QStringLiteral("MJD");
    case TimeScale::BJD:      return QStringLiteral("BJD");
    case TimeScale::HJD:      return QStringLiteral("HJD");
    case TimeScale::BTJD:     return QStringLiteral("BTJD (TESS)");
    case TimeScale::BKJD:     return QStringLiteral("BKJD (Kepler)");
    case TimeScale::GaiaTCB:  return QStringLiteral("Gaia TCB");
    case TimeScale::Unknown:  return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

TimeScale Time::stringToScale(const QString& str)
{
    QString lower = str.trimmed().toLower();
    if (lower == "jd")                          return TimeScale::JD;
    if (lower == "mjd")                         return TimeScale::MJD;
    if (lower == "bjd" || lower == "bjd_tdb")   return TimeScale::BJD;
    if (lower == "hjd")                         return TimeScale::HJD;
    if (lower == "btjd")                        return TimeScale::BTJD;
    if (lower == "bkjd")                        return TimeScale::BKJD;
    if (lower == "gaiatcb" || lower == "tcb")   return TimeScale::GaiaTCB;
    return TimeScale::Unknown;
}

// ═════════════════════════════════════════════════════════════════════════════
// Scale‑guessing helpers
// ═════════════════════════════════════════════════════════════════════════════

TimeScale Time::guessScaleFromInstrument(const QString& instrument)
{
    QString lower = instrument.toLower().trimmed();
    if (lower == "tess")                          return TimeScale::BTJD;
    if (lower == "kepler" || lower == "k2")       return TimeScale::BKJD;
    if (lower == "gaia")                          return TimeScale::GaiaTCB;
    if (lower == "atlas"   || lower == "ztf"
        || lower == "asas-sn" || lower == "css"
        || lower == "ogle")                       return TimeScale::MJD;
    if (lower == "hipparcos")                     return TimeScale::BJD;
    if (lower == "aavso")                         return TimeScale::BJD;
    return TimeScale::Unknown;
}

TimeScale Time::guessScaleFromValue(double firstTime)
{
    if (firstTime > 2400000.0)
        return TimeScale::BJD;
    if (firstTime > 40000.0 && firstTime < 100000.0)
        return TimeScale::MJD;
    if (firstTime > 0.0 && firstTime < 5000.0)
        return TimeScale::BTJD;
    return TimeScale::Unknown;
}