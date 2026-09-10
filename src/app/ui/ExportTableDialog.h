#pragma once

#include <QDialog>
#include <QString>

class QRadioButton;
class QLineEdit;
class QCheckBox;
class QLabel;

/**
 * Options dialog for exporting the project's star table. The user picks which
 * rows/columns to export (the currently shown set, everything, or the current
 * selection) and the destination format (a FITS binary table, a CSV file with a
 * configurable separator, or the clipboard). The actual data gathering and
 * writing is done by ProjectView, which has access to the table model/selection.
 */
class ExportTableDialog : public QDialog
{
    Q_OBJECT
public:
    enum class Scope  { Shown, All, Selection };
    enum class Format { Fits, Csv, Clipboard };

    explicit ExportTableDialog(bool hasSelection, QWidget* parent = nullptr);

    Scope   scope() const;
    Format  format() const;
    QString separator() const;     ///< field separator for CSV / clipboard
    bool    includeHeader() const; ///< write a header row (CSV / clipboard)

private:
    void updateEnabledState();

    QRadioButton* _scopeShown     = nullptr;
    QRadioButton* _scopeAll       = nullptr;
    QRadioButton* _scopeSelection = nullptr;

    QRadioButton* _fmtFits      = nullptr;
    QRadioButton* _fmtCsv       = nullptr;
    QRadioButton* _fmtClipboard = nullptr;

    QLineEdit* _sepEdit       = nullptr;
    QCheckBox* _headerCheck   = nullptr;
    QLabel*    _sepLabel      = nullptr;
};
