#include "InstrumentMode.h"
#include <QJsonDocument>

// ── ResolutionModel ─────────────────────────────────────────────────────────

QJsonArray ResolutionModel::toJsonArray() const
{
    QJsonArray arr;
    for (double c : coefficients)
        arr.append(c);
    return arr;
}

ResolutionModel ResolutionModel::fromJsonArray(const QJsonArray& arr)
{
    ResolutionModel rm;
    for (const auto& v : arr)
        rm.coefficients.append(v.toDouble());
    return rm;
}

// ── WavelengthSetup ────────────────────────────────────────────────────────

QJsonObject WavelengthSetup::toJson() const
{
    QJsonObject obj;
    obj["label"]              = label;
    obj["central_wavelength"] = centralWavelength;
    obj["wavelength_min"]     = wavelengthMin;
    obj["wavelength_max"]     = wavelengthMax;
    if (!config.isEmpty())
        obj["config"] = QJsonObject::fromVariantMap(config);
    return obj;
}

WavelengthSetup WavelengthSetup::fromJson(const QJsonObject& obj)
{
    WavelengthSetup ws;
    ws.label             = obj["label"].toString();
    ws.centralWavelength = obj["central_wavelength"].toDouble();
    ws.wavelengthMin     = obj["wavelength_min"].toDouble();
    ws.wavelengthMax     = obj["wavelength_max"].toDouble();
    if (obj.contains("config"))
        ws.config = obj["config"].toObject().toVariantMap();
    return ws;
}

// ── SpectralProperties ─────────────────────────────────────────────────────

QJsonObject SpectralProperties::toJson() const
{
    QJsonObject obj;
    obj["disperser"]                = disperser;
    obj["resolution_coefficients"]  = resolution.toJsonArray();
    obj["wavelength_min"]           = wavelengthMin;
    obj["wavelength_max"]           = wavelengthMax;

    if (!fitDefaults.isEmpty()) obj["fitDefaults"] = fitDefaults.toJson();

    if (!commonSetups.isEmpty()) {
        QJsonArray arr;
        for (const auto& s : commonSetups)
            arr.append(s.toJson());
        obj["common_setups"] = arr;
    }
    return obj;
}

SpectralProperties SpectralProperties::fromJson(const QJsonObject& obj)
{
    SpectralProperties sp;
    sp.disperser     = obj["disperser"].toString();
    sp.resolution    = ResolutionModel::fromJsonArray(obj["resolution_coefficients"].toArray());
    sp.wavelengthMin = obj["wavelength_min"].toDouble();
    sp.wavelengthMax = obj["wavelength_max"].toDouble();

    if (obj.contains("fitDefaults"))
    sp.fitDefaults = DiggaFitDefaults::fromJson(obj["fitDefaults"].toObject());

    for (const auto& v : obj["common_setups"].toArray())
        sp.commonSetups.append(WavelengthSetup::fromJson(v.toObject()));
    return sp;
}

// ── PhotometricProperties ──────────────────────────────────────────────────

QJsonObject PhotometricProperties::toJson() const
{
    QJsonObject obj;
    if (cadence > 0)    obj["cadence"]     = cadence;
    if (fov > 0)        obj["fov"]         = fov;
    if (pixelScale > 0) obj["pixel_scale"] = pixelScale;
    if (!filters.isEmpty()) {
        QJsonArray arr;
        for (const auto& f : filters) arr.append(f);
        obj["filters"] = arr;
    }
    return obj;
}

PhotometricProperties PhotometricProperties::fromJson(const QJsonObject& obj)
{
    PhotometricProperties pp;
    pp.cadence    = obj["cadence"].toDouble();
    pp.fov        = obj["fov"].toDouble();
    pp.pixelScale = obj["pixel_scale"].toDouble();
    for (const auto& v : obj["filters"].toArray())
        pp.filters.append(v.toString());
    return pp;
}

// ── InstrumentMode ─────────────────────────────────────────────────────────

InstrumentMode::InstrumentMode(const QString& key, const QString& displayName, DataType dataType)
    : _key(key), _displayName(displayName), _dataType(dataType)
{
}

QString InstrumentMode::dataTypeToString(DataType dt)
{
    switch (dt) {
    case Spectroscopy: return QStringLiteral("spectroscopy");
    case Photometry:   return QStringLiteral("photometry");
    case Imaging:      return QStringLiteral("imaging");
    case IFU:          return QStringLiteral("ifu");
    default:           return QStringLiteral("other");
    }
}

InstrumentMode::DataType InstrumentMode::dataTypeFromString(const QString& s)
{
    if (s == QLatin1String("spectroscopy")) return Spectroscopy;
    if (s == QLatin1String("photometry"))   return Photometry;
    if (s == QLatin1String("imaging"))      return Imaging;
    if (s == QLatin1String("ifu"))          return IFU;
    return Other;
}

QJsonObject InstrumentMode::toJson() const
{
    QJsonObject obj;
    obj["key"]          = _key;
    obj["display_name"] = _displayName;
    obj["data_type"]    = dataTypeToString(_dataType);
    if (!_description.isEmpty())
        obj["description"] = _description;

    if (_spectral)
        obj["spectral"] = _spectral->toJson();
    if (_photometric)
        obj["photometric"] = _photometric->toJson();
    if (!_extras.isEmpty())
        obj["extras"] = QJsonObject::fromVariantMap(_extras);

    return obj;
}

InstrumentMode InstrumentMode::fromJson(const QJsonObject& obj)
{
    InstrumentMode mode;
    mode._key         = obj["key"].toString();
    mode._displayName = obj["display_name"].toString();
    mode._description = obj["description"].toString();
    mode._dataType    = dataTypeFromString(obj["data_type"].toString());

    if (obj.contains("spectral"))
        mode._spectral = SpectralProperties::fromJson(obj["spectral"].toObject());
    if (obj.contains("photometric"))
        mode._photometric = PhotometricProperties::fromJson(obj["photometric"].toObject());
    if (obj.contains("extras"))
        mode._extras = obj["extras"].toObject().toVariantMap();

    return mode;
}

QJsonObject IgnoreRange::toJson() const
{ return { {"wlLow", wlLow}, {"wlHigh", wlHigh} }; }

IgnoreRange IgnoreRange::fromJson(const QJsonObject& o)
{ return { o["wlLow"].toDouble(), o["wlHigh"].toDouble() }; }

QJsonObject AnchorRange::toJson() const
{ return { {"wlLow", wlLow}, {"wlHigh", wlHigh}, {"spacing", spacing} }; }

AnchorRange AnchorRange::fromJson(const QJsonObject& o)
{ return { o["wlLow"].toDouble(), o["wlHigh"].toDouble(),
           o["spacing"].toDouble(50.0) }; }

QJsonObject DiggaFitDefaults::toJson() const
{
    QJsonObject o;
    if (wlMin)     o["wlMin"]     = *wlMin;
    if (wlMax)     o["wlMax"]     = *wlMax;
    if (resOffset) o["resOffset"] = *resOffset;
    if (resSlope)  o["resSlope"]  = *resSlope;

    if (!ignore.isEmpty()) {
        QJsonArray arr;
        for (const auto& r : ignore) arr.append(r.toJson());
        o["ignore"] = arr;
    }
    if (!anchors.isEmpty()) {
        QJsonArray arr;
        for (const auto& a : anchors) arr.append(a.toJson());
        o["anchors"] = arr;
    }
    return o;
}

DiggaFitDefaults DiggaFitDefaults::fromJson(const QJsonObject& o)
{
    DiggaFitDefaults d;
    if (o.contains("wlMin"))     d.wlMin     = o["wlMin"].toDouble();
    if (o.contains("wlMax"))     d.wlMax     = o["wlMax"].toDouble();
    if (o.contains("resOffset")) d.resOffset = o["resOffset"].toDouble();
    if (o.contains("resSlope"))  d.resSlope  = o["resSlope"].toDouble();

    for (const auto& v : o["ignore"].toArray())
        d.ignore.append(IgnoreRange::fromJson(v.toObject()));
    for (const auto& v : o["anchors"].toArray())
        d.anchors.append(AnchorRange::fromJson(v.toObject()));
    return d;
}