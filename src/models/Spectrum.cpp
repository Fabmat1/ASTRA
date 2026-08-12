#include "Spectrum.h"
#include "utils/DataStore.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QDataStream>
#include <QDebug>
#include "utils/Logger.h"

// Trailing extension block of a SpectralFit data file. The original format is
// six length-prefixed arrays with no version field, so there is nowhere to bump
// a version: new arrays are appended behind this magic instead. A reader that
// finds anything else (or nothing) after modelIgnore is looking at a file
// written before the extension existed.
static constexpr quint32 kFitExtMagic   = 0xA57EA001;
static constexpr quint32 kFitExtVersion = 1;

// SpectralFit implementation
SpectralFit::SpectralFit()
    : isBestFit(false)
    , isFlagged(false)
    , teff(0.0)
    , teffError(0.0)
    , logg(0.0)
    , loggError(0.0)
    , he(0.0)
    , heError(0.0)
    , vsini(0.0)
    , vsiniError(0.0)
    , radialVelocity(0.0)
    , radialVelocityError(0.0)
    , chi2(0.0)
    , metallicity(0.0)
    , metallicityError(0.0)
    , macroturbulence(0.0)
    , macroturbulenceError(0.0)
    , microturbulence(0.0)
    , microturbulenceError(0.0)
{
    creationDate = QDateTime::currentDateTime();
}

Spectrum::Spectrum()
    : _time()                          
    , _isBarycentricallyCorrected(false)
{
}

Spectrum::~Spectrum()
{
}

void Spectrum::setData(const std::vector<double>& wavelengths,
                      const std::vector<double>& fluxes,
                      const std::vector<double>& errors)
{
    _wavelengths = wavelengths;
    _fluxes = fluxes;
    _fluxErrors = errors;
}

void Spectrum::notifyBestFitChanged()
{
    if (_bestFitChangedCb) _bestFitChangedCb(this, getBestFit());
}

void Spectrum::addSpectralFit(std::shared_ptr<SpectralFit> fit)
{
    const bool willBecomeBest = fit->isBestFit;
    if (willBecomeBest) {
        for (auto& existing : _spectralFits)
            existing->isBestFit = false;
    }
    _spectralFits.push_back(fit);
    if (willBecomeBest) notifyBestFitChanged();
}

void Spectrum::removeSpectralFit(const QString& fitId)
{
    bool removedBest = false;
    for (const auto& f : _spectralFits)
        if (f && f->getId() == fitId && f->isBestFit) { removedBest = true; break; }

    _spectralFits.erase(
        std::remove_if(_spectralFits.begin(), _spectralFits.end(),
            [&fitId](const std::shared_ptr<SpectralFit>& f) {
                return f && f->getId() == fitId;
            }),
        _spectralFits.end());

    // Deleting the best fit would otherwise leave a spectrum that still has
    // fits but no best one. The RV curve cannot represent that: its point stays
    // linked to a fit that no longer exists, so it can neither be reset (no
    // best fit to reset to) nor removed (the spectrum is still there). Hand the
    // role to the next remaining fit so the RV point re-links to it. Callers
    // that own a DB connection are expected to persist the new flag.
    if (removedBest && !_spectralFits.empty() && !getBestFit())
        _spectralFits.front()->isBestFit = true;

    if (removedBest) notifyBestFitChanged();
}

std::vector<std::shared_ptr<SpectralFit>> Spectrum::getSpectralFits() const
{
    return _spectralFits;
}

std::shared_ptr<SpectralFit> Spectrum::getBestFit() const
{
    for (const auto& fit : _spectralFits) {
        if (fit->isBestFit) {
            return fit;
        }
    }
    return nullptr;
}

bool Spectrum::loadFromFile(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    std::vector<double> wavelengths;
    std::vector<double> fluxes;
    std::vector<double> errors;

    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        if (line.trimmed().isEmpty() || line.startsWith("#")) {
            continue;
        }

        QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (parts.size() >= 3) {
            bool ok1, ok2, ok3;
            double wl = parts[0].toDouble(&ok1);
            double flux = parts[1].toDouble(&ok2);
            double err = parts[2].toDouble(&ok3);

            if (ok1 && ok2 && ok3) {
                wavelengths.push_back(wl);
                fluxes.push_back(flux);
                errors.push_back(err);
            }
        }
    }

    file.close();

    if (!wavelengths.empty()) {
        setData(wavelengths, fluxes, errors);
        _file = filepath;
        return true;
    }

    return false;
}

bool Spectrum::saveDataToFile(const QString& filepath)
{
    if (_wavelengths.empty()) {
        LOG_ERROR("Spectrum", QString("REFUSING to save empty spectrum data to %1 (id=%2)")
            .arg(filepath).arg(_id));
        // Print a stack trace or breakpoint here
        return false;
    }
    QByteArray buffer;
    {
        QDataStream s(&buffer, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);

        s << static_cast<quint32>(_wavelengths.size());
        s << static_cast<quint32>(_fluxes.size());
        s << static_cast<quint32>(_fluxErrors.size());

        for (const auto& v : _wavelengths) s << v;
        for (const auto& v : _fluxes)      s << v;
        for (const auto& v : _fluxErrors)  s << v;
    }
    return DataStore::writeCompressed(filepath, DataStore::SpectrumData, buffer);
}


bool Spectrum::loadDataFromFile(const QString& filepath)
{
    auto parse = [this](QDataStream& s) -> bool {
        quint32 wlN, fN, eN;
        s >> wlN >> fN >> eN;

        _wavelengths.clear();  _wavelengths.reserve(wlN);
        for (quint32 i = 0; i < wlN; ++i) { double v; s >> v; _wavelengths.push_back(v); }

        _fluxes.clear();  _fluxes.reserve(fN);
        for (quint32 i = 0; i < fN; ++i) { double v; s >> v; _fluxes.push_back(v); }

        _fluxErrors.clear();  _fluxErrors.reserve(eN);
        for (quint32 i = 0; i < eN; ++i) { double v; s >> v; _fluxErrors.push_back(v); }

        return s.status() == QDataStream::Ok;
    };

    // Try new compressed format
    QByteArray buf;
    if (DataStore::readCompressed(filepath, DataStore::SpectrumData, buf)) {
        QDataStream s(&buf, QIODevice::ReadOnly);
        s.setVersion(QDataStream::Qt_6_0);
        return parse(s);
    }

    // Legacy uncompressed fallback
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Failed to open spectrum data file for reading:" << filepath;
        return false;
    }
    QDataStream s(&file);
    s.setVersion(QDataStream::Qt_5_0);
    return parse(s);
}

bool SpectralFit::saveDataToFile(const QString &filepath) {
    if (!hasData()) {
        LOG_ERROR("SpectralFit",
                  QString("REFUSING to save empty fit data to %1 (id=%2)")
                      .arg(filepath)
                      .arg(_id));
        return false;
    }
    QByteArray buffer;
    {
        QDataStream s(&buffer, QIODevice::WriteOnly);
        s.setVersion(QDataStream::Qt_6_0);

        s << static_cast<quint32>(modelWavelengths.size());
        s << static_cast<quint32>(modelFluxes.size());
        s << static_cast<quint32>(rebinnedFluxes.size());
        s << static_cast<quint32>(rebinnedSigmas.size());
        s << static_cast<quint32>(modelSplines.size());
        s << static_cast<quint32>(modelIgnore.size());

        for (const auto& v : modelWavelengths) s << v;
        for (const auto& v : modelFluxes)      s << v;
        for (const auto& v : rebinnedFluxes)   s << v;
        for (const auto& v : rebinnedSigmas)   s << v;
        for (const auto& v : modelSplines)     s << v;
        for (const auto& v : modelIgnore)      s << static_cast<quint8>(v);

        // Extension: per-component models and the fitted telluric transmission.
        // Appended after the original six arrays so an older reader simply
        // stops before it.
        s << kFitExtMagic << kFitExtVersion;
        auto writeArray = [&s](const std::vector<double>& v) {
            s << static_cast<quint32>(v.size());
            for (const auto& x : v) s << x;
        };
        writeArray(modelFluxesComp1);
        writeArray(modelFluxesComp2);
        writeArray(telluricTransmission);
    }
    return DataStore::writeCompressed(filepath, DataStore::SpectralFitData,
                                      buffer);
}

bool SpectralFit::loadDataFromFile(const QString& filepath)
{
    LOG_INFO_F("SpectralFit", QString("loadDataFromFile: %1").arg(filepath));

    auto readDoubles = [](QDataStream& s, std::vector<double>& vec, quint32 n) {
        vec.clear(); vec.reserve(n);
        for (quint32 i = 0; i < n; ++i) { double v; s >> v; vec.push_back(v); }
    };

    auto parse = [this, &readDoubles](QDataStream& s, qint64 bufferSize, bool legacy = false) -> bool {
        quint32 wlN, fN, rbfN = 0, rbsN = 0, splN = 0, ignN = 0;
        s >> wlN >> fN;
        if (!legacy) s >> rbfN >> rbsN >> splN >> ignN;

        if (s.status() != QDataStream::Ok) {
            LOG_ERROR("SpectralFit", "Stream error reading headers");
            return false;
        }

        // Minimum size the headers imply. A file may legitimately be larger:
        // everything after modelIgnore is the optional extension block read at
        // the end of this function.
        quint64 expectedBytes = (quint64(wlN) + fN + rbfN + rbsN + splN) * sizeof(double)
                              + ignN * sizeof(quint8);
        quint64 headerBytes = legacy ? 8 : 24;

        LOG_DEBUG_F("SpectralFit",
            QString("parse (legacy=%1): wlN=%2, fN=%3, rbfN=%4, rbsN=%5, splN=%6, ignN=%7")
            .arg(legacy).arg(wlN).arg(fN).arg(rbfN).arg(rbsN).arg(splN).arg(ignN));

        if (bufferSize > 0 && (headerBytes + expectedBytes) > quint64(bufferSize)) {
            LOG_WARNING_F("SpectralFit",
                QString("Header counts require %1 bytes but buffer is only %2 bytes; retrying as legacy")
                .arg(headerBytes + expectedBytes).arg(bufferSize));
            return false;
        }

        readDoubles(s, modelWavelengths, wlN);
        if (s.status() != QDataStream::Ok) return false;

        readDoubles(s, modelFluxes, fN);
        if (s.status() != QDataStream::Ok) return false;

        readDoubles(s, rebinnedFluxes, rbfN);
        if (s.status() != QDataStream::Ok) return false;

        readDoubles(s, rebinnedSigmas, rbsN);
        if (s.status() != QDataStream::Ok) return false;

        readDoubles(s, modelSplines, splN);
        if (s.status() != QDataStream::Ok) return false;

        modelIgnore.clear(); modelIgnore.reserve(ignN);
        for (quint32 i = 0; i < ignN; ++i) { quint8 v; s >> v; modelIgnore.push_back(v); }

        if (s.status() != QDataStream::Ok) return false;

        // Optional extension block. Absent in every file written before it
        // existed, so a stream that ends here — or holds anything other than
        // the magic — just leaves these three arrays empty; the six arrays
        // above are already valid either way.
        modelFluxesComp1.clear();
        modelFluxesComp2.clear();
        telluricTransmission.clear();

        if (!s.atEnd()) {
            quint32 magic = 0;
            s >> magic;
            if (s.status() == QDataStream::Ok && magic == kFitExtMagic) {
                quint32 version = 0;
                s >> version;

                auto readExtArray = [&s](std::vector<double>& vec) -> bool {
                    quint32 n = 0;
                    s >> n;
                    if (s.status() != QDataStream::Ok) return false;
                    // Never let a corrupt count turn into a huge reserve().
                    const QIODevice* dev = s.device();
                    if (dev && quint64(n) * sizeof(double) >
                                   quint64(dev->bytesAvailable()))
                        return false;
                    vec.clear(); vec.reserve(n);
                    for (quint32 i = 0; i < n; ++i) { double v; s >> v; vec.push_back(v); }
                    return s.status() == QDataStream::Ok;
                };

                if (!(readExtArray(modelFluxesComp1) &&
                      readExtArray(modelFluxesComp2) &&
                      readExtArray(telluricTransmission))) {
                    LOG_WARNING_F("SpectralFit",
                        QString("Truncated extension block (v%1); keeping the "
                                "combined model only").arg(version));
                    modelFluxesComp1.clear();
                    modelFluxesComp2.clear();
                    telluricTransmission.clear();
                }
            }
        }

        return true;
    };

    QByteArray buf;
    if (DataStore::readCompressed(filepath, DataStore::SpectralFitData, buf)) {
        LOG_DEBUG_F("SpectralFit", QString("readCompressed succeeded, buffer size=%1 bytes").arg(buf.size()));

        // Try new 6-header format first
        QDataStream s(&buf, QIODevice::ReadOnly);
        s.setVersion(QDataStream::Qt_6_0);
        if (parse(s, buf.size()))
            return true;

        // Fall back to 2-header (legacy) format on the same compressed buffer
        LOG_DEBUG("SpectralFit", "Retrying compressed buffer as legacy format");
        QDataStream s2(&buf, QIODevice::ReadOnly);
        s2.setVersion(QDataStream::Qt_6_0);
        return parse(s2, buf.size(), /*legacy=*/true);
    }

    LOG_DEBUG("SpectralFit", "readCompressed failed, falling back to legacy file path");

    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_ERROR_F("SpectralFit", QString("Failed to open file for reading: %1").arg(filepath));
        return false;
    }
    QByteArray legacyBuf = file.readAll();
    file.close();

    QDataStream s(&legacyBuf, QIODevice::ReadOnly);
    s.setVersion(QDataStream::Qt_5_0);
    return parse(s, legacyBuf.size(), /*legacy=*/true);
}

void Spectrum::setBestFitById(const QString& fitId)
{
    for (auto& f : _spectralFits)
        if (f) f->isBestFit = (!fitId.isEmpty() && f->getId() == fitId);
    notifyBestFitChanged();
}

void Spectrum::notifyFitChanged(const std::shared_ptr<SpectralFit>& fit)
{
    if (_fitChangedCb) _fitChangedCb(this, fit);
}