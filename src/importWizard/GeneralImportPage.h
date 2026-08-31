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
class QDoubleSpinBox;
class GaiaWorker;
class QThread;

// One parsed row of the input file, stored positionally against the shared
// column list (GeneralImportPage::_columnNames).
//
// This used to be a per-row std::unordered_map<QString, QVariant>, which costs
// a node allocation and a string hash for every single cell. On a wide
// catalogue that dominates the import: 88 k rows x 128 columns spent about a
// second and roughly a gigabyte of memory building the maps, and the
// deduplication pass then copied them again. A vector indexed by column
// position has none of that per-cell overhead.
struct DataRow {
    // Indexed by column position. May be shorter than the column list when a
    // row has trailing empty cells; at() treats missing and invalid alike.
    std::vector<QVariant> values;

    // How many leading cells the reader actually wrote, whether or not they
    // parsed to anything. The old name-keyed map recorded this implicitly:
    // readCSV() inserted a key for every in-range cell, so a cell holding
    // "NaN" still counted as written, while readFITS() inserted nothing for a
    // null cell. mergeRows() keys its unmapped-column carry-over off that
    // distinction, so it has to survive the move to positional storage.
    int recordedCells = 0;

    /// True when the reader wrote something at this position, even if the text
    /// there did not parse. Only mergeRows() needs this; everywhere else a cell
    /// that parsed to nothing is treated as absent, which at() already does.
    bool wasRecorded(int column) const {
        if (column < 0) return false;
        if (column < recordedCells) return true;
        return column < static_cast<int>(values.size()) && values[column].isValid();
    }

    /// Value at a column position, or nullptr when the row has nothing there.
    const QVariant* at(int column) const {
        if (column < 0 || column >= static_cast<int>(values.size()))
            return nullptr;
        return values[column].isValid() ? &values[column] : nullptr;
    }

    void set(int column, const QVariant& value) {
        if (column < 0) return;
        if (static_cast<int>(values.size()) <= column)
            values.resize(column + 1);
        values[column] = value;
    }
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
    QDoubleSpinBox* _matchToleranceSpin;
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

    /// Refreshes _columnIndex / _mappedColumns / _fieldColumn. Must be called
    /// after anything changes _columnNames or _columnMappings.
    void rebuildColumnLookups();
    /// Position of a column by name, or -1.
    int columnIndexOf(const QString& name) const;
    QString normalizeValue(const QVariant& value) const;

    std::vector<QString> _columnNames;
    std::unordered_map<QString, QString> _columnMappings;

    // Derived from _columnNames / _columnMappings by rebuildColumnLookups().
    // The per-row loops walk these instead of hashing a column name per cell.
    struct MappedColumn { int column; QString field; };
    std::vector<MappedColumn> _mappedColumns;         // mapped columns, by position
    std::unordered_map<QString, int> _columnIndex;    // column name -> position
    std::unordered_map<QString, int> _fieldColumn;    // star field -> position
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

    void updateStarFromParsed(std::shared_ptr<Star> existing, std::shared_ptr<Star> parsed);
    
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
                       const std::vector<QString>& columnNames,
                       const std::vector<DataRow>& sampleData,
                       QWidget* parent = nullptr);
    
    std::unordered_map<QString, QString> getMappings() const;

private:
    QTableWidget* _mappingTable;
    QTableWidget* _previewTable;
    std::unordered_map<QString, QString> _mappings;
    std::vector<DataRow> _sampleData;
    std::unordered_map<QString, int> _sampleColumnIndex;  // column name -> position
    std::vector<QString> _unmappedColumns;
    
    void updatePreview();
};

#endif // GENERALIMPORTPAGE_H