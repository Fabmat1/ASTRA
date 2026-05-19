#include "SpectrumReader.h"
#include "Logger.h"
#include "models/Spectrum.h"
#include "models/Time.h"

#include <fitsio.h>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QRegularExpression>
#include <QTimeZone>
#include <cmath>

// ============================================================================
// DefaultFitsSpectrumReader Implementation
// ============================================================================

const QStringList DefaultFitsSpectrumReader::RA_KEYWORDS = {
    "RA", "RA_OBJ", "OBJRA", "RA_TARG", "TARG_RA", "CRVAL1", "RA_DEG"
};

const QStringList DefaultFitsSpectrumReader::DEC_KEYWORDS = {
    "DEC", "DEC_OBJ", "OBJDEC", "DEC_TARG", "TARG_DEC", "CRVAL2", "DEC_DEG"
};

const QStringList DefaultFitsSpectrumReader::MJD_KEYWORDS = {
    "MJD-OBS", "MJD", "MJD_OBS", "MJDOBS", "MJD-MID", "MJD_MID"
};

const QStringList DefaultFitsSpectrumReader::BJD_KEYWORDS = {
    "BJD", "BJD-OBS", "BJD_OBS", "BJDOBS", "BJD-MID", "BJD_MID", "BJD_TDB"
};

const QStringList DefaultFitsSpectrumReader::EXPTIME_KEYWORDS = {
    "EXPTIME", "EXPOSURE", "EXP_TIME", "ITIME", "TEXP", "EXPTIM"
};

const QStringList DefaultFitsSpectrumReader::INSTRUMENT_KEYWORDS = {
    "INSTRUME", "INSTRUMENT", "INST", "DETECTOR", "SPECTROGRAPH"
};

const QStringList DefaultFitsSpectrumReader::OBJECT_KEYWORDS = {
    "OBJECT", "OBJNAME", "TARGET", "TARGNAME", "SRCNAME"
};

bool DefaultFitsSpectrumReader::canRead(const QString& filepath) const
{
    QFileInfo info(filepath);
    QString ext = info.suffix().toLower();
    return supportedExtensions().contains(ext);
}

std::optional<double> DefaultFitsSpectrumReader::findDoubleHeader(void* fptr, const QStringList& keywords) const
{
    fitsfile* fits = static_cast<fitsfile*>(fptr);
    int status = 0;
    double value;
    char comment[FLEN_COMMENT];
    
    for (const QString& keyword : keywords) {
        status = 0;
        if (fits_read_key(fits, TDOUBLE, keyword.toUtf8().constData(), &value, comment, &status) == 0) {
            return value;
        }
    }
    
    return std::nullopt;
}

std::optional<QString> DefaultFitsSpectrumReader::findStringHeader(void* fptr, const QStringList& keywords) const
{
    fitsfile* fits = static_cast<fitsfile*>(fptr);
    int status = 0;
    char value[FLEN_VALUE];
    char comment[FLEN_COMMENT];
    
    for (const QString& keyword : keywords) {
        status = 0;
        if (fits_read_key(fits, TSTRING, keyword.toUtf8().constData(), value, comment, &status) == 0) {
            return QString(value).trimmed();
        }
    }
    
    return std::nullopt;
}

SpectrumMetadata DefaultFitsSpectrumReader::readMetadata(const QString& filepath) const
{
    LOG_DEBUG("SpectrumReader", QString("Reading FITS metadata from: %1").arg(filepath));
    
    SpectrumMetadata metadata;
    metadata.filepath = filepath;
    
    fitsfile* fptr = nullptr;
    int status = 0;
    
    if (fits_open_file(&fptr, filepath.toUtf8().constData(), READONLY, &status)) {
        char errMsg[FLEN_ERRMSG];
        fits_read_errmsg(errMsg);
        metadata.errors << QString("Failed to open FITS file: %1").arg(errMsg);
        return metadata;
    }
    
    // Move to primary HDU
    fits_movabs_hdu(fptr, 1, nullptr, &status);
    
    // Read metadata from header
    metadata.ra = findDoubleHeader(fptr, RA_KEYWORDS);
    metadata.dec = findDoubleHeader(fptr, DEC_KEYWORDS);
    metadata.mjd = findDoubleHeader(fptr, MJD_KEYWORDS);
    metadata.bjd = findDoubleHeader(fptr, BJD_KEYWORDS);
    metadata.exposureTime = findDoubleHeader(fptr, EXPTIME_KEYWORDS);
    metadata.instrument = findStringHeader(fptr, INSTRUMENT_KEYWORDS);
    metadata.objectName = findStringHeader(fptr, OBJECT_KEYWORDS);
    
    // Try to get MJD from DATE-OBS if MJD not found
    if (!metadata.mjd.has_value()) {
        auto dateObs = findStringHeader(fptr, {"DATE-OBS", "DATE_OBS", "DATEOBS"});
        if (dateObs.has_value()) {
            // Parse ISO date format and convert to MJD
            QDateTime dt = QDateTime::fromString(dateObs.value(), Qt::ISODate);
            if (dt.isValid()) {
                // MJD = JD - 2400000.5
                // JD for J2000 epoch (2000-01-01T12:00:00) = 2451545.0
                QDateTime j2000(QDate(2000, 1, 1), QTime(12, 0, 0), QTimeZone::utc());
                double daysSinceJ2000 = j2000.msecsTo(dt) / 86400000.0;
                double jd = 2451545.0 + daysSinceJ2000;
                metadata.mjd = jd - Time::MJD_OFFSET;
            }
        }
    }
    
    // Check for Gaia source ID in header
    auto sourceId = findStringHeader(fptr, {"GAIA_ID", "GAIAID", "SOURCE_ID", "SOURCEID", "GAIA_DR3"});
    if (sourceId.has_value()) {
        metadata.sourceId = sourceId.value();
    }
    
    // Add warnings for missing critical data
    if (!metadata.ra.has_value()) {
        metadata.warnings << "RA not found in header";
    }
    if (!metadata.dec.has_value()) {
        metadata.warnings << "DEC not found in header";
    }
    if (!metadata.mjd.has_value() && !metadata.bjd.has_value()) {
        metadata.warnings << "Observation time (MJD/BJD) not found in header";
    }
    if (!metadata.exposureTime.has_value()) {
        metadata.warnings << "Exposure time not found in header";
    }
    
    fits_close_file(fptr, &status);
    
    LOG_DEBUG("SpectrumReader", QString("FITS metadata: RA=%1, DEC=%2, MJD=%3")
              .arg(metadata.ra.value_or(0.0))
              .arg(metadata.dec.value_or(0.0))
              .arg(metadata.mjd.value_or(0.0)));
    
    return metadata;
}

bool DefaultFitsSpectrumReader::findDataTable(void* fptr, int& wavelengthCol, int& fluxCol, int& errorCol) const
{
    fitsfile* fits = static_cast<fitsfile*>(fptr);
    int status = 0;
    int ncols = 0;
    
    // Get number of columns
    fits_get_num_cols(fits, &ncols, &status);
    if (status != 0 || ncols == 0) {
        return false;
    }
    
    wavelengthCol = -1;
    fluxCol = -1;
    errorCol = -1;
    
    // Column name patterns to search for
    QStringList wavelengthNames = {"WAVELENGTH", "WAVE", "LAMBDA", "WAV", "WLEN", "WL"};
    QStringList fluxNames = {"FLUX", "COUNTS", "INTENSITY", "SPEC", "DATA", "F_LAMBDA"};
    QStringList errorNames = {"ERROR", "ERR", "SIGMA", "FLUX_ERR", "ERR_FLUX", "IVAR", "UNCERTAINTY"};
    
    for (int col = 1; col <= ncols; ++col) {
        char colname[FLEN_VALUE];
        status = 0;
        
        // Get column name using TTYPEn keyword
        char keyword[FLEN_KEYWORD];
        snprintf(keyword, sizeof(keyword), "TTYPE%d", col);
        
        char value[FLEN_VALUE];
        if (fits_read_key(fits, TSTRING, keyword, value, nullptr, &status) == 0) {
            QString name = QString(value).toUpper().trimmed();
            
            for (const QString& wn : wavelengthNames) {
                if (name.contains(wn)) {
                    wavelengthCol = col;
                    break;
                }
            }
            for (const QString& fn : fluxNames) {
                if (name.contains(fn) && !name.contains("ERR")) {
                    fluxCol = col;
                    break;
                }
            }
            for (const QString& en : errorNames) {
                if (name.contains(en)) {
                    errorCol = col;
                    break;
                }
            }
        }
    }
    
    // If no named columns found, assume column order: wavelength, flux, [error]
    if (wavelengthCol < 0 && ncols >= 2) {
        wavelengthCol = 1;
        fluxCol = 2;
        if (ncols >= 3) {
            errorCol = 3;
        }
    }
    
    return wavelengthCol > 0 && fluxCol > 0;
}

SpectrumReadResult DefaultFitsSpectrumReader::readSpectrum(const QString& filepath) const
{
    LOG_DEBUG("SpectrumReader", QString("Reading FITS spectrum from: %1").arg(filepath));  // Changed from LOG_INFO
    
    SpectrumReadResult result;
    result.success = false;
    result.metadata = readMetadata(filepath);
    
    if (!result.metadata.isValid()) {
        result.errorMessage = result.metadata.errors.join("; ");
        return result;
    }
    
    fitsfile* fptr = nullptr;
    int status = 0;
    
    if (fits_open_file(&fptr, filepath.toUtf8().constData(), READONLY, &status)) {
        char errMsg[FLEN_ERRMSG];
        fits_read_errmsg(errMsg);
        result.errorMessage = QString("Failed to open FITS file: %1").arg(errMsg);
        return result;
    }
    
    // Find table extension with spectral data
    int numHdus = 0;
    fits_get_num_hdus(fptr, &numHdus, &status);
    
    bool foundData = false;
    int wavelengthCol, fluxCol, errorCol;
    
    for (int hdu = 1; hdu <= numHdus && !foundData; ++hdu) {
        int hduType;
        fits_movabs_hdu(fptr, hdu, &hduType, &status);
        
        if (hduType == BINARY_TBL || hduType == ASCII_TBL) {
            if (findDataTable(fptr, wavelengthCol, fluxCol, errorCol)) {
                foundData = true;
            }
        }
    }
    
    if (!foundData) {
        // Try reading as image (1D spectrum)
        fits_movabs_hdu(fptr, 1, nullptr, &status);
        
        int naxis;
        long naxes[10];
        fits_get_img_dim(fptr, &naxis, &status);
        
        if (status == 0 && naxis >= 1) {
            fits_get_img_size(fptr, 10, naxes, &status);
            
            if (status == 0 && naxes[0] > 0) {
                long npixels = naxes[0];
                std::vector<double> fluxes(npixels);
                std::vector<double> wavelengths(npixels);
                std::vector<double> errors(npixels, 0.0);
                
                long fpixel = 1;
                int anynul;
                fits_read_img(fptr, TDOUBLE, fpixel, npixels, nullptr, 
                             fluxes.data(), &anynul, &status);
                
                if (status == 0) {
                    // Try to get wavelength calibration from header
                    double crval1 = 0, cdelt1 = 1, crpix1 = 1;
                    int tmpStatus = 0;
                    
                    fits_read_key(fptr, TDOUBLE, "CRVAL1", &crval1, nullptr, &tmpStatus);
                    tmpStatus = 0;
                    fits_read_key(fptr, TDOUBLE, "CDELT1", &cdelt1, nullptr, &tmpStatus);
                    if (tmpStatus != 0) {
                        tmpStatus = 0;
                        fits_read_key(fptr, TDOUBLE, "CD1_1", &cdelt1, nullptr, &tmpStatus);
                    }
                    tmpStatus = 0;
                    fits_read_key(fptr, TDOUBLE, "CRPIX1", &crpix1, nullptr, &tmpStatus);
                    
                    for (long i = 0; i < npixels; ++i) {
                        wavelengths[i] = crval1 + (i + 1 - crpix1) * cdelt1;
                    }
                    
                    result.spectrum = std::make_shared<Spectrum>();
                    result.spectrum->setData(wavelengths, fluxes, errors);
                    result.spectrum->setFile(filepath);
                    foundData = true;
                }
            }
        }
    } else {
        // Read from table
        long nrows;
        fits_get_num_rows(fptr, &nrows, &status);
        
        if (status == 0 && nrows > 0) {
            std::vector<double> wavelengths(nrows);
            std::vector<double> fluxes(nrows);
            std::vector<double> errors(nrows, 0.0);
            
            int anynul;
            fits_read_col(fptr, TDOUBLE, wavelengthCol, 1, 1, nrows, nullptr,
                         wavelengths.data(), &anynul, &status);
            fits_read_col(fptr, TDOUBLE, fluxCol, 1, 1, nrows, nullptr,
                         fluxes.data(), &anynul, &status);
            
            if (errorCol > 0) {
                fits_read_col(fptr, TDOUBLE, errorCol, 1, 1, nrows, nullptr,
                             errors.data(), &anynul, &status);
            }
            
            if (status == 0) {
                result.spectrum = std::make_shared<Spectrum>();
                result.spectrum->setData(wavelengths, fluxes, errors);
                result.spectrum->setFile(filepath);
            }
        }
    }
    
    fits_close_file(fptr, &status);
    
    if (!foundData || !result.spectrum) {
        result.errorMessage = "Could not find spectral data in FITS file";
        return result;
    }
    
    // Apply metadata to spectrum

    double mjd = result.metadata.mjd.value_or(0.0);
    double bjd = result.metadata.bjd.value_or(0.0);
    double exp = result.metadata.exposureTime.value_or(0.0);
    result.spectrum->setTime(Time::fromMjdBjd(mjd, bjd,
                                            exp > 0.0 ? exp : -1.0));
    if (result.metadata.instrument.has_value()) {
        result.spectrum->setInstrument(result.metadata.instrument.value());
    }
    
    result.success = true;
    
    LOG_DEBUG("SpectrumReader", QString("Successfully read spectrum with %1 points") 
             .arg(result.spectrum->getWavelengths().size()));
    
    return result;
}

// ============================================================================
// AsciiSpectrumReader Implementation
// ============================================================================

bool AsciiSpectrumReader::canRead(const QString& filepath) const
{
    QFileInfo info(filepath);
    QString ext = info.suffix().toLower();
    return supportedExtensions().contains(ext);
}

QChar AsciiSpectrumReader::detectDelimiter(const QString& line) const
{
    static const QRegularExpression wsRe(QStringLiteral("\\s+"));   // compiled once, ever

    const int tabs       = line.count(QLatin1Char('\t'));
    const int commas     = line.count(QLatin1Char(','));
    const int semicolons = line.count(QLatin1Char(';'));
    const int spaces     = line.count(wsRe) - tabs;

    if (tabs > 0 && tabs >= commas && tabs >= semicolons) return QLatin1Char('\t');
    if (commas > 0 && commas >= semicolons)               return QLatin1Char(',');
    if (semicolons > 0)                                    return QLatin1Char(';');
    Q_UNUSED(spaces);
    return QLatin1Char(' ');
}

char AsciiSpectrumReader::detectDelimiterFast(const char* data, qsizetype len)
{
    // Look at the first few non-comment, non-blank lines and count delimiter occurrences.
    int tabs = 0, commas = 0, semis = 0;
    int linesSampled = 0;
    const char* p   = data;
    const char* end = data + len;

    while (p < end && linesSampled < 5) {
        // skip leading whitespace
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p >= end) break;

        if (*p == '\n' || *p == '\r') { ++p; continue; }
        if (isCommentChar(*p)) {
            while (p < end && *p != '\n') ++p;
            if (p < end) ++p;
            continue;
        }

        const char* lineStart = p;
        while (p < end && *p != '\n') ++p;

        for (const char* q = lineStart; q < p; ++q) {
            switch (*q) {
                case '\t': ++tabs;   break;
                case ',':  ++commas; break;
                case ';':  ++semis;  break;
                default: break;
            }
        }
        ++linesSampled;
        if (p < end) ++p;
    }

    if (tabs   > 0 && tabs   >= commas && tabs   >= semis) return '\t';
    if (commas > 0 && commas >= semis)                     return ',';
    if (semis  > 0)                                        return ';';
    return ' ';   // whitespace
}

bool AsciiSpectrumReader::parseDataLine(const QString& line, double& wavelength,
                                        double& flux, double& error) const
{
    static const QRegularExpression wsSplit(QStringLiteral("\\s+"));   // compiled once, ever

    QString trimmed = line.trimmed();
    if (trimmed.isEmpty() || trimmed.startsWith(QLatin1Char('#'))
                          || trimmed.startsWith(QLatin1Char(';')))
        return false;

    const QChar delimiter = detectDelimiter(trimmed);
    QStringList parts;
    if (delimiter == QLatin1Char(' '))
        parts = trimmed.split(wsSplit, Qt::SkipEmptyParts);
    else
        parts = trimmed.split(delimiter, Qt::SkipEmptyParts);

    if (parts.size() < 2) return false;

    bool ok1, ok2, ok3 = true;
    wavelength = parts[0].toDouble(&ok1);
    flux       = parts[1].toDouble(&ok2);
    if (parts.size() >= 3) error = parts[2].toDouble(&ok3);
    else                   error = 0.0;

    return ok1 && ok2 && ok3;
}

SpectrumMetadata AsciiSpectrumReader::readMetadata(const QString& filepath) const
{
    // ASCII files don't have embedded metadata, return external metadata
    SpectrumMetadata metadata = _externalMetadata;
    metadata.filepath = filepath;
    
    if (!metadata.mjd.has_value() && !metadata.bjd.has_value()) {
        metadata.warnings << "Observation time must be provided externally";
    }
    if (!metadata.exposureTime.has_value()) {
        metadata.warnings << "Exposure time must be provided externally";
    }
    
    return metadata;
}

SpectrumReadResult AsciiSpectrumReader::readSpectrum(const QString& filepath) const
{
    LOG_DEBUG("SpectrumReader", QString("Reading ASCII spectrum from: %1").arg(filepath));

    SpectrumReadResult result;
    result.success  = false;
    result.metadata = readMetadata(filepath);

    QFile file(filepath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.errorMessage = QString("Cannot open file: %1").arg(file.errorString());
        return result;
    }

    // Spectra are normally KB–MB; reading all at once is fine and avoids per-line I/O cost.
    const QByteArray contents = file.readAll();
    file.close();

    const char* const data = contents.constData();
    const qsizetype   len  = contents.size();
    const char delim       = detectDelimiterFast(data, len);   // 0 → whitespace if ' '
    Q_UNUSED(delim); // strtod skips any whitespace already; we treat ',' ';' '\t' uniformly below

    // Reserve aggressively: ~25 bytes/line is a safe lower bound for "w f e"
    const size_t estRows = static_cast<size_t>(len) / 24 + 16;
    std::vector<double> wavelengths; wavelengths.reserve(estRows);
    std::vector<double> fluxes;      fluxes.reserve(estRows);
    std::vector<double> errors;      errors.reserve(estRows);

    const char* p   = data;
    const char* end = data + len;

    auto skipToEol = [&]() {
        while (p < end && *p != '\n') ++p;
        if (p < end) ++p;
    };
    auto skipSeparators = [&]() {
        // Treat any of these as field separators between numbers
        while (p < end && (*p == ' ' || *p == '\t' || *p == ',' || *p == ';'))
            ++p;
    };

    while (p < end) {
        // Skip leading whitespace on the line
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p >= end) break;

        // Blank or comment line?
        if (*p == '\n' || *p == '\r') { ++p; continue; }
        if (isCommentChar(*p))        { skipToEol(); continue; }

        // --- parse wavelength ---
        char* tail;
        const double w = std::strtod(p, &tail);
        if (tail == p) { skipToEol(); continue; }       // not a number → skip line
        p = tail;

        skipSeparators();

        // --- parse flux ---
        const double f = std::strtod(p, &tail);
        if (tail == p) { skipToEol(); continue; }
        p = tail;

        // --- optional error column ---
        skipSeparators();
        double e = 0.0;
        if (p < end && *p != '\n' && *p != '\r') {
            const double tmp = std::strtod(p, &tail);
            if (tail != p) { e = tmp; p = tail; }
        }

        wavelengths.push_back(w);
        fluxes.push_back(f);
        errors.push_back(e);

        skipToEol();
    }

    if (wavelengths.empty()) {
        result.errorMessage = "No valid spectral data found in file";
        return result;
    }

    result.spectrum = std::make_shared<Spectrum>();
    result.spectrum->setData(wavelengths, fluxes, errors);
    result.spectrum->setFile(filepath);

    // Apply external metadata
    if (result.metadata.mjd.has_value())
        result.spectrum->setMJD(result.metadata.mjd.value());
    if (result.metadata.bjd.has_value())
        result.spectrum->setBJD(result.metadata.bjd.value());
    if (result.metadata.exposureTime.has_value())
        result.spectrum->setExposureTime(result.metadata.exposureTime.value());
    if (result.metadata.instrument.has_value())
        result.spectrum->setInstrument(result.metadata.instrument.value());

    result.success = true;

    LOG_DEBUG("SpectrumReader", QString("Successfully read ASCII spectrum with %1 points")
              .arg(wavelengths.size()));

    return result;
}

// ============================================================================
// SpectrumReaderRegistry Implementation
// ============================================================================

SpectrumReaderRegistry& SpectrumReaderRegistry::instance()
{
    static SpectrumReaderRegistry registry;
    return registry;
}

SpectrumReaderRegistry::SpectrumReaderRegistry()
{
    // Register default readers
    registerReader(std::make_shared<DefaultFitsSpectrumReader>());
    registerReader(std::make_shared<AsciiSpectrumReader>());
}

void SpectrumReaderRegistry::registerReader(std::shared_ptr<SpectrumReader> reader)
{
    _readers.push_back(reader);
    LOG_DEBUG("SpectrumReader", QString("Registered spectrum reader: %1").arg(reader->name()));
}

std::shared_ptr<SpectrumReader> SpectrumReaderRegistry::getReaderForFile(const QString& filepath) const
{
    // First check by extension
    QFileInfo info(filepath);
    QString ext = info.suffix().toLower();
    
    // Check if it's a FITS file by magic bytes (even if extension is wrong)
    QFile file(filepath);
    if (file.open(QIODevice::ReadOnly)) {
        QByteArray header = file.read(30);
        file.close();
        
        // FITS files start with "SIMPLE  ="
        if (header.startsWith("SIMPLE  =")) {
            for (const auto& reader : _readers) {
                if (reader->name().contains("FITS", Qt::CaseInsensitive)) {
                    return reader;
                }
            }
        }
    }
    
    // Fall back to extension-based detection
    for (const auto& reader : _readers) {
        if (reader->canRead(filepath)) {
            return reader;
        }
    }
    
    return nullptr;
}

std::vector<std::shared_ptr<SpectrumReader>> SpectrumReaderRegistry::getAllReaders() const
{
    return _readers;
}