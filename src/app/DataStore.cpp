#include "app/DataStore.h"
#include "app/ArrayCodec.h"

#include <QDataStream>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QtEndian>
#include <cstring>

// ── Compressed I/O ──────────────────────────────────────────────



/*  File layout. Version 1: "ASTR", u16 version, u16 type, then a QByteArray
 *  (u32 length + bytes) holding qCompress(payload) - zlib, which gains almost
 *  nothing on the raw doubles these files mostly hold. Version 2 adds a u8
 *  codec after the type: CodecZstd holds the classic payload zstd-compressed,
 *  CodecNone a payload whose arrays ArrayCodec has already compressed (the
 *  coded layout of spectra, fits and their shared bases). Version 1 files stay
 *  readable; a build that knows only version 1 rejects version 2 cleanly.     */

bool DataStore::write(const QString& filepath, DataType type, Codec codec,
                      const QByteArray& body)
{
    if (!ensureDirForFile(filepath)) {
        qWarning() << "DataStore: cannot create directory for" << filepath;
        return false;
    }

    // Atomic write: nothing touches `filepath` until commit() succeeds.
    QSaveFile file(filepath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "DataStore: cannot open for writing:" << filepath;
        return false;
    }

    file.write(MAGIC, 4);

    QDataStream out(&file);
    out.setVersion(QDataStream::Qt_6_0);
    out << FORMAT_VERSION;
    out << static_cast<quint16>(type);
    out << static_cast<quint8>(codec);
    out << body;

    if (out.status() != QDataStream::Ok) {
        file.cancelWriting(); // original preserved
        qWarning() << "DataStore: stream error writing" << filepath;
        return false;
    }

    if (!file.commit()) { // atomic rename happens here
        qWarning() << "DataStore: commit failed for" << filepath;
        return false;
    }
    return true;
}

bool DataStore::writeCompressed(const QString &filepath, DataType type,
                                const QByteArray &payload) {
    const QByteArray body = ArrayCodec::compress(payload);
    if (body.isEmpty()) {
        qWarning() << "DataStore: compression failed for" << filepath;
        return false;
    }
    return write(filepath, type, CodecZstd, body);
}

bool DataStore::writeCoded(const QString& filepath, DataType type, const QByteArray& payload)
{
    return write(filepath, type, CodecNone, payload);
}

bool DataStore::readHeader(const QString& filepath, quint16& version, quint16& type)
{
    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    char head[8];
    if (file.read(head, 8) != 8 || std::memcmp(head, MAGIC, 4) != 0)
        return false;
    version = qFromBigEndian<quint16>(head + 4);
    type    = qFromBigEndian<quint16>(head + 6);
    return true;
}

bool DataStore::readCompressed(const QString& filepath, DataType expectedType,
                               QByteArray& payload, bool* coded)
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

    quint8 codec = CodecZstd;
    if (version >= 2) in >> codec;

    QByteArray body;
    in >> body;
    if (in.status() != QDataStream::Ok) {
        qWarning() << "DataStore: truncated file" << filepath;
        return false;
    }

    if (coded) *coded = false;
    if (version < 2) {
        payload = qUncompress(body);
        if (payload.isEmpty() && body.size() > 4) {
            // qCompress of empty data produces a 4-byte header;
            // anything larger that decompresses to empty → corruption.
            qWarning() << "DataStore: decompression failed for" << filepath;
            return false;
        }
        return true;
    }
    if (codec == CodecNone) {
        if (!coded) {
            qWarning() << "DataStore: coded layout not expected in" << filepath;
            return false;
        }
        *coded = true;
        payload = body;
        return true;
    }
    if (codec != CodecZstd || !ArrayCodec::decompress(body.constData(), body.size(), payload)) {
        qWarning() << "DataStore: decompression failed for" << filepath;
        return false;
    }
    return true;
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

QString DataStore::sedPhotometryPointsPath(const QString& base, const QString& starId,
                                           const QString& photometryId)
{
    return starDir(base, starId) + "/photometry/sedpoints_" + photometryId + FILE_EXT;
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