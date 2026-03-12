// src/utils/GeneralImportPage.h

#ifndef GENERALIMPORTPAGE_H
#define GENERALIMPORTPAGE_H

#include <QWizardPage>
#include <QDialog>
#include <QVariant>
#include <QString>
#include <QFutureWatcher>
#include <memory>
#include <vector>
#include <unordered_map>

class Star;
class QLineEdit;
class QCheckBox;
class QTableWidget;
class QComboBox;
class QLabel;
class SimbadWorker;
class GaiaWorker;
class QThread;

// Data row for storing parsed file data
struct DataRow {
    std::unordered_map<QString, QVariant> values;
};

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
    QLabel* _simbadWarningLabel;
    QThread* _simbadThread;
    SimbadWorker* _simbadWorker;
    QThread* _gaiaThread;
    GaiaWorker* _gaiaWorker;

    QString generateIdentityKey(const DataRow& row) const;
    bool areRowsCompatible(const DataRow& a, const DataRow& b) const;
    bool areNumericValuesCompatible(double a, double b, const QString& fieldName) const;
    DataRow mergeRows(const DataRow& existing, const DataRow& incoming) const;
    int numericPrecision(const QVariant& value) const;
    double toleranceForField(const QString& fieldName) const;
    QString fieldForColumn(const QString& columnName) const;
    QLabel* _deduplicationLabel;

    void queryGaiaData(std::vector<std::shared_ptr<Star>>& stars);
    void removeDuplicateRows();
    QString generateRowKey(const DataRow& row) const;
    QString normalizeValue(const QVariant& value) const;

    std::vector<QString> _columnNames;
    std::unordered_map<QString, QString> _columnMappings;
    std::vector<QString> _unmappedColumns;
    std::vector<DataRow> _dataRows;
    QFutureWatcher<bool>* _fitsWatcher;
    
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
    void updateSimbadWarning();
    void querySimbadBibcodes(const std::vector<std::shared_ptr<Star>>& stars);
    
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

private:
    QTableWidget* _mappingTable;
    QTableWidget* _previewTable;
    std::unordered_map<QString, QString> _mappings;
    std::vector<DataRow> _sampleData;
    std::vector<QString> _unmappedColumns;
    
    void updatePreview();
};

#endif // GENERALIMPORTPAGE_H