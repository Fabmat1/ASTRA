// src/utils/StarImportWizard.h

#ifndef STARIMPORTWIZARD_H
#define STARIMPORTWIZARD_H

#include <QWizard>
#include <memory>
#include <vector>
#include <unordered_map>
#include <QVariant>

class ApplicationController;
class Project;
class Star;
class QLineEdit;
class QCheckBox;
class QTableWidget;
class QProgressDialog;
class QComboBox;

class StarImportWizard : public QWizard
{
    Q_OBJECT

public:
    StarImportWizard(ApplicationController* controller, 
                     std::shared_ptr<Project> project,
                     QWidget* parent = nullptr);

    enum { Page_GeneralImport, Page_ColumnMapping, Page_Spectra, Page_RadialVelocity, Page_Photometry };
    
    // Public getters for access by wizard pages
    ApplicationController* controller() const { return _controller; }
    std::shared_ptr<Project> project() const { return _project; }
    
    // Store imported stars for later steps
    void setImportedStars(const std::vector<std::shared_ptr<Star>>& stars) { _importedStars = stars; }
    std::vector<std::shared_ptr<Star>> importedStars() const { return _importedStars; }

private:
    ApplicationController* _controller;
    std::shared_ptr<Project> _project;
    std::vector<std::shared_ptr<Star>> _importedStars;
};

// Data row for storing parsed file data
struct DataRow {
    std::unordered_map<QString, QVariant> values;
};

// First wizard page - General Import
class GeneralImportPage : public QWizardPage
{
    Q_OBJECT

public:
    GeneralImportPage(QWidget* parent = nullptr);

    bool isComplete() const override;
    bool validatePage() override;
    int nextId() const override;

private slots:
    void onBrowseFile();
    void onFilePathChanged(const QString& path);
    void onDelimiterChanged();
    void onCommentCharChanged(const QString& text);

private:
    QLineEdit* _filePathEdit;
    QComboBox* _delimiterCombo;
    QLineEdit* _customDelimiterEdit;
    QLineEdit* _commentCharEdit;
    QCheckBox* _hasHeaderCheckBox;
    QCheckBox* _gaiaCheckBox;
    QCheckBox* _simbadCheckBox;
    QTableWidget* _previewTable;
    
    std::vector<QString> _columnNames;
    std::unordered_map<QString, QString> _columnMappings;
    std::vector<QString> _unmappedColumns;
    std::vector<DataRow> _dataRows;
    
    bool readFile(const QString& filePath);
    bool readCSV(const QString& filePath);
    bool readFITS(const QString& filePath);
    void setupColumnAliases();
    void mapColumns();
    void updatePreview();
    std::vector<std::shared_ptr<Star>> createStarsFromData();
    
    QChar detectDelimiter(const QString& line) const;
    QStringList parseCSVLine(const QString& line, QChar delimiter) const;
    QVariant convertValue(const QString& value) const;
    
    // Column name aliases (case-insensitive matching)
    std::unordered_map<QString, std::vector<QString>> _columnAliases;
    
    // Helper to apply mapped value to star
    void applyValueToStar(std::shared_ptr<Star> star, const QString& field, const QVariant& value);
};

// Column mapping dialog
class ColumnMappingDialog : public QDialog
{
    Q_OBJECT

public:
    ColumnMappingDialog(const std::vector<QString>& unmappedColumns,
                       const std::unordered_map<QString, QString>& currentMappings,
                       const std::vector<QString>& availableFields,
                       const std::vector<DataRow>& sampleData,
                       QWidget* parent = nullptr);
    
    std::unordered_map<QString, QString> getMappings() const;

//private slots:
//    void onMappingChanged(int row, int column);

private:
    QTableWidget* _mappingTable;
    QTableWidget* _previewTable;
    std::unordered_map<QString, QString> _mappings;
    std::vector<DataRow> _sampleData;
    std::vector<QString> _unmappedColumns;
    
    void updatePreview();
};

// Skeleton pages for other steps
class SpectraImportPage : public QWizardPage
{
    Q_OBJECT
public:
    SpectraImportPage(QWidget* parent = nullptr);
};

class RadialVelocityImportPage : public QWizardPage
{
    Q_OBJECT
public:
    RadialVelocityImportPage(QWidget* parent = nullptr);
};

class PhotometryImportPage : public QWizardPage
{
    Q_OBJECT
public:
    PhotometryImportPage(QWidget* parent = nullptr);
};

#endif // STARIMPORTWIZARD_H