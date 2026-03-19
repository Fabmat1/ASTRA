#ifndef COLUMNCONFIGDIALOG_H
#define COLUMNCONFIGDIALOG_H

#include <QDialog>
#include <QListWidget>
#include <QComboBox>
#include <QPushButton>
#include <QTreeWidget>
#include <vector>

class ColumnConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ColumnConfigDialog(const std::vector<QString>& currentColumns,
                                 QWidget* parent = nullptr);

    std::vector<QString> selectedColumns() const;

signals:
    void columnsChanged(const std::vector<QString>& columns);

private slots:
    void onPresetSelected(int index);
    void onAddColumns();
    void onRemoveColumns();
    void onMoveUp();
    void onMoveDown();
    void onSaveAsPreset();
    void onDeletePreset();
    void onAddAll();
    void onRemoveAll();

private:
    void setupUi();
    void populateAvailableTree();
    void loadColumnsIntoSelected(const std::vector<QString>& keys);
    void syncPresetCombo();
    int  currentPresetUserDataIndex() const;

    QComboBox*    _presetCombo;
    QPushButton*  _savePresetBtn;
    QPushButton*  _deletePresetBtn;

    QTreeWidget*  _availableTree;
    QListWidget*  _selectedList;

    QPushButton*  _addBtn;
    QPushButton*  _removeBtn;
    QPushButton*  _addAllBtn;
    QPushButton*  _removeAllBtn;
    QPushButton*  _upBtn;
    QPushButton*  _downBtn;

    QPushButton*  _okBtn;
    QPushButton*  _cancelBtn;
    QPushButton*  _applyBtn;
};

#endif // COLUMNCONFIGDIALOG_H