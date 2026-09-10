#include "core/Time.h"
#include "core/BarycentricCorrection.h"
#include "core/Instrument.h"

#include <cmath>
#include <QDebug>
#include <QRegularExpression>

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

void Time::adoptNativeScale(TimeScale scale, double v)
{
    // A default-constructed Time carries no native scale, so isValid() is
    // false and consumers (e.g. RVPanel) drop it even once a value has been
    // filled in. Setting a real value is exactly the point at which the scale
    // becomes known, so claim it here - matching fromMjdBjd(), which is the
    // path the DB loader uses. Only Unknown is upgraded: a Time that already
    // knows it is BTJD/BKJD/… keeps its own scale and value.
    if (_nativeScale != TimeScale::Unknown) return;
    if (v == 0.0 || std::isnan(v)) return;
    _nativeScale = scale;
    _nativeValue = v;
}

void Time::setMJD(double v)
{
    _mjd = v;
    _jd  = v + MJD_OFFSET;
    // Invalidate cached BJD – the MJD changed, so any previously
    // lazily‑computed BJD is stale.  Explicit setBJD() values are also
    // overridden; the caller should re‑set BJD if they know it.
    _bjd.reset();
    adoptNativeScale(TimeScale::MJD, v);
}

void Time::setBJD(double v)
{
    _bjd = v;
    adoptNativeScale(TimeScale::BJD, v);
}

void Time::setHJD(double v)
{
    _hjd = v;
    // Deliberately no invalidation of _mjd/_jd/_bjd, unlike setMJD(): those are
    // the scales an HJD is *derived into*, so a reader that already found a
    // real MJD in the header keeps it, and a Time that has only the HJD gets
    // the others filled in by computeMJD() once the coordinates turn up.
    adoptNativeScale(TimeScale::HJD, v);
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

// ═════════════════════════════════════════════════════════════════════════════
// Scale accessors with lazy auto‑conversion
// ═════════════════════════════════════════════════════════════════════════════

void Time::ensureMjdFromHjd() const
{
    if (_mjd.has_value()) return;
    if (!_hjd.has_value() || !_autoInst) return;

    _mjd = _autoInst->hjdToMjd(*_hjd, _autoRA, _autoDec);
    _jd  = *_mjd + MJD_OFFSET;
}

std::optional<double> Time::mjd() const
{
    ensureMjdFromHjd();
    return _mjd;
}

std::optional<double> Time::jd() const
{
    ensureMjdFromHjd();
    return _jd;
}

std::optional<double> Time::bjd() const
{
    if (_bjd.has_value())
        return _bjd;

    // Attempt lazy conversion: need MJD + instrument + coordinates. An
    // HJD-only Time qualifies too - mjd() undoes the heliocentric leg first.
    const auto m = mjd();
    if (!m.has_value() || !_autoInst)
        return std::nullopt;

    // Perform the conversion and cache the result
    _bjd = _autoInst->mjdToBjd(*m, _autoRA, _autoDec);
    return _bjd;
}

std::optional<double> Time::hjd() const
{
    if (_hjd.has_value())
        return _hjd;

    if (!_mjd.has_value() || !_autoInst)
        return std::nullopt;

    _hjd = _autoInst->mjdToHjd(*_mjd, _autoRA, _autoDec);
    return _hjd;
}

// ═════════════════════════════════════════════════════════════════════════════
// Coordinate‑dependent conversions (explicit)
// ═════════════════════════════════════════════════════════════════════════════

void Time::computeBJD(const Instrument& inst, double raDeg, double decDeg)
{
    if (_bjd.has_value() && *_bjd > 0.0) return;

    // An HJD-only Time is one heliocentric correction away from an MJD, which
    // is what the barycentric leg needs; do that first rather than refusing.
    computeMJD(&inst, raDeg, decDeg);

    if (!_mjd.has_value()) {
        qWarning() << "Time::computeBJD: MJD not available – cannot convert.";
        return;
    }
    _bjd = inst.mjdToBjd(*_mjd, raDeg, decDeg);
    adoptNativeScale(TimeScale::BJD, *_bjd);
}

void Time::computeHJD(const Instrument& inst, double raDeg, double decDeg)
{
    if (_hjd.has_value()) return;
    if (!_mjd.has_value()) {
        qWarning() << "Time::computeHJD: MJD not available – cannot convert.";
        return;
    }
    _hjd = inst.mjdToHjd(*_mjd, raDeg, decDeg);
    adoptNativeScale(TimeScale::HJD, *_hjd);
}

void Time::computeMJD(const Instrument* inst, double raDeg, double decDeg)
{
    if (_mjd.has_value()) return;
    if (!_hjd.has_value()) return;   // nothing to undo; not an error

    // No instrument: the geocentre. The site is worth ≤ 21 ms here, and an
    // import that has matched the star but not the telescope would otherwise
    // have to throw the timestamp away.
    _mjd = inst ? inst->hjdToMjd(*_hjd, raDeg, decDeg)
                : BarycentricCorrection::hjdUtcToMjdUtc(
                      *_hjd, raDeg, decDeg, 0.0, 0.0, 0.0);
    _jd  = *_mjd + MJD_OFFSET;
    adoptNativeScale(TimeScale::MJD, *_mjd);
}

void Time::resolveScales(const Instrument* inst, double raDeg, double decDeg)
{
    computeMJD(inst, raDeg, decDeg);
    if (inst) computeBJD(*inst, raDeg, decDeg);
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
    // An HJD is the same epoch to within the ±8.3 minutes of the heliocentric
    // correction, which is close enough to order a series by.
    if (_hjd.has_value())      return *_hjd;
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

bool Time::isBarycentric(TimeScale ts)
{
    switch (ts) {
    case TimeScale::BJD:
    case TimeScale::BTJD:
    case TimeScale::BKJD:
    case TimeScale::GaiaTCB:
        return true;
    case TimeScale::JD:
    case TimeScale::MJD:
    case TimeScale::HJD:
    case TimeScale::Unknown:
        return false;
    }
    return false;
}

// ═════════════════════════════════════════════════════════════════════════════
// Reduced (offset) Julian dates
// ═════════════════════════════════════════════════════════════════════════════

double Time::parseEpochOffset(const QString& text)
{
    // A JD-family word, then the offset the table has subtracted. The word is
    // required: a bare "2450000" in a description is as likely to be a range
    // bound or a bibcode fragment as an epoch offset, and guessing at those is
    // how timestamps get quietly ruined.
    //
    // The separator may be '-', '+' or '_'. Underscore is the file-name form
    // ("HJD_2450000") and always means subtraction, like the hyphen; only an
    // explicit '+' reverses the sign.
    static const QRegularExpression re(
        QStringLiteral(R"([a-z]*jd[a-z_]*\s*([-+_])\s*(2[0-9]{6}(?:\.[0-9]+)?))"),
        QRegularExpression::CaseInsensitiveOption);

    const auto m = re.match(text);
    if (!m.hasMatch()) return 0.0;

    const double magnitude = m.captured(2).toDouble();
    return (m.captured(1) == QLatin1String("+")) ? -magnitude : magnitude;
}

double Time::epochOffsetFor(const QString& columnLabel,
                            const QStringList& metadataLines)
{
    // The header cell wins: if it says "HJD-2450000" there is nothing to
    // interpret.
    if (const double fromLabel = parseEpochOffset(columnLabel);
        fromLabel != 0.0)
        return fromLabel;

    // Otherwise the file's own metadata. VizieR writes one line per column,
    //   #Column HJD (F13.8) [3852.76/9935.72] Heliocentric Julian date; HJD-2450000.0
    // so the line that names this column and states an offset is the one.
    const QString needle = columnLabel.trimmed();
    if (needle.isEmpty()) return 0.0;

    for (const QString& line : metadataLines) {
        if (!line.contains(needle, Qt::CaseInsensitive)) continue;
        if (const double fromMeta = parseEpochOffset(line); fromMeta != 0.0)
            return fromMeta;
    }
    return 0.0;
}

bool Time::isPlausibleFor(double value, TimeScale scale)
{
    if (std::isnan(value)) return false;

    switch (scale) {
    case TimeScale::JD:
    case TimeScale::HJD:
    case TimeScale::BJD:
        // JD 2000000 is 1720 CE, comfortably before any observation ASTRA will
        // ever see, so this rejects reduced epochs without second-guessing a
        // genuinely old one.
        return value >= 2.0e6;

    case TimeScale::MJD:
        // MJD 1e6 is the year 4600. A value that large is a JD mislabelled.
        return value < 1.0e6;

    case TimeScale::BTJD:
    case TimeScale::BKJD:
    case TimeScale::GaiaTCB:
    case TimeScale::Unknown:
        return true;
    }
    return true;
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