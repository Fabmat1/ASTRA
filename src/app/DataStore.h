#ifndef DATASTORE_H
#define DATASTORE_H

#include <QString>
#include <QByteArray>

class DataStore
{
public:
    enum DataType : quint16 {
        SpectrumData          = 1,
        SpectralFitData       = 2,
        SEDModelData          = 3,
        LightcurveModelData   = 4,
        PhotometricPointsData = 5,
        LightcurveData        = 6,
        PeriodogramData       = 7,
        LCFitData             = 8,
        SEDPhotometryPointsData = 9,
        FitBaseData           = 10,   ///< wavelengths + rebinned fluxes shared by fits
    };

    // Write payload compressed (zstd) to filepath (creates parent dirs
    // automatically). The payload is the type's classic layout.
    static bool writeCompressed(const QString& filepath, DataType type,
                                const QByteArray& payload);

    // Write a payload whose arrays are already compressed by ArrayCodec (the
    // type's coded layout), stored as it is.
    static bool writeCoded(const QString& filepath, DataType type,
                           const QByteArray& payload);

    // Read and decompress. Returns false if file is not in ASTRA format
    // (caller can fall back to legacy read). `coded` says which layout the
    // payload has: false for the classic one (every version 1 file), true for
    // one written by writeCoded. A caller that passes no `coded` only accepts
    // the classic layout.
    static bool readCompressed(const QString& filepath, DataType expectedType,
                               QByteArray& payload, bool* coded = nullptr);

    // The header of an ASTRA file: false when the file is not one.
    static bool readHeader(const QString& filepath, quint16& version, quint16& type);

    static constexpr quint16 FORMAT_VERSION = 2;

    // ── Star-centric path helpers ────────────────────────────────
    static QString starDir       (const QString& base, const QString& starId);
    static QString spectrumPath  (const QString& base, const QString& starId,
                                  const QString& spectrumId);
    static QString spectralFitPath(const QString& base, const QString& starId,
                                   const QString& spectrumId, const QString& fitId);
    static QString photometricPointsPath(const QString& base, const QString& starId,
                                         const QString& photometryId);
    static QString sedPhotometryPointsPath(const QString& base, const QString& starId,
                                           const QString& photometryId);
    static QString lightcurvePath(const QString& base, const QString& starId,
                                  const QString& photometryId, const QString& source);
    static QString sedModelPath  (const QString& base, const QString& starId,
                                  const QString& photometryId, const QString& modelId);
    static QString periodogramPath(const QString& base, const QString& starId,
                                  const QString& periodogramId);
    static QString lcFitPath(const QString& base, const QString& starId,
                                  const QString& photometryId, const QString& fitId);


    // Create parent directories for filepath. Returns success.
    static bool ensureDirForFile(const QString& filepath);

    // Recursively delete everything under stars/{starId}.
    static bool removeStarData(const QString& base, const QString& starId);

    static constexpr const char* FILE_EXT = ".asd";   // ASTRA Stellar Data

private:
    static constexpr char   MAGIC[4]        = {'A','S','T','R'};

    // Version 2 stores this byte after the type: how the body is encoded.
    enum Codec : quint8 { CodecNone = 0, CodecZstd = 1 };
    static bool write(const QString& filepath, DataType type, Codec codec,
                      const QByteArray& body);

    static QString sanitize(const QString& name);      // make safe for filenames
};

#endif // DATASTORE_H