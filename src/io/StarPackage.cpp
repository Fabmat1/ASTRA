#include "io/StarPackage.h"

#include "models/Instrument.h"
#include "models/InstrumentMode.h"
#include "models/Photometry.h"
#include "models/RadialVelocity.h"
#include "models/Spectrum.h"
#include "models/Star.h"
#include "models/Time.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSysInfo>
#include <QtEndian>

#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>

namespace {

constexpr char MAGIC[8] = {'A', 'S', 'T', 'R', 'A', 'P', 'K', 'G'};
constexpr int  HEADER_SIZE =
    20; // magic(8)+verMaj(2)+verMin(2)+bo(1)+rsv(3)+flags(4)
constexpr int COMPRESS_LVL = 6;

const double kNaN = std::numeric_limits<double>::quiet_NaN();

inline quint8 hostByteOrderCode() {
    return (QSysInfo::ByteOrder == QSysInfo::LittleEndian) ? 0 : 1;
}
inline double bswapD(double d) {
    quint64 u;
    std::memcpy(&u, &d, 8);
    u = qbswap(u);
    std::memcpy(&d, &u, 8);
    return d;
}

// ── Scalar JSON helpers (NaN-safe) ──────────────────────────────────────────
inline QJsonValue jD(double v) {
    return std::isfinite(v) ? QJsonValue(v) : QJsonValue(QJsonValue::Null);
}
inline double rD(const QJsonObject &o, const char *k, double def = kNaN) {
    const QJsonValue v = o.value(QLatin1String(k));
    if (v.isUndefined() || v.isNull())
        return def;
    return v.toDouble(def);
}
inline void pD(QJsonObject &o, const char *k, double v) {
    o[QLatin1String(k)] = jD(v);
}
inline QString rS(const QJsonObject &o, const char *k) {
    return o.value(QLatin1String(k)).toString();
}
inline int rI(const QJsonObject &o, const char *k, int d = 0) {
    return o.value(QLatin1String(k)).toInt(d);
}
inline bool rB(const QJsonObject &o, const char *k, bool d = false) {
    return o.value(QLatin1String(k)).toBool(d);
}

QString dtToIso(const QDateTime &dt) {
    return dt.isValid() ? dt.toUTC().toString(Qt::ISODate) : QString();
}
QDateTime dtFromIso(const QString &s) {
    return s.isEmpty() ? QDateTime() : QDateTime::fromString(s, Qt::ISODate);
}

// ── Blob pool (writer) ──────────────────────────────────────────────────────
struct BlobPool {
    QByteArray data;
    QJsonArray dir;

    int addDoubles(const std::vector<double> &v) {
        if (v.empty())
            return -1;
        const qint64 off = data.size();
        const qint64 len = qint64(v.size()) * 8;
        data.append(reinterpret_cast<const char *>(v.data()), int(len));
        QJsonObject e;
        e["o"] = double(off);
        e["l"] = double(len);
        dir.append(e);
        return dir.size() - 1;
    }
    int addBytes(const std::vector<uint8_t> &v) {
        if (v.empty())
            return -1;
        const qint64 off = data.size();
        const qint64 len = qint64(v.size());
        data.append(reinterpret_cast<const char *>(v.data()), int(len));
        QJsonObject e;
        e["o"] = double(off);
        e["l"] = double(len);
        dir.append(e);
        return dir.size() - 1;
    }
};

// ── Blob pool (reader) ──────────────────────────────────────────────────────
struct BlobReader {
    QByteArray   pool;
    QJsonArray   dir;
    bool         swap     = false;
    QStringList *warnings = nullptr;

    void warn(const QString &w) {
        if (warnings)
            *warnings << w;
    }

    std::vector<double> getDoubles(int idx) {
        if (idx < 0)
            return {};
        if (idx >= dir.size()) {
            warn("blob index out of range (doubles)");
            return {};
        }
        const QJsonObject e   = dir.at(idx).toObject();
        const qint64      off = qint64(e.value("o").toDouble());
        const qint64      len = qint64(e.value("l").toDouble());
        if (off < 0 || len < 0 || off + len > pool.size() || (len % 8) != 0) {
            warn("corrupt blob entry (doubles)");
            return {};
        }
        const int           n = int(len / 8);
        std::vector<double> v(n);
        if (n)
            std::memcpy(v.data(), pool.constData() + off, size_t(len));
        if (swap)
            for (auto &x : v)
                x = bswapD(x);
        return v;
    }
    std::vector<uint8_t> getBytes(int idx) {
        if (idx < 0)
            return {};
        if (idx >= dir.size()) {
            warn("blob index out of range (bytes)");
            return {};
        }
        const QJsonObject e   = dir.at(idx).toObject();
        const qint64      off = qint64(e.value("o").toDouble());
        const qint64      len = qint64(e.value("l").toDouble());
        if (off < 0 || len < 0 || off + len > pool.size()) {
            warn("corrupt blob entry (bytes)");
            return {};
        }
        std::vector<uint8_t> v;
        v.resize(size_t(len));
        if (len)
            std::memcpy(v.data(), pool.constData() + off, size_t(len));
        return v;
    }
};

// ═════════════════════════════════════════════════════════════════════════════
//  Time
// ═════════════════════════════════════════════════════════════════════════════
QJsonObject timeToJson(const Time &t) {
    QJsonObject o;
    o["scale"] = Time::scaleToString(t.nativeScale());
    pD(o, "val", t.nativeValue());
    if (t.mjd())
        pD(o, "mjd", *t.mjd());
    if (t.hasBjd()) {
        if (auto b = t.bjd())
            pD(o, "bjd", *b);
    }
    if (t.hjd())
        pD(o, "hjd", *t.hjd());
    if (t.hasExposureTime())
        pD(o, "exp", t.exposureTimeSec());
    return o;
}
Time timeFromJson(const QJsonObject &o) {
    Time   t(rD(o, "val", 0.0), Time::stringToScale(rS(o, "scale")));
    double m = rD(o, "mjd");
    if (std::isfinite(m))
        t.setMJD(m);
    double b = rD(o, "bjd");
    if (std::isfinite(b))
        t.setBJD(b);
    double h = rD(o, "hjd");
    if (std::isfinite(h))
        t.setHJD(h);
    double e = rD(o, "exp");
    if (std::isfinite(e))
        t.setExposureTime(e);
    return t;
}

// ═════════════════════════════════════════════════════════════════════════════
//  SpectralFit
// ═════════════════════════════════════════════════════════════════════════════
QJsonObject fitToJson(const SpectralFit &f, BlobPool &bp) {
    QJsonObject o;
    o["id"]        = f.getId();
    o["modelId"]   = f.modelId;
    o["isBestFit"] = f.isBestFit;
    o["isFlagged"] = f.isFlagged;
    o["created"]   = dtToIso(f.creationDate);

    pD(o, "teff", f.teff);
    pD(o, "teffErr", f.teffError);
    pD(o, "logg", f.logg);
    pD(o, "loggErr", f.loggError);
    pD(o, "he", f.he);
    pD(o, "heErr", f.heError);
    pD(o, "vsini", f.vsini);
    pD(o, "vsiniErr", f.vsiniError);
    pD(o, "rv", f.radialVelocity);
    pD(o, "rvErr", f.radialVelocityError);
    pD(o, "chi2", f.chi2);
    pD(o, "metal", f.metallicity);
    pD(o, "metalErr", f.metallicityError);
    pD(o, "macro", f.macroturbulence);
    pD(o, "macroErr", f.macroturbulenceError);
    pD(o, "micro", f.microturbulence);
    pD(o, "microErr", f.microturbulenceError);

    pD(o, "teffErrUp", f.teffErrorUp);
    pD(o, "teffErrDown", f.teffErrorDown);
    pD(o, "loggErrUp", f.loggErrorUp);
    pD(o, "loggErrDown", f.loggErrorDown);
    pD(o, "heErrUp", f.heErrorUp);
    pD(o, "heErrDown", f.heErrorDown);
    pD(o, "vsiniErrUp", f.vsiniErrorUp);
    pD(o, "vsiniErrDown", f.vsiniErrorDown);
    pD(o, "rvErrUp", f.radialVelocityErrorUp);
    pD(o, "rvErrDown", f.radialVelocityErrorDown);
    pD(o, "metalErrUp", f.metallicityErrorUp);
    pD(o, "metalErrDown", f.metallicityErrorDown);
    pD(o, "macroErrUp", f.macroturbulenceErrorUp);
    pD(o, "macroErrDown", f.macroturbulenceErrorDown);
    pD(o, "microErrUp", f.microturbulenceErrorUp);
    pD(o, "microErrDown", f.microturbulenceErrorDown);

    // Model arrays are lazy: after a normal DB load only the side-file path is
    // set (loadSpectralFits does not read the blob). Pull it in here so fits
    // are always exported with their data, regardless of whether the UI
    // happened to load them. Load into a temporary to avoid mutating the live
    // model object that may be shared with the UI.
    const SpectralFit *src = &f;
    SpectralFit        loaded;
    if (!f.hasData() && !f.getModelDataFile().isEmpty()) {
        loaded = f;
        if (loaded.loadDataFromFile(loaded.getModelDataFile()))
            src = &loaded;
    }
    if (src->hasData()) {
        o["b_modelWl"]   = bp.addDoubles(src->modelWavelengths);
        o["b_modelFlux"] = bp.addDoubles(src->modelFluxes);
        o["b_rebinFlux"] = bp.addDoubles(src->rebinnedFluxes);
        o["b_rebinSig"]  = bp.addDoubles(src->rebinnedSigmas);
        o["b_splines"]   = bp.addDoubles(src->modelSplines);
        o["b_ignore"]    = bp.addBytes(src->modelIgnore);
    }
    return o;
}
std::shared_ptr<SpectralFit> fitFromJson(const QJsonObject &o, BlobReader &br) {
    auto f = std::make_shared<SpectralFit>();
    f->setId(rS(o, "id"));
    f->modelId      = rS(o, "modelId");
    f->isBestFit    = rB(o, "isBestFit");
    f->isFlagged    = rB(o, "isFlagged");
    f->creationDate = dtFromIso(rS(o, "created"));

    f->teff                 = rD(o, "teff");
    f->teffError            = rD(o, "teffErr");
    f->logg                 = rD(o, "logg");
    f->loggError            = rD(o, "loggErr");
    f->he                   = rD(o, "he");
    f->heError              = rD(o, "heErr");
    f->vsini                = rD(o, "vsini");
    f->vsiniError           = rD(o, "vsiniErr");
    f->radialVelocity       = rD(o, "rv");
    f->radialVelocityError  = rD(o, "rvErr");
    f->chi2                 = rD(o, "chi2");
    f->metallicity          = rD(o, "metal");
    f->metallicityError     = rD(o, "metalErr");
    f->macroturbulence      = rD(o, "macro");
    f->macroturbulenceError = rD(o, "macroErr");
    f->microturbulence      = rD(o, "micro");
    f->microturbulenceError = rD(o, "microErr");

    f->teffErrorUp             = rD(o, "teffErrUp");
    f->teffErrorDown           = rD(o, "teffErrDown");
    f->loggErrorUp             = rD(o, "loggErrUp");
    f->loggErrorDown           = rD(o, "loggErrDown");
    f->heErrorUp               = rD(o, "heErrUp");
    f->heErrorDown             = rD(o, "heErrDown");
    f->vsiniErrorUp            = rD(o, "vsiniErrUp");
    f->vsiniErrorDown          = rD(o, "vsiniErrDown");
    f->radialVelocityErrorUp   = rD(o, "rvErrUp");
    f->radialVelocityErrorDown = rD(o, "rvErrDown");
    f->metallicityErrorUp      = rD(o, "metalErrUp");
    f->metallicityErrorDown    = rD(o, "metalErrDown");
    f->macroturbulenceErrorUp  = rD(o, "macroErrUp");
    f->macroturbulenceErrorDown = rD(o, "macroErrDown");
    f->microturbulenceErrorUp  = rD(o, "microErrUp");
    f->microturbulenceErrorDown = rD(o, "microErrDown");

    f->modelWavelengths = br.getDoubles(rI(o, "b_modelWl", -1));
    f->modelFluxes      = br.getDoubles(rI(o, "b_modelFlux", -1));
    f->rebinnedFluxes   = br.getDoubles(rI(o, "b_rebinFlux", -1));
    f->rebinnedSigmas   = br.getDoubles(rI(o, "b_rebinSig", -1));
    f->modelSplines     = br.getDoubles(rI(o, "b_splines", -1));
    f->modelIgnore      = br.getBytes(rI(o, "b_ignore", -1));
    return f;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Spectrum
// ═════════════════════════════════════════════════════════════════════════════
QJsonObject spectrumToJson(Spectrum &s, BlobPool &bp,
                           const StarPackage::ExportOptions &opt) {
    QJsonObject o;
    o["id"]           = s.getId();
    o["file"]         = s.getFile();
    o["instrument"]   = s.getInstrument();
    o["instrumentId"] = s.getInstrumentId();
    o["modeKey"]      = s.getModeKey();
    o["flagged"]      = s.isFlagged();
    o["baryCorr"]     = s.isBarycentricallyCorrected();
    o["time"]         = timeToJson(s.time());

    // Raw spectrum arrays are lazy too: loadSpectra only stores the data_file
    // path, so getWavelengths() is empty until something loads it. Pull it in
    // (into a temporary) so the spectrum is never exported without its data.
    std::vector<double> wl = s.getWavelengths();
    std::vector<double> fl = s.getFluxes();
    std::vector<double> er = s.getFluxErrors();
    if (wl.empty() && !s.getDataFile().isEmpty()) {
        Spectrum tmp;
        if (tmp.loadDataFromFile(s.getDataFile())) {
            wl = tmp.getWavelengths();
            fl = tmp.getFluxes();
            er = tmp.getFluxErrors();
        }
    }
    o["b_wl"]   = bp.addDoubles(wl);
    o["b_flux"] = bp.addDoubles(fl);
    o["b_err"]  = bp.addDoubles(er);

    if (opt.includeSpectralFits) {
        QJsonArray fits;
        for (const auto &f : s.getSpectralFits())
            if (f)
                fits.append(fitToJson(*f, bp));
        o["fits"] = fits;
    }
    return o;
}
std::shared_ptr<Spectrum> spectrumFromJson(const QJsonObject &o,
                                           BlobReader        &br) {
    auto s = std::make_shared<Spectrum>();
    s->setId(rS(o, "id"));
    s->setFile(rS(o, "file"));
    s->setInstrument(rS(o, "instrument"));
    s->setInstrumentId(rS(o, "instrumentId"));
    s->setModeKey(rS(o, "modeKey"));
    s->setFlagged(rB(o, "flagged"));
    s->setBarycentricallyCorrected(rB(o, "baryCorr"));
    s->setTime(timeFromJson(o.value("time").toObject()));

    s->setData(br.getDoubles(rI(o, "b_wl", -1)),
               br.getDoubles(rI(o, "b_flux", -1)),
               br.getDoubles(rI(o, "b_err", -1)));

    QString bestFitId;
    for (const QJsonValue &fv : o.value("fits").toArray()) {
        auto f = fitFromJson(fv.toObject(), br);
        if (f->isBestFit)
            bestFitId = f->getId();
        s->addSpectralFit(f);
    }
    if (!bestFitId.isEmpty())
        s->setBestFitById(bestFitId);
    return s;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Lightcurve (columnar blobs)
// ═════════════════════════════════════════════════════════════════════════════
QJsonObject lightcurveToJson(const std::vector<LightcurvePoint> &pts,
                             BlobPool                           &bp) {
    QJsonObject o;
    const int   n = int(pts.size());
    o["n"]        = n;

    std::vector<double> val(n), mjd(n), bjd(n), flux(n), ferr(n), mag(n),
        merr(n), qual(n);
    std::vector<uint8_t>   scale(n), uflag(n), filtIdx(n);
    QStringList            filters; // unique-filter dictionary
    std::map<QString, int> filtMap;

    for (int i = 0; i < n; ++i) {
        const LightcurvePoint &p = pts[i];
        val[i]                   = p.time.nativeValue();
        scale[i] = uint8_t(static_cast<int>(p.time.nativeScale()));
        mjd[i]   = p.time.mjd() ? *p.time.mjd() : kNaN;
        bjd[i]   = p.time.hasBjd() ? p.time.bjdOr(kNaN) : kNaN;
        flux[i]  = p.flux;
        ferr[i]  = p.fluxError;
        mag[i]   = p.magnitude;
        merr[i]  = p.magnitudeError;
        qual[i]  = double(p.qualityFlag);
        uflag[i] = p.userFlagged ? 1 : 0;
        auto it  = filtMap.find(p.filter);
        int  fi;
        if (it == filtMap.end()) {
            fi                = filters.size();
            filtMap[p.filter] = fi;
            filters << p.filter;
        } else
            fi = it->second;
        filtIdx[i] = uint8_t(fi & 0xff);
    }
    o["b_val"]   = bp.addDoubles(val);
    o["b_scale"] = bp.addBytes(scale);
    o["b_mjd"]   = bp.addDoubles(mjd);
    o["b_bjd"]   = bp.addDoubles(bjd);
    o["b_flux"]  = bp.addDoubles(flux);
    o["b_ferr"]  = bp.addDoubles(ferr);
    o["b_mag"]   = bp.addDoubles(mag);
    o["b_merr"]  = bp.addDoubles(merr);
    o["b_qual"]  = bp.addDoubles(qual);
    o["b_uflag"] = bp.addBytes(uflag);
    o["b_fidx"]  = bp.addBytes(filtIdx);
    o["filters"] = QJsonArray::fromStringList(filters);
    return o;
}
std::vector<LightcurvePoint> lightcurveFromJson(const QJsonObject &o,
                                                BlobReader        &br) {
    const int            n    = rI(o, "n");
    std::vector<double>  val  = br.getDoubles(rI(o, "b_val", -1));
    std::vector<uint8_t> sc   = br.getBytes(rI(o, "b_scale", -1));
    std::vector<double>  mjd  = br.getDoubles(rI(o, "b_mjd", -1));
    std::vector<double>  bjd  = br.getDoubles(rI(o, "b_bjd", -1));
    std::vector<double>  flux = br.getDoubles(rI(o, "b_flux", -1));
    std::vector<double>  ferr = br.getDoubles(rI(o, "b_ferr", -1));
    std::vector<double>  mag  = br.getDoubles(rI(o, "b_mag", -1));
    std::vector<double>  merr = br.getDoubles(rI(o, "b_merr", -1));
    std::vector<double>  qual = br.getDoubles(rI(o, "b_qual", -1));
    std::vector<uint8_t> uf   = br.getBytes(rI(o, "b_uflag", -1));
    std::vector<uint8_t> fidx = br.getBytes(rI(o, "b_fidx", -1));
    QStringList          filters;
    for (const QJsonValue &v : o.value("filters").toArray())
        filters << v.toString();

    auto at = [](const std::vector<double> &v, int i) {
        return (i < int(v.size())) ? v[i] : kNaN;
    };

    std::vector<LightcurvePoint> pts;
    pts.reserve(n);
    for (int i = 0; i < n; ++i) {
        LightcurvePoint p;
        const TimeScale ts = (i < int(sc.size()))
                                 ? static_cast<TimeScale>(sc[i])
                                 : TimeScale::Unknown;
        p.time             = Time(at(val, i), ts);
        double m           = at(mjd, i);
        if (std::isfinite(m))
            p.time.setMJD(m);
        double b = at(bjd, i);
        if (std::isfinite(b))
            p.time.setBJD(b);
        p.flux           = at(flux, i);
        p.fluxError      = at(ferr, i);
        p.magnitude      = at(mag, i);
        p.magnitudeError = at(merr, i);
        p.qualityFlag    = (i < int(qual.size())) ? int(qual[i]) : 0;
        p.userFlagged    = (i < int(uf.size())) && uf[i] != 0;
        const int fi     = (i < int(fidx.size())) ? fidx[i] : -1;
        if (fi >= 0 && fi < filters.size())
            p.filter = filters[fi];
        pts.push_back(std::move(p));
    }
    return pts;
}

// ═════════════════════════════════════════════════════════════════════════════
//  SED model
// ═════════════════════════════════════════════════════════════════════════════
QJsonObject sedPointToJson(const SEDPhotometryPoint &p) {
    QJsonObject o;
    pD(o, "lmin", p.lambdaMin);
    pD(o, "l", p.lambda);
    pD(o, "lmax", p.lambdaMax);
    pD(o, "fmin", p.fluxMin);
    pD(o, "f", p.flux);
    pD(o, "fmax", p.fluxMax);
    pD(o, "diff", p.diff);
    pD(o, "diffErr", p.diffErr);
    o["passband"] = p.passband;
    o["system"]   = p.system;
    o["flag"]     = p.flag;
    o["vizier"]   = p.vizierCatalog;
    pD(o, "mag", p.magnitude);
    pD(o, "magErr", p.magnitudeErr);
    o["type"] = p.type;
    pD(o, "angDist", p.angularDist);
    return o;
}
SEDPhotometryPoint sedPointFromJson(const QJsonObject &o) {
    SEDPhotometryPoint p;
    p.lambdaMin     = rD(o, "lmin", 0);
    p.lambda        = rD(o, "l", 0);
    p.lambdaMax     = rD(o, "lmax", 0);
    p.fluxMin       = rD(o, "fmin", 0);
    p.flux          = rD(o, "f", 0);
    p.fluxMax       = rD(o, "fmax", 0);
    p.diff          = rD(o, "diff", 0);
    p.diffErr       = rD(o, "diffErr", 0);
    p.passband      = rS(o, "passband");
    p.system        = rS(o, "system");
    p.flag          = rI(o, "flag");
    p.vizierCatalog = rS(o, "vizier");
    p.magnitude     = rD(o, "mag", 0);
    p.magnitudeErr  = rD(o, "magErr", 0);
    p.type          = rS(o, "type");
    p.angularDist   = rD(o, "angDist", 0);
    return p;
}
QJsonObject sedModelToJson(const SEDModel &m, BlobPool &bp) {
    QJsonObject o;
    o["id"]            = m.getId();
    o["modelId"]       = m.modelId;
    o["objectName"]    = m.objectName;
    o["isBestFit"]     = m.isBestFit;
    o["numComponents"] = m.numComponents;
    o["created"]       = dtToIso(m.creationDate);
    pD(o, "ebvSFD", m.ebvSFD);
    pD(o, "ebvSFDErr", m.ebvSFDError);
    pD(o, "ebvSF", m.ebvSF);
    pD(o, "ebvSFErr", m.ebvSFError);
    pD(o, "e4455", m.e4455);
    pD(o, "e4455Err", m.e4455Error);
    pD(o, "r55", m.r55);
    pD(o, "logTheta", m.logTheta);
    pD(o, "logThetaErr", m.logThetaError);
    pD(o, "plx", m.parallax);
    pD(o, "plxErr", m.parallaxError);
    pD(o, "ruwe", m.parallaxRuwe);
    pD(o, "zpo", m.parallaxZpo);
    pD(o, "distMode", m.distanceMode);
    pD(o, "distModeErr", m.distanceModeError);
    pD(o, "distMed", m.distanceMedian);
    pD(o, "distMedErr", m.distanceMedianError);
    pD(o, "chi2r", m.chi2Reduced);
    pD(o, "excessNoise", m.excessNoise);

    QJsonArray comps;
    for (const auto &c : m.components)
        comps.append(c.toJson());
    o["components"] = comps;
    QJsonArray obs;
    for (const auto &p : m.observedPoints)
        obs.append(sedPointToJson(p));
    o["observed"] = obs;

    o["b_modelWl"]   = bp.addDoubles(m.modelWavelengths);
    o["b_modelFlux"] = bp.addDoubles(m.modelFluxes);
    QJsonArray cf;
    for (const auto &comp : m.componentFluxes)
        cf.append(bp.addDoubles(comp));
    o["b_compFlux"] = cf;
    return o;
}
std::shared_ptr<SEDModel> sedModelFromJson(const QJsonObject &o,
                                           BlobReader        &br) {
    auto m = std::make_shared<SEDModel>();
    m->setId(rS(o, "id"));
    m->modelId             = rS(o, "modelId");
    m->objectName          = rS(o, "objectName");
    m->isBestFit           = rB(o, "isBestFit");
    m->numComponents       = rI(o, "numComponents", 1);
    m->creationDate        = dtFromIso(rS(o, "created"));
    m->ebvSFD              = rD(o, "ebvSFD", 0);
    m->ebvSFDError         = rD(o, "ebvSFDErr", 0);
    m->ebvSF               = rD(o, "ebvSF", 0);
    m->ebvSFError          = rD(o, "ebvSFErr", 0);
    m->e4455               = rD(o, "e4455", 0);
    m->e4455Error          = rD(o, "e4455Err", 0);
    m->r55                 = rD(o, "r55", 0);
    m->logTheta            = rD(o, "logTheta", 0);
    m->logThetaError       = rD(o, "logThetaErr", 0);
    m->parallax            = rD(o, "plx", 0);
    m->parallaxError       = rD(o, "plxErr", 0);
    m->parallaxRuwe        = rD(o, "ruwe", 0);
    m->parallaxZpo         = rD(o, "zpo", 0);
    m->distanceMode        = rD(o, "distMode", 0);
    m->distanceModeError   = rD(o, "distModeErr", 0);
    m->distanceMedian      = rD(o, "distMed", 0);
    m->distanceMedianError = rD(o, "distMedErr", 0);
    m->chi2Reduced         = rD(o, "chi2r", 0);
    m->excessNoise         = rD(o, "excessNoise", 0);

    for (const QJsonValue &v : o.value("components").toArray())
        m->components.push_back(SEDComponentParams::fromJson(v.toObject()));
    for (const QJsonValue &v : o.value("observed").toArray())
        m->observedPoints.push_back(sedPointFromJson(v.toObject()));

    m->modelWavelengths = br.getDoubles(rI(o, "b_modelWl", -1));
    m->modelFluxes      = br.getDoubles(rI(o, "b_modelFlux", -1));
    for (const QJsonValue &v : o.value("b_compFlux").toArray())
        m->componentFluxes.push_back(br.getDoubles(v.toInt(-1)));
    return m;
}

// ═════════════════════════════════════════════════════════════════════════════
//  LC fit
// ═════════════════════════════════════════════════════════════════════════════
QJsonObject lcDataToJson(const std::vector<LCFitDataPoint> &v, BlobPool &bp) {
    QJsonObject o;
    const int   n = int(v.size());
    o["n"]        = n;
    std::vector<double> ph(n), dph(n), fl(n), fe(n), w(n), fac(n);
    for (int i = 0; i < n; ++i) {
        const auto &p = v[i];
        ph[i]         = p.phase;
        dph[i]        = p.dPhase;
        fl[i]         = p.flux;
        fe[i]         = p.fluxError;
        w[i]          = p.weight;
        fac[i]        = p.factor;
    }
    o["b_phase"]  = bp.addDoubles(ph);
    o["b_dphase"] = bp.addDoubles(dph);
    o["b_flux"]   = bp.addDoubles(fl);
    o["b_ferr"]   = bp.addDoubles(fe);
    o["b_w"]      = bp.addDoubles(w);
    o["b_fac"]    = bp.addDoubles(fac);
    return o;
}
std::vector<LCFitDataPoint> lcDataFromJson(const QJsonObject &o,
                                           BlobReader        &br) {
    const int n   = rI(o, "n");
    auto      ph  = br.getDoubles(rI(o, "b_phase", -1));
    auto      dph = br.getDoubles(rI(o, "b_dphase", -1));
    auto      fl  = br.getDoubles(rI(o, "b_flux", -1));
    auto      fe  = br.getDoubles(rI(o, "b_ferr", -1));
    auto      w   = br.getDoubles(rI(o, "b_w", -1));
    auto      fac = br.getDoubles(rI(o, "b_fac", -1));
    auto      at  = [](const std::vector<double> &v, int i) {
        return i < int(v.size()) ? v[i] : 0.0;
    };
    std::vector<LCFitDataPoint> out;
    out.reserve(n);
    for (int i = 0; i < n; ++i) {
        LCFitDataPoint p;
        p.phase     = at(ph, i);
        p.dPhase    = at(dph, i);
        p.flux      = at(fl, i);
        p.fluxError = at(fe, i);
        p.weight    = at(w, i);
        p.factor    = at(fac, i);
        out.push_back(p);
    }
    return out;
}
QJsonObject lcFitToJson(const LCFit &f, BlobPool &bp) {
    QJsonObject o;
    o["id"]        = f.getId();
    o["label"]     = f.label;
    o["isBestFit"] = f.isBestFit;
    o["created"]   = dtToIso(f.creationDate);
    o["filter"]    = f.filter;
    pD(o, "wavelengthNm", f.wavelengthNm);
    pD(o, "q", f.q);
    pD(o, "qErr", f.qError);
    pD(o, "incl", f.inclination);
    pD(o, "inclErr", f.inclinationError);
    pD(o, "r1", f.r1);
    pD(o, "r1Err", f.r1Error);
    pD(o, "r2", f.r2);
    pD(o, "r2Err", f.r2Error);
    pD(o, "vscale", f.velocityScale);
    pD(o, "vscaleErr", f.velocityScaleError);
    pD(o, "t1", f.t1);
    pD(o, "t1Err", f.t1Error);
    pD(o, "t2", f.t2);
    pD(o, "t2Err", f.t2Error);
    pD(o, "period", f.period);
    pD(o, "periodErr", f.periodError);
    pD(o, "t0BJD", f.t0BJD);
    pD(o, "t0BJDErr", f.t0BJDError);
    pD(o, "qErrUp", f.qErrorUp);
    pD(o, "qErrDown", f.qErrorDown);
    pD(o, "inclErrUp", f.inclinationErrorUp);
    pD(o, "inclErrDown", f.inclinationErrorDown);
    pD(o, "r1ErrUp", f.r1ErrorUp);
    pD(o, "r1ErrDown", f.r1ErrorDown);
    pD(o, "r2ErrUp", f.r2ErrorUp);
    pD(o, "r2ErrDown", f.r2ErrorDown);
    pD(o, "vscaleErrUp", f.velocityScaleErrorUp);
    pD(o, "vscaleErrDown", f.velocityScaleErrorDown);
    pD(o, "t1ErrUp", f.t1ErrorUp);
    pD(o, "t1ErrDown", f.t1ErrorDown);
    pD(o, "t2ErrUp", f.t2ErrorUp);
    pD(o, "t2ErrDown", f.t2ErrorDown);
    pD(o, "periodErrUp", f.periodErrorUp);
    pD(o, "periodErrDown", f.periodErrorDown);
    pD(o, "t0BJDErrUp", f.t0BJDErrorUp);
    pD(o, "t0BJDErrDown", f.t0BJDErrorDown);
    pD(o, "chi2", f.chi2);
    pD(o, "rms", f.rms);
    o["config"] = f.config.json();
    o["input"]  = lcDataToJson(f.inputPoints, bp);
    o["model"]  = lcDataToJson(f.modelPoints, bp);
    return o;
}
std::shared_ptr<LCFit> lcFitFromJson(const QJsonObject &o, BlobReader &br) {
    auto f = std::make_shared<LCFit>();
    f->setId(rS(o, "id"));
    f->label              = rS(o, "label");
    f->isBestFit          = rB(o, "isBestFit");
    f->creationDate       = dtFromIso(rS(o, "created"));
    f->filter             = rS(o, "filter");
    f->wavelengthNm       = rD(o, "wavelengthNm", 0);
    f->q                  = rD(o, "q", 0);
    f->qError             = rD(o, "qErr", 0);
    f->inclination        = rD(o, "incl", 0);
    f->inclinationError   = rD(o, "inclErr", 0);
    f->r1                 = rD(o, "r1", 0);
    f->r1Error            = rD(o, "r1Err", 0);
    f->r2                 = rD(o, "r2", 0);
    f->r2Error            = rD(o, "r2Err", 0);
    f->velocityScale      = rD(o, "vscale", 0);
    f->velocityScaleError = rD(o, "vscaleErr", 0);
    f->t1                 = rD(o, "t1", 0);
    f->t1Error            = rD(o, "t1Err", 0);
    f->t2                 = rD(o, "t2", 0);
    f->t2Error            = rD(o, "t2Err", 0);
    f->period             = rD(o, "period", 0);
    f->periodError        = rD(o, "periodErr", 0);
    f->t0BJD              = rD(o, "t0BJD", 0);
    f->t0BJDError         = rD(o, "t0BJDErr", 0);
    f->qErrorUp             = rD(o, "qErrUp");
    f->qErrorDown           = rD(o, "qErrDown");
    f->inclinationErrorUp   = rD(o, "inclErrUp");
    f->inclinationErrorDown = rD(o, "inclErrDown");
    f->r1ErrorUp            = rD(o, "r1ErrUp");
    f->r1ErrorDown          = rD(o, "r1ErrDown");
    f->r2ErrorUp            = rD(o, "r2ErrUp");
    f->r2ErrorDown          = rD(o, "r2ErrDown");
    f->velocityScaleErrorUp   = rD(o, "vscaleErrUp");
    f->velocityScaleErrorDown = rD(o, "vscaleErrDown");
    f->t1ErrorUp            = rD(o, "t1ErrUp");
    f->t1ErrorDown          = rD(o, "t1ErrDown");
    f->t2ErrorUp            = rD(o, "t2ErrUp");
    f->t2ErrorDown          = rD(o, "t2ErrDown");
    f->periodErrorUp        = rD(o, "periodErrUp");
    f->periodErrorDown      = rD(o, "periodErrDown");
    f->t0BJDErrorUp         = rD(o, "t0BJDErrUp");
    f->t0BJDErrorDown       = rD(o, "t0BJDErrDown");
    f->chi2               = rD(o, "chi2", 0);
    f->rms                = rD(o, "rms", 0);
    f->config.json()      = o.value("config").toObject();
    f->inputPoints        = lcDataFromJson(o.value("input").toObject(), br);
    f->modelPoints        = lcDataFromJson(o.value("model").toObject(), br);
    return f;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Photometry
// ═════════════════════════════════════════════════════════════════════════════
QJsonObject photometryToJson(Photometry &ph, BlobPool &bp,
                             const StarPackage::ExportOptions &opt) {
    QJsonObject o;
    o["id"] = ph.getId();

    // General photometric points (small → inline JSON)
    QJsonArray pts;
    for (const auto &p : ph.getPhotometricPoints()) {
        QJsonObject po;
        po["instrument"] = p.instrument;
        po["filter"]     = p.filter;
        pD(po, "mag", p.magnitude);
        pD(po, "magErr", p.magnitudeError);
        pD(po, "flux", p.flux);
        pD(po, "fluxErr", p.fluxError);
        pD(po, "wl", p.wavelength);
        pts.append(po);
    }
    o["points"] = pts;

    if (opt.includeLightcurves) {
        QJsonObject lcs;
        for (const QString &src : ph.getLightcurveSources())
            lcs[src] = lightcurveToJson(ph.getLightcurve(src), bp);
        o["lightcurves"] = lcs;
    }
    if (opt.includeSEDModels) {
        QJsonArray seds;
        for (const auto &m : ph.getSEDModels())
            if (m)
                seds.append(sedModelToJson(*m, bp));
        o["sedModels"] = seds;
    }
    if (opt.includeLCFits) {
        QJsonObject fitsBySrc;
        for (const QString &src : ph.getLightcurveSources()) {
            QJsonArray arr;
            for (const auto &f : ph.getLCFits(src))
                if (f)
                    arr.append(lcFitToJson(*f, bp));
            if (!arr.isEmpty())
                fitsBySrc[src] = arr;
        }
        o["lcFits"] = fitsBySrc;
    }
    return o;
}
std::shared_ptr<Photometry> photometryFromJson(const QJsonObject &o,
                                               BlobReader        &br) {
    auto ph = std::make_shared<Photometry>();
    ph->setId(rS(o, "id"));

    for (const QJsonValue &v : o.value("points").toArray()) {
        const QJsonObject po = v.toObject();
        PhotometricPoint  p;
        p.instrument     = rS(po, "instrument");
        p.filter         = rS(po, "filter");
        p.magnitude      = rD(po, "mag", 0);
        p.magnitudeError = rD(po, "magErr", 0);
        p.flux           = rD(po, "flux", 0);
        p.fluxError      = rD(po, "fluxErr", 0);
        p.wavelength     = rD(po, "wl", 0);
        ph->addPhotometricPoint(p);
    }
    const QJsonObject lcs = o.value("lightcurves").toObject();
    for (auto it = lcs.begin(); it != lcs.end(); ++it)
        ph->addLightcurve(it.key(),
                          lightcurveFromJson(it.value().toObject(), br));

    for (const QJsonValue &v : o.value("sedModels").toArray())
        ph->addSEDModel(sedModelFromJson(v.toObject(), br));

    const QJsonObject lcFits = o.value("lcFits").toObject();
    for (auto it = lcFits.begin(); it != lcFits.end(); ++it)
        for (const QJsonValue &fv : it.value().toArray())
            ph->addLCFit(it.key(), lcFitFromJson(fv.toObject(), br));
    return ph;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Radial velocity
// ═════════════════════════════════════════════════════════════════════════════
QJsonObject rvFitToJson(const RVFit &f) {
    QJsonObject o;
    o["id"]        = f.getId();
    o["isBestFit"] = f.isBestFit();
    o["method"]    = f.getFitMethod();
    o["created"]   = dtToIso(f.getCreationDate());
    pD(o, "K", f.getK());
    pD(o, "KErr", f.getKError());
    pD(o, "gamma", f.getGamma());
    pD(o, "gammaErr", f.getGammaError());
    pD(o, "period", f.getPeriod());
    pD(o, "periodErr", f.getPeriodError());
    pD(o, "phi", f.getPhi());
    pD(o, "phiErr", f.getPhiError());
    pD(o, "t0", f.getT0());
    pD(o, "t0Err", f.getT0Error());
    o["eccentric"] = f.isEccentric();
    pD(o, "ecc", f.getEccentricity());
    pD(o, "eccErr", f.getEccentricityError());
    pD(o, "omega", f.getOmega());
    pD(o, "omegaErr", f.getOmegaError());
    pD(o, "KErrUp", f.getKErrorUp());
    pD(o, "KErrDown", f.getKErrorDown());
    pD(o, "gammaErrUp", f.getGammaErrorUp());
    pD(o, "gammaErrDown", f.getGammaErrorDown());
    pD(o, "periodErrUp", f.getPeriodErrorUp());
    pD(o, "periodErrDown", f.getPeriodErrorDown());
    pD(o, "phiErrUp", f.getPhiErrorUp());
    pD(o, "phiErrDown", f.getPhiErrorDown());
    pD(o, "t0ErrUp", f.getT0ErrorUp());
    pD(o, "t0ErrDown", f.getT0ErrorDown());
    pD(o, "eccErrUp", f.getEccentricityErrorUp());
    pD(o, "eccErrDown", f.getEccentricityErrorDown());
    pD(o, "omegaErrUp", f.getOmegaErrorUp());
    pD(o, "omegaErrDown", f.getOmegaErrorDown());
    pD(o, "chi2", f.getChi2());
    pD(o, "rms", f.getRms());
    return o;
}
std::shared_ptr<RVFit> rvFitFromJson(const QJsonObject &o) {
    auto f = std::make_shared<RVFit>();
    f->setId(rS(o, "id"));
    f->setBestFit(rB(o, "isBestFit"));
    f->setFitMethod(rS(o, "method"));
    f->setCreationDate(dtFromIso(rS(o, "created")));
    f->setK(rD(o, "K", 0));
    f->setKError(rD(o, "KErr", 0));
    f->setGamma(rD(o, "gamma", 0));
    f->setGammaError(rD(o, "gammaErr", 0));
    f->setPeriod(rD(o, "period", 0));
    f->setPeriodError(rD(o, "periodErr", 0));
    f->setPhi(rD(o, "phi", 0));
    f->setPhiError(rD(o, "phiErr", 0));
    f->setT0(rD(o, "t0", 0));
    f->setT0Error(rD(o, "t0Err", 0));
    f->setEccentricity(rD(o, "ecc", 0));
    f->setEccentricityError(rD(o, "eccErr", 0));
    f->setEccentric(rB(o, "eccentric"));
    f->setOmega(rD(o, "omega", 0));
    f->setOmegaError(rD(o, "omegaErr", 0));
    f->setKErrorUp(rD(o, "KErrUp"));
    f->setKErrorDown(rD(o, "KErrDown"));
    f->setGammaErrorUp(rD(o, "gammaErrUp"));
    f->setGammaErrorDown(rD(o, "gammaErrDown"));
    f->setPeriodErrorUp(rD(o, "periodErrUp"));
    f->setPeriodErrorDown(rD(o, "periodErrDown"));
    f->setPhiErrorUp(rD(o, "phiErrUp"));
    f->setPhiErrorDown(rD(o, "phiErrDown"));
    f->setT0ErrorUp(rD(o, "t0ErrUp"));
    f->setT0ErrorDown(rD(o, "t0ErrDown"));
    f->setEccentricityErrorUp(rD(o, "eccErrUp"));
    f->setEccentricityErrorDown(rD(o, "eccErrDown"));
    f->setOmegaErrorUp(rD(o, "omegaErrUp"));
    f->setOmegaErrorDown(rD(o, "omegaErrDown"));
    f->setChi2(rD(o, "chi2", 0));
    f->setRms(rD(o, "rms", 0));
    return f;
}
QJsonObject rvPointToJson(const RadialVelocityPoint &p) {
    QJsonObject o;
    o["id"] = p.getId();
    pD(o, "rv", p.getRV());
    pD(o, "errFormal", p.getRVErrorFormal());
    pD(o, "errSys", p.getRVErrorSystematic());
    pD(o, "helio", p.getHeliocentricCorrection());
    o["helioApplied"]  = p.isHeliocentricCorrectionApplied();
    o["spectrumId"]    = p.getSpectrumId();
    o["spectralFitId"] = p.getSpectralFitId();
    o["source"]        = p.getSource();
    o["flagged"]       = p.isFlagged();
    o["rvSource"]      = int(p.getRVSource());
    pD(o, "rvManual", p.getRVManual());
    pD(o, "rvManualErrFormal", p.getRVManualErrorFormal());
    pD(o, "rvManualErrSys", p.getRVManualErrorSystematic());
    o["instrumentId"] =
        p.getInstrument() ? p.getInstrument()->getId() : QString();
    o["time"] = timeToJson(p.time());
    return o;
}
std::shared_ptr<RadialVelocityPoint> rvPointFromJson(
    const QJsonObject                                    &o,
    const std::map<QString, std::shared_ptr<Instrument>> &instById) {
    auto p = std::make_shared<RadialVelocityPoint>();
    p->setId(rS(o, "id"));
    p->setRV(rD(o, "rv"));
    p->setRVErrorFormal(rD(o, "errFormal", 0));
    p->setRVErrorSystematic(rD(o, "errSys", 0));
    p->setHeliocentricCorrection(rD(o, "helio", 0));
    p->setHeliocentricCorrectionApplied(rB(o, "helioApplied"));
    p->setSpectrumId(rS(o, "spectrumId"));
    p->setSpectralFitId(rS(o, "spectralFitId"));
    p->setSource(rS(o, "source"));
    p->setFlagged(rB(o, "flagged"));
    p->setRVSource(
        static_cast<RadialVelocityPoint::RVSource>(rI(o, "rvSource")));
    double rm = rD(o, "rvManual");
    if (std::isfinite(rm))
        p->setRVManual(rm);
    p->setRVManualErrorFormal(rD(o, "rvManualErrFormal", 0));
    p->setRVManualErrorSystematic(rD(o, "rvManualErrSys", 0));
    const QString instId = rS(o, "instrumentId");
    if (!instId.isEmpty()) {
        auto it = instById.find(instId);
        if (it != instById.end())
            p->setInstrument(it->second);
    }
    p->setTime(timeFromJson(o.value("time").toObject()));
    return p;
}
QJsonObject rvCurveToJson(RadialVelocityCurve &c) {
    QJsonObject o;
    o["id"]     = c.getId();
    o["starId"] = c.getStarId();
    pD(o, "logP", c.getLogP());
    QJsonArray pts;
    for (const auto &p : c.getRVPoints())
        if (p)
            pts.append(rvPointToJson(*p));
    o["points"] = pts;
    QJsonArray fits;
    for (const auto &f : c.getRVFits())
        if (f)
            fits.append(rvFitToJson(*f));
    o["fits"] = fits;
    return o;
}
std::shared_ptr<RadialVelocityCurve> rvCurveFromJson(
    const QJsonObject                                    &o,
    const std::map<QString, std::shared_ptr<Instrument>> &instById) {
    auto c = std::make_shared<RadialVelocityCurve>();
    c->setId(rS(o, "id"));
    c->setStarId(rS(o, "starId"));
    c->setLogP(rD(o, "logP"));
    for (const QJsonValue &v : o.value("points").toArray())
        c->addRVPoint(rvPointFromJson(v.toObject(), instById));
    QString bestId;
    for (const QJsonValue &v : o.value("fits").toArray()) {
        auto f = rvFitFromJson(v.toObject());
        if (f->isBestFit())
            bestId = f->getId();
        c->addRVFit(f);
    }
    if (!bestId.isEmpty())
        c->setBestFit(bestId);
    c->updateFitReferences();
    return c;
}

// ═════════════════════════════════════════════════════════════════════════════
//  Star
// ═════════════════════════════════════════════════════════════════════════════
QJsonObject starToJson(Star &s, BlobPool &bp,
                       const StarPackage::ExportOptions &opt,
                       std::set<QString>                &referencedInstIds) {
    QJsonObject o;
    // Identity
    o["id"]       = s.getId();
    o["alias"]    = s.getAlias();
    o["sourceId"] = s.getSourceId();
    o["tic"]      = s.getTic();
    o["jname"]    = s.getJName();
    // Astrometry
    pD(o, "ra", s.getRa());
    pD(o, "dec", s.getDec());
    pD(o, "pmra", s.getPmra());
    pD(o, "pmdec", s.getPmdec());
    pD(o, "e_pmra", s.getEPmra());
    pD(o, "e_pmdec", s.getEPmdec());
    pD(o, "plx", s.getPlx());
    pD(o, "e_plx", s.getEPlx());
    pD(o, "pmra_pmdec_corr", s.getPmraPmdecCorr());
    pD(o, "plx_pmdec_corr", s.getPlxPmdecCorr());
    pD(o, "plx_pmra_corr", s.getPlxPmraCorr());
    // Gaia photometry
    pD(o, "gmag", s.getGmag());
    pD(o, "e_gmag", s.getEGmag());
    pD(o, "bp", s.getBp());
    pD(o, "e_bp", s.getEBp());
    pD(o, "rp", s.getRp());
    pD(o, "e_rp", s.getERp());
    pD(o, "bp_rp", s.getBpRp());
    // Spectroscopic
    o["spec_class"] = s.getSpecClass();
    pD(o, "teff", s.getTeff());
    pD(o, "e_teff", s.getETeff());
    pD(o, "logg", s.getLogg());
    pD(o, "e_logg", s.getELogg());
    pD(o, "he", s.getHe());
    pD(o, "e_he", s.getEHe());
    pD(o, "e_teff_up", s.getETeffUp());
    pD(o, "e_teff_down", s.getETeffDown());
    pD(o, "e_logg_up", s.getELoggUp());
    pD(o, "e_logg_down", s.getELoggDown());
    pD(o, "e_he_up", s.getEHeUp());
    pD(o, "e_he_down", s.getEHeDown());
    // RV scalar
    pD(o, "logp", s.getLogP());
    pD(o, "deltaRV", s.getDeltaRV());
    pD(o, "e_deltaRV", s.getEDeltaRV());
    pD(o, "rv_avg", s.getRVAvg());
    pD(o, "e_rv_avg", s.getERVAvg());
    pD(o, "rv_med", s.getRVMed());
    pD(o, "e_rv_med", s.getERVMed());
    // counts
    o["nSpectra"]    = s.getNSpectra();
    o["nFitSpectra"] = s.getNFitSpectra();
    // RV summary
    pD(o, "rvTimespan", s.getRVTimespan());
    o["rvNPoints"] = s.getRVNPoints();
    pD(o, "rvK", s.getRVK());
    pD(o, "rvEK", s.getRVEK());
    pD(o, "rvEKUp", s.getRVEKUp());
    pD(o, "rvEKDown", s.getRVEKDown());
    pD(o, "rvPeriod", s.getRVPeriod());
    pD(o, "rvEPeriod", s.getRVEPeriod());
    pD(o, "rvEPeriodUp", s.getRVEPeriodUp());
    pD(o, "rvEPeriodDown", s.getRVEPeriodDown());
    pD(o, "rvGamma", s.getRVGamma());
    pD(o, "rvEGamma", s.getRVEGamma());
    pD(o, "rvEGammaUp", s.getRVEGammaUp());
    pD(o, "rvEGammaDown", s.getRVEGammaDown());
    pD(o, "rvEcc", s.getRVEcc());
    pD(o, "rvPhi", s.getRVPhi());
    pD(o, "rvT0", s.getRVT0());
    pD(o, "rvChi2", s.getRVChi2());
    pD(o, "rvRms", s.getRVRms());
    // SED
    pD(o, "sedMass1", s.getSedMass1());
    pD(o, "sedEMass1", s.getSedEMass1());
    pD(o, "sedRadius1", s.getSedRadius1());
    pD(o, "sedERadius1", s.getSedERadius1());
    pD(o, "sedLum1", s.getSedLum1());
    pD(o, "sedELum1", s.getSedELum1());
    pD(o, "sedMass2", s.getSedMass2());
    pD(o, "sedEMass2", s.getSedEMass2());
    pD(o, "sedRadius2", s.getSedRadius2());
    pD(o, "sedERadius2", s.getSedERadius2());
    pD(o, "sedLum2", s.getSedLum2());
    pD(o, "sedELum2", s.getSedELum2());
    pD(o, "sedEMass1Up", s.getSedEMass1Up());
    pD(o, "sedEMass1Down", s.getSedEMass1Down());
    pD(o, "sedERadius1Up", s.getSedERadius1Up());
    pD(o, "sedERadius1Down", s.getSedERadius1Down());
    pD(o, "sedELum1Up", s.getSedELum1Up());
    pD(o, "sedELum1Down", s.getSedELum1Down());
    pD(o, "sedEMass2Up", s.getSedEMass2Up());
    pD(o, "sedEMass2Down", s.getSedEMass2Down());
    pD(o, "sedERadius2Up", s.getSedERadius2Up());
    pD(o, "sedERadius2Down", s.getSedERadius2Down());
    pD(o, "sedELum2Up", s.getSedELum2Up());
    pD(o, "sedELum2Down", s.getSedELum2Down());
    // Companion mass
    pD(o, "compMassMin", s.getCompMassMin());
    pD(o, "eCompMassMin", s.getECompMassMin());
    pD(o, "eCompMassMinUp", s.getECompMassMinUp());
    pD(o, "eCompMassMinDown", s.getECompMassMinDown());
    pD(o, "compMassTrue", s.getCompMassTrue());
    pD(o, "eCompMassTrue", s.getECompMassTrue());
    pD(o, "eCompMassTrueUp", s.getECompMassTrueUp());
    pD(o, "eCompMassTrueDown", s.getECompMassTrueDown());
    // Photometric LC params
    pD(o, "photPeriod", s.getPhotPeriod());
    pD(o, "photEPeriod", s.getPhotEPeriod());
    pD(o, "photEPeriodUp", s.getPhotEPeriodUp());
    pD(o, "photEPeriodDown", s.getPhotEPeriodDown());
    pD(o, "photIncl", s.getPhotIncl());
    pD(o, "photEIncl", s.getPhotEIncl());
    pD(o, "photEInclUp", s.getPhotEInclUp());
    pD(o, "photEInclDown", s.getPhotEInclDown());
    pD(o, "photQ", s.getPhotQ());
    pD(o, "photEQ", s.getPhotEQ());
    pD(o, "photEQUp", s.getPhotEQUp());
    pD(o, "photEQDown", s.getPhotEQDown());
    // Galactic kinematics
    pD(o, "galU", s.getGalU());
    pD(o, "galEU", s.getGalEU());
    pD(o, "galEUUp", s.getGalEUUp());
    pD(o, "galEUDown", s.getGalEUDown());
    pD(o, "galV", s.getGalV());
    pD(o, "galEV", s.getGalEV());
    pD(o, "galEVUp", s.getGalEVUp());
    pD(o, "galEVDown", s.getGalEVDown());
    pD(o, "galW", s.getGalW());
    pD(o, "galEW", s.getGalEW());
    pD(o, "galEWUp", s.getGalEWUp());
    pD(o, "galEWDown", s.getGalEWDown());
    pD(o, "galX", s.getGalX());
    pD(o, "galEX", s.getGalEX());
    pD(o, "galEXUp", s.getGalEXUp());
    pD(o, "galEXDown", s.getGalEXDown());
    pD(o, "galY", s.getGalY());
    pD(o, "galEY", s.getGalEY());
    pD(o, "galEYUp", s.getGalEYUp());
    pD(o, "galEYDown", s.getGalEYDown());
    pD(o, "galZ", s.getGalZ());
    pD(o, "galEZ", s.getGalEZ());
    pD(o, "galEZUp", s.getGalEZUp());
    pD(o, "galEZDown", s.getGalEZDown());
    pD(o, "galPThin", s.getGalPThin());
    pD(o, "galEPThin", s.getGalEPThin());
    pD(o, "galPThick", s.getGalPThick());
    pD(o, "galEPThick", s.getGalEPThick());
    pD(o, "galPHalo", s.getGalPHalo());
    pD(o, "galEPHalo", s.getGalEPHalo());
    // Availability flags + crowding
    o["hasTess"]     = s.getHasTess();
    o["hasGaia"]     = s.getHasGaia();
    o["hasZtf"]      = s.getHasZtf();
    o["hasAtlas"]    = s.getHasAtlas();
    o["hasBlackgem"] = s.getHasBlackgem();
    pD(o, "tessCrowdsap", s.getTessCrowdsap());
    // Bibcodes
    QJsonArray bibs;
    for (const QString &b : s.getBibcodes())
        bibs.append(b);
    o["bibcodes"] = bibs;

    // ── Associated data (triggers lazy loaders) ────────────────────────────
    if (opt.includeSpectra) {
        QJsonArray specs;
        for (const auto &sp : s.getSpectra()) {
            if (!sp)
                continue;
            if (!sp->getInstrumentId().isEmpty())
                referencedInstIds.insert(sp->getInstrumentId());
            specs.append(spectrumToJson(*sp, bp, opt));
        }
        o["spectra"] = specs;
    }
    if (opt.includePhotometry) {
        if (auto ph = s.getPhotometry())
            o["photometry"] = photometryToJson(*ph, bp, opt);
    }
    if (opt.includeRV) {
        if (auto c = s.getRVCurve()) {
            for (const auto &p : c->getRVPoints())
                if (p && p->getInstrument())
                    referencedInstIds.insert(p->getInstrument()->getId());
            o["rv"] = rvCurveToJson(*c);
        }
    }
    return o;
}
std::shared_ptr<Star>
starFromJson(const QJsonObject &o, BlobReader &br,
             const std::map<QString, std::shared_ptr<Instrument>> &instById) {
    auto s = std::make_shared<Star>();
    s->setId(rS(o, "id"));
    s->setAlias(rS(o, "alias"));
    s->setSourceId(rS(o, "sourceId"));
    s->setTic(rS(o, "tic"));
    s->setJName(rS(o, "jname"));
    s->setRa(rD(o, "ra", 0));
    s->setDec(rD(o, "dec", 0));
    s->setPmra(rD(o, "pmra", 0));
    s->setPmdec(rD(o, "pmdec", 0));
    s->setEPmra(rD(o, "e_pmra", 0));
    s->setEPmdec(rD(o, "e_pmdec", 0));
    s->setPlx(rD(o, "plx", 0));
    s->setEPlx(rD(o, "e_plx", 0));
    s->setPmraPmdecCorr(rD(o, "pmra_pmdec_corr", 0));
    s->setPlxPmdecCorr(rD(o, "plx_pmdec_corr", 0));
    s->setPlxPmraCorr(rD(o, "plx_pmra_corr", 0));
    s->setGmag(rD(o, "gmag", 0));
    s->setEGmag(rD(o, "e_gmag", 0));
    s->setBp(rD(o, "bp", 0));
    s->setEBp(rD(o, "e_bp", 0));
    s->setRp(rD(o, "rp", 0));
    s->setERp(rD(o, "e_rp", 0));
    s->setBpRp(rD(o, "bp_rp", 0));
    s->setSpecClass(rS(o, "spec_class"));
    s->setTeff(rD(o, "teff"));
    s->setETeff(rD(o, "e_teff"));
    s->setLogg(rD(o, "logg"));
    s->setELogg(rD(o, "e_logg"));
    s->setHe(rD(o, "he"));
    s->setEHe(rD(o, "e_he"));
    s->setETeffUp(rD(o, "e_teff_up"));
    s->setETeffDown(rD(o, "e_teff_down"));
    s->setELoggUp(rD(o, "e_logg_up"));
    s->setELoggDown(rD(o, "e_logg_down"));
    s->setEHeUp(rD(o, "e_he_up"));
    s->setEHeDown(rD(o, "e_he_down"));
    s->setLogP(rD(o, "logp"));
    s->setDeltaRV(rD(o, "deltaRV"));
    s->setEDeltaRV(rD(o, "e_deltaRV"));
    s->setRVAvg(rD(o, "rv_avg"));
    s->setERVAvg(rD(o, "e_rv_avg"));
    s->setRVMed(rD(o, "rv_med"));
    s->setERVMed(rD(o, "e_rv_med"));
    s->setNSpectra(rI(o, "nSpectra"));
    s->setNFitSpectra(rI(o, "nFitSpectra"));
    s->setRVTimespan(rD(o, "rvTimespan"));
    s->setRVNPoints(rI(o, "rvNPoints"));
    s->setRVK(rD(o, "rvK"));
    s->setRVEK(rD(o, "rvEK"));
    s->setRVEKUp(rD(o, "rvEKUp"));
    s->setRVEKDown(rD(o, "rvEKDown"));
    s->setRVPeriod(rD(o, "rvPeriod"));
    s->setRVEPeriod(rD(o, "rvEPeriod"));
    s->setRVEPeriodUp(rD(o, "rvEPeriodUp"));
    s->setRVEPeriodDown(rD(o, "rvEPeriodDown"));
    s->setRVGamma(rD(o, "rvGamma"));
    s->setRVEGamma(rD(o, "rvEGamma"));
    s->setRVEGammaUp(rD(o, "rvEGammaUp"));
    s->setRVEGammaDown(rD(o, "rvEGammaDown"));
    s->setRVEcc(rD(o, "rvEcc"));
    s->setRVPhi(rD(o, "rvPhi"));
    s->setRVT0(rD(o, "rvT0"));
    s->setRVChi2(rD(o, "rvChi2"));
    s->setRVRms(rD(o, "rvRms"));
    s->setSedMass1(rD(o, "sedMass1"));
    s->setSedEMass1(rD(o, "sedEMass1"));
    s->setSedRadius1(rD(o, "sedRadius1"));
    s->setSedERadius1(rD(o, "sedERadius1"));
    s->setSedLum1(rD(o, "sedLum1"));
    s->setSedELum1(rD(o, "sedELum1"));
    s->setSedMass2(rD(o, "sedMass2"));
    s->setSedEMass2(rD(o, "sedEMass2"));
    s->setSedRadius2(rD(o, "sedRadius2"));
    s->setSedERadius2(rD(o, "sedERadius2"));
    s->setSedLum2(rD(o, "sedLum2"));
    s->setSedELum2(rD(o, "sedELum2"));
    s->setSedEMass1Up(rD(o, "sedEMass1Up"));
    s->setSedEMass1Down(rD(o, "sedEMass1Down"));
    s->setSedERadius1Up(rD(o, "sedERadius1Up"));
    s->setSedERadius1Down(rD(o, "sedERadius1Down"));
    s->setSedELum1Up(rD(o, "sedELum1Up"));
    s->setSedELum1Down(rD(o, "sedELum1Down"));
    s->setSedEMass2Up(rD(o, "sedEMass2Up"));
    s->setSedEMass2Down(rD(o, "sedEMass2Down"));
    s->setSedERadius2Up(rD(o, "sedERadius2Up"));
    s->setSedERadius2Down(rD(o, "sedERadius2Down"));
    s->setSedELum2Up(rD(o, "sedELum2Up"));
    s->setSedELum2Down(rD(o, "sedELum2Down"));
    s->setCompMassMin(rD(o, "compMassMin"));
    s->setECompMassMin(rD(o, "eCompMassMin"));
    s->setECompMassMinUp(rD(o, "eCompMassMinUp"));
    s->setECompMassMinDown(rD(o, "eCompMassMinDown"));
    s->setCompMassTrue(rD(o, "compMassTrue"));
    s->setECompMassTrue(rD(o, "eCompMassTrue"));
    s->setECompMassTrueUp(rD(o, "eCompMassTrueUp"));
    s->setECompMassTrueDown(rD(o, "eCompMassTrueDown"));
    s->setPhotPeriod(rD(o, "photPeriod"));
    s->setPhotEPeriod(rD(o, "photEPeriod"));
    s->setPhotEPeriodUp(rD(o, "photEPeriodUp"));
    s->setPhotEPeriodDown(rD(o, "photEPeriodDown"));
    s->setPhotIncl(rD(o, "photIncl"));
    s->setPhotEIncl(rD(o, "photEIncl"));
    s->setPhotEInclUp(rD(o, "photEInclUp"));
    s->setPhotEInclDown(rD(o, "photEInclDown"));
    s->setPhotQ(rD(o, "photQ"));
    s->setPhotEQ(rD(o, "photEQ"));
    s->setPhotEQUp(rD(o, "photEQUp"));
    s->setPhotEQDown(rD(o, "photEQDown"));
    s->setGalU(rD(o, "galU"));
    s->setGalEU(rD(o, "galEU"));
    s->setGalEUUp(rD(o, "galEUUp"));
    s->setGalEUDown(rD(o, "galEUDown"));
    s->setGalV(rD(o, "galV"));
    s->setGalEV(rD(o, "galEV"));
    s->setGalEVUp(rD(o, "galEVUp"));
    s->setGalEVDown(rD(o, "galEVDown"));
    s->setGalW(rD(o, "galW"));
    s->setGalEW(rD(o, "galEW"));
    s->setGalEWUp(rD(o, "galEWUp"));
    s->setGalEWDown(rD(o, "galEWDown"));
    s->setGalX(rD(o, "galX"));
    s->setGalEX(rD(o, "galEX"));
    s->setGalEXUp(rD(o, "galEXUp"));
    s->setGalEXDown(rD(o, "galEXDown"));
    s->setGalY(rD(o, "galY"));
    s->setGalEY(rD(o, "galEY"));
    s->setGalEYUp(rD(o, "galEYUp"));
    s->setGalEYDown(rD(o, "galEYDown"));
    s->setGalZ(rD(o, "galZ"));
    s->setGalEZ(rD(o, "galEZ"));
    s->setGalEZUp(rD(o, "galEZUp"));
    s->setGalEZDown(rD(o, "galEZDown"));
    s->setGalPThin(rD(o, "galPThin"));
    s->setGalEPThin(rD(o, "galEPThin"));
    s->setGalPThick(rD(o, "galPThick"));
    s->setGalEPThick(rD(o, "galEPThick"));
    s->setGalPHalo(rD(o, "galPHalo"));
    s->setGalEPHalo(rD(o, "galEPHalo"));
    s->setHasTess(rB(o, "hasTess"));
    s->setHasGaia(rB(o, "hasGaia"));
    s->setHasZtf(rB(o, "hasZtf"));
    s->setHasAtlas(rB(o, "hasAtlas"));
    s->setHasBlackgem(rB(o, "hasBlackgem"));
    s->setTessCrowdsap(rD(o, "tessCrowdsap"));

    std::vector<QString> bibs;
    for (const QJsonValue &v : o.value("bibcodes").toArray())
        bibs.push_back(v.toString());
    s->setBibcodes(bibs);

    // Spectra (set directly → marks loaded, no DB loader needed)
    if (o.contains("spectra")) {
        std::vector<std::shared_ptr<Spectrum>> specs;
        for (const QJsonValue &v : o.value("spectra").toArray())
            specs.push_back(spectrumFromJson(v.toObject(), br));
        s->setSpectra(specs);
    }
    if (o.contains("photometry"))
        s->setPhotometry(
            photometryFromJson(o.value("photometry").toObject(), br));
    if (o.contains("rv"))
        s->setRVCurve(rvCurveFromJson(o.value("rv").toObject(), instById));
    return s;
}

} // anonymous namespace

// ═════════════════════════════════════════════════════════════════════════════
//  Public API
// ═════════════════════════════════════════════════════════════════════════════
QByteArray
StarPackage::writeToBuffer(const std::vector<std::shared_ptr<Star>> &stars,
                           const ExportOptions &opts, QString *error,
                           const InstrumentResolver &resolver,
                           const ProgressFn         &progress) {
    BlobPool          bp;
    std::set<QString> referencedInstIds;

    QJsonArray starArr;
    const int  nStars = int(stars.size());
    int        si     = 0;
    for (const auto &s : stars) {
        ++si;
        if (s)
            starArr.append(starToJson(*s, bp, opts, referencedInstIds));
        if (progress)
            progress(nStars > 0 ? int(85.0 * si / nStars) : 85,
                     QStringLiteral("Serializing star %1 of %2")
                         .arg(si)
                         .arg(nStars));
    }

    // Instruments - from RV-point objects directly + resolver for the rest.
    QJsonArray instArr;
    if (opts.includeInstruments) {
        std::map<QString, QJsonObject> byId;
        for (const auto &s : stars) {
            if (!s)
                continue;
            if (auto c = s->getRVCurve())
                for (const auto &p : c->getRVPoints())
                    if (p && p->getInstrument())
                        byId[p->getInstrument()->getId()] =
                            p->getInstrument()->toJson();
        }
        if (resolver) {
            for (const QString &id : referencedInstIds) {
                if (byId.count(id))
                    continue;
                if (auto inst = resolver(id))
                    byId[id] = inst->toJson();
            }
        }
        for (auto &kv : byId)
            instArr.append(kv.second);
    }

    QJsonObject manifest;
    manifest["format"]       = "astra-package";
    manifest["versionMajor"] = VERSION_MAJOR;
    manifest["versionMinor"] = VERSION_MINOR;
    manifest["createdBy"]    = "ASTRA";
    manifest["createdAt"] =
        QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    if (!opts.creatorNote.isEmpty())
        manifest["note"] = opts.creatorNote;
    manifest["stars"]       = starArr;
    manifest["instruments"] = instArr;
    manifest["blobs"]       = bp.dir;

    const QByteArray manifestBytes =
        QJsonDocument(manifest).toJson(QJsonDocument::Compact);

    // inner = [manifestLen u32 LE][manifest][blob pool]
    QByteArray    inner;
    const quint32 mlen = quint32(manifestBytes.size());
    inner.append(char(mlen & 0xff));
    inner.append(char((mlen >> 8) & 0xff));
    inner.append(char((mlen >> 16) & 0xff));
    inner.append(char((mlen >> 24) & 0xff));
    inner.append(manifestBytes);
    inner.append(bp.data);

    if (progress)
        progress(90, QStringLiteral("Compressing…"));
    const QByteArray body = qCompress(inner, COMPRESS_LVL);
    if (progress)
        progress(95, QStringLiteral("Finalizing…"));

    QByteArray  out;
    QDataStream ds(&out, QIODevice::WriteOnly);
    ds.setByteOrder(QDataStream::LittleEndian);
    ds.writeRawData(MAGIC, 8);
    ds << quint16(VERSION_MAJOR) << quint16(VERSION_MINOR);
    ds << quint8(hostByteOrderCode()) << quint8(0) << quint16(0) << quint32(0);
    ds.writeRawData(body.constData(), body.size());

    if (error)
        error->clear();
    return out;
}

bool StarPackage::writeToFile(const QString &filepath,
                              const std::vector<std::shared_ptr<Star>> &stars,
                              const ExportOptions &opts, QString *error,
                              const InstrumentResolver &resolver,
                              const ProgressFn         &progress) {
    const QByteArray bytes =
        writeToBuffer(stars, opts, error, resolver, progress);
    if (bytes.isEmpty()) {
        if (error && error->isEmpty())
            *error = "Nothing to write.";
        return false;
    }

    QSaveFile f(filepath);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("Cannot open '%1' for writing: %2")
                         .arg(filepath, f.errorString());
        return false;
    }
    if (f.write(bytes) != bytes.size()) {
        if (error)
            *error = QStringLiteral("Short write to '%1'.").arg(filepath);
        return false;
    }
    if (!f.commit()) {
        if (error)
            *error = QStringLiteral("Failed to commit '%1': %2")
                         .arg(filepath, f.errorString());
        return false;
    }
    if (progress)
        progress(100, QStringLiteral("Done"));
    return true;
}

StarPackage::ImportResult StarPackage::readFromBuffer(const QByteArray &data,
                                                      const ProgressFn &progress) {
    ImportResult r;
    if (data.size() < HEADER_SIZE ||
        std::memcmp(data.constData(), MAGIC, 8) != 0) {
        r.error = "Not an ASTRA package (bad magic).";
        return r;
    }
    QDataStream ds(data);
    ds.setByteOrder(QDataStream::LittleEndian);
    char magic[8];
    ds.readRawData(magic, 8);
    quint16 vMaj, vMin;
    ds >> vMaj >> vMin;
    quint8  bo, rsv0;
    quint16 rsv1;
    quint32 flags;
    ds >> bo >> rsv0 >> rsv1 >> flags;
    r.fileVersionMajor = vMaj;
    r.fileVersionMinor = vMin;

    if (vMaj > VERSION_MAJOR) {
        r.error =
            QStringLiteral("File format v%1.%2 is newer than supported v%3.%4. "
                           "Please update ASTRA.")
                .arg(vMaj)
                .arg(vMin)
                .arg(VERSION_MAJOR)
                .arg(VERSION_MINOR);
        return r;
    }
    if (vMaj < VERSION_MAJOR)
        r.warnings << QStringLiteral("Reading older package format v%1.%2.")
                          .arg(vMaj)
                          .arg(vMin);

    if (progress)
        progress(5, QStringLiteral("Decompressing…"));
    const QByteArray body  = data.mid(HEADER_SIZE);
    const QByteArray inner = qUncompress(body);
    if (inner.size() < 4) {
        r.error = "Corrupt or empty package body.";
        return r;
    }

    const uchar  *ip   = reinterpret_cast<const uchar *>(inner.constData());
    const quint32 mlen = quint32(ip[0]) | (quint32(ip[1]) << 8) |
                         (quint32(ip[2]) << 16) | (quint32(ip[3]) << 24);
    if (qint64(4) + mlen > inner.size()) {
        r.error = "Corrupt manifest length.";
        return r;
    }

    const QByteArray manifestBytes = inner.mid(4, int(mlen));
    const QByteArray pool          = inner.mid(4 + int(mlen));

    QJsonParseError     perr;
    const QJsonDocument doc = QJsonDocument::fromJson(manifestBytes, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        r.error =
            QStringLiteral("Manifest JSON error: %1").arg(perr.errorString());
        return r;
    }
    const QJsonObject manifest = doc.object();
    r.creatorApp               = manifest.value("createdBy").toString();
    r.createdAtIso             = manifest.value("createdAt").toString();
    r.creatorNote              = manifest.value("note").toString();

    BlobReader br;
    br.pool     = pool;
    br.dir      = manifest.value("blobs").toArray();
    br.swap     = (bo != hostByteOrderCode());
    br.warnings = &r.warnings;

    if (progress)
        progress(10, QStringLiteral("Reading instruments…"));
    // Instruments first (RV points link to them by id)
    std::map<QString, std::shared_ptr<Instrument>> instById;
    for (const QJsonValue &v : manifest.value("instruments").toArray()) {
        auto inst =
            std::make_shared<Instrument>(Instrument::fromJson(v.toObject()));
        instById[inst->getId()] = inst;
        r.instruments.push_back(inst);
    }

    const QJsonArray starsArr = manifest.value("stars").toArray();
    const int        nStars   = starsArr.size();
    int              si       = 0;
    for (const QJsonValue &v : starsArr) {
        r.stars.push_back(starFromJson(v.toObject(), br, instById));
        ++si;
        if (progress)
            progress(nStars > 0 ? 10 + int(85.0 * si / nStars) : 95,
                     QStringLiteral("Reading star %1 of %2")
                         .arg(si)
                         .arg(nStars));
    }

    if (progress)
        progress(100, QStringLiteral("Done"));
    r.success = true;
    return r;
}

StarPackage::ImportResult StarPackage::readFromFile(const QString    &filepath,
                                                    const ProgressFn &progress) {
    ImportResult r;
    QFile        f(filepath);
    if (!f.open(QIODevice::ReadOnly)) {
        r.error = QStringLiteral("Cannot open '%1': %2")
                      .arg(filepath, f.errorString());
        return r;
    }
    if (progress)
        progress(2, QStringLiteral("Reading file…"));
    return readFromBuffer(f.readAll(), progress);
}

bool StarPackage::isStarPackage(const QString &filepath) {
    QFile f(filepath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    char magic[8];
    return f.read(magic, 8) == 8 && std::memcmp(magic, MAGIC, 8) == 0;
}

bool StarPackage::peekVersion(const QString &filepath, quint16 &major,
                              quint16 &minor) {
    QFile f(filepath);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    QByteArray head = f.read(HEADER_SIZE);
    if (head.size() < HEADER_SIZE ||
        std::memcmp(head.constData(), MAGIC, 8) != 0)
        return false;
    QDataStream ds(head);
    ds.setByteOrder(QDataStream::LittleEndian);
    char m[8];
    ds.readRawData(m, 8);
    ds >> major >> minor;
    return true;
}