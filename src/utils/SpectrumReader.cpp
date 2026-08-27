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
#include <algorithm>
#include <cmath>
#include <numeric>

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
    // Mid- and observation-time cards first; the exposure-start cards after
    // them cover the archives that file no other epoch: EXPSTART on HST
    // products, MJD-BEG on the HASP coadds, OBSSTART on FUSE.
    "MJD-OBS", "MJD", "MJD_OBS", "MJDOBS", "MJD-MID", "MJD_MID",
    "EXPSTART", "MJD-BEG", "MJDBEG", "MJD_BEG", "MJD-START", "MJDSTART",
    "OBSSTART"
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

// Parse "DD:MM:SS(.s)" / "DD MM SS" / "DDhMMmSSs" / plain decimal into a value.
// Sign is taken from the raw string so "-00:41:43" survives the leading-zero
// trap. Does NOT apply any hours->degrees conversion.
static std::optional<double> parseSexagesimal(const QString &rawIn) {
    QString s = rawIn.trimmed();
    if (s.isEmpty())
        return std::nullopt;

    const double sign = s.startsWith('-') ? -1.0 : 1.0;

    // Normalise all the common separators to spaces.
    static const QRegularExpression sep(R"([:hHdDmMsS'""°’″\s]+)");
    const QStringList               parts = s.split(sep, Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return std::nullopt;

    bool         ok = false;
    const double a  = std::abs(parts[0].toDouble(&ok));
    if (!ok)
        return std::nullopt;

    if (parts.size() == 1) // plain decimal value
        return sign * a;

    const double m = parts[1].toDouble(&ok);
    if (!ok)
        return std::nullopt;
    const double sec = (parts.size() > 2) ? parts[2].toDouble(&ok) : 0.0;
    if (parts.size() > 2 && !ok)
        return std::nullopt;

    return sign * (a + m / 60.0 + sec / 3600.0);
}

// Reads a coord keyword that may be a numeric value (degrees) OR a sexagesimal
// string. For RA, sexagesimal values are treated as hours and converted to deg.
static std::optional<double>
findCoordinateHeader(fitsfile *fptr, const QStringList &keywords, bool isRA) {
    for (const QString &key : keywords) {
        int  status                = 0;
        char value[FLEN_VALUE]     = {0};
        char comment[FLEN_COMMENT] = {0};
        if (fits_read_keyword(fptr, key.toUtf8().constData(), value, comment,
                              &status) != 0)
            continue; // keyword absent -> try next alias

        QString raw = QString::fromLatin1(value).trimmed();
        if (raw.startsWith('\'') && raw.endsWith('\'')) // unquote string cards
            raw = raw.mid(1, raw.length() - 2).trimmed();
        if (raw.isEmpty())
            continue;

        // CRVALn doubles as the wavelength zero-point in spectral WCS headers;
        // only trust it as a coordinate if the matching CTYPEn agrees (or is
        // absent, matching historic behaviour for plain imaging headers).
        if (key.startsWith(QStringLiteral("CRVAL"))) {
            char ctVal[FLEN_VALUE] = {0};
            int  ctStatus          = 0;
            const QByteArray ctypeKey =
                (QStringLiteral("CTYPE") + key.mid(5)).toUtf8();
            if (fits_read_key(fptr, TSTRING, ctypeKey.constData(), ctVal,
                              nullptr, &ctStatus) == 0) {
                const QString ctype = QString::fromLatin1(ctVal).toUpper();
                if (isRA ? !ctype.contains(QStringLiteral("RA"))
                         : !ctype.contains(QStringLiteral("DEC")))
                    continue;
            }
        }

        const bool looksSexagesimal =
            raw.contains(':') ||
            raw.contains(QRegularExpression(R"([hHdD])")) ||
            raw.contains(QRegularExpression(R"(\d\s+\d)"));

        auto parsed = parseSexagesimal(raw);
        if (!parsed.has_value())
            continue;

        double deg = parsed.value();
        if (isRA && looksSexagesimal)
            deg *= 15.0; // hours -> degrees
        return deg;
    }
    return std::nullopt;
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
    
    // Scan every HDU, keeping the first value found per field: pipelines like
    // PMAS/p3d leave the primary header nearly empty and write all metadata
    // into the IMAGE extensions that hold the actual data.
    int numHdus = 0;
    fits_get_num_hdus(fptr, &numHdus, &status);
    if (numHdus < 1)
        numHdus = 1;

    std::optional<QString> dateObs;
    for (int hdu = 1; hdu <= numHdus; ++hdu) {
        status = 0;
        if (fits_movabs_hdu(fptr, hdu, nullptr, &status) != 0)
            break;

        if (!metadata.ra.has_value())
            metadata.ra = findCoordinateHeader(fptr, RA_KEYWORDS, /*isRA=*/true);
        if (!metadata.dec.has_value())
            metadata.dec = findCoordinateHeader(fptr, DEC_KEYWORDS, /*isRA=*/false);
        if (!metadata.mjd.has_value())
            metadata.mjd = findDoubleHeader(fptr, MJD_KEYWORDS);
        if (!metadata.bjd.has_value())
            metadata.bjd = findDoubleHeader(fptr, BJD_KEYWORDS);
        if (!metadata.exposureTime.has_value())
            metadata.exposureTime = findDoubleHeader(fptr, EXPTIME_KEYWORDS);
        if (!metadata.instrument.has_value())
            metadata.instrument = findStringHeader(fptr, INSTRUMENT_KEYWORDS);
        if (!metadata.objectName.has_value())
            metadata.objectName = findStringHeader(fptr, OBJECT_KEYWORDS);
        if (!dateObs.has_value())
            dateObs = findStringHeader(fptr, {"DATE-OBS", "DATE_OBS", "DATEOBS"});

        // PMAS (Calar Alto 3.5m) headers carry no INSTRUME card; the telltale
        // is the PVERSION card whose comment reads "PMAS fitsheader version".
        if (!metadata.instrument.has_value()) {
            int  st                = 0;
            char val[FLEN_VALUE]   = {0};
            char com[FLEN_COMMENT] = {0};
            if (fits_read_key(fptr, TSTRING, "PVERSION", val, com, &st) == 0 &&
                QString::fromLatin1(com).contains(QStringLiteral("PMAS"),
                                                  Qt::CaseInsensitive))
                metadata.instrument = QStringLiteral("PMAS");
        }
    }
    status = 0;

    // Try to get MJD from DATE-OBS if MJD not found
    {
        if (dateObs.has_value() && !metadata.mjd.has_value()) {
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
    if (!metadata.sourceId.has_value() && metadata.objectName.has_value()) {
        // Gaia DR2/DR3 source ids are long integer runs; ignore short numeric
        // tokens.
        static const QRegularExpression gaiaRe(R"((\d{10,19}))");
        auto m = gaiaRe.match(metadata.objectName.value());
        if (m.hasMatch())
            metadata.sourceId = m.captured(
                1);
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

namespace {
// A spectral column has to hold numbers. Without this check the positional
// fallback below would take the string columns of an HST association table
// (MEMNAME/MEMTYPE) for wavelength and flux.
bool isNumericColumn(fitsfile* fits, int col)
{
    int  typecode = 0;
    long repeat = 0, width = 0;
    int  status = 0;
    if (fits_get_coltype(fits, col, &typecode, &repeat, &width, &status) != 0)
        return false;
    switch (std::abs(typecode)) {
    case TBYTE:
    case TSHORT:
    case TUSHORT:
    case TINT:
    case TUINT:
    case TLONG:
    case TULONG:
    case TLONGLONG:
    case TFLOAT:
    case TDOUBLE:
        return true;
    default:
        return false;
    }
}
}   // namespace

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
    
    // Best match wins, not the last one: a STIS sx1 table carries both ERROR
    // and NET_ERROR, and an exact hit on the name is what tells them apart.
    auto rank = [](const QString& name, const QStringList& candidates) {
        int best = 0;
        for (const QString& c : candidates) {
            if (name == c)                 best = std::max(best, 3);
            else if (name.startsWith(c))   best = std::max(best, 2);
            else if (name.contains(c))     best = std::max(best, 1);
        }
        return best;
    };

    int wavelengthRank = 0, fluxRank = 0, errorRank = 0;

    for (int col = 1; col <= ncols; ++col) {
        status = 0;

        // Get column name using TTYPEn keyword
        char keyword[FLEN_KEYWORD];
        snprintf(keyword, sizeof(keyword), "TTYPE%d", col);

        char value[FLEN_VALUE];
        if (fits_read_key(fits, TSTRING, keyword, value, nullptr, &status) == 0) {
            const QString name = QString(value).toUpper().trimmed();

            if (const int r = rank(name, wavelengthNames); r > wavelengthRank) {
                wavelengthRank = r;
                wavelengthCol  = col;
            }
            if (!name.contains("ERR")) {
                if (const int r = rank(name, fluxNames); r > fluxRank) {
                    fluxRank = r;
                    fluxCol  = col;
                }
            }
            if (const int r = rank(name, errorNames); r > errorRank) {
                errorRank = r;
                errorCol  = col;
            }
        }
    }
    
    // If no named columns found, assume column order: wavelength, flux, [error]
    if (wavelengthCol < 0 && ncols >= 2 && isNumericColumn(fits, 1) &&
        isNumericColumn(fits, 2)) {
        wavelengthCol = 1;
        fluxCol = 2;
        if (ncols >= 3 && isNumericColumn(fits, 3)) {
            errorCol = 3;
        }
    }

    // A table whose named columns are text (association tables, provenance
    // extensions) is not the spectrum; let the caller keep looking.
    if (wavelengthCol > 0 && !isNumericColumn(fits, wavelengthCol))
        wavelengthCol = -1;
    if (fluxCol > 0 && !isNumericColumn(fits, fluxCol))
        fluxCol = -1;
    if (errorCol > 0 && !isNumericColumn(fits, errorCol))
        errorCol = -1;

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
        int hduType = ANY_HDU;
        status = 0;   // one unreadable HDU must not poison the rest
        fits_movabs_hdu(fptr, hdu, &hduType, &status);
        if (status != 0) continue;

        if (hduType == BINARY_TBL || hduType == ASCII_TBL) {
            if (findDataTable(fptr, wavelengthCol, fluxCol, errorCol)) {
                foundData = true;
            }
        }
    }
    
    if (!foundData) {
        // Try reading as image (1D spectrum). The flux array may live in the
        // primary HDU or in an IMAGE extension (e.g. PMAS/p3d writes it to an
        // extension named DATA, with the uncertainty in ERROR).
        auto readImageHdu = [&](int hdu, std::vector<double>& out) -> bool {
            int st = 0;
            if (fits_movabs_hdu(fptr, hdu, nullptr, &st) != 0)
                return false;
            int  naxis = 0;
            long naxes[10] = {0};
            if (fits_get_img_dim(fptr, &naxis, &st) != 0 || naxis < 1)
                return false;
            if (fits_get_img_size(fptr, 10, naxes, &st) != 0 || naxes[0] <= 0)
                return false;
            // Only accept true 1-D vectors (allow degenerate trailing axes).
            for (int a = 1; a < naxis; ++a)
                if (naxes[a] > 1)
                    return false;
            out.resize(naxes[0]);
            int anynul = 0;
            return fits_read_img(fptr, TDOUBLE, 1, naxes[0], nullptr,
                                 out.data(), &anynul, &st) == 0;
        };

        auto hduExtname = [&](int hdu) -> QString {
            int st = 0;
            if (fits_movabs_hdu(fptr, hdu, nullptr, &st) != 0)
                return {};
            char val[FLEN_VALUE] = {0};
            if (fits_read_key(fptr, TSTRING, "EXTNAME", val, nullptr, &st) != 0)
                return {};
            return QString::fromLatin1(val).trimmed().toUpper();
        };

        // Pick the flux HDU: prefer well-known extension names, else the first
        // HDU that holds a plausible 1-D vector. Skip mask/quality vectors.
        static const QStringList kFluxExtnames  = {"DATA", "FLUX", "SCI",
                                                   "SPECTRUM", "PRIMARY"};
        static const QStringList kSkipExtnames  = {"ERROR", "ERR", "SIGMA",
                                                   "IVAR", "VARIANCE", "MASK",
                                                   "QUALITY", "DQ", "SEMASK",
                                                   "WLBINS", "DATA_BG",
                                                   "ERROR_BG"};
        int fluxHdu = -1, fallbackHdu = -1;
        for (int hdu = 1; hdu <= numHdus; ++hdu) {
            int hduType = ANY_HDU;
            status = 0;
            fits_movabs_hdu(fptr, hdu, &hduType, &status);
            if (status != 0 || hduType != IMAGE_HDU)
                continue;

            const QString ext = hduExtname(hdu);
            if (kFluxExtnames.contains(ext)) { fluxHdu = hdu; break; }
            if (kSkipExtnames.contains(ext))
                continue;
            if (fallbackHdu < 0) {
                std::vector<double> probe;
                if (readImageHdu(hdu, probe) && probe.size() >= 2)
                    fallbackHdu = hdu;
            }
        }
        if (fluxHdu < 0)
            fluxHdu = fallbackHdu;

        std::vector<double> fluxes;
        bool imageRead = fluxHdu > 0 && readImageHdu(fluxHdu, fluxes);

        // Legacy fallback: read the first NAXIS1 pixels of the primary HDU
        // even if it is multi-dimensional (pre-existing behaviour).
        if (!imageRead) {
            int st = 0;
            fits_movabs_hdu(fptr, 1, nullptr, &st);
            int  naxis = 0;
            long naxes[10] = {0};
            if (st == 0 && fits_get_img_dim(fptr, &naxis, &st) == 0 &&
                naxis >= 1 && fits_get_img_size(fptr, 10, naxes, &st) == 0 &&
                naxes[0] > 0) {
                fluxes.resize(naxes[0]);
                int anynul = 0;
                imageRead = fits_read_img(fptr, TDOUBLE, 1, naxes[0], nullptr,
                                          fluxes.data(), &anynul, &st) == 0;
                if (imageRead)
                    fluxHdu = 1;
            }
        }

        if (imageRead && fluxes.size() >= 1) {
            const long npixels = static_cast<long>(fluxes.size());
            std::vector<double> wavelengths(npixels);
            std::vector<double> errors(npixels, 0.0);

            // Wavelength calibration from the flux HDU's own header.
            status = 0;
            fits_movabs_hdu(fptr, fluxHdu, nullptr, &status);
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

            // Companion uncertainty vector, if present with matching length.
            static const QStringList kErrorExtnames = {"ERROR", "ERR", "SIGMA",
                                                       "STDEV", "UNCERT"};
            for (int hdu = 1; hdu <= numHdus; ++hdu) {
                if (hdu == fluxHdu || !kErrorExtnames.contains(hduExtname(hdu)))
                    continue;
                std::vector<double> errVec;
                if (readImageHdu(hdu, errVec) &&
                    errVec.size() == static_cast<size_t>(npixels)) {
                    errors = std::move(errVec);
                }
                break;
            }

            result.spectrum = std::make_shared<Spectrum>();
            result.spectrum->setData(wavelengths, fluxes, errors);
            result.spectrum->setFile(filepath);
            foundData = true;
        }
    } else {
        // Read from table. A spectral bintable holds its points either as one
        // row per point (scalar columns) or as a single row whose columns are
        // N-element vectors - the shape used by the MAST/HASP coadds, the FUSE
        // NVO products and most VO "spectral container" files. Taking nrows
        // values would leave those with one point, so the element count comes
        // from the column's repeat, not from the row count.
        long nrows = 0;
        status = 0;
        fits_get_num_rows(fptr, &nrows, &status);

        auto readColumn = [&](int col, std::vector<double>& out) -> bool {
            out.clear();
            if (col <= 0 || nrows <= 0) return false;

            int  typecode = 0;
            long repeat = 0, width = 0;
            int  st = 0;
            if (fits_get_coltype(fptr, col, &typecode, &repeat, &width, &st) != 0)
                return false;

            // Variable-length column ('1PE(...)'): each row carries its own
            // element count, so the rows are read one at a time.
            if (typecode < 0) {
                for (long row = 1; row <= nrows; ++row) {
                    long len = 0, offset = 0;
                    st = 0;
                    if (fits_read_descript(fptr, col, row, &len, &offset, &st) != 0
                        || len <= 0)
                        continue;
                    const size_t at = out.size();
                    out.resize(at + size_t(len));
                    int anynul = 0;
                    if (fits_read_col(fptr, TDOUBLE, col, row, 1, len, nullptr,
                                      out.data() + at, &anynul, &st) != 0) {
                        out.resize(at);
                        return false;
                    }
                }
                return !out.empty();
            }

            if (repeat < 1) repeat = 1;
            const long nelem = nrows * repeat;
            if (nelem <= 0) return false;
            out.assign(size_t(nelem), 0.0);
            int anynul = 0;
            st = 0;
            // One read spanning every row: cfitsio walks a fixed-length vector
            // column as one continuous array.
            return fits_read_col(fptr, TDOUBLE, col, 1, 1, nelem, nullptr,
                                 out.data(), &anynul, &st) == 0;
        };

        std::vector<double> wavelengths, fluxes, errors;
        if (status == 0 && readColumn(wavelengthCol, wavelengths) &&
            readColumn(fluxCol, fluxes)) {
            // Mismatched lengths would pair unrelated points; keep the overlap.
            const size_t n = std::min(wavelengths.size(), fluxes.size());
            wavelengths.resize(n);
            fluxes.resize(n);

            if (!(errorCol > 0 && readColumn(errorCol, errors) &&
                  errors.size() >= n))
                errors.assign(n, 0.0);
            errors.resize(n);

            // Echelle products (STIS x1d, UVES) file one order per row, so the
            // concatenation above runs order by order rather than in
            // wavelength order. Everything downstream assumes a rising axis.
            bool ascending = true;
            for (size_t i = 1; i < n && ascending; ++i)
                if (wavelengths[i] < wavelengths[i - 1]) ascending = false;
            if (!ascending) {
                std::vector<size_t> order(n);
                std::iota(order.begin(), order.end(), size_t(0));
                std::stable_sort(order.begin(), order.end(),
                                 [&wavelengths](size_t a, size_t b) {
                                     return wavelengths[a] < wavelengths[b];
                                 });
                std::vector<double> w(n), f(n), e(n);
                for (size_t i = 0; i < n; ++i) {
                    w[i] = wavelengths[order[i]];
                    f[i] = fluxes[order[i]];
                    e[i] = errors[order[i]];
                }
                wavelengths.swap(w);
                fluxes.swap(f);
                errors.swap(e);
            }

            if (n > 0) {
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