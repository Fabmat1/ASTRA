#include "fitting/FitTypesJson.h"

#include <QJsonValue>

#include <cmath>

namespace astra::fitting {

namespace json {

double readDouble(const QJsonObject& o, const char* key, double def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toDouble() : def;
}

int readInt(const QJsonObject& o, const char* key, int def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? v.toInt(def) : def;
}

bool readBool(const QJsonObject& o, const char* key, bool def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isBool() ? v.toBool() : def;
}

QString readString(const QJsonObject& o, const char* key, const QString& def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isString() ? v.toString() : def;
}

QStringList readStringList(const QJsonObject& o, const char* key,
                           const QStringList& def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isArray()) return def;
    QStringList out;
    for (const QJsonValue& e : v.toArray()) out << e.toString();
    return out;
}

QJsonValue writeDouble(double v)
{
    // JSON cannot express NaN or infinity; QJsonValue(NaN) silently becomes
    // null anyway, so make that explicit rather than relying on it.
    return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QJsonValue::Null);
}

double readNanDouble(const QJsonObject& o, const char* key, double def)
{
    const QJsonValue v = o.value(QLatin1String(key));
    if (!v.isDouble()) return def;           // absent, or the null we wrote
    const double d = v.toDouble();
    return std::isfinite(d) ? d : def;
}

}   // namespace json

// ── IgnoreRegion ─────────────────────────────────────────────────────────

QJsonObject toJson(const IgnoreRegion& v)
{
    QJsonObject o;
    o["wlLow"]  = v.wlLow;
    o["wlHigh"] = v.wlHigh;
    return o;
}

IgnoreRegion ignoreRegionFromJson(const QJsonObject& o)
{
    IgnoreRegion v;
    v.wlLow  = json::readDouble(o, "wlLow",  v.wlLow);
    v.wlHigh = json::readDouble(o, "wlHigh", v.wlHigh);
    return v;
}

// ── ContinuumAnchor ──────────────────────────────────────────────────────

QJsonObject toJson(const ContinuumAnchor& v)
{
    QJsonObject o;
    o["wlLow"]   = v.wlLow;
    o["wlHigh"]  = v.wlHigh;
    o["spacing"] = v.spacing;
    return o;
}

ContinuumAnchor continuumAnchorFromJson(const QJsonObject& o)
{
    ContinuumAnchor v;
    v.wlLow   = json::readDouble(o, "wlLow",   v.wlLow);
    v.wlHigh  = json::readDouble(o, "wlHigh",  v.wlHigh);
    v.spacing = json::readDouble(o, "spacing", v.spacing);
    return v;
}

// ── SpectrumFitConfig ────────────────────────────────────────────────────

QJsonObject toJson(const SpectrumFitConfig& v)
{
    QJsonObject o;
    o["enabled"]       = v.enabled;
    o["wlMin"]         = v.wlMin;
    o["wlMax"]         = v.wlMax;
    o["ignore"]        = json::toArray(v.ignore,
                             [](const IgnoreRegion& r) { return toJson(r); });
    o["anchors"]       = json::toArray(v.anchors,
                             [](const ContinuumAnchor& a) { return toJson(a); });
    o["resOffset"]     = v.resOffset;
    o["resSlope"]      = v.resSlope;
    o["inferFromFits"] = v.inferFromFits;
    o["airmass"]       = v.airmass;
    o["pwv"]           = v.pwv;
    return o;
}

SpectrumFitConfig spectrumFitConfigFromJson(const QJsonObject& o)
{
    SpectrumFitConfig v;
    v.enabled       = json::readBool(o, "enabled", v.enabled);
    v.wlMin         = json::readDouble(o, "wlMin", v.wlMin);
    v.wlMax         = json::readDouble(o, "wlMax", v.wlMax);
    v.ignore        = json::fromArray<IgnoreRegion>(o, "ignore",
                                                    ignoreRegionFromJson);
    v.anchors       = json::fromArray<ContinuumAnchor>(o, "anchors",
                                                       continuumAnchorFromJson);
    v.resOffset     = json::readDouble(o, "resOffset", v.resOffset);
    v.resSlope      = json::readDouble(o, "resSlope", v.resSlope);
    v.inferFromFits = json::readBool(o, "inferFromFits", v.inferFromFits);
    v.airmass       = json::readDouble(o, "airmass", v.airmass);
    v.pwv           = json::readDouble(o, "pwv", v.pwv);
    return v;
}

// ── StellarComponent ─────────────────────────────────────────────────────

QJsonObject toJson(const StellarComponent& v)
{
    QJsonObject o;
    o["gridPath"] = v.gridPath;
    o["teff"]     = v.teff;
    o["logg"]     = v.logg;
    o["vsini"]    = v.vsini;
    o["he"]       = v.he;
    o["zeta"]     = v.zeta;
    o["xi"]       = v.xi;
    o["z"]        = v.z;

    o["freezeTeff"]  = v.freezeTeff;
    o["freezeLogg"]  = v.freezeLogg;
    o["freezeVsini"] = v.freezeVsini;
    o["freezeHe"]    = v.freezeHe;
    o["freezeZeta"]  = v.freezeZeta;
    o["freezeXi"]    = v.freezeXi;
    o["freezeZ"]     = v.freezeZ;

    o["surRatio"]       = v.surRatio;
    o["freezeSurRatio"] = v.freezeSurRatio;

    // Abundances go out as two parallel objects keyed by species name rather
    // than as an array of pairs: a plan file is read by humans often enough
    // that "FE": -4.5 is worth the two maps.
    QJsonObject ab;
    for (auto it = v.abundances.constBegin(); it != v.abundances.constEnd(); ++it)
        ab[it.key()] = it.value();
    o["abundances"] = ab;

    QJsonObject fz;
    for (auto it = v.freezeAbundances.constBegin();
         it != v.freezeAbundances.constEnd(); ++it)
        fz[it.key()] = it.value();
    o["freezeAbundances"] = fz;

    return o;
}

StellarComponent stellarComponentFromJson(const QJsonObject& o)
{
    StellarComponent v;
    v.gridPath = json::readString(o, "gridPath", v.gridPath);
    v.teff     = json::readDouble(o, "teff",  v.teff);
    v.logg     = json::readDouble(o, "logg",  v.logg);
    v.vsini    = json::readDouble(o, "vsini", v.vsini);
    v.he       = json::readDouble(o, "he",    v.he);
    v.zeta     = json::readDouble(o, "zeta",  v.zeta);
    v.xi       = json::readDouble(o, "xi",    v.xi);
    v.z        = json::readDouble(o, "z",     v.z);

    v.freezeTeff  = json::readBool(o, "freezeTeff",  v.freezeTeff);
    v.freezeLogg  = json::readBool(o, "freezeLogg",  v.freezeLogg);
    v.freezeVsini = json::readBool(o, "freezeVsini", v.freezeVsini);
    v.freezeHe    = json::readBool(o, "freezeHe",    v.freezeHe);
    v.freezeZeta  = json::readBool(o, "freezeZeta",  v.freezeZeta);
    v.freezeXi    = json::readBool(o, "freezeXi",    v.freezeXi);
    v.freezeZ     = json::readBool(o, "freezeZ",     v.freezeZ);

    v.surRatio       = json::readDouble(o, "surRatio", v.surRatio);
    v.freezeSurRatio = json::readBool(o, "freezeSurRatio", v.freezeSurRatio);

    const QJsonObject ab = o.value(QLatin1String("abundances")).toObject();
    for (auto it = ab.constBegin(); it != ab.constEnd(); ++it)
        v.abundances.insert(it.key(), it.value().toDouble());

    const QJsonObject fz = o.value(QLatin1String("freezeAbundances")).toObject();
    for (auto it = fz.constBegin(); it != fz.constEnd(); ++it)
        v.freezeAbundances.insert(it.key(), it.value().toBool());

    return v;
}

// ── ISIS options ─────────────────────────────────────────────────────────

QJsonObject toJson(const IsisOptions& v)
{
    QJsonObject o;
    o["xrange"]           = v.xrange;
    o["errorEstimation"]  = v.errorEstimation;
    o["autoFreezeVsini"]  = v.autoFreezeVsini;
    o["addTelluricModel"] = v.addTelluricModel;
    o["applyMask"]        = v.applyMask;
    o["saveModel"]        = v.saveModel;
    o["xfigIgnore"]       = v.xfigIgnore;
    return o;
}

IsisOptions isisOptionsFromJson(const QJsonObject& o)
{
    IsisOptions v;
    v.xrange           = json::readDouble(o, "xrange", v.xrange);
    v.errorEstimation  = json::readBool(o, "errorEstimation", v.errorEstimation);
    v.autoFreezeVsini  = json::readBool(o, "autoFreezeVsini", v.autoFreezeVsini);
    v.addTelluricModel = json::readBool(o, "addTelluricModel", v.addTelluricModel);
    v.applyMask        = json::readBool(o, "applyMask", v.applyMask);
    v.saveModel        = json::readString(o, "saveModel", v.saveModel);
    v.xfigIgnore       = json::readInt(o, "xfigIgnore", v.xfigIgnore);
    return v;
}

QJsonObject toJson(const IsisInteractiveOptions& v)
{
    QJsonObject o;
    o["rvCorrection"]    = v.rvCorrection;
    o["rvAnchors"]       = v.rvAnchors;
    o["macrobroadening"] = v.macrobroadening;
    return o;
}

IsisInteractiveOptions isisInteractiveOptionsFromJson(const QJsonObject& o)
{
    IsisInteractiveOptions v;
    v.rvCorrection    = json::readBool(o, "rvCorrection", v.rvCorrection);
    v.rvAnchors       = json::readString(o, "rvAnchors", v.rvAnchors);
    v.macrobroadening = json::readString(o, "macrobroadening", v.macrobroadening);
    return v;
}

// ── JobGlobals ───────────────────────────────────────────────────────────

QJsonObject toJson(const JobGlobals& v)
{
    QJsonObject o;
    o["backend"]      = v.backend;
    o["untiedParams"] = QJsonArray::fromStringList(v.untiedParams);

    o["filterSnr"]      = v.filterSnr;
    o["requireBlue"]    = v.requireBlue;
    o["nitNoiseMax"]    = v.nitNoiseMax;
    o["outlierSigmaLo"] = v.outlierSigmaLo;
    o["outlierSigmaHi"] = v.outlierSigmaHi;
    o["verbose"]        = v.verbose;

    o["addTelluricModel"]   = v.addTelluricModel;
    o["autoFreezeSurRatio"] = v.autoFreezeSurRatio;
    o["surRatioThres"]      = v.surRatioThres;
    o["c2DetectionThres"]   = v.c2DetectionThres;
    o["contJitterK"]        = v.contJitterK;
    o["workerThreads"]      = v.workerThreads;

    o["basePaths"] = QJsonArray::fromStringList(v.basePaths);

    o["isis"]            = toJson(v.isis);
    o["isisInteractive"] = toJson(v.isisInteractive);
    return o;
}

JobGlobals jobGlobalsFromJson(const QJsonObject& o)
{
    JobGlobals v;
    v.backend      = json::readString(o, "backend", v.backend);
    v.untiedParams = json::readStringList(o, "untiedParams", v.untiedParams);

    v.filterSnr      = json::readDouble(o, "filterSnr", v.filterSnr);
    v.requireBlue    = json::readDouble(o, "requireBlue", v.requireBlue);
    v.nitNoiseMax    = json::readInt(o, "nitNoiseMax", v.nitNoiseMax);
    v.outlierSigmaLo = json::readDouble(o, "outlierSigmaLo", v.outlierSigmaLo);
    v.outlierSigmaHi = json::readDouble(o, "outlierSigmaHi", v.outlierSigmaHi);
    v.verbose        = json::readBool(o, "verbose", v.verbose);

    v.addTelluricModel   = json::readBool(o, "addTelluricModel", v.addTelluricModel);
    v.autoFreezeSurRatio = json::readBool(o, "autoFreezeSurRatio", v.autoFreezeSurRatio);
    v.surRatioThres      = json::readDouble(o, "surRatioThres", v.surRatioThres);
    v.c2DetectionThres   = json::readDouble(o, "c2DetectionThres", v.c2DetectionThres);
    v.contJitterK        = json::readInt(o, "contJitterK", v.contJitterK);
    v.workerThreads      = json::readInt(o, "workerThreads", v.workerThreads);

    v.basePaths = json::readStringList(o, "basePaths", v.basePaths);

    // A missing sub-object gives an empty QJsonObject, and every reader in it
    // then falls back to its own default - which is exactly right.
    if (o.contains(QLatin1String("isis")))
        v.isis = isisOptionsFromJson(o.value(QLatin1String("isis")).toObject());
    if (o.contains(QLatin1String("isisInteractive")))
        v.isisInteractive = isisInteractiveOptionsFromJson(
            o.value(QLatin1String("isisInteractive")).toObject());

    return v;
}

}   // namespace astra::fitting
