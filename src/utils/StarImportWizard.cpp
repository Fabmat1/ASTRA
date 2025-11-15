// src/utils/StarImportWizard.cpp

#include "StarImportWizard.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"
#include "models/Star.h"
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
    addPage(new GeneralImportPage);
    addPage(new SpectraImportPage);
    addPage(new RadialVelocityImportPage);
    addPage(new PhotometryImportPage);
    
    resize(900, 700);
}

// GeneralImportPage implementation
GeneralImportPage::GeneralImportPage(QWidget* parent)
    : QWizardPage(parent)
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
    
    _gaiaCheckBox = new QCheckBox("Query Gaia DR3 via Vizier for missing data");
    queryLayout->addWidget(_gaiaCheckBox);
    
    _simbadCheckBox = new QCheckBox("Query SIMBAD for bibliography codes");
    queryLayout->addWidget(_simbadCheckBox);
    
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
    try {
        std::unique_ptr<CCfits::FITS> pInfile(new CCfits::FITS(filePath.toStdString(), CCfits::Read, true));
        
        // Get all extensions
        const CCfits::ExtMap& extMap = pInfile->extension();
        
        if (extMap.empty()) {
            QMessageBox::warning(this, "FITS Error", "No table extensions found in FITS file");
            return false;
        }
        
        // Use first table extension (usually extension 1)
        CCfits::ExtHDU* table = nullptr;
        for (const auto& ext : extMap) {
            if (dynamic_cast<CCfits::Table*>(ext.second) != nullptr) {
                table = dynamic_cast<CCfits::Table*>(ext.second);  
                break;
            }
        }
        
        if (!table) {
            QMessageBox::warning(this, "FITS Error", "No table found in FITS file");
            return false;
        }
        
        long nrows = table->rows();
        if (nrows == 0) {
            QMessageBox::warning(this, "FITS Error", "Table has no rows");
            return false;
        }
        
        // Get column information
        const CCfits::ColMap& columns = table->column();
        //std::map<std::string, CCfits::Column*>& columns = table->column();
        std::vector<CCfits::Column*> columnPtrs;
        
        for (const auto& col : columns) {
            _columnNames.push_back(QString::fromStdString(col.first));
            columnPtrs.push_back(col.second);
        }
        
        // Read data
        _dataRows.clear();
        
        // Read all rows (or limit for very large files)
        long maxRows = std::min(nrows, 100000L); // Limit to 100k rows for memory
        
        for (long row = 1; row <= maxRows; ++row) {
            DataRow dataRow;
            
            for (size_t colIdx = 0; colIdx < columnPtrs.size(); ++colIdx) {
                CCfits::Column* col = columnPtrs[colIdx];
                QString colName = _columnNames[colIdx];
                
                try {
                    // Handle different column types
                    if (col->type() == CCfits::Tdouble) {
                        std::vector<double> value;
                        col->read(value, row, row);
                        if (!value.empty() && !std::isnan(value[0])) {
                            dataRow.values[colName] = value[0];
                        }
                    } else if (col->type() == CCfits::Tfloat) {
                        std::vector<float> value;
                        col->read(value, row, row);
                        if (!value.empty() && !std::isnan(value[0])) {
                            dataRow.values[colName] = static_cast<double>(value[0]);
                        }
                    } else if (col->type() == CCfits::Tint || col->type() == CCfits::Tlong || col->type() == CCfits::Tlonglong) {
                        std::vector<long> value;
                        col->read(value, row, row);
                        if (!value.empty()) {
                            dataRow.values[colName] = static_cast<double>(value[0]);
                        }
                    } else if (col->type() == CCfits::Tstring) {
                        std::vector<std::string> value;
                        col->read(value, row, row);
                        if (!value.empty()) {
                            dataRow.values[colName] = QString::fromStdString(value[0]).trimmed();
                        }
                    }
                } catch (CCfits::Column::InvalidRowNumber&) {
                    // Skip invalid rows
                    continue;
                } catch (...) {
                    // Skip problematic values
                    continue;
                }
            }
            
            _dataRows.push_back(dataRow);
        }
        
        if (maxRows < nrows) {
            QMessageBox::information(this, "Large File", 
                QString("File contains %1 rows. Loaded first %2 for preview.")
                .arg(nrows).arg(maxRows));
        }
        
        return true;
        
    } catch (CCfits::FITS::CantOpen& e) {
        QMessageBox::warning(this, "FITS Error", 
            QString("Cannot open FITS file: %1").arg(QString::fromStdString(e.message())));
        return false;
    } catch (CCfits::FitsException& e) {
        QMessageBox::warning(this, "FITS Error", QString::fromStdString(e.message()));
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
    
    // Check if there are unmapped columns
    if (!_unmappedColumns.empty()) {
        QMessageBox::StandardButton reply = QMessageBox::question(this, "Unmapped Columns",
            QString("There are %1 unmapped columns. Would you like to map them manually?")
                .arg(_unmappedColumns.size()),
            QMessageBox::Yes | QMessageBox::No);
        
        if (reply == QMessageBox::Yes) {
            // Show mapping dialog with data preview
            std::vector<QString> availableFields = {
                "alias", "source_id", "tic", "jname", "ra", "dec", 
                "pmra", "pmdec", "e_pmra", "e_pmdec", "plx", "e_plx",
                "gmag", "e_gmag", "bp", "e_bp", "rp", "e_rp", "bp_rp",
                "teff", "e_teff", "logg", "e_logg", "he", "e_he",
                "rv_med", "e_rv_med", "rv_avg", "e_rv_avg", "deltaRV", 
                "e_deltaRV", "logp", "spec_class",
                "pmra_pmdec_corr", "plx_pmdec_corr", "plx_pmra_corr"
            };
            
            // Get sample data for preview (first 5 rows)
            std::vector<DataRow> sampleData;
            for (size_t i = 0; i < std::min(size_t(5), _dataRows.size()); ++i) {
                sampleData.push_back(_dataRows[i]);
            }
            
            ColumnMappingDialog dialog(_unmappedColumns, _columnMappings, availableFields, sampleData, this);
            if (dialog.exec() == QDialog::Accepted) {
                _columnMappings = dialog.getMappings();
                updatePreview(); // Refresh preview with new mappings
            }
        }
    }
    
    // Create stars from data
    std::vector<std::shared_ptr<Star>> stars = createStarsFromData();
    
    if (stars.empty()) {
        QMessageBox::warning(this, "Error", "No stars could be created from the data.");
        return false;
    }
    
    // Import the stars
    QProgressDialog progress("Importing stars...", "Cancel", 0, stars.size(), this);
    progress.setWindowModality(Qt::WindowModal);
    
    // Access controller and project
    StarImportWizard* importWizard = qobject_cast<StarImportWizard*>(wizard());
    if (!importWizard) {
        QMessageBox::critical(this, "Error", "Internal error: Could not access wizard");
        return false;
    }
    
    auto controller = importWizard->controller();
    auto project = importWizard->project();
    
    // Store imported stars in the wizard for use in later steps
    importWizard->setImportedStars(stars);
    
    // Save all stars at once
    progress.setLabelText("Saving stars to database...");
    if (!controller->saveStarsToProject(project, stars)) {
        QMessageBox::critical(this, "Error", "Failed to save stars to database");
        return false;
    }
    
    // Query Gaia DR3 if requested
    if (_gaiaCheckBox->isChecked()) {
        progress.setLabelText("Querying Gaia DR3...");
        // TODO: Implementation for Gaia query
    }
    
    // Query SIMBAD if requested
    if (_simbadCheckBox->isChecked()) {
        progress.setLabelText("Querying SIMBAD for bibcodes...");
        // TODO: Implementation for SIMBAD query
    }
    
    progress.setValue(stars.size());
    
    QMessageBox::information(this, "Import Complete", 
        QString("Successfully imported %1 stars.").arg(stars.size()));
    
    return true;
}

int GeneralImportPage::nextId() const
{
    // Skip to photometry page for now (until other pages are implemented)
    return -1; // Return -1 to finish the wizard after this page
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
    QLabel* label = new QLabel("Spectra import functionality to be implemented");
    layout->addWidget(label);
}

RadialVelocityImportPage::RadialVelocityImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import Radial Velocity Data");
    setSubTitle("Import RV measurements and curves");
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel("Radial velocity import functionality to be implemented");
    layout->addWidget(label);
}

PhotometryImportPage::PhotometryImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import Photometry");
    setSubTitle("Import lightcurves and photometric measurements");
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel("Photometry import functionality to be implemented");
    layout->addWidget(label);
}