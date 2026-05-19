#ifndef SPECTRUMREADER_H
#define SPECTRUMREADER_H

#include <QString>
#include <QStringList>
#include <memory>
#include <vector>
#include <optional>

class Spectrum;

// Metadata extracted from spectrum file
struct SpectrumMetadata {
    QString filepath;
    std::optional<double> ra;
    std::optional<double> dec;
    std::optional<double> mjd;
    std::optional<double> bjd;
    std::optional<double> exposureTime;
    std::optional<QString> instrument;
    std::optional<QString> objectName;
    std::optional<QString> sourceId;
    
    QStringList warnings;
    QStringList errors;
    
    bool isValid() const { return errors.isEmpty(); }
};

// Result of spectrum reading
struct SpectrumReadResult {
    std::shared_ptr<Spectrum> spectrum;
    SpectrumMetadata metadata;
    bool success;
    QString errorMessage;
};

// Base class for spectrum readers - extensible for different formats
class SpectrumReader
{
public:
    virtual ~SpectrumReader() = default;
    
    // Get reader name for UI
    virtual QString name() const = 0;
    
    // Get supported file extensions
    virtual QStringList supportedExtensions() const = 0;
    
    // Check if this reader can handle the file
    virtual bool canRead(const QString& filepath) const = 0;
    
    // Read metadata only (fast, for matching)
    virtual SpectrumMetadata readMetadata(const QString& filepath) const = 0;
    
    // Read full spectrum
    virtual SpectrumReadResult readSpectrum(const QString& filepath) const = 0;
};

// Default FITS reader - looks for standard headers
class DefaultFitsSpectrumReader : public SpectrumReader
{
public:
    QString name() const override { return "Default FITS Reader"; }
    QStringList supportedExtensions() const override { return {"fits", "fit", "fts"}; }
    bool canRead(const QString& filepath) const override;
    SpectrumMetadata readMetadata(const QString& filepath) const override;
    SpectrumReadResult readSpectrum(const QString& filepath) const override;
    
private:
    // Header field names to search for (in order of preference)
    static const QStringList RA_KEYWORDS;
    static const QStringList DEC_KEYWORDS;
    static const QStringList MJD_KEYWORDS;
    static const QStringList BJD_KEYWORDS;
    static const QStringList EXPTIME_KEYWORDS;
    static const QStringList INSTRUMENT_KEYWORDS;
    static const QStringList OBJECT_KEYWORDS;
    
    std::optional<double> findDoubleHeader(void* fptr, const QStringList& keywords) const;
    std::optional<QString> findStringHeader(void* fptr, const QStringList& keywords) const;
    bool findDataTable(void* fptr, int& wavelengthCol, int& fluxCol, int& errorCol) const;
};

// ASCII spectrum reader for simple text files
class AsciiSpectrumReader : public SpectrumReader
{
public:
    QString name() const override { return "ASCII Spectrum Reader"; }
    QStringList supportedExtensions() const override { return {"txt", "dat", "ascii", "csv"}; }
    bool canRead(const QString& filepath) const override;
    SpectrumMetadata readMetadata(const QString& filepath) const override;
    SpectrumReadResult readSpectrum(const QString& filepath) const override;

    void setExternalMetadata(const SpectrumMetadata& metadata) { _externalMetadata = metadata; }

private:
    SpectrumMetadata _externalMetadata;

    // Kept for API compatibility — now uses a static regex internally
    // and takes the delimiter as a parameter (no per-line re-detection).
    bool parseDataLine(const QString& line, double& wavelength,
                       double& flux, double& error) const;            // old signature; reimplemented
    QChar detectDelimiter(const QString& line) const;                  // hoists regex to static

    // New fast path
    static char detectDelimiterFast(const char* data, qsizetype len);
    static bool isCommentChar(char c) { return c == '#' || c == ';' || c == '!'; }
};

// Registry for spectrum readers
class SpectrumReaderRegistry
{
public:
    static SpectrumReaderRegistry& instance();
    
    void registerReader(std::shared_ptr<SpectrumReader> reader);
    std::shared_ptr<SpectrumReader> getReaderForFile(const QString& filepath) const;
    std::vector<std::shared_ptr<SpectrumReader>> getAllReaders() const;
    
private:
    SpectrumReaderRegistry();
    std::vector<std::shared_ptr<SpectrumReader>> _readers;
};

#endif // SPECTRUMREADER_H