#include "spectra/Spectrum.h"
#include "core/StreamBytes.h"
#include "app/ArrayCodec.h"
#include "app/DataStore.h"

#include <algorithm>
#include <cstring>
#include <QDateTime>
#include <QDir>
#include <QSet>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QDataStream>
#include <QDebug>
#include "app/Logger.h"

// Trailing extension block of a SpectralFit data file. The original format is
// six length-prefixed arrays with no version field, so there is nowhere to bump
// a version: new arrays are appended behind this magic instead. A reader that
// finds anything else (or nothing) after modelIgnore is looking at a file
// written before the extension existed.
static constexpr quint32 kFitExtMagic   = 0xA57EA001;
// v2 appends lightFractionComp1 after the v1 arrays.
static constexpr quint32 kFitExtVersion = 2;

/*  Coded layouts (DataStore version 2; arrays through ArrayCodec).
 *
 *  Spectrum: u8 layout, wavelengths, fluxes, errors.
 *
 *  Fit: u8 layout, u8 flags, u64 base hash when FitHasBase, then
 *  [wavelengths, rebinned fluxes unless FitHasBase], model fluxes, rebinned
 *  sigmas, splines, ignore mask, [comp1 unless FitComp1IsModel], comp2,
 *  telluric transmission, component 1's light fraction.
 *
 *  Every fit of a spectrum has the same wavelength grid and rebinned fluxes,
 *  so those two arrays live once per star directory in a base file named
 *  after their hash (fitbase_<hash>.asd, layout: u8 layout, wavelengths,
 *  rebinned fluxes). Identical arrays share one file, so writing needs no
 *  bookkeeping; pruneFitBases() removes the ones no fit refers to.          */
static constexpr quint8 kCodedLayout = 1;
enum : quint8 { FitComp1IsModel = 1, FitHasBase = 2 };

namespace {

bool bitEqual(const std::vector<double>& a, const std::vector<double>& b)
{
    return a.size() == b.size() &&
           (a.empty() || std::memcmp(a.data(), b.data(), a.size() * 8) == 0);
}

quint64 baseHash(const std::vector<double>& wl, const std::vector<double>& rbf)
{
    QByteArray b;
    StreamBytes::putU32(b, quint32(wl.size()));
    StreamBytes::putF64s(b, wl);
    StreamBytes::putU32(b, quint32(rbf.size()));
    StreamBytes::putF64s(b, rbf);
    return ArrayCodec::hash64(b.constData(), b.size());
}

QString basePath(const QString& fitPath, quint64 hash)
{
    return QFileInfo(fitPath).absolutePath() + QStringLiteral("/fitbase_") +
           QStringLiteral("%1").arg(hash, 16, 16, QLatin1Char('0')) + DataStore::FILE_EXT;
}

bool readBase(const QString& path, std::vector<double>& wl, std::vector<double>& rbf)
{
    QByteArray buf;
    bool coded = false;
    if (!DataStore::readCompressed(path, DataStore::FitBaseData, buf, &coded) || !coded)
        return false;
    StreamBytes::Reader r(buf);
    if (r.u8() != kCodedLayout) return false;
    ArrayCodec::getF64s(r, wl);
    ArrayCodec::getF64s(r, rbf);
    return r.ok;
}

/// Makes sure the base file for (wl, rbf) exists beside `fitPath` and holds
/// exactly these arrays. False when it can't: the fit then keeps them itself.
bool ensureBase(const QString& fitPath, quint64 hash,
                const std::vector<double>& wl, const std::vector<double>& rbf)
{
    const QString path = basePath(fitPath, hash);
    if (QFile::exists(path)) {
        std::vector<double> w, f;
        if (readBase(path, w, f) && bitEqual(w, wl) && bitEqual(f, rbf)) return true;
        LOG_WARNING("SpectralFit", QString("base file %1 does not hold this fit's arrays; "
                                           "the fit keeps its own").arg(path));
        return false;
    }
    QByteArray b;
    StreamBytes::putU8(b, kCodedLayout);
    ArrayCodec::putF64s(b, wl);
    ArrayCodec::putF64s(b, rbf);
    return DataStore::writeCoded(path, DataStore::FitBaseData, b);
}

/// The header of a coded fit file without reading the rest: its flags and
/// base hash. False for anything else.
bool fitHead(const QString& path, quint8& flags, quint64& hash)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray h = f.read(4 + 2 + 2 + 1 + 4 + 1 + 1 + 8);
    if (h.size() < 15 || !h.startsWith("ASTR")) return false;
    StreamBytes::Reader r(h);
    r.p += 4;
    const quint16 version = quint16(r.u8() << 8 | r.u8());
    const quint16 type    = quint16(r.u8() << 8 | r.u8());
    if (version < 2 || type != DataStore::SpectralFitData) return false;
    if (r.u8() != 0) return false;                  // codec: coded payloads are stored as they are
    r.u32();                                        // payload length
    if (r.u8() != kCodedLayout) return false;
    flags = r.u8();
    hash  = (flags & FitHasBase) ? r.u64() : 0;
    return r.ok;
}

}   // namespace

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
    QByteArray b;
    StreamBytes::putU8(b, kCodedLayout);
    ArrayCodec::putF64s(b, _wavelengths);
    ArrayCodec::putF64s(b, _fluxes);
    ArrayCodec::putF64s(b, _fluxErrors);
    return DataStore::writeCoded(filepath, DataStore::SpectrumData, b);
}


bool Spectrum::loadDataFromFile(const QString& filepath)
{
    auto parse = [this](QDataStream& s) -> bool {
        quint32 wlN, fN, eN;
        s >> wlN >> fN >> eN;

        StreamBytes::readF64s(s, _wavelengths, wlN);
        StreamBytes::readF64s(s, _fluxes, fN);
        StreamBytes::readF64s(s, _fluxErrors, eN);

        return s.status() == QDataStream::Ok;
    };

    // Try new compressed format
    QByteArray buf;
    bool coded = false;
    if (DataStore::readCompressed(filepath, DataStore::SpectrumData, buf, &coded)) {
        if (coded) {
            StreamBytes::Reader r(buf);
            if (r.u8() != kCodedLayout) return false;
            ArrayCodec::getF64s(r, _wavelengths);
            ArrayCodec::getF64s(r, _fluxes);
            ArrayCodec::getF64s(r, _fluxErrors);
            return r.ok;
        }
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

    quint8 flags = 0;
    // For a one-component fit comp1 is the combined model, bit for bit.
    if (!modelFluxesComp1.empty() && bitEqual(modelFluxesComp1, modelFluxes))
        flags |= FitComp1IsModel;
    quint64 hash = 0;
    if (!modelWavelengths.empty()) {
        hash = baseHash(modelWavelengths, rebinnedFluxes);
        if (ensureBase(filepath, hash, modelWavelengths, rebinnedFluxes))
            flags |= FitHasBase;
    }

    QByteArray b;
    StreamBytes::putU8(b, kCodedLayout);
    StreamBytes::putU8(b, flags);
    if (flags & FitHasBase) {
        StreamBytes::putU64(b, hash);
    } else {
        ArrayCodec::putF64s(b, modelWavelengths);
        ArrayCodec::putF64s(b, rebinnedFluxes);
    }
    ArrayCodec::putF64s(b, modelFluxes);
    ArrayCodec::putF64s(b, rebinnedSigmas);
    ArrayCodec::putF64s(b, modelSplines);
    ArrayCodec::putMask(b, modelIgnore);
    if (!(flags & FitComp1IsModel)) ArrayCodec::putF64s(b, modelFluxesComp1);
    ArrayCodec::putF64s(b, modelFluxesComp2);
    ArrayCodec::putF64s(b, telluricTransmission);
    ArrayCodec::putF64s(b, lightFractionComp1);
    return DataStore::writeCoded(filepath, DataStore::SpectralFitData, b);
}

QString SpectralFit::basePathOf(const QString& filepath)
{
    quint8 flags = 0;
    quint64 hash = 0;
    if (!fitHead(filepath, flags, hash) || !(flags & FitHasBase)) return {};
    return basePath(filepath, hash);
}

int SpectralFit::pruneFitBases(const QString& spectraDir, int minAgeSecs)
{
    QDir dir(spectraDir);
    const QStringList bases = dir.entryList({QStringLiteral("fitbase_*") + DataStore::FILE_EXT},
                                            QDir::Files);
    if (bases.isEmpty()) return 0;

    QSet<QString> used;
    for (const QString& f : dir.entryList({QStringLiteral("fit_*") + DataStore::FILE_EXT},
                                          QDir::Files)) {
        const QString b = basePathOf(dir.filePath(f));
        if (!b.isEmpty()) used.insert(QFileInfo(b).fileName());
    }
    const QDateTime recent = QDateTime::currentDateTime().addSecs(-minAgeSecs);
    int removed = 0;
    for (const QString& b : bases) {
        if (used.contains(b)) continue;
        if (minAgeSecs > 0 && QFileInfo(dir.filePath(b)).lastModified() > recent) continue;
        if (QFile::remove(dir.filePath(b))) ++removed;
    }
    return removed;
}

static bool parseCodedFit(SpectralFit& fit, const QByteArray& buf, const QString& filepath)
{
    StreamBytes::Reader r(buf);
    if (r.u8() != kCodedLayout) {
        LOG_ERROR_F("SpectralFit", QString("unknown layout in %1").arg(filepath));
        return false;
    }
    const quint8 flags = r.u8();
    if (flags & FitHasBase) {
        const quint64 hash = r.u64();
        const QString base = basePath(filepath, hash);
        if (!r.ok || !readBase(base, fit.modelWavelengths, fit.rebinnedFluxes) ||
            baseHash(fit.modelWavelengths, fit.rebinnedFluxes) != hash) {
            LOG_ERROR_F("SpectralFit", QString("%1 needs the base file %2, which is missing "
                                               "or damaged").arg(filepath, base));
            fit.modelWavelengths.clear();
            fit.rebinnedFluxes.clear();
            return false;
        }
    } else {
        ArrayCodec::getF64s(r, fit.modelWavelengths);
        ArrayCodec::getF64s(r, fit.rebinnedFluxes);
    }
    ArrayCodec::getF64s(r, fit.modelFluxes);
    ArrayCodec::getF64s(r, fit.rebinnedSigmas);
    ArrayCodec::getF64s(r, fit.modelSplines);
    ArrayCodec::getMask(r, fit.modelIgnore);
    if (flags & FitComp1IsModel) fit.modelFluxesComp1 = fit.modelFluxes;
    else ArrayCodec::getF64s(r, fit.modelFluxesComp1);
    ArrayCodec::getF64s(r, fit.modelFluxesComp2);
    ArrayCodec::getF64s(r, fit.telluricTransmission);
    ArrayCodec::getF64s(r, fit.lightFractionComp1);
    if (!r.ok) {
        LOG_ERROR_F("SpectralFit", QString("damaged fit file %1").arg(filepath));
        return false;
    }
    return true;
}

bool SpectralFit::loadDataFromFile(const QString& filepath)
{
    LOG_INFO_F("SpectralFit", QString("loadDataFromFile: %1").arg(filepath));

    auto readDoubles = [](QDataStream& s, std::vector<double>& vec, quint32 n) {
        StreamBytes::readF64s(s, vec, n);
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
        // existed, so a stream that ends here, or holds anything other than
        // the magic, just leaves these three arrays empty; the six arrays
        // above are already valid either way.
        modelFluxesComp1.clear();
        modelFluxesComp2.clear();
        telluricTransmission.clear();
        lightFractionComp1.clear();

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
                } else if (version >= 2 && !readExtArray(lightFractionComp1)) {
                    // The v1 arrays are complete; only the fractions are lost,
                    // which just hides the component overlays.
                    LOG_WARNING("SpectralFit",
                        "Truncated light fractions; component overlays hidden");
                    lightFractionComp1.clear();
                }
            }
        }

        return true;
    };

    QByteArray buf;
    bool coded = false;
    if (DataStore::readCompressed(filepath, DataStore::SpectralFitData, buf, &coded)) {
        LOG_DEBUG_F("SpectralFit", QString("readCompressed succeeded, buffer size=%1 bytes").arg(buf.size()));
        if (coded) return parseCodedFit(*this, buf, filepath);

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