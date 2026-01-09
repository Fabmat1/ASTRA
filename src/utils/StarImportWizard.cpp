// src/utils/StarImportWizard.cpp

#include "StarImportWizard.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
#include "utils/BackgroundTaskManager.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QFileDialog>
#include <QMessageBox>
#include <QProgressDialog>
#include <QTextStream>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDebug>
#include <algorithm>
#include <limits>
#include <cmath>
#include <QGroupBox>
#include <QDialogButtonBox>
#include <QSplitter>
#include <QUrlQuery>
#include <QRegularExpression>

// Include CCfits headers
#ifdef HAVE_CCFITS
#include <CCfits/CCfits>
#include <CCfits/Column.h>
#include <CCfits/Table.h>
#endif

// Constructor
StarImportWizard::StarImportWizard(ApplicationController* controller,
    std::shared_ptr<Project> project,
    QWidget* parent)
    : QWizard(parent)
    , _controller(controller)
    , _project(project)
{
    setWindowTitle("Star Import Wizard");
    setWizardStyle(QWizard::ModernStyle);

    // Add pages
    setPage(Page_GeneralImport, new GeneralImportPage);
    setPage(Page_Spectra, new SpectraImportPage);
    setPage(Page_RadialVelocity, new RadialVelocityImportPage);
    setPage(Page_Photometry, new PhotometryImportPage);

    // Configure button layout
    setOptions(QWizard::NoBackButtonOnStartPage | 
    QWizard::NoCancelButtonOnLastPage);

    resize(900, 700);
}

// GeneralImportPage implementation
GeneralImportPage::GeneralImportPage(QWidget* parent)
    : QWizardPage(parent)
    , _simbadThread(nullptr)
    , _simbadWorker(nullptr)
    , _gaiaThread(nullptr)
    , _gaiaWorker(nullptr)
    , _fitsWatcher(nullptr)
{
    setTitle("Import Stars - General Information");
    setSubTitle("Import stellar data from FITS or CSV/ASCII files");
    
    setupColumnAliases();
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    
    // File selection
    QLabel* fileLabel = new QLabel("Data File:");
    layout->addWidget(fileLabel);
    
    QHBoxLayout* fileLayout = new QHBoxLayout;
    _filePathEdit = new QLineEdit;
    connect(_filePathEdit, &QLineEdit::textChanged, this, &GeneralImportPage::onFilePathChanged);
    fileLayout->addWidget(_filePathEdit);
    
    QPushButton* browseButton = new QPushButton("Browse...");
    connect(browseButton, &QPushButton::clicked, this, &GeneralImportPage::onBrowseFile);
    fileLayout->addWidget(browseButton);
    layout->addLayout(fileLayout);
    
    // File parsing options
    QGroupBox* parseOptionsGroup = new QGroupBox("Parsing Options");
    QGridLayout* parseLayout = new QGridLayout;
    
    // Delimiter selection
    QLabel* delimLabel = new QLabel("Column Delimiter:");
    _delimiterCombo = new QComboBox;
    _delimiterCombo->addItems({"Auto-detect", "Comma (,)", "Tab", "Space", "Semicolon (;)", "Pipe (|)", "Custom"});
    connect(_delimiterCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), 
            this, &GeneralImportPage::onDelimiterChanged);
    parseLayout->addWidget(delimLabel, 0, 0);
    parseLayout->addWidget(_delimiterCombo, 0, 1);
    
    _customDelimiterEdit = new QLineEdit;
    _customDelimiterEdit->setMaxLength(1);
    _customDelimiterEdit->setEnabled(false);
    _customDelimiterEdit->setPlaceholderText("Enter custom delimiter");
    connect(_customDelimiterEdit, &QLineEdit::textChanged, 
            [this]() { if (!_filePathEdit->text().isEmpty()) onFilePathChanged(_filePathEdit->text()); });
    parseLayout->addWidget(_customDelimiterEdit, 0, 2);
    
    // Comment character
    QLabel* commentLabel = new QLabel("Comment Character:");
    _commentCharEdit = new QLineEdit("#");
    _commentCharEdit->setMaxLength(1);
    connect(_commentCharEdit, &QLineEdit::textChanged, this, &GeneralImportPage::onCommentCharChanged);
    parseLayout->addWidget(commentLabel, 1, 0);
    parseLayout->addWidget(_commentCharEdit, 1, 1);
    
    // Header checkbox
    _hasHeaderCheckBox = new QCheckBox("First row contains column names");
    _hasHeaderCheckBox->setChecked(true);
    connect(_hasHeaderCheckBox, &QCheckBox::toggled, 
            [this]() { if (!_filePathEdit->text().isEmpty()) onFilePathChanged(_filePathEdit->text()); });
    parseLayout->addWidget(_hasHeaderCheckBox, 2, 0, 1, 2);
    
    parseOptionsGroup->setLayout(parseLayout);
    layout->addWidget(parseOptionsGroup);
    
    // Query options
    QGroupBox* queryOptionsGroup = new QGroupBox("Additional Queries");
    QVBoxLayout* queryLayout = new QVBoxLayout;
    
    _gaiaCheckBox = new QCheckBox("Query Gaia DR3 via VizieR for missing astrometry data");
    queryLayout->addWidget(_gaiaCheckBox);
    
    _simbadCheckBox = new QCheckBox("Query SIMBAD for bibliography codes");
    queryLayout->addWidget(_simbadCheckBox);
    
    _simbadWarningLabel = new QLabel();
    _simbadWarningLabel->setWordWrap(true);
    _simbadWarningLabel->setStyleSheet("QLabel { color: #666666; font-size: 10pt; }");
    _simbadWarningLabel->hide();
    queryLayout->addWidget(_simbadWarningLabel);
    
    connect(_filePathEdit, &QLineEdit::textChanged, this, [this]() {
        updateSimbadWarning();
    });

    connect(_simbadCheckBox, &QCheckBox::toggled, this, [this]() {
        updateSimbadWarning();
    });

    queryOptionsGroup->setLayout(queryLayout);
    layout->addWidget(queryOptionsGroup);
    
    // Preview table
    QLabel* previewLabel = new QLabel("Data Preview (first 10 rows):");
    layout->addWidget(previewLabel);
    
    _previewTable = new QTableWidget(10, 0);
    _previewTable->setAlternatingRowColors(true);
    _previewTable->horizontalHeader()->setStretchLastSection(true);
    layout->addWidget(_previewTable);
    
    registerField("filePath*", _filePathEdit);
    registerField("queryGaia", _gaiaCheckBox);
    registerField("querySimbad", _simbadCheckBox);
}

void GeneralImportPage::updateSimbadWarning()
{
    if (_simbadCheckBox->isChecked() && _dataRows.size() > 100) {
        _simbadWarningLabel->setText(QString("⚠ Querying SIMBAD for %1 stars may take several minutes.")
                                    .arg(_dataRows.size()));
        _simbadWarningLabel->show();
    } else {
        _simbadWarningLabel->hide();
    }
}

void GeneralImportPage::setupColumnAliases()
{
    // Define common aliases for each field (case-insensitive)
    _columnAliases["source_id"] = {"source_id", "gaia_id", "gaia_source_id", "id", "gaia dr3", "gaiaid", "gaia"};
    _columnAliases["alias"] = {"alias", "name", "star_name", "identifier", "star", "object"};
    _columnAliases["tic"] = {"tic", "tic_id", "tess_id", "tessid"};
    _columnAliases["jname"] = {"jname", "2mass", "2mass_id", "j2000_name", "2massid", "twomass"};
    _columnAliases["ra"] = {"ra", "raj2000", "ra_j2000", "right_ascension", "alpha", "_ra"};
    _columnAliases["dec"] = {"dec", "decj2000", "dec_j2000", "declination", "delta", "de", "_dec"};
    _columnAliases["pmra"] = {"pmra", "pm_ra", "proper_motion_ra", "mu_alpha", "mura"};
    _columnAliases["pmdec"] = {"pmdec", "pm_dec", "proper_motion_dec", "mu_delta", "mudec", "pmde"};
    _columnAliases["e_pmra"] = {"e_pmra", "pmra_error", "pmra_err", "u_pmra", "err_pmra", "epmra", "pmra_e"};
    _columnAliases["e_pmdec"] = {"e_pmdec", "pmdec_error", "pmdec_err", "u_pmdec", "err_pmdec", "epmdec", "pmde_e", "pmdec_e"};
    _columnAliases["plx"] = {"plx", "parallax", "par", "parx"};
    _columnAliases["e_plx"] = {"e_plx", "parallax_error", "plx_err", "u_plx", "err_plx", "eplx", "plx_e", "parallax_e"};
    _columnAliases["gmag"] = {"gmag", "phot_g_mean_mag", "g_mag", "gaia_g", "g", "mag_g"};
    _columnAliases["e_gmag"] = {"e_gmag", "phot_g_mean_mag_error", "gmag_err", "u_gmag", "egmag", "gmag_e"};
    _columnAliases["bp"] = {"bp", "bp_mag", "phot_bp_mean_mag", "gaia_bp", "bpmag", "mag_bp"};
    _columnAliases["e_bp"] = {"e_bp", "bp_error", "phot_bp_mean_mag_error", "u_bp", "ebp", "bp_e"};
    _columnAliases["rp"] = {"rp", "rp_mag", "phot_rp_mean_mag", "gaia_rp", "rpmag", "mag_rp"};
    _columnAliases["e_rp"] = {"e_rp", "rp_error", "phot_rp_mean_mag_error", "u_rp", "erp", "rp_e"};
    _columnAliases["bp_rp"] = {"bp_rp", "bp-rp", "color", "bprp", "bp_rp_color"};
    _columnAliases["teff"] = {"teff", "t_eff", "temperature", "effective_temperature", "temp"};
    _columnAliases["e_teff"] = {"e_teff", "teff_error", "teff_err", "u_teff", "eteff", "teff_e"};
    _columnAliases["logg"] = {"logg", "log_g", "surface_gravity", "grav"};
    _columnAliases["e_logg"] = {"e_logg", "logg_error", "logg_err", "u_logg", "elogg", "logg_e"};
    _columnAliases["he"] = {"he", "helium", "he_abundance", "[he/h]", "he_h"};
    _columnAliases["e_he"] = {"e_he", "he_error", "he_err", "u_he", "ehe", "he_e"};
    _columnAliases["rv_med"] = {"rv_med", "medrv", "median_rv", "rv_median", "rvmed"};
    _columnAliases["e_rv_med"] = {"e_rv_med", "medrv_err", "rv_med_err", "u_rv_med", "ervmed", "rvmed_e"};
    _columnAliases["rv_avg"] = {"rv_avg", "rv_mean", "mean_rv", "avgvr", "rvavg", "rv"};
    _columnAliases["e_rv_avg"] = {"e_rv_avg", "rv_avg_err", "rv_mean_err", "u_rv_avg", "ervavg", "rv_e", "erv"};
    _columnAliases["deltaRV"] = {"deltarv", "delta_rv", "rv_amplitude", "rv_amp", "dRV"};
    _columnAliases["e_deltaRV"] = {"e_deltarv", "deltarv_err", "delta_rv_err", "u_deltarv", "edeltarv"};
    _columnAliases["logp"] = {"logp", "log_p"};
    _columnAliases["spec_class"] = {"spec_class", "spectral_type", "sp_type", "spectral_class", "sptype", "spectype"};
    _columnAliases["pmra_pmdec_corr"] = {"pmra_pmdec_corr", "pmra_pmdec_correlation", "corr_pmra_pmdec"};
    _columnAliases["plx_pmdec_corr"] = {"plx_pmdec_corr", "parallax_pmdec_corr", "corr_plx_pmdec"};
    _columnAliases["plx_pmra_corr"] = {"plx_pmra_corr", "parallax_pmra_corr", "corr_plx_pmra"};
}

QString GeneralImportPage::normalizeValue(const QVariant& value) const
{
    if (value.isNull()) {
        return QString();
    }
    
    QString str = value.toString().trimmed();
    
    // Treat these as empty/null values
    static const QSet<QString> emptyValues = {
        "", "-", "--", "---", ".", "..", "...",
        "none", "null", "nan", "na", "n/a", 
        "undefined", "unknown", "missing",
        " ", "  ", "   "
    };
    
    if (emptyValues.contains(str.toLower())) {
        return QString();
    }
    
    // For numeric values, normalize the format
    if (value.typeId() == QMetaType::Double) {
        double d = value.toDouble();
        if (std::isnan(d) || std::isinf(d)) {
            return QString();
        }
        // Use consistent precision for comparison
        return QString::number(d, 'g', 12);
    }
    
    return str;
}

QString GeneralImportPage::generateRowKey(const DataRow& row) const
{
    QStringList keyParts;
    
    // Only use MAPPED columns for duplicate detection
    for (const auto& [columnName, fieldName] : _columnMappings) {
        auto it = row.values.find(columnName);
        QString normalizedValue;
        
        if (it != row.values.end()) {
            normalizedValue = normalizeValue(it->second);
        }
        
        // Include field name and normalized value (empty string if null/missing)
        keyParts << QString("%1=%2").arg(fieldName, normalizedValue);
    }
    
    // Sort to ensure consistent ordering
    keyParts.sort();
    
    return keyParts.join("|");
}

void GeneralImportPage::removeDuplicateRows()
{
    if (_dataRows.empty() || _columnMappings.empty()) return;
    
    QSet<QString> seenKeys;
    std::vector<DataRow> uniqueRows;
    uniqueRows.reserve(_dataRows.size());
    
    int duplicatesRemoved = 0;
    
    for (const DataRow& row : _dataRows) {
        QString key = generateRowKey(row);
        
        // Skip rows where all mapped values are empty (completely empty rows)
        bool hasAnyValue = false;
        for (const auto& [columnName, fieldName] : _columnMappings) {
            auto it = row.values.find(columnName);
            if (it != row.values.end()) {
                QString normalized = normalizeValue(it->second);
                if (!normalized.isEmpty()) {
                    hasAnyValue = true;
                    break;
                }
            }
        }
        
        if (!hasAnyValue) {
            duplicatesRemoved++;  // Skip empty rows
            continue;
        }
        
        if (!seenKeys.contains(key)) {
            seenKeys.insert(key);
            uniqueRows.push_back(row);
        } else {
            duplicatesRemoved++;
        }
    }
    
    if (duplicatesRemoved > 0) {
        _dataRows = std::move(uniqueRows);
        qDebug() << "Removed" << duplicatesRemoved << "duplicate/empty rows, kept" << _dataRows.size();
    }
}

void GeneralImportPage::onBrowseFile()
{
    QString filter = "All Supported (*.csv *.txt *.dat *.fits *.fit);;CSV/ASCII Files (*.csv *.txt *.dat);;FITS Files (*.fits *.fit)";
    QString filePath = QFileDialog::getOpenFileName(this, "Select Data File", "", filter);
    
    if (!filePath.isEmpty()) {
        _filePathEdit->setText(filePath);
    }
}

void GeneralImportPage::onFilePathChanged(const QString& path)
{
    if (!path.isEmpty() && QFile::exists(path)) {
        readFile(path);
    }
}

void GeneralImportPage::onDelimiterChanged()
{
    _customDelimiterEdit->setEnabled(_delimiterCombo->currentText() == "Custom");
    if (!_filePathEdit->text().isEmpty()) {
        onFilePathChanged(_filePathEdit->text());
    }
}

void GeneralImportPage::onCommentCharChanged(const QString& text)
{
    Q_UNUSED(text)
    if (!_filePathEdit->text().isEmpty()) {
        onFilePathChanged(_filePathEdit->text());
    }
}

bool GeneralImportPage::readFile(const QString& filePath)
{
    _dataRows.clear();
    _columnNames.clear();
    _columnMappings.clear();
    _unmappedColumns.clear();
    
    bool success = false;
    if (filePath.endsWith(".fits", Qt::CaseInsensitive) || 
        filePath.endsWith(".fit", Qt::CaseInsensitive)) {
        success = readFITS(filePath);
    } else {
        success = readCSV(filePath);
    }
    
    if (success) {
        mapColumns();
        updatePreview();
    }
    
    return success;
}

QChar GeneralImportPage::detectDelimiter(const QString& line) const
{
    // Count occurrences of common delimiters
    QMap<QChar, int> counts;
    counts[','] = line.count(',');
    counts['\t'] = line.count('\t');
    counts[';'] = line.count(';');
    counts['|'] = line.count('|');
    
    // Count spaces, but only if they appear consistently
    QStringList spaceSplit = line.split(' ', Qt::SkipEmptyParts);
    if (spaceSplit.size() > 1) {
        counts[' '] = spaceSplit.size() - 1;
    }
    
    // Find delimiter with highest count
    QChar bestDelim = ',';
    int maxCount = 0;
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        if (it.value() > maxCount) {
            maxCount = it.value();
            bestDelim = it.key();
        }
    }
    
    return bestDelim;
}

QStringList GeneralImportPage::parseCSVLine(const QString& line, QChar delimiter) const
{
    QStringList result;
    QString current;
    bool inQuotes = false;
    bool wasQuote = false;
    
    for (int i = 0; i < line.length(); ++i) {
        QChar ch = line[i];
        
        if (wasQuote) {
            wasQuote = false;
            if (ch == '"') {
                // Double quote - add single quote to result
                current += '"';
                continue;
            } else {
                // End of quoted section
                inQuotes = false;
                // Fall through to process current character
            }
        }
        
        if (ch == '"') {
            if (inQuotes) {
                wasQuote = true;
            } else {
                inQuotes = true;
            }
        } else if (ch == delimiter && !inQuotes) {
            result.append(current.trimmed());
            current.clear();
        } else {
            current += ch;
        }
    }
    
    // Add last field
    result.append(current.trimmed());
    
    return result;
}

QVariant GeneralImportPage::convertValue(const QString& value) const
{
    if (value.isEmpty() || value.toLower() == "nan" || value.toLower() == "null" || value == "--") {
        return QVariant();
    }
    
    // Try to convert to number
    bool ok;
    double doubleVal = value.toDouble(&ok);
    if (ok) {
        // Check if this looks like a large integer (no decimal point and outside safe integer range)
        if (!value.contains('.') && !value.contains('e', Qt::CaseInsensitive)) {
            qlonglong intVal = value.toLongLong(&ok);
            if (ok && (intVal > 9007199254740991LL || intVal < -9007199254740991LL)) {
                // Large integer outside double's safe range - preserve as string
                return value;
            }
        }
        return doubleVal;
    }
    
    // Return as string
    return value;
}

bool GeneralImportPage::readCSV(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Error", "Could not open file: " + filePath);
        return false;
    }
    
    QTextStream stream(&file);
    QString commentChar = _commentCharEdit->text();
    
    // Determine delimiter
    QChar delimiter = ',';
    if (_delimiterCombo->currentText() == "Comma (,)") delimiter = ',';
    else if (_delimiterCombo->currentText() == "Tab") delimiter = '\t';
    else if (_delimiterCombo->currentText() == "Space") delimiter = ' ';
    else if (_delimiterCombo->currentText() == "Semicolon (;)") delimiter = ';';
    else if (_delimiterCombo->currentText() == "Pipe (|)") delimiter = '|';
    else if (_delimiterCombo->currentText() == "Custom" && !_customDelimiterEdit->text().isEmpty()) {
        delimiter = _customDelimiterEdit->text()[0];
    }
    else if (_delimiterCombo->currentText() == "Auto-detect") {
        // Read first non-comment line to detect delimiter
        QString firstLine;
        while (!stream.atEnd()) {
            QString line = stream.readLine();
            if (!commentChar.isEmpty() && line.startsWith(commentChar)) {
                continue;
            }
            firstLine = line;
            break;
        }
        if (!firstLine.isEmpty()) {
            delimiter = detectDelimiter(firstLine);
        }
        stream.seek(0); // Reset to beginning
    }
    
    // Read lines
    QStringList lines;
    while (!stream.atEnd()) {
        QString line = stream.readLine();
        // Skip comment lines
        if (!commentChar.isEmpty() && line.startsWith(commentChar)) {
            continue;
        }
        // Skip empty lines
        if (line.trimmed().isEmpty()) {
            continue;
        }
        lines.append(line);
    }
    
    file.close();
    
    if (lines.isEmpty()) {
        QMessageBox::warning(this, "Error", "File is empty or contains only comments");
        return false;
    }
    
    // Parse header
    int startRow = 0;
    if (_hasHeaderCheckBox->isChecked()) {
        _columnNames.clear();
        QStringList headers = parseCSVLine(lines[0], delimiter);
        for (const QString& header : headers) {
            _columnNames.push_back(header.trimmed());
        }
        startRow = 1;
    } else {
        // Generate column names
        QStringList firstRow = parseCSVLine(lines[0], delimiter);
        _columnNames.clear();
        for (int i = 0; i < firstRow.size(); ++i) {
            _columnNames.push_back(QString("Column_%1").arg(i + 1));
        }
    }
    
    // Read data rows
    _dataRows.clear();
    for (int i = startRow; i < lines.size(); ++i) {
        QStringList values = parseCSVLine(lines[i], delimiter);
        
        DataRow row;
        for (int j = 0; j < std::min(static_cast<int>(values.size()), static_cast<int>(_columnNames.size())); ++j) {
            row.values[_columnNames[j]] = convertValue(values[j]);
        }
        _dataRows.push_back(row);
    }
    
    return true;
}

bool GeneralImportPage::readFITS(const QString& filePath)
{
#ifdef HAVE_CCFITS
    QProgressDialog progress("Reading FITS file...", "Cancel", 0, 100, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.show();
    
    // Clear existing data
    _dataRows.clear();
    _columnNames.clear();
    
    try {
        std::unique_ptr<CCfits::FITS> pInfile(new CCfits::FITS(filePath.toStdString(), CCfits::Read, true));
        
        const CCfits::ExtMap& extMap = pInfile->extension();
        if (extMap.empty()) {
            return false;
        }
        
        // Find first table extension
        CCfits::ExtHDU* table = nullptr;
        for (const auto& ext : extMap) {
            if (dynamic_cast<CCfits::Table*>(ext.second) != nullptr) {
                table = ext.second;
                break;
            }
        }
        
        if (!table) {
            return false;
        }
        
        long nrows = table->rows();
        if (nrows == 0) {
            return false;
        }
        
        // Limit rows if needed
        long maxRows = std::min(nrows, 100000L);
        
        // Get column information and prepare bulk read
        const CCfits::ColMap& columns = table->column();
        struct ColumnInfo {
            CCfits::Column* ptr;
            QString name;
            int type;  // Use int instead of ColumnType
        };
        std::vector<ColumnInfo> columnInfos;
        
        for (const auto& col : columns) {
            _columnNames.push_back(QString::fromStdString(col.first));
            ColumnInfo info;
            info.ptr = col.second;
            info.name = QString::fromStdString(col.first);
            info.type = col.second->type();
            columnInfos.push_back(info);
        }
        
        // Pre-allocate data storage
        _dataRows.reserve(maxRows);
        
        // Prepare column data buffers for bulk reading
        struct ColumnData {
            std::vector<double> doubleData;
            std::vector<float> floatData;
            std::vector<long> longData;
            std::vector<std::string> stringData;
        };
        std::vector<ColumnData> columnBuffers(columnInfos.size());
        
        // Read data in large chunks (10000 rows at a time or all at once if less)
        const long chunkSize = std::min(10000L, maxRows);
        
        for (long startRow = 1; startRow <= maxRows; startRow += chunkSize) {
            if (progress.wasCanceled()) {
                _dataRows.clear();
                _columnNames.clear();
                return false;
            }
            
            long endRow = std::min(startRow + chunkSize - 1, maxRows);
            long rowsInChunk = endRow - startRow + 1;
            
            // Update progress
            int progressValue = static_cast<int>((startRow * 100) / maxRows);
            progress.setValue(progressValue);
            progress.setLabelText(QString("Reading FITS file... %1/%2 rows").arg(startRow).arg(maxRows));
            QApplication::processEvents();
            
            // Bulk read entire columns for this chunk
            for (size_t colIdx = 0; colIdx < columnInfos.size(); ++colIdx) {
                const auto& colInfo = columnInfos[colIdx];
                auto& buffer = columnBuffers[colIdx];
                
                try {
                    if (colInfo.type == CCfits::Tdouble) {
                        buffer.doubleData.clear();
                        colInfo.ptr->read(buffer.doubleData, startRow, endRow);
                    } else if (colInfo.type == CCfits::Tfloat) {
                        buffer.floatData.clear();
                        colInfo.ptr->read(buffer.floatData, startRow, endRow);
                    // NEW:
                    } else if (colInfo.type == CCfits::Tint || 
                            colInfo.type == CCfits::Tlong || 
                            colInfo.type == CCfits::Tlonglong) {
                        buffer.longData.clear();
                        colInfo.ptr->read(buffer.longData, startRow, endRow);
                        // Also try to read as string for large integers
                        if (colInfo.type == CCfits::Tlonglong) {
                            buffer.stringData.clear();
                            colInfo.ptr->read(buffer.stringData, startRow, endRow);
                        }
                    } else if (colInfo.type == CCfits::Tstring) {
                        buffer.stringData.clear();
                        colInfo.ptr->read(buffer.stringData, startRow, endRow);
                    }
                } catch (...) {
                    // If column read fails, continue with other columns
                    continue;
                }
            }
            
            // Process the chunk data into DataRows
            for (long i = 0; i < rowsInChunk; ++i) {
                DataRow dataRow;
                
                for (size_t colIdx = 0; colIdx < columnInfos.size(); ++colIdx) {
                    const auto& colInfo = columnInfos[colIdx];
                    const auto& buffer = columnBuffers[colIdx];
                    
                    try {
                        if (colInfo.type == CCfits::Tdouble) {
                            if (i < static_cast<long>(buffer.doubleData.size()) && !std::isnan(buffer.doubleData[i])) {
                                dataRow.values[colInfo.name] = buffer.doubleData[i];
                            }
                        } else if (colInfo.type == CCfits::Tfloat) {
                            if (i < static_cast<long>(buffer.floatData.size()) && !std::isnan(buffer.floatData[i])) {
                                dataRow.values[colInfo.name] = static_cast<double>(buffer.floatData[i]);
                            }
                        } else if (colInfo.type == CCfits::Tint || 
                                colInfo.type == CCfits::Tlong || 
                                colInfo.type == CCfits::Tlonglong) {
                            if (i < static_cast<long>(buffer.longData.size())) {
                                long val = buffer.longData[i];
                                // Check for invalid/error values (INT_MIN, INT_MAX, etc.)
                                if (val == std::numeric_limits<int>::min() || 
                                    val == std::numeric_limits<long>::min()) {
                                    // Skip invalid values
                                    continue;
                                }
                                // For large integers, convert through string to preserve precision
                                if (colInfo.type == CCfits::Tlonglong && i < static_cast<long>(buffer.stringData.size())) {
                                    QVariant converted = convertValue(QString::fromStdString(buffer.stringData[i]));
                                    if (!converted.isNull()) {
                                        dataRow.values[colInfo.name] = converted;
                                    }
                                } else {
                                    QString strVal = QString::number(val);
                                    QVariant converted = convertValue(strVal);
                                    if (!converted.isNull()) {
                                        dataRow.values[colInfo.name] = converted;
                                    }
                                }
                            }
                        } else if (colInfo.type == CCfits::Tstring) {
                            if (i < static_cast<long>(buffer.stringData.size())) {
                                dataRow.values[colInfo.name] = QString::fromStdString(buffer.stringData[i]).trimmed();
                            }
                        }
                    } catch (...) {
                        continue;
                    }
                }
                
                _dataRows.push_back(std::move(dataRow));
            }
        }
        
        progress.setValue(100);
        updateSimbadWarning();
        return true;
        
    } catch (const std::exception& e) {
        QMessageBox::warning(this, "FITS Error", 
                           QString("Failed to read FITS file: %1").arg(e.what()));
        return false;
    }
#else
    Q_UNUSED(filePath)
    QMessageBox::warning(this, "Error", "FITS support not compiled in. Please install CCfits library.");
    return false;
#endif
}

void GeneralImportPage::mapColumns()
{
    _columnMappings.clear();
    _unmappedColumns.clear();
    
    // Try to map each column
    for (const QString& columnName : _columnNames) {
        QString lowerCol = columnName.toLower();
        // Remove common prefixes/suffixes
        lowerCol.remove(QRegularExpression("^_+|_+$"));
        
        bool mapped = false;
        
        // Check against aliases
        for (const auto& [field, aliases] : _columnAliases) {
            for (const QString& alias : aliases) {
                if (lowerCol == alias.toLower()) {
                    _columnMappings[columnName] = field;
                    mapped = true;
                    break;
                }
            }
            if (mapped) break;
        }
        
        if (!mapped) {
            _unmappedColumns.push_back(columnName);
        }
    }
}

void GeneralImportPage::updatePreview()
{
    _previewTable->clear();
    _previewTable->setColumnCount(_columnNames.size());
    
    // Set headers with mapping info
    QStringList headers;
    for (const QString& colName : _columnNames) {
        if (_columnMappings.find(colName) != _columnMappings.end()) {
            headers << QString("%1\n→ %2").arg(colName).arg(_columnMappings[colName]);
        } else {
            headers << QString("%1\n(unmapped)").arg(colName);
        }
    }
    _previewTable->setHorizontalHeaderLabels(headers);
    
    // Show data
    int rowsToShow = std::min(10, static_cast<int>(_dataRows.size()));
    _previewTable->setRowCount(rowsToShow);
    
    for (int i = 0; i < rowsToShow; ++i) {
        const DataRow& row = _dataRows[i];
        for (int j = 0; j < _columnNames.size(); ++j) {
            QString colName = _columnNames[j];
            QString displayText;
            
            if (row.values.find(colName) != row.values.end()) {
                QVariant value = row.values.at(colName);
                if (value.typeId() == QMetaType::Double) {
                    displayText = QString::number(value.toDouble(), 'g', 6);
                } else {
                    displayText = value.toString();
                }
            }
            
            QTableWidgetItem* item = new QTableWidgetItem(displayText);
            
            // Color code based on mapping status
            if (_columnMappings.find(colName) != _columnMappings.end()) {
                item->setBackground(QColor(200, 255, 200)); // Light green for mapped
            } else {
                item->setBackground(QColor(255, 200, 200)); // Light red for unmapped
            }
            
            _previewTable->setItem(i, j, item);
        }
    }
    
    _previewTable->resizeColumnsToContents();
}

void GeneralImportPage::applyValueToStar(std::shared_ptr<Star> star, const QString& field, const QVariant& value)
{
    if (!value.isValid() || value.isNull()) {
        return;
    }
    
    // String fields
    if (field == "alias") star->setAlias(value.toString());
    else if (field == "source_id") star->setSourceId(value.toString());
    else if (field == "tic") star->setTic(value.toString());
    else if (field == "jname") star->setJName(value.toString());
    else if (field == "spec_class") star->setSpecClass(value.toString());
    
    // Numeric fields
    else if (field == "ra") star->setRa(value.toDouble());
    else if (field == "dec") star->setDec(value.toDouble());
    else if (field == "pmra") star->setPmra(value.toDouble());
    else if (field == "pmdec") star->setPmdec(value.toDouble());
    else if (field == "e_pmra") star->setEPmra(value.toDouble());
    else if (field == "e_pmdec") star->setEPmdec(value.toDouble());
    else if (field == "plx") star->setPlx(value.toDouble());
    else if (field == "e_plx") star->setEPlx(value.toDouble());
    else if (field == "pmra_pmdec_corr") star->setPmraPmdecCorr(value.toDouble());
    else if (field == "plx_pmdec_corr") star->setPlxPmdecCorr(value.toDouble());
    else if (field == "plx_pmra_corr") star->setPlxPmraCorr(value.toDouble());
    else if (field == "gmag") star->setGmag(value.toDouble());
    else if (field == "e_gmag") star->setEGmag(value.toDouble());
    else if (field == "bp") star->setBp(value.toDouble());
    else if (field == "e_bp") star->setEBp(value.toDouble());
    else if (field == "rp") star->setRp(value.toDouble());
    else if (field == "e_rp") star->setERp(value.toDouble());
    else if (field == "bp_rp") star->setBpRp(value.toDouble());
    else if (field == "teff") star->setTeff(value.toDouble());
    else if (field == "e_teff") star->setETeff(value.toDouble());
    else if (field == "logg") star->setLogg(value.toDouble());
    else if (field == "e_logg") star->setELogg(value.toDouble());
    else if (field == "he") star->setHe(value.toDouble());
    else if (field == "e_he") star->setEHe(value.toDouble());
    else if (field == "logp") star->setLogP(value.toDouble());
    else if (field == "deltaRV") star->setDeltaRV(value.toDouble());
    else if (field == "e_deltaRV") star->setEDeltaRV(value.toDouble());
    else if (field == "rv_avg") star->setRVAvg(value.toDouble());
    else if (field == "e_rv_avg") star->setERVAvg(value.toDouble());
    else if (field == "rv_med") star->setRVMed(value.toDouble());
    else if (field == "e_rv_med") star->setERVMed(value.toDouble());
}

std::vector<std::shared_ptr<Star>> GeneralImportPage::createStarsFromData()
{
    std::vector<std::shared_ptr<Star>> stars;
    
    for (const DataRow& row : _dataRows) {
        auto star = std::make_shared<Star>();
        
        // Apply mapped values
        for (const auto& [columnName, fieldName] : _columnMappings) {
            if (row.values.find(columnName) != row.values.end()) {
                applyValueToStar(star, fieldName, row.values.at(columnName));
            }
        }
        
        stars.push_back(star);
    }
    
    return stars;
}

bool GeneralImportPage::isComplete() const
{
    return !_filePathEdit->text().isEmpty() && QFile::exists(_filePathEdit->text()) && !_dataRows.empty();
}

bool GeneralImportPage::validatePage()
{
    if (_dataRows.empty()) {
        QMessageBox::warning(this, "No Data", "No data was loaded from the file.");
        return false;
    }
    
    // Remove duplicate rows before processing
    removeDuplicateRows();
    
    // Check if there are unmapped columns
    if (!_unmappedColumns.empty()) {
        QMessageBox::StandardButton reply = QMessageBox::question(this, "Unmapped Columns",
            QString("There are %1 unmapped columns. Would you like to map them manually?")
                .arg(_unmappedColumns.size()),
            QMessageBox::Yes | QMessageBox::No);
        
        if (reply == QMessageBox::Yes) {
            std::vector<QString> availableFields = {
                "alias", "source_id", "tic", "jname", "ra", "dec", 
                "pmra", "pmdec", "e_pmra", "e_pmdec", "plx", "e_plx",
                "gmag", "e_gmag", "bp", "e_bp", "rp", "e_rp", "bp_rp",
                "teff", "e_teff", "logg", "e_logg", "he", "e_he",
                "rv_med", "e_rv_med", "rv_avg", "e_rv_avg", "deltaRV", 
                "e_deltaRV", "logp", "spec_class",
                "pmra_pmdec_corr", "plx_pmdec_corr", "plx_pmra_corr"
            };
            
            std::vector<DataRow> sampleData;
            for (size_t i = 0; i < std::min(size_t(5), _dataRows.size()); ++i) {
                sampleData.push_back(_dataRows[i]);
            }
            
            ColumnMappingDialog dialog(_unmappedColumns, _columnMappings, availableFields, sampleData, this);
            if (dialog.exec() == QDialog::Accepted) {
                _columnMappings = dialog.getMappings();
                updatePreview();
            }
        }
    }
    
    // Create stars from data
    std::vector<std::shared_ptr<Star>> stars = createStarsFromData();
    
    if (stars.empty()) {
        QMessageBox::warning(this, "Error", "No stars could be created from the data.");
        return false;
    }
    
    StarImportWizard* importWizard = qobject_cast<StarImportWizard*>(wizard());
    if (!importWizard) {
        QMessageBox::critical(this, "Error", "Internal error: Could not access wizard");
        return false;
    }
    
    auto controller = importWizard->controller();
    auto project = importWizard->project();
    
    // Store imported stars in the wizard
    importWizard->setImportedStars(stars);
    
    // Save all stars at once
    if (!controller->saveStarsToProject(project, stars)) {
        QMessageBox::critical(this, "Error", "Failed to save stars to database");
        return false;
    }
    
    // Queue background tasks for Gaia and SIMBAD queries
    BackgroundTaskManager* taskManager = controller->backgroundTaskManager();
    
    if (_gaiaCheckBox->isChecked()) {
        // Make a copy of stars for the background task
        std::vector<std::shared_ptr<Star>> starsCopy = stars;
        auto gaiaTask = new GaiaQueryTask(std::move(starsCopy), project->getId(), controller);
        taskManager->queueTask(gaiaTask);
    }
    
    if (_simbadCheckBox->isChecked()) {
        // Make a copy of stars for the background task
        std::vector<std::shared_ptr<Star>> starsCopy = stars;
        auto simbadTask = new SimbadQueryTask(std::move(starsCopy), project->getId(), controller);
        taskManager->queueTask(simbadTask);
    }
    
    QString message = QString("Successfully imported %1 stars.").arg(stars.size());
    if (_gaiaCheckBox->isChecked() || _simbadCheckBox->isChecked()) {
        message += "\n\nBackground queries have been started and will update the data automatically.";
    }
    
    QMessageBox::information(this, "Import Complete", message);
    
    return true;
}

void GeneralImportPage::queryGaiaData(std::vector<std::shared_ptr<Star>>& stars)
{
    // Create progress dialog
    QProgressDialog* progress = new QProgressDialog("Querying Gaia DR3 via VizieR...", 
                                                    "Cancel", 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->show();
    
    // Create worker and thread
    _gaiaThread = new QThread;
    _gaiaWorker = new GaiaWorker(stars);
    _gaiaWorker->moveToThread(_gaiaThread);
    
    // Use a local event loop to wait for completion
    QEventLoop loop;
    
    connect(_gaiaThread, &QThread::started, _gaiaWorker, &GaiaWorker::process);
    
    connect(_gaiaWorker, &GaiaWorker::progress, this,
            [progress](int current, int total, const QString& message) {
        progress->setValue(current);
        progress->setMaximum(total);
        progress->setLabelText(message);
        QApplication::processEvents();
    }, Qt::QueuedConnection);
    
    connect(_gaiaWorker, &GaiaWorker::finished, this,
            [this, progress, &loop](int updatedCount) {
        progress->close();
        progress->deleteLater();
        
        if (updatedCount > 0) {
            QMessageBox::information(this, "Gaia Query Complete",
                QString("Updated astrometry data for %1 stars from Gaia DR3.")
                .arg(updatedCount));
        }
        
        _gaiaThread->quit();
        _gaiaThread->wait();
        _gaiaThread->deleteLater();
        _gaiaWorker->deleteLater();
        _gaiaThread = nullptr;
        _gaiaWorker = nullptr;
        
        loop.quit();
    }, Qt::QueuedConnection);
    
    connect(_gaiaWorker, &GaiaWorker::error, this,
            [this, progress, &loop](const QString& error) {
        QMessageBox::warning(this, "Gaia Query Error", error);
        progress->close();
        progress->deleteLater();
        
        _gaiaThread->quit();
        _gaiaThread->wait();
        _gaiaThread->deleteLater();
        _gaiaWorker->deleteLater();
        _gaiaThread = nullptr;
        _gaiaWorker = nullptr;
        
        loop.quit();
    }, Qt::QueuedConnection);
    
    connect(progress, &QProgressDialog::canceled, [this, &loop]() {
        if (_gaiaThread && _gaiaThread->isRunning()) {
            _gaiaThread->quit();
            _gaiaThread->wait();
        }
        loop.quit();
    });
    
    _gaiaThread->start();
    loop.exec();  // Wait for query to complete
}

int GeneralImportPage::nextId() const
{
    return StarImportWizard::Page_Spectra;
}

// Add SIMBAD query method
void GeneralImportPage::querySimbadBibcodes(const std::vector<std::shared_ptr<Star>>& stars)
{
    // Create progress dialog
    QProgressDialog* progress = new QProgressDialog("Querying SIMBAD for bibliography codes...", 
                                                    "Cancel", 0, stars.size(), this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->show();
    
    // Create worker and thread
    _simbadThread = new QThread;
    _simbadWorker = new SimbadWorker(stars);
    _simbadWorker->moveToThread(_simbadThread);
    
    // Connect signals - use Qt::QueuedConnection to ensure slots run on main thread
    connect(_simbadThread, &QThread::started, _simbadWorker, &SimbadWorker::process);
    
    connect(_simbadWorker, &SimbadWorker::progress, this,
            [progress](int current, int total, const QString& message) {
        progress->setValue(current);
        progress->setMaximum(total);
        progress->setLabelText(message);
        QApplication::processEvents();
    }, Qt::QueuedConnection);
    
    connect(_simbadWorker, &SimbadWorker::finished, this,
            [this, stars, progress](const QMap<QString, QStringList>& bibcodes) {
        // This will now run on the main thread
        StarImportWizard* importWizard = qobject_cast<StarImportWizard*>(wizard());
        if (importWizard) {
            auto controller = importWizard->controller();
            auto project = importWizard->project();
            
            // Update stars with bibcodes
            int updatedCount = 0;
            for (auto& star : stars) {
                QString sourceId = star->getSourceId();
                if (bibcodes.contains(sourceId)) {
                    const QStringList& starBibcodes = bibcodes[sourceId];
                    for (const QString& bibcode : starBibcodes) {
                        star->addBibcode(bibcode);
                    }
                    updatedCount++;
                }
            }
            
            // Save all stars with updated bibcodes in a single batch
            if (updatedCount > 0) {
                if (!controller->saveStarsToProject(project, stars)) {
                    QMessageBox::warning(this, "Update Error", 
                        "Failed to update stars with bibliography codes");
                } else {
                    QMessageBox::information(this, "SIMBAD Query Complete",
                        QString("Successfully retrieved bibliography codes for %1 stars")
                        .arg(updatedCount));
                }
            }
        }
        
        progress->close();
        progress->deleteLater();
        
        // Clean up thread
        _simbadThread->quit();
        _simbadThread->wait();
        _simbadThread->deleteLater();
        _simbadWorker->deleteLater();
        _simbadThread = nullptr;
        _simbadWorker = nullptr;
    }, Qt::QueuedConnection);  // This ensures the slot runs on the main thread
    
    connect(_simbadWorker, &SimbadWorker::error, this,
            [progress](const QString& error) {
        QMessageBox::warning(nullptr, "SIMBAD Error", error);
        progress->close();
        progress->deleteLater();
    }, Qt::QueuedConnection);
    
    // Handle progress dialog cancellation
    connect(progress, &QProgressDialog::canceled, [this]() {
        if (_simbadThread && _simbadThread->isRunning()) {
            _simbadThread->quit();
            _simbadThread->wait();
        }
    });
    
    // Start the thread
    _simbadThread->start();
}

// SimbadWorker implementation
SimbadWorker::SimbadWorker(const std::vector<std::shared_ptr<Star>>& stars, QObject* parent)
    : QObject(parent)
    , _stars(stars)
    , _networkManager(new QNetworkAccessManager(this))
{
}

void SimbadWorker::process()
{
    emit progress(0, _stars.size(), "Generating SIMBAD script...");
    
    QString script = generateSimbadScript();
    if (script.isEmpty()) {
        emit error("No stars with valid Gaia IDs to query");
        return;
    }
    
    //qDebug() << "Generated SIMBAD script with" << _stars.size() << "stars";
    
    // Create temporary file for script
    QTemporaryFile scriptFile;
    if (!scriptFile.open()) {
        emit error("Failed to create temporary script file");
        return;
    }
    scriptFile.write(script.toUtf8());
    scriptFile.flush();  // Ensure data is written
    QString scriptPath = scriptFile.fileName();
    scriptFile.close();  // Close but keep the file
    
    emit progress(0, _stars.size(), "Sending query to SIMBAD...");
    
    // Prepare POST request
    QNetworkRequest request(QUrl("http://simbad.u-strasbg.fr/simbad/sim-script"));
    request.setRawHeader("User-Agent", "ASTRA/1.0");
    
    // Create multipart form data
    QHttpMultiPart* multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    
    // Add script file
    QFile* file = new QFile(scriptPath);
    if (!file->open(QIODevice::ReadOnly)) {
        emit error("Failed to open script file");
        delete multiPart;
        return;
    }
    
    QHttpPart scriptPart;
    scriptPart.setHeader(QNetworkRequest::ContentDispositionHeader, 
        QVariant("form-data; name=\"scriptFile\"; filename=\"script.txt\""));
    scriptPart.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("text/plain"));
    scriptPart.setBody(script.toUtf8());
    multiPart->append(scriptPart);
    
    // Send request
    QNetworkReply* reply = _networkManager->post(request, multiPart);
    multiPart->setParent(reply);  // reply takes ownership
    
    // Wait for response
    QEventLoop loop;
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    
    // Add timeout
    QTimer::singleShot(60000, &loop, &QEventLoop::quit); // 60 second timeout
    
    loop.exec();
    
    if (reply->error() != QNetworkReply::NoError) {
        emit error(QString("Network error: %1").arg(reply->errorString()));
        reply->deleteLater();
        return;
    }
    
    emit progress(_stars.size() / 2, _stars.size(), "Parsing SIMBAD response...");
    
    QString response = QString::fromUtf8(reply->readAll());
    reply->deleteLater();
    
    //qDebug() << "Received SIMBAD response of size:" << response.size();
    
    QMap<QString, QStringList> bibcodes = parseSimbadResponse(response);
    
    emit progress(_stars.size(), _stars.size(), "Complete");
    emit finished(bibcodes);
}

// GaiaWorker implementation
GaiaWorker::GaiaWorker(std::vector<std::shared_ptr<Star>>& stars, QObject* parent)
    : QObject(parent)
    , _stars(stars)
    , _networkManager(new QNetworkAccessManager(this))
{
}

bool GaiaWorker::starNeedsGaiaData(const std::shared_ptr<Star>& star) const
{
    // Check if star is missing astrometry data that Gaia can provide
    return (star->getRa() == 0.0 && star->getDec() == 0.0) ||
           star->getPmra() == 0.0 ||
           star->getPmdec() == 0.0 ||
           star->getPlx() == 0.0 ||
           star->getGmag() == 0.0 ||
           star->getBp() == 0.0 ||
           star->getRp() == 0.0 ||
           star->getEPmra() == 0.0 ||
           star->getEPmdec() == 0.0 ||
           star->getEPlx() == 0.0 ||
           star->getPmraPmdecCorr() == 0.0 ||
           star->getPlxPmdecCorr() == 0.0 ||
           star->getPlxPmraCorr() == 0.0;
}

QString GaiaWorker::buildADQLQuery()
{
    // Collect all source IDs for stars that need data
    QStringList sourceIds;
    
    for (const auto& star : _stars) {
        if (!starNeedsGaiaData(star)) continue;
        
        QString sourceId = star->getSourceId();
        if (!sourceId.isEmpty()) {
            // Clean up source_id - ensure it's just the numeric ID
            sourceId = sourceId.trimmed();
            // Remove any "Gaia DR3" prefix if present
            if (sourceId.contains("DR3")) {
                QRegularExpression re("\\d{10,}");
                QRegularExpressionMatch match = re.match(sourceId);
                if (match.hasMatch()) {
                    sourceId = match.captured(0);
                }
            }
            sourceIds << sourceId;
        }
    }
    
    if (sourceIds.isEmpty()) {
        return QString();
    }
    
    // VizieR uses different column names than the Gaia archive!
    // Build ADQL query - limit chunk size to avoid timeout
    // VizieR column names for I/355/gaiadr3:
    // Source, RA_ICRS, DE_ICRS, pmRA, pmDE, e_pmRA, e_pmDE, 
    // Plx, e_Plx, Gmag, BPmag, RPmag, pmRApmDEcor, PlxpmRAcor, PlxpmDEcor
    
    QString query = "SELECT Source, RA_ICRS, DE_ICRS, pmRA, pmDE, e_pmRA, e_pmDE, "
                   "Plx, e_Plx, Gmag, BPmag, RPmag, "
                   "pmRApmDEcor, PlxpmRAcor, PlxpmDEcor "
                   "FROM \"I/355/gaiadr3\" WHERE Source IN (";
    
    query += sourceIds.join(",");
    query += ")";
    
    return query;
}

void GaiaWorker::process()
{
    emit progress(0, 100, "Checking which stars need Gaia data...");
    
    // Count how many stars need data
    int starsNeedingData = 0;
    for (const auto& star : _stars) {
        if (starNeedsGaiaData(star)) {
            starsNeedingData++;
        }
    }
    
    if (starsNeedingData == 0) {
        emit progress(100, 100, "All stars already have complete data");
        emit finished(0);
        return;
    }
    
    emit progress(5, 100, QString("Building query for %1 stars...").arg(starsNeedingData));
    
    QString adqlQuery = buildADQLQuery();
    if (adqlQuery.isEmpty()) {
        emit progress(100, 100, "No valid source IDs to query");
        emit finished(0);
        return;
    }
    
    // Log query for debugging
    qDebug() << "Gaia ADQL query:" << adqlQuery.left(500) << "...";
    
    emit progress(10, 100, "Sending query to VizieR TAP...");
    
    // Prepare TAP request
    QUrl url("http://tapvizier.u-strasbg.fr/TAPVizieR/tap/sync");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("User-Agent", "ASTRA/1.0");
    
    // Build POST data
    QUrlQuery postParams;
    postParams.addQueryItem("REQUEST", "doQuery");
    postParams.addQueryItem("LANG", "ADQL");
    postParams.addQueryItem("FORMAT", "csv");
    postParams.addQueryItem("QUERY", adqlQuery);
    
    QByteArray postData = postParams.toString(QUrl::FullyEncoded).toUtf8();
    
    QNetworkReply* reply = _networkManager->post(request, postData);
    
    // Connect to download progress
    connect(reply, &QNetworkReply::downloadProgress, this, 
            [this](qint64 received, qint64 total) {
        if (total > 0) {
            int pct = 10 + (received * 40 / total);
            emit progress(pct, 100, QString("Downloading... %1 KB").arg(received / 1024));
        }
    });
    
    // Wait for response with timeout
    QEventLoop loop;
    QTimer timeoutTimer;
    timeoutTimer.setSingleShot(true);
    
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timeoutTimer, &QTimer::timeout, &loop, &QEventLoop::quit);
    
    timeoutTimer.start(300000);  // 5 minute timeout for large queries
    loop.exec();
    
    if (!timeoutTimer.isActive()) {
        reply->abort();
        emit error("Gaia query timed out after 5 minutes");
        reply->deleteLater();
        return;
    }
    
    if (reply->error() != QNetworkReply::NoError) {
        QString errorDetails = reply->errorString();
        QByteArray responseData = reply->readAll();
        if (!responseData.isEmpty()) {
            errorDetails += "\nResponse: " + QString::fromUtf8(responseData.left(500));
        }
        emit error(QString("VizieR error: %1").arg(errorDetails));
        reply->deleteLater();
        return;
    }
    
    emit progress(50, 100, "Parsing Gaia response...");
    
    QString response = QString::fromUtf8(reply->readAll());
    reply->deleteLater();
    
    qDebug() << "Gaia response size:" << response.size() << "bytes";
    
    // Check for error in response
    if (response.contains("Error") || response.contains("error")) {
        qDebug() << "Gaia response (first 1000 chars):" << response.left(1000);
    }
    
    parseVizierResponse(response);
}

void GaiaWorker::parseVizierResponse(const QString& response)
{
    QStringList lines = response.split('\n', Qt::SkipEmptyParts);
    
    if (lines.size() < 2) {
        qDebug() << "Gaia response too short:" << response;
        emit progress(100, 100, "No Gaia data found");
        emit finished(0);
        return;
    }
    
    // Parse header to get column indices
    // Expected: Source,RA_ICRS,DE_ICRS,pmRA,pmDE,e_pmRA,e_pmDE,Plx,e_Plx,Gmag,BPmag,RPmag,pmRApmDEcor,PlxpmRAcor,PlxpmDEcor
    QStringList headers = lines[0].split(',');
    QMap<QString, int> colIndex;
    for (int i = 0; i < headers.size(); ++i) {
        QString header = headers[i].trimmed().toLower();
        // Remove quotes if present
        header.remove('"');
        colIndex[header] = i;
    }
    
    qDebug() << "Gaia columns found:" << colIndex.keys();
    
    // Build lookup map by source_id
    QMap<QString, QStringList> gaiaData;
    for (int i = 1; i < lines.size(); ++i) {
        QString line = lines[i].trimmed();
        if (line.isEmpty()) continue;
        
        QStringList values = line.split(',');
        int sourceIdx = colIndex.value("source", -1);
        if (sourceIdx >= 0 && sourceIdx < values.size()) {
            QString sourceId = values[sourceIdx].trimmed();
            sourceId.remove('"');  // Remove quotes if present
            gaiaData[sourceId] = values;
        }
    }
    
    qDebug() << "Parsed" << gaiaData.size() << "Gaia records";
    
    emit progress(70, 100, "Updating star data...");
    
    int updatedCount = 0;
    
    for (auto& star : _stars) {
        QString sourceId = star->getSourceId();
        if (sourceId.isEmpty()) continue;
        
        // Clean up source ID for matching
        sourceId = sourceId.trimmed();
        if (sourceId.contains("DR3")) {
            QRegularExpression re("\\d{10,}");
            QRegularExpressionMatch match = re.match(sourceId);
            if (match.hasMatch()) {
                sourceId = match.captured(0);
            }
        }
        
        if (!gaiaData.contains(sourceId)) {
            continue;
        }
        
        const QStringList& values = gaiaData[sourceId];
        bool updated = false;
        
        auto getValue = [&](const QString& col) -> double {
            int idx = colIndex.value(col.toLower(), -1);
            if (idx >= 0 && idx < values.size()) {
                QString valStr = values[idx].trimmed();
                valStr.remove('"');
                if (valStr.isEmpty()) return 0.0;
                bool ok;
                double val = valStr.toDouble(&ok);
                return ok ? val : 0.0;
            }
            return 0.0;
        };
        
        // Update missing fields only - using VizieR column names
        if (star->getRa() == 0.0 && star->getDec() == 0.0) {
            double ra = getValue("ra_icrs");
            double dec = getValue("de_icrs");
            if (ra != 0.0 || dec != 0.0) {
                star->setRa(ra);
                star->setDec(dec);
                updated = true;
            }
        }
        
        if (star->getPmra() == 0.0) {
            double val = getValue("pmra");
            if (val != 0.0) { star->setPmra(val); updated = true; }
        }
        
        if (star->getPmdec() == 0.0) {
            double val = getValue("pmde");
            if (val != 0.0) { star->setPmdec(val); updated = true; }
        }
        
        if (star->getEPmra() == 0.0) {
            double val = getValue("e_pmra");
            if (val != 0.0) { star->setEPmra(val); updated = true; }
        }
        
        if (star->getEPmdec() == 0.0) {
            double val = getValue("e_pmde");
            if (val != 0.0) { star->setEPmdec(val); updated = true; }
        }
        
        if (star->getPlx() == 0.0) {
            double val = getValue("plx");
            if (val != 0.0) { star->setPlx(val); updated = true; }
        }
        
        if (star->getEPlx() == 0.0) {
            double val = getValue("e_plx");
            if (val != 0.0) { star->setEPlx(val); updated = true; }
        }
        
        if (star->getGmag() == 0.0) {
            double val = getValue("gmag");
            if (val != 0.0) { star->setGmag(val); updated = true; }
        }
        
        if (star->getBp() == 0.0) {
            double val = getValue("bpmag");
            if (val != 0.0) { star->setBp(val); updated = true; }
        }
        
        if (star->getRp() == 0.0) {
            double val = getValue("rpmag");
            if (val != 0.0) { star->setRp(val); updated = true; }
        }
        
        if (star->getPmraPmdecCorr() == 0.0) {
            double val = getValue("pmrapmdecor");
            if (val != 0.0) { star->setPmraPmdecCorr(val); updated = true; }
        }
        
        if (star->getPlxPmdecCorr() == 0.0) {
            double val = getValue("plxpmdecor");
            if (val != 0.0) { star->setPlxPmdecCorr(val); updated = true; }
        }
        
        if (star->getPlxPmraCorr() == 0.0) {
            double val = getValue("plxpmracor");
            if (val != 0.0) { star->setPlxPmraCorr(val); updated = true; }
        }
        
        // Calculate BP-RP if we have both and it's missing
        if (star->getBpRp() == 0.0 && star->getBp() != 0.0 && star->getRp() != 0.0) {
            star->setBpRp(star->getBp() - star->getRp());
            updated = true;
        }
        
        if (updated) updatedCount++;
    }
    
    emit progress(100, 100, QString("Updated %1 stars").arg(updatedCount));
    emit finished(updatedCount);
}

QString SimbadWorker::generateSimbadScript()
{
    QString script = "format object f1 \"start %OBJECT\\n\"+\n";
    script += "\"%BIBCODELIST\"\n";
    
    int validStars = 0;
    for (const auto& star : _stars) {
        QString sourceId = star->getSourceId();
        if (!sourceId.isEmpty()) {
            script += QString("query id GAIA DR3 %1\n").arg(sourceId);
            validStars++;
        } else if (!star->getAlias().isEmpty()) {
            script += QString("query id %1\n").arg(star->getAlias());
            validStars++;
        }
    }
    
    return validStars > 0 ? script : QString();
}

QMap<QString, QStringList> SimbadWorker::parseSimbadResponse(const QString& response)
{
    QMap<QString, QStringList> bibcodesMap;
    QStringList lines = response.split('\n');
    
    // Debug output
    //qDebug() << "SIMBAD response size:" << response.size() << "bytes";
    
    // Find data section
    int dataIndex = -1;
    int errorIndex = -1;
    
    for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].contains("::data::")) {
            dataIndex = i;
            //qDebug() << "Found data section at line" << i;
        } else if (lines[i].contains("::error::")) {
            errorIndex = i;
            //qDebug() << "Found error section at line" << i;
        }
    }
    
    if (dataIndex == -1) {
        //qDebug() << "No data section found in SIMBAD response";
        // Try to parse anyway if we have "start" lines
        for (const QString& line : lines) {
            if (line.trimmed().startsWith("start ")) {
                dataIndex = 0;  // Found at least one result
                break;
            }
        }
        
        if (dataIndex == -1) {
            emit error("Invalid SIMBAD response format - no data section found");
            return bibcodesMap;
        }
    }
    
    // Process all lines looking for star entries and bibcodes
    QString currentStar;
    QStringList currentBibcodes;
    
    for (const QString& line : lines) {
        QString trimmedLine = line.trimmed();
        
        if (trimmedLine.startsWith("start GAIA DR3 ")) {
            // Save previous star's bibcodes if any
            if (!currentStar.isEmpty() && !currentBibcodes.isEmpty()) {
                bibcodesMap[currentStar] = currentBibcodes;
                //qDebug() << "Found" << currentBibcodes.size() << "bibcodes for" << currentStar;
            }
            
            // Start new star - extract just the ID number
            currentStar = trimmedLine.mid(15).trimmed();
            // Remove any trailing colons or extra text
            if (currentStar.contains(':')) {
                currentStar = currentStar.left(currentStar.indexOf(':'));
            }
            currentBibcodes.clear();
        } else if (trimmedLine.startsWith("start ")) {
            // Handle other star identifiers
            if (!currentStar.isEmpty() && !currentBibcodes.isEmpty()) {
                bibcodesMap[currentStar] = currentBibcodes;
                //qDebug() << "Found" << currentBibcodes.size() << "bibcodes for" << currentStar;
            }
            
            currentStar = trimmedLine.mid(6).trimmed();
            if (currentStar.contains(':')) {
                currentStar = currentStar.left(currentStar.indexOf(':'));
            }
            currentBibcodes.clear();
        } else if (!trimmedLine.isEmpty() && 
                   !trimmedLine.startsWith("::") && 
                   !trimmedLine.startsWith("#") &&
                   trimmedLine.length() > 10 &&  // Bibcodes are typically 19 chars
                   !currentStar.isEmpty()) {
            // This looks like a bibcode (format: YYYYJJJJJVVVVPPPPPA)
            // Basic validation: should contain year at start and have proper format
            bool looksLikeBibcode = false;
            if (trimmedLine.length() >= 19) {
                // Check if first 4 chars could be a year
                QString yearStr = trimmedLine.left(4);
                bool yearOk;
                int year = yearStr.toInt(&yearOk);
                if (yearOk && year >= 1800 && year <= 2100) {
                    looksLikeBibcode = true;
                }
            }
            
            if (looksLikeBibcode || trimmedLine.contains("...")) {  // Some bibcodes have dots
                currentBibcodes.append(trimmedLine);
            }
        }
    }
    
    // Save last star's bibcodes if any
    if (!currentStar.isEmpty() && !currentBibcodes.isEmpty()) {
        bibcodesMap[currentStar] = currentBibcodes;
        //qDebug() << "Found" << currentBibcodes.size() << "bibcodes for" << currentStar;
    }
    
    //qDebug() << "Total stars with bibcodes:" << bibcodesMap.size();
    
    return bibcodesMap;
}


// ColumnMappingDialog implementation
ColumnMappingDialog::ColumnMappingDialog(const std::vector<QString>& unmappedColumns,
                                       const std::unordered_map<QString, QString>& currentMappings,
                                       const std::vector<QString>& availableFields,
                                       const std::vector<DataRow>& sampleData,
                                       QWidget* parent)
    : QDialog(parent)
    , _mappings(currentMappings)
    , _sampleData(sampleData)
    , _unmappedColumns(unmappedColumns)
{
    setWindowTitle("Map Columns to Star Fields");
    setModal(true);
    resize(800, 600);
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    
    QLabel* label = new QLabel("Map unmapped columns to star fields. You can see sample data below.");
    layout->addWidget(label);
    
    // Create splitter for mapping table and preview
    QSplitter* splitter = new QSplitter(Qt::Vertical);
    
    // Mapping table
    _mappingTable = new QTableWidget(unmappedColumns.size(), 3);
    _mappingTable->setHorizontalHeaderLabels({"Column Name", "Sample Values", "Maps To"});
    _mappingTable->horizontalHeader()->setStretchLastSection(true);
    
    for (size_t i = 0; i < unmappedColumns.size(); ++i) {
        QString colName = unmappedColumns[i];
        
        // Column name
        _mappingTable->setItem(i, 0, new QTableWidgetItem(colName));
        
        // Sample values
        QStringList samples;
        for (const auto& row : _sampleData) {
            if (row.values.find(colName) != row.values.end()) {
                QVariant val = row.values.at(colName);
                if (!val.isNull()) {
                    if (val.typeId() == QMetaType::Double) {
                        samples << QString::number(val.toDouble(), 'g', 4);
                    } else {
                        samples << val.toString();
                    }
                    if (samples.size() >= 3) break; // Show max 3 samples
                }
            }
        }
        _mappingTable->setItem(i, 1, new QTableWidgetItem(samples.join(", ")));
        
        // Combo box for mapping
        QComboBox* combo = new QComboBox;
        combo->addItem("<Skip>");
        for (const QString& field : availableFields) {
            combo->addItem(field);
        }
        
        // Check if already mapped
        if (_mappings.find(colName) != _mappings.end()) {
            combo->setCurrentText(_mappings.at(colName));
        }
        
        _mappingTable->setCellWidget(i, 2, combo);
    }
    
    _mappingTable->resizeColumnsToContents();
    splitter->addWidget(_mappingTable);
    
    // Preview table
    QLabel* previewLabel = new QLabel("Data Preview:");
    layout->addWidget(previewLabel);
    
    _previewTable = new QTableWidget();
    _previewTable->setAlternatingRowColors(true);
    updatePreview();
    splitter->addWidget(_previewTable);
    
    layout->addWidget(splitter);
    
    // Buttons
    QDialogButtonBox* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

void ColumnMappingDialog::updatePreview()
{
    if (_sampleData.empty()) return;
    
    // Set up preview table with sample data
    _previewTable->setRowCount(_sampleData.size());
    _previewTable->setColumnCount(_unmappedColumns.size());
    _previewTable->setHorizontalHeaderLabels(QStringList(_unmappedColumns.begin(), _unmappedColumns.end()));
    
    for (size_t row = 0; row < _sampleData.size(); ++row) {
        for (size_t col = 0; col < _unmappedColumns.size(); ++col) {
            QString colName = _unmappedColumns[col];
            QString displayText;
            
            if (_sampleData[row].values.find(colName) != _sampleData[row].values.end()) {
                QVariant value = _sampleData[row].values.at(colName);
                if (value.typeId() == QMetaType::Double) {
                    displayText = QString::number(value.toDouble(), 'g', 6);
                } else {
                    displayText = value.toString();
                }
            }
            
            _previewTable->setItem(row, col, new QTableWidgetItem(displayText));
        }
    }
    
    _previewTable->resizeColumnsToContents();
}

std::unordered_map<QString, QString> ColumnMappingDialog::getMappings() const
{
    auto mappings = _mappings;
    
    for (int i = 0; i < _mappingTable->rowCount(); ++i) {
        QString column = _mappingTable->item(i, 0)->text();
        QComboBox* combo = qobject_cast<QComboBox*>(_mappingTable->cellWidget(i, 2));
        if (combo && combo->currentText() != "<Skip>") {
            mappings[column] = combo->currentText();
        } else {
            // Remove mapping if set to skip
            mappings.erase(column);
        }
    }
    
    return mappings;
}

// Skeleton implementations remain the same
SpectraImportPage::SpectraImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import Spectra");
    setSubTitle("Associate spectral data files with imported stars");
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel("Spectra import functionality to be implemented.\n\n"
                               "This page will allow you to:\n"
                               "• Browse for spectrum files (FITS, ASCII)\n"
                               "• Match spectra to stars by identifier or position\n"
                               "• Preview spectral data before import");
    label->setWordWrap(true);
    layout->addWidget(label);
    layout->addStretch();
}

int SpectraImportPage::nextId() const
{
    return StarImportWizard::Page_RadialVelocity;
}

RadialVelocityImportPage::RadialVelocityImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import Radial Velocity Data");
    setSubTitle("Import RV measurements and time series");
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel("Radial velocity import functionality to be implemented.\n\n"
                               "This page will allow you to:\n"
                               "• Import RV measurements from tables\n"
                               "• Import RV time series/curves\n"
                               "• Associate RV data with imported stars");
    label->setWordWrap(true);
    layout->addWidget(label);
    layout->addStretch();
}

int RadialVelocityImportPage::nextId() const
{
    return StarImportWizard::Page_Photometry;
}

PhotometryImportPage::PhotometryImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import Photometry");
    setSubTitle("Import lightcurves and photometric measurements");
    setFinalPage(true);  // This makes the Next button say "Finish"
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel("Photometry import functionality to be implemented.\n\n"
                               "This page will allow you to:\n"
                               "• Import photometric measurements\n"
                               "• Import lightcurve data (TESS, Kepler, etc.)\n"
                               "• Associate photometry with imported stars");
    label->setWordWrap(true);
    layout->addWidget(label);
    layout->addStretch();
}

int PhotometryImportPage::nextId() const
{
    return -1;  // This is the last page
}