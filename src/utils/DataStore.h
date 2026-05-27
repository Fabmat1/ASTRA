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
    };

    // Write payload compressed to filepath (creates parent dirs automatically).
    static bool writeCompressed(const QString& filepath, DataType type,
                                const QByteArray& payload);

    // Read and decompress. Returns false if file is not in ASTRA format
    // (caller can fall back to legacy read).
    static bool readCompressed(const QString& filepath, DataType expectedType,
                               QByteArray& payload);

    // Returns true when the file starts with the ASTR magic.
    static bool isCompressedFormat(const QString& filepath);

    // ── Star-centric path helpers ────────────────────────────────
    static QString starDir       (const QString& base, const QString& starId);
    static QString spectrumPath  (const QString& base, const QString& starId,
                                  const QString& spectrumId);
    static QString spectralFitPath(const QString& base, const QString& starId,
                                   const QString& spectrumId, const QString& fitId);
    static QString photometricPointsPath(const QString& base, const QString& starId,
                                         const QString& photometryId);
    static QString lightcurvePath(const QString& base, const QString& starId,
                                  const QString& photometryId, const QString& source);
    static QString sedModelPath  (const QString& base, const QString& starId,
                                  const QString& photometryId, const QString& modelId);
    static QString lcModelPath   (const QString& base, const QString& starId,
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
    static constexpr quint16 FORMAT_VERSION = 1;
    static constexpr int    COMPRESSION_LEVEL = 1;    // zlib default (≈6)

    static QString sanitize(const QString& name);      // make safe for filenames
};

#endif // DATASTORE_H