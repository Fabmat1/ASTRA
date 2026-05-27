#include "DataStore.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QDataStream>
#include <QRegularExpression>
#include <QDebug>
#include <cstring>

// ── Compressed I/O ──────────────────────────────────────────────

bool DataStore::writeCompressed(const QString& filepath, DataType type,
                                const QByteArray& payload)
{
    if (!ensureDirForFile(filepath)) {
        qWarning() << "DataStore: cannot create directory for" << filepath;
        return false;
    }

    QFile file(filepath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "DataStore: cannot open for writing:" << filepath;
        return false;
    }

    file.write(MAGIC, 4);

    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_6_0);
    out << FORMAT_VERSION;
    out << static_cast<quint16>(type);

    // Don't bother compressing tiny payloads — zlib overhead dwarfs the savings.
    QByteArray compressed = (payload.size() < 256)
        ? qCompress(payload, 1)           
        : qCompress(payload, COMPRESSION_LEVEL);

    out << compressed;
    file.close();
    return file.error() == QFileDevice::NoError;
}

bool DataStore::readCompressed(const QString& filepath, DataType expectedType,
                               QByteArray& payload)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly))
        return false;

    char magic[4];
    if (file.read(magic, 4) != 4 || std::memcmp(magic, MAGIC, 4) != 0)
        return false;                        // not our format → caller uses legacy

    QDataStream in(&file);
    in.setVersion(QDataStream::Qt_6_0);

    quint16 version, type;
    in >> version >> type;

    if (version > FORMAT_VERSION) {
        qWarning() << "DataStore: file version" << version
                    << "newer than supported" << FORMAT_VERSION;
        return false;
    }
    if (type != static_cast<quint16>(expectedType)) {
        qWarning() << "DataStore: type mismatch in" << filepath
                    << "expected" << expectedType << "got" << type;
        return false;
    }

    QByteArray compressed;
    in >> compressed;

    payload = qUncompress(compressed);
    if (payload.isEmpty() && compressed.size() > 4) {
        // qCompress of empty data produces a 4-byte header;
        // anything larger that decompresses to empty → corruption.
        qWarning() << "DataStore: decompression failed for" << filepath;
        return false;
    }
    return true;
}

bool DataStore::isCompressedFormat(const QString& filepath)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    char magic[4];
    return file.read(magic, 4) == 4 && std::memcmp(magic, MAGIC, 4) == 0;
}

// ── Path helpers ────────────────────────────────────────────────

QString DataStore::sanitize(const QString& name)
{
    QString safe = name;
    safe.replace(QRegularExpression("[^a-zA-Z0-9_.-]"), "_");
    if (safe.isEmpty()) safe = "_";
    return safe;
}

QString DataStore::starDir(const QString& base, const QString& starId)
{
    return base + "/stars/" + starId;
}

QString DataStore::spectrumPath(const QString& base, const QString& starId,
                                const QString& spectrumId)
{
    return starDir(base, starId) + "/spectra/" + spectrumId + FILE_EXT;
}

QString DataStore::spectralFitPath(const QString& base, const QString& starId,
                                   const QString& spectrumId, const QString& fitId)
{
    return starDir(base, starId) + "/spectra/fit_" + spectrumId + "_" + fitId + FILE_EXT;
}

QString DataStore::photometricPointsPath(const QString& base, const QString& starId,
                                         const QString& photometryId)
{
    return starDir(base, starId) + "/photometry/points_" + photometryId + FILE_EXT;
}

QString DataStore::lightcurvePath(const QString& base, const QString& starId,
                                  const QString& photometryId, const QString& source)
{
    return starDir(base, starId) + "/photometry/lc_" + photometryId
           + "_" + sanitize(source) + FILE_EXT;
}

QString DataStore::sedModelPath(const QString& base, const QString& starId,
                                const QString& photometryId, const QString& modelId)
{
    return starDir(base, starId) + "/photometry/sed_" + photometryId
           + "_" + modelId + FILE_EXT;
}

QString DataStore::lcModelPath(const QString& base, const QString& starId,
                               const QString& photometryId, const QString& modelId)
{
    return starDir(base, starId) + "/photometry/lcm_" + photometryId
           + "_" + modelId + FILE_EXT;
}

QString DataStore::periodogramPath(const QString& base, const QString& starId,
    const QString& periodogramId)
{
return starDir(base, starId) + "/periodograms/" + periodogramId + FILE_EXT;
}

QString DataStore::lcFitPath(const QString& base, const QString& starId,
                             const QString& photometryId, const QString& fitId)
{
    return starDir(base, starId) + "/photometry/lcfit_" + photometryId
           + "_" + fitId + FILE_EXT;
}

bool DataStore::ensureDirForFile(const QString& filepath)
{
    return QDir().mkpath(QFileInfo(filepath).absolutePath());
}

bool DataStore::removeStarData(const QString& base, const QString& starId)
{
    QDir dir(starDir(base, starId));
    return !dir.exists() || dir.removeRecursively();
}