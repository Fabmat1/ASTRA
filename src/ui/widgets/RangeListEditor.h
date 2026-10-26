#pragma once

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QGridLayout;
class QLabel;
class QPushButton;

namespace astra {

/// An editable list of numeric rows (wavelength ranges, anchor ranges, ...)
/// laid out as a grid: a header row, one spin box per column, and a remove
/// button per row, so every column lines up however many rows there are.
/// An add button sits under the list.
///
/// Domain-free on purpose: rows are plain vectors of doubles in column
/// order, and callers convert to and from their own range types.
class RangeListEditor : public QWidget
{
    Q_OBJECT
public:
    struct Column {
        QString header;
        double  min      = 0.0;
        double  max      = 100000.0;
        int     decimals = 2;
        double  step     = 0.5;
        QString suffix;
    };
    using Row = QVector<double>;

    RangeListEditor(QVector<Column> columns, const QString& addLabel,
                    QWidget* parent = nullptr);

    /// Replaces the rows without emitting rowsChanged().
    void setRows(const QVector<Row>& rows);
    /// The rows, with any half-typed value committed first.
    QVector<Row> rows() const;

    /// Produces the row the add button appends, given the current rows.
    /// The default appends the last row shifted past its own end.
    void setNewRowFactory(std::function<Row(const QVector<Row>&)> f);

    /// Shown in place of the list while it is empty.
    void setEmptyText(const QString& text);

    /// Extra buttons for the row under the list (next to the add button).
    void addButton(QPushButton* button);

signals:
    /// A value was edited or a row added or removed (not on setRows()).
    void rowsChanged();

private:
    void rebuild();

    QVector<Column> _columns;
    QVector<Row>    _rows;
    std::function<Row(const QVector<Row>&)> _newRow;

    QWidget*     _gridHost  = nullptr;
    QGridLayout* _grid      = nullptr;
    QLabel*      _emptyLbl  = nullptr;
    QWidget*     _buttonRow = nullptr;
};

}   // namespace astra
