#ifndef INSTRUMENTMODE_H
#define INSTRUMENTMODE_H

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QJsonObject>
#include <QJsonArray>
#include <optional>

struct ResolutionModel {
    // R(λ) = c[0] + c[1]*λ + c[2]*λ² + ...   (λ in Angstroms)
    QVector<double> coefficients;

    double at(double wavelengthAngstrom) const
    {
        if (coefficients.isEmpty()) return 0.0;
        double result = coefficients.last();
        for (int i = coefficients.size() - 2; i >= 0; --i)
            result = result * wavelengthAngstrom + coefficients[i];
        return result;
    }

    bool isValid() const { return !coefficients.isEmpty(); }

    static ResolutionModel constant(double R)                    { return {{R}}; }
    static ResolutionModel linear(double offset, double slope)   { return {{offset, slope}}; }
    static ResolutionModel polynomial(const QVector<double>& c)  { return {c}; }

    QJsonArray toJsonArray() const;
    static ResolutionModel fromJsonArray(const QJsonArray& arr);
};

struct WavelengthSetup {
    QString     label;
    double      centralWavelength = 0.0;
    double      wavelengthMin     = 0.0;
    double      wavelengthMax     = 0.0;
    QVariantMap config;

    QJsonObject toJson() const;
    static WavelengthSetup fromJson(const QJsonObject& obj);
};

struct IgnoreRange {
    double wlLow  = 0.0;
    double wlHigh = 0.0;
    QJsonObject toJson() const;
    static IgnoreRange fromJson(const QJsonObject& o);
};

struct AnchorRange {
    double wlLow   = 0.0;
    double wlHigh  = 0.0;
    double spacing = 50.0;
    QJsonObject toJson() const;
    static AnchorRange fromJson(const QJsonObject& o);
};

struct DiggaFitDefaults {
    std::optional<double>   wlMin;
    std::optional<double>   wlMax;
    QVector<IgnoreRange>    ignore;
    QVector<AnchorRange>    anchors;
    std::optional<double>   resOffset;
    std::optional<double>   resSlope;

    bool isEmpty() const {
        return !wlMin && !wlMax && ignore.isEmpty() && anchors.isEmpty()
            && !resOffset && !resSlope;
    }
    QJsonObject toJson() const;
    static DiggaFitDefaults fromJson(const QJsonObject& o);
};

struct SpectralProperties {
    ResolutionModel             resolution;       // R(λ)
    QString                     disperser;
    double                      wavelengthMin = 0.0;
    double                      wavelengthMax = 0.0;
    QVector<WavelengthSetup>    commonSetups;
    DiggaFitDefaults            fitDefaults;

    QJsonObject toJson() const;
    static SpectralProperties fromJson(const QJsonObject& obj);
};

struct PhotometricProperties {
    double      cadence    = 0.0;
    double      fov        = 0.0;
    double      pixelScale = 0.0;
    QStringList filters;

    QJsonObject toJson() const;
    static PhotometricProperties fromJson(const QJsonObject& obj);
};

class InstrumentMode
{
public:
    enum DataType { Spectroscopy, Photometry, Imaging, IFU, Other };

    InstrumentMode() = default;
    InstrumentMode(const QString& key, const QString& displayName, DataType dataType);

    QString  key() const         { return _key; }
    QString  displayName() const { return _displayName; }
    QString  description() const { return _description; }
    DataType dataType() const    { return _dataType; }

    void setKey(const QString& k)         { _key = k; }
    void setDisplayName(const QString& n) { _displayName = n; }
    void setDescription(const QString& d) { _description = d; }
    void setDataType(DataType t)          { _dataType = t; }

    bool hasSpectralProperties() const    { return _spectral.has_value(); }
    bool hasPhotometricProperties() const { return _photometric.has_value(); }

    const SpectralProperties&    spectral() const    { return _spectral.value(); }
    const PhotometricProperties& photometric() const { return _photometric.value(); }

    void setSpectralProperties(const SpectralProperties& sp)    { _spectral = sp; }
    void setPhotometricProperties(const PhotometricProperties& pp) { _photometric = pp; }

    void clearSpectralProperties()    { _spectral.reset(); }
    void clearPhotometricProperties() { _photometric.reset(); }

    double resolutionAt(double wavelengthAngstrom) const
    {
        return _spectral ? _spectral->resolution.at(wavelengthAngstrom) : 0.0;
    }

    const WavelengthSetup* setup(const QString& label) const
    {
        if (!_spectral) return nullptr;
        for (const auto& s : _spectral->commonSetups)
            if (s.label == label) return &s;
        return nullptr;
    }

    QVariantMap&       extras()       { return _extras; }
    const QVariantMap& extras() const { return _extras; }

    QJsonObject toJson() const;
    static InstrumentMode fromJson(const QJsonObject& obj);
    static QString   dataTypeToString(DataType dt);
    static DataType  dataTypeFromString(const QString& s);

private:
    QString  _key;
    QString  _displayName;
    QString  _description;
    DataType _dataType = Other;

    std::optional<SpectralProperties>    _spectral;
    std::optional<PhotometricProperties> _photometric;
    QVariantMap _extras;
};

#endif // INSTRUMENTMODE_H