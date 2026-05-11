#ifndef STARFILTERWIDGET_H
#define STARFILTERWIDGET_H

#include <QWidget>
#include <QFrame>
#include <QVector>
#include <QVariant>
#include <QString>
#include <QSortFilterProxyModel>
#include <functional>
#include <memory>
#include <QDate>
#include <vector>
#include "utils/ObservabilityCalculator.h"

QT_BEGIN_NAMESPACE
class QLineEdit;
class QComboBox;
class QHBoxLayout;
class QVBoxLayout;
class QPushButton;
class QLabel;
class QToolButton;
class QDateEdit;
class QDoubleSpinBox;
class QGroupBox;
QT_END_NAMESPACE

class Instrument;
class Star;
class StarTableModel;

// =============================================================================
// Filter Condition
// =============================================================================

struct FilterCondition
{
    enum Operator {
        // Text operators
        Contains, NotContains, Equals, NotEquals, StartsWith, EndsWith, MatchesRegex,
        // Numeric operators
        GreaterThan, GreaterEqual, LessThan, LessEqual, Between,
        // Universal
        IsEmpty, IsNotEmpty,
        // Boolean operators
        IsTrue, IsFalse
    };

    QString columnName;
    Operator op = Contains;
    QVariant value1;
    QVariant value2;  // Only used for Between
    bool enabled = true;

    bool isNumericOperator() const {
        return op == GreaterThan || op == GreaterEqual || op == LessThan
            || op == LessEqual || op == Between;
    }

    bool isBooleanOperator() const {
        return op == IsTrue || op == IsFalse;
    }

    bool evaluate(const QVariant& cellValue) const;

    static QStringList textOperatorNames();
    static QStringList numericOperatorNames();
    static QStringList booleanOperatorNames();
    static QStringList universalOperatorNames();
    static Operator operatorFromName(const QString& name);
    static QString operatorToName(Operator op);
};

struct ObservabilityFilterSpec
{
    bool   enabled         = false;
    std::shared_ptr<Instrument> instrument;
    QDate  date;
    double minAltitudeDeg  = 30.0;
    double sunAltitudeDeg  = -18.0;
    double thresholdHours  = 4.0;
    bool   above           = true;   // true: hours >= threshold, false: hours < threshold
};

// =============================================================================
// StarFilterProxyModel
// =============================================================================

class StarFilterProxyModel : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit StarFilterProxyModel(QObject* parent = nullptr);

    // Quick search
    void setQuickSearchText(const QString& text);
    void setQuickSearchColumns(const QStringList& columns);

    // Advanced filters
    void setFilterConditions(const QVector<FilterCondition>& conditions);
    void addFilterCondition(const FilterCondition& condition);
    void removeFilterCondition(int index);
    void clearFilterConditions();
    QVector<FilterCondition> getFilterConditions() const;
    int activeFilterCount() const;

    // Logical combination
    enum LogicMode { And, Or };
    void setLogicMode(LogicMode mode);
    LogicMode logicMode() const { return _logicMode; }
    void setObservabilityFilter(const ObservabilityFilterSpec& spec);
    const ObservabilityFilterSpec& observabilityFilter() const { return _obsFilter; }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const override;
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const override;

private:
    bool matchesQuickSearch(int sourceRow, const QModelIndex& sourceParent) const;
    bool matchesAdvancedFilters(int sourceRow, const QModelIndex& sourceParent) const;
    bool matchesObservability(int sourceRow, const QModelIndex& sourceParent) const;
    int columnIndexForName(const QString& columnName) const;

    void beginBatchFilter();
    void endBatchFilter();
    int _batchDepth = 0;

    QString _quickSearchText;
    QStringList _quickSearchColumns;  // Empty = search all columns
    QVector<FilterCondition> _conditions;
    LogicMode _logicMode = And;
    
    ObservabilityFilterSpec        _obsFilter;
    Observability::NightWindow     _obsNight;    
};

// =============================================================================
// FilterConditionRow - single filter row widget
// =============================================================================

class FilterConditionRow : public QFrame
{
    Q_OBJECT

public:
    explicit FilterConditionRow(const QStringList& columnNames,
                                const QStringList& numericColumns,
                                const QStringList& booleanColumns,
                                QWidget* parent = nullptr);

    FilterCondition getCondition() const;
    void setCondition(const FilterCondition& condition);

signals:
    void conditionChanged();
    void removeRequested();

private slots:
    void onColumnChanged(int index);
    void onOperatorChanged(int index);
    void onValueChanged();

private:
    void populateOperators();

    QComboBox* _columnCombo;
    QComboBox* _operatorCombo;
    QLineEdit* _valueEdit1;
    QLineEdit* _valueEdit2;
    QLabel* _andLabel;
    QToolButton* _enableButton;
    QToolButton* _removeButton;

    QStringList _booleanColumns;
    bool _isCurrentColumnBoolean = false;
    QStringList _numericColumns;
    bool _isCurrentColumnNumeric = false;
    bool _enabled = true;
};

// =============================================================================
// StarFilterWidget - the full filter panel
// =============================================================================

class StarFilterWidget : public QWidget
{
    Q_OBJECT

public:
    explicit StarFilterWidget(QWidget* parent = nullptr);

    void setColumns(const QStringList& allColumns,
        const QStringList& numericColumns,
        const QStringList& booleanColumns);
    void setInstruments(const std::vector<std::shared_ptr<Instrument>>& instruments);
    void connectToProxy(StarFilterProxyModel* proxy);
    int activeFilterCount() const;
    QWidget* advancedPanelWidget() const;

signals:
    void filtersChanged();

public slots:
    void clearAllFilters();

private slots:
    void onObservabilityChanged();
    void onQuickSearchChanged(const QString& text);
    void addFilterRow();
    void removeFilterRow(FilterConditionRow* row);
    void onAnyConditionChanged();
    void onLogicModeChanged(int index);
    void updateFilterCountLabel();

private:
    void applyFilters();
    void setupUi();

    QLineEdit* _searchEdit;
    QComboBox* _logicCombo;
    QVBoxLayout* _filterRowsLayout;
    QPushButton* _addFilterButton;
    QPushButton* _clearAllButton;
    QLabel* _filterCountLabel;
    QWidget* _advancedPanel;
    QToolButton* _toggleAdvancedButton;

    QVector<FilterConditionRow*> _filterRows;
    StarFilterProxyModel* _proxy = nullptr;

    QStringList _allColumns;
    QStringList _numericColumns;
    QStringList _booleanColumns;

    QToolButton*    _obsToggleButton  = nullptr;
    QWidget*        _obsBody          = nullptr;
    QToolButton*    _obsEnableButton  = nullptr;
    QComboBox*      _obsInstrumentCombo = nullptr;
    QDateEdit*      _obsDateEdit      = nullptr;
    QComboBox*      _obsCompCombo     = nullptr;
    QDoubleSpinBox* _obsThresholdSpin = nullptr;
    QDoubleSpinBox* _obsAltSpin       = nullptr;
    QDoubleSpinBox* _obsSunAltSpin    = nullptr;
    std::vector<std::shared_ptr<Instrument>> _obsInstruments;
};

#endif // STARFILTERWIDGET_H