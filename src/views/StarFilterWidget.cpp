#include "StarFilterWidget.h"
#include "ProjectView.h"
#include "models/Star.h"
#include "models/ColumnPreset.h"
#include "models/Instrument.h"
#include "utils/ObservabilityCalculator.h"

#include <QDateEdit>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QGridLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QFrame>
#include <QRegularExpression>
#include <QTimer>
#include <QAbstractTableModel>
#include <cmath>

// =============================================================================
// FilterCondition
// =============================================================================

bool FilterCondition::evaluate(const QVariant& cellValue) const
{
    if (!enabled) return true;  // Disabled conditions always pass

    // Universal operators
    if (op == IsEmpty) {
        return cellValue.isNull() || cellValue.toString().trimmed().isEmpty()
            || (cellValue.canConvert<double>() && cellValue.toDouble() == 0.0);
    }
    if (op == IsNotEmpty) {
        return !cellValue.isNull() && !cellValue.toString().trimmed().isEmpty()
            && !(cellValue.canConvert<double>() && cellValue.toDouble() == 0.0);
    }
    if (op == IsTrue) {
        if (cellValue.typeId() == QMetaType::Bool) return cellValue.toBool();
        if (cellValue.canConvert<int>()) return cellValue.toInt() != 0;
        return cellValue.toString().toLower() == "true" || cellValue.toString() == "1";
    }
    if (op == IsFalse) {
        if (cellValue.typeId() == QMetaType::Bool) return !cellValue.toBool();
        if (cellValue.canConvert<int>()) return cellValue.toInt() == 0;
        QString s = cellValue.toString().toLower();
        return s.isEmpty() || s == "false" || s == "0";
    }

    QString cellStr = cellValue.toString();

    // Numeric operators
    if (isNumericOperator()) {
        bool cellOk = false;
        double cellNum = cellValue.toDouble(&cellOk);
        if (!cellOk) {
            cellNum = cellStr.toDouble(&cellOk);
        }
        if (!cellOk) return false;

        bool valOk = false;
        double threshold = value1.toDouble(&valOk);
        if (!valOk) return false;

        switch (op) {
            case GreaterThan:  return cellNum > threshold;
            case GreaterEqual: return cellNum >= threshold;
            case LessThan:     return cellNum < threshold;
            case LessEqual:    return cellNum <= threshold;
            case Between: {
                bool val2Ok = false;
                double upper = value2.toDouble(&val2Ok);
                if (!val2Ok) return false;
                double lo = qMin(threshold, upper);
                double hi = qMax(threshold, upper);
                return cellNum >= lo && cellNum <= hi;
            }
            default: break;
        }
        return false;
    }

    // Text operators
    switch (op) {
        case Contains:
            return cellStr.contains(value1.toString(), Qt::CaseInsensitive);
        case NotContains:
            return !cellStr.contains(value1.toString(), Qt::CaseInsensitive);
        case Equals:
            return cellStr.compare(value1.toString(), Qt::CaseInsensitive) == 0;
        case NotEquals:
            return cellStr.compare(value1.toString(), Qt::CaseInsensitive) != 0;
        case StartsWith:
            return cellStr.startsWith(value1.toString(), Qt::CaseInsensitive);
        case EndsWith:
            return cellStr.endsWith(value1.toString(), Qt::CaseInsensitive);
        case MatchesRegex: {
            QRegularExpression re(value1.toString(),
                QRegularExpression::CaseInsensitiveOption);
            return re.isValid() && re.match(cellStr).hasMatch();
        }
        default: break;
    }
    return true;
}

QStringList FilterCondition::textOperatorNames()
{
    return {"contains", "not contains", "equals", "not equals",
            "starts with", "ends with", "matches regex"};
}

QStringList FilterCondition::numericOperatorNames()
{
    return {">", ">=", "<", "<=", "between"};
}

QStringList FilterCondition::booleanOperatorNames()
{
    return {"is true", "is false"};
}

QStringList FilterCondition::universalOperatorNames()
{
    return {"is empty", "is not empty"};
}

FilterCondition::Operator FilterCondition::operatorFromName(const QString& name)
{
    if (name == "contains")        return Contains;
    if (name == "not contains")    return NotContains;
    if (name == "equals")          return Equals;
    if (name == "not equals")      return NotEquals;
    if (name == "starts with")     return StartsWith;
    if (name == "ends with")       return EndsWith;
    if (name == "matches regex")   return MatchesRegex;
    if (name == ">")               return GreaterThan;
    if (name == ">=")              return GreaterEqual;
    if (name == "<")               return LessThan;
    if (name == "<=")              return LessEqual;
    if (name == "between")         return Between;
    if (name == "is true")         return IsTrue;
    if (name == "is false")        return IsFalse;
    if (name == "is empty")        return IsEmpty;
    if (name == "is not empty")    return IsNotEmpty;
    return Contains;
}

QString FilterCondition::operatorToName(Operator op)
{
    switch (op) {
        case Contains:      return "contains";
        case NotContains:   return "not contains";
        case Equals:        return "equals";
        case NotEquals:     return "not equals";
        case StartsWith:    return "starts with";
        case EndsWith:      return "ends with";
        case MatchesRegex:  return "matches regex";
        case GreaterThan:   return ">";
        case GreaterEqual:  return ">=";
        case LessThan:      return "<";
        case LessEqual:     return "<=";
        case Between:       return "between";
        case IsEmpty:       return "is empty";
        case IsNotEmpty:    return "is not empty";
    }
    return "contains";
}

// =============================================================================
// StarFilterProxyModel
// =============================================================================

StarFilterProxyModel::StarFilterProxyModel(QObject* parent)
    : QSortFilterProxyModel(parent)
{
}

void StarFilterProxyModel::setQuickSearchText(const QString& text)
{
    _quickSearchText = text.trimmed();
    beginBatchFilter();
endBatchFilter();
}

void StarFilterProxyModel::setQuickSearchColumns(const QStringList& columns)
{
    _quickSearchColumns = columns;
    beginBatchFilter();
endBatchFilter();
}

void StarFilterProxyModel::setFilterConditions(const QVector<FilterCondition>& conditions)
{
    _conditions = conditions;
    beginBatchFilter();
endBatchFilter();
}

void StarFilterProxyModel::addFilterCondition(const FilterCondition& condition)
{
    _conditions.append(condition);
    beginBatchFilter();
endBatchFilter();
}

void StarFilterProxyModel::removeFilterCondition(int index)
{
    if (index >= 0 && index < _conditions.size()) {
        _conditions.remove(index);
        beginBatchFilter();
endBatchFilter();
    }
}

void StarFilterProxyModel::clearFilterConditions()
{
    _conditions.clear();
    beginBatchFilter();
endBatchFilter();
}

QVector<FilterCondition> StarFilterProxyModel::getFilterConditions() const
{
    return _conditions;
}

int StarFilterProxyModel::activeFilterCount() const
{
    int count = 0;
    for (const auto& c : _conditions) if (c.enabled) ++count;
    if (!_quickSearchText.isEmpty())             ++count;
    if (_obsFilter.enabled && _obsFilter.instrument && _obsNight.valid) ++count;
    return count;
}

void StarFilterProxyModel::setLogicMode(LogicMode mode)
{
    _logicMode = mode;
    beginBatchFilter();
endBatchFilter();
}

bool StarFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    if (!_quickSearchText.isEmpty() && !matchesQuickSearch(sourceRow, sourceParent))
        return false;
    if (!_conditions.isEmpty() && !matchesAdvancedFilters(sourceRow, sourceParent))
        return false;
    if (_obsFilter.enabled && !matchesObservability(sourceRow, sourceParent))
        return false;
    return true;
}


bool StarFilterProxyModel::lessThan(const QModelIndex& left, const QModelIndex& right) const
{
    QVariant leftData = sourceModel()->data(left);
    QVariant rightData = sourceModel()->data(right);

    // Try numeric comparison first
    bool leftOk, rightOk;
    double leftNum = leftData.toDouble(&leftOk);
    double rightNum = rightData.toDouble(&rightOk);

    if (leftOk && rightOk) {
        return leftNum < rightNum;
    }

    // Fall back to string comparison
    return QString::localeAwareCompare(leftData.toString(), rightData.toString()) < 0;
}

bool StarFilterProxyModel::matchesQuickSearch(int sourceRow, const QModelIndex& sourceParent) const
{
    if (_quickSearchText.isEmpty()) return true;

    int colCount = sourceModel()->columnCount(sourceParent);

    // Search through specified columns, or all columns if none specified
    for (int col = 0; col < colCount; ++col) {
        if (!_quickSearchColumns.isEmpty()) {
            // Check if this column is in the search list
            QVariant header = sourceModel()->headerData(col, Qt::Horizontal, Qt::DisplayRole);
            if (!_quickSearchColumns.contains(header.toString())) {
                continue;
            }
        }

        QModelIndex idx = sourceModel()->index(sourceRow, col, sourceParent);
        QString cellText = sourceModel()->data(idx, Qt::DisplayRole).toString();
        if (cellText.contains(_quickSearchText, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

bool StarFilterProxyModel::matchesAdvancedFilters(int sourceRow, const QModelIndex& sourceParent) const
{
    bool hasAnyEnabled = false;

    for (const auto& condition : _conditions) {
        if (!condition.enabled) continue;
        hasAnyEnabled = true;

        int colIdx = columnIndexForName(condition.columnName);
        if (colIdx < 0) {
            // Column not found — treat as not matching in AND, matching in OR
            if (_logicMode == And) return false;
            continue;
        }

        QModelIndex idx = sourceModel()->index(sourceRow, colIdx, sourceParent);
        QVariant cellValue = sourceModel()->data(idx, Qt::DisplayRole);
        bool matches = condition.evaluate(cellValue);

        if (_logicMode == And && !matches) return false;
        if (_logicMode == Or && matches) return true;
    }

    if (!hasAnyEnabled) return true;

    // AND: all passed; OR: none passed
    return (_logicMode == And);
}

int StarFilterProxyModel::columnIndexForName(const QString& columnName) const
{
    if (!sourceModel()) return -1;
    for (int i = 0; i < sourceModel()->columnCount(); ++i) {
        if (sourceModel()->headerData(i, Qt::Horizontal, Qt::DisplayRole).toString() == columnName) {
            return i;
        }
    }
    return -1;
}

// =============================================================================
// FilterConditionRow
// =============================================================================

FilterConditionRow::FilterConditionRow(const QStringList& columnNames,
                                       const QStringList& numericColumns,
                                       const QStringList& booleanColumns,
                                       QWidget* parent)
    : QFrame(parent)
    , _numericColumns(numericColumns)
    , _booleanColumns(booleanColumns)
{
    setFrameShape(QFrame::StyledPanel);
    setObjectName("filterConditionRow");

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(6);

    // Enable/disable toggle
    _enableButton = new QToolButton(this);
    _enableButton->setCheckable(true);
    _enableButton->setChecked(true);
    _enableButton->setText("✓");
    _enableButton->setFixedSize(24, 24);
    _enableButton->setToolTip("Enable/disable this filter");
    connect(_enableButton, &QToolButton::toggled, this, [this](bool checked) {
        _enabled = checked;
        _enableButton->setText(checked ? "✓" : "○");
        _columnCombo->setEnabled(checked);
        _operatorCombo->setEnabled(checked);
        _valueEdit1->setEnabled(checked);
        _valueEdit2->setEnabled(checked);

        emit conditionChanged();
    });
    layout->addWidget(_enableButton);

    // Column selector
    _columnCombo = new QComboBox(this);
    _columnCombo->addItems(columnNames);
    _columnCombo->setMinimumWidth(100);
    _columnCombo->setToolTip("Column to filter");
    connect(_columnCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FilterConditionRow::onColumnChanged);
    layout->addWidget(_columnCombo);

    // Operator selector
    _operatorCombo = new QComboBox(this);
    _operatorCombo->setMinimumWidth(90);
    _operatorCombo->setToolTip("Filter operator");
    connect(_operatorCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &FilterConditionRow::onOperatorChanged);
    layout->addWidget(_operatorCombo);

    // Value inputs
    _valueEdit1 = new QLineEdit(this);
    _valueEdit1->setPlaceholderText("Value...");
    _valueEdit1->setMinimumWidth(80);
    connect(_valueEdit1, &QLineEdit::textChanged, this, &FilterConditionRow::onValueChanged);
    layout->addWidget(_valueEdit1, 1);

    _andLabel = new QLabel("and", this);
    _andLabel->setVisible(false);
    layout->addWidget(_andLabel);

    _valueEdit2 = new QLineEdit(this);
    _valueEdit2->setPlaceholderText("Value...");
    _valueEdit2->setMinimumWidth(80);
    _valueEdit2->setVisible(false);
    connect(_valueEdit2, &QLineEdit::textChanged, this, &FilterConditionRow::onValueChanged);
    layout->addWidget(_valueEdit2, 1);

    // Remove button
    _removeButton = new QToolButton(this);
    _removeButton->setText("✕");
    _removeButton->setFixedSize(24, 24);
    _removeButton->setToolTip("Remove this filter");
    connect(_removeButton, &QToolButton::clicked, this, &FilterConditionRow::removeRequested);
    layout->addWidget(_removeButton);

    // Initialize operators for the first column
    populateOperators();
}

void FilterConditionRow::onColumnChanged(int index)
{
    Q_UNUSED(index);
    populateOperators();
    emit conditionChanged();
}

void FilterConditionRow::populateOperators()
{
    QString col = _columnCombo->currentText();
    _isCurrentColumnNumeric = _numericColumns.contains(col);
    _isCurrentColumnBoolean = _booleanColumns.contains(col);

    _operatorCombo->blockSignals(true);
    _operatorCombo->clear();

    if (_isCurrentColumnBoolean) {
        _operatorCombo->addItems(FilterCondition::booleanOperatorNames());
    } else if (_isCurrentColumnNumeric) {
        _operatorCombo->addItems(FilterCondition::numericOperatorNames());
    } else {
        _operatorCombo->addItems(FilterCondition::textOperatorNames());
    }

    // Always add universal operators at the end
    _operatorCombo->insertSeparator(_operatorCombo->count());
    _operatorCombo->addItems(FilterCondition::universalOperatorNames());

    _operatorCombo->blockSignals(false);
    onOperatorChanged(_operatorCombo->currentIndex());
}

void FilterConditionRow::onOperatorChanged(int index)
{
    Q_UNUSED(index);
    QString opName = _operatorCombo->currentText();
    FilterCondition::Operator op = FilterCondition::operatorFromName(opName);

    bool showValue = (op != FilterCondition::IsEmpty && op != FilterCondition::IsNotEmpty
                      && op != FilterCondition::IsTrue && op != FilterCondition::IsFalse);
    bool showSecondValue = (op == FilterCondition::Between);

    _valueEdit1->setVisible(showValue);
    _andLabel->setVisible(showSecondValue);
    _valueEdit2->setVisible(showSecondValue);

    if (_isCurrentColumnNumeric && showValue) {
        _valueEdit1->setPlaceholderText("Number...");
        _valueEdit2->setPlaceholderText("Number...");
    } else if (showValue) {
        _valueEdit1->setPlaceholderText("Value...");
    }

    emit conditionChanged();
}

void FilterConditionRow::onValueChanged()
{
    emit conditionChanged();
}

FilterCondition FilterConditionRow::getCondition() const
{
    FilterCondition cond;
    cond.columnName = _columnCombo->currentText();
    cond.op = FilterCondition::operatorFromName(_operatorCombo->currentText());
    cond.value1 = _valueEdit1->text();
    cond.value2 = _valueEdit2->text();
    cond.enabled = _enabled;
    return cond;
}

void FilterConditionRow::setCondition(const FilterCondition& condition)
{
    _columnCombo->setCurrentText(condition.columnName);
    populateOperators();
    _operatorCombo->setCurrentText(FilterCondition::operatorToName(condition.op));
    _valueEdit1->setText(condition.value1.toString());
    _valueEdit2->setText(condition.value2.toString());
    _enabled = condition.enabled;
    _enableButton->setChecked(_enabled);
}

QWidget* StarFilterWidget::advancedPanelWidget() const
{
    return _advancedPanel;
}

// =============================================================================
// StarFilterWidget
// =============================================================================

StarFilterWidget::StarFilterWidget(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
}

void StarFilterWidget::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(4);

    // --- Top bar: search + toggle + filter count ---
    auto* topBar = new QHBoxLayout();
    topBar->setSpacing(8);

    _searchEdit = new QLineEdit(this);
    _searchEdit->setPlaceholderText("Search stars...");
    _searchEdit->setClearButtonEnabled(true);
    _searchEdit->setMinimumWidth(200);
    topBar->addWidget(_searchEdit, 1);

    _filterCountLabel = new QLabel(this);
    _filterCountLabel->setProperty("subtext", true);
    topBar->addWidget(_filterCountLabel);

    _toggleAdvancedButton = new QToolButton(this);
    _toggleAdvancedButton->setText("Filters ▼");
    _toggleAdvancedButton->setCheckable(true);
    _toggleAdvancedButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    _toggleAdvancedButton->setToolTip("Show/hide advanced filters");
    topBar->addWidget(_toggleAdvancedButton);

    mainLayout->addLayout(topBar);

    // --- Advanced filter panel (created here but NOT added to this layout) ---
    // ProjectView will retrieve it via advancedPanelWidget() and place it
    // below the full-width title bar.
    _advancedPanel = new QWidget(nullptr);
    _advancedPanel->setVisible(false);
    auto* advLayout = new QVBoxLayout(_advancedPanel);
    advLayout->setContentsMargins(10, 4, 10, 0);
    advLayout->setSpacing(4);

    // Logic mode + buttons row
    auto* controlBar = new QHBoxLayout();
    controlBar->setSpacing(6);

    QLabel* matchLabel = new QLabel("Match:", _advancedPanel);
    controlBar->addWidget(matchLabel);

    _logicCombo = new QComboBox(_advancedPanel);
    _logicCombo->addItems({"All conditions (AND)", "Any condition (OR)"});
    _logicCombo->setFixedWidth(180);
    connect(_logicCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &StarFilterWidget::onLogicModeChanged);
    controlBar->addWidget(_logicCombo);

    controlBar->addStretch();

    _addFilterButton = new QPushButton("+ Add Filter", _advancedPanel);
    connect(_addFilterButton, &QPushButton::clicked, this, &StarFilterWidget::addFilterRow);
    controlBar->addWidget(_addFilterButton);

    _clearAllButton = new QPushButton("Clear All", _advancedPanel);
    connect(_clearAllButton, &QPushButton::clicked, this, &StarFilterWidget::clearAllFilters);
    controlBar->addWidget(_clearAllButton);

    advLayout->addLayout(controlBar);

    // Filter rows container
    _filterRowsLayout = new QVBoxLayout();
    _filterRowsLayout->setSpacing(2);
    advLayout->addLayout(_filterRowsLayout);

    // ── Observability filter (collapsible, off by default) ─────────────────
    _obsToggleButton = new QToolButton(_advancedPanel);
    _obsToggleButton->setText("▸ Observability filter");
    _obsToggleButton->setCheckable(true);
    _obsToggleButton->setChecked(false);
    _obsToggleButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    _obsToggleButton->setAutoRaise(true);
    _obsToggleButton->setStyleSheet("QToolButton { font-weight: normal; }");
    advLayout->addWidget(_obsToggleButton);

    _obsBody = new QWidget(_advancedPanel);
    _obsBody->setVisible(false);
    auto* obsLayout = new QGridLayout(_obsBody);
    obsLayout->setContentsMargins(18, 2, 8, 6);
    obsLayout->setHorizontalSpacing(8);
    obsLayout->setVerticalSpacing(4);

    _obsEnableButton = new QToolButton(_obsBody);
    _obsEnableButton->setText("Apply");
    _obsEnableButton->setCheckable(true);
    _obsEnableButton->setChecked(false);
    _obsEnableButton->setToolTip("Enable this filter");
    obsLayout->addWidget(_obsEnableButton, 0, 0);

    obsLayout->addWidget(new QLabel("Observatory:", _obsBody), 0, 1);
    _obsInstrumentCombo = new QComboBox(_obsBody);
    _obsInstrumentCombo->setMinimumWidth(180);
    obsLayout->addWidget(_obsInstrumentCombo, 0, 2);

    obsLayout->addWidget(new QLabel("Date (UTC):", _obsBody), 0, 3);
    _obsDateEdit = new QDateEdit(QDate::currentDate(), _obsBody);
    _obsDateEdit->setCalendarPopup(true);
    _obsDateEdit->setDisplayFormat("yyyy-MM-dd");
    obsLayout->addWidget(_obsDateEdit, 0, 4);

    obsLayout->addWidget(new QLabel("Hours:", _obsBody), 1, 1);
    _obsCompCombo = new QComboBox(_obsBody);
    _obsCompCombo->addItems({"≥", "<"});
    obsLayout->addWidget(_obsCompCombo, 1, 2);

    _obsThresholdSpin = new QDoubleSpinBox(_obsBody);
    _obsThresholdSpin->setRange(0.0, 24.0);
    _obsThresholdSpin->setSingleStep(0.5);
    _obsThresholdSpin->setDecimals(2);
    _obsThresholdSpin->setSuffix(" h");
    _obsThresholdSpin->setValue(4.0);
    obsLayout->addWidget(_obsThresholdSpin, 1, 3);

    obsLayout->addWidget(new QLabel("Min alt.:", _obsBody), 1, 4);
    _obsAltSpin = new QDoubleSpinBox(_obsBody);
    _obsAltSpin->setRange(0.0, 90.0);
    _obsAltSpin->setSingleStep(1.0);
    _obsAltSpin->setDecimals(1);
    _obsAltSpin->setSuffix("°");
    _obsAltSpin->setValue(30.0);
    obsLayout->addWidget(_obsAltSpin, 1, 5);

    obsLayout->addWidget(new QLabel("Sun alt.:", _obsBody), 1, 6);
    _obsSunAltSpin = new QDoubleSpinBox(_obsBody);
    _obsSunAltSpin->setRange(-90.0, 0.0);
    _obsSunAltSpin->setSingleStep(1.0);
    _obsSunAltSpin->setDecimals(1);
    _obsSunAltSpin->setSuffix("°");
    _obsSunAltSpin->setValue(-18.0);
    _obsSunAltSpin->setToolTip("Twilight threshold: -18° astronomical, -12° nautical, -6° civil, 0° geometric horizon");
    obsLayout->addWidget(_obsSunAltSpin, 1, 7);

    advLayout->addWidget(_obsBody);

    connect(_obsToggleButton, &QToolButton::toggled, this, [this](bool open) {
        _obsBody->setVisible(open);
        _obsToggleButton->setText(open ? "▾ Observability filter"
                                       : "▸ Observability filter");
    });

    connect(_obsEnableButton, &QToolButton::toggled,
            this, &StarFilterWidget::onObservabilityChanged);
    connect(_obsInstrumentCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &StarFilterWidget::onObservabilityChanged);
    connect(_obsDateEdit, &QDateEdit::dateChanged,
            this, &StarFilterWidget::onObservabilityChanged);
    connect(_obsCompCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &StarFilterWidget::onObservabilityChanged);
    connect(_obsThresholdSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &StarFilterWidget::onObservabilityChanged);
    connect(_obsAltSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &StarFilterWidget::onObservabilityChanged);
    connect(_obsSunAltSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, &StarFilterWidget::onObservabilityChanged);

    // --- Connections ---

    // Debounced search
    auto* searchTimer = new QTimer(this);
    searchTimer->setSingleShot(true);
    searchTimer->setInterval(200);
    connect(_searchEdit, &QLineEdit::textChanged, this, [searchTimer]() {
        searchTimer->start();
    });
    connect(searchTimer, &QTimer::timeout, this, [this]() {
        onQuickSearchChanged(_searchEdit->text());
    });

    connect(_toggleAdvancedButton, &QToolButton::toggled, this, [this](bool checked) {
        _advancedPanel->setVisible(checked);
        _toggleAdvancedButton->setText(checked ? "Filters ▲" : "Filters ▼");
    });

    updateFilterCountLabel();
}

void StarFilterWidget::setColumns(const QStringList& allColumns,
                                   const QStringList& numericColumns,
                                   const QStringList& booleanColumns)
{
    _allColumns = allColumns;
    _numericColumns = numericColumns;
    _booleanColumns = booleanColumns;
}

void StarFilterWidget::connectToProxy(StarFilterProxyModel* proxy)
{
    _proxy = proxy;
}

int StarFilterWidget::activeFilterCount() const
{
    return _proxy ? _proxy->activeFilterCount() : 0;
}

void StarFilterWidget::onQuickSearchChanged(const QString& text)
{
    if (_proxy) {
        _proxy->setQuickSearchText(text);
        updateFilterCountLabel();
        emit filtersChanged();
    }
}

void StarFilterWidget::addFilterRow()
{
    if (_allColumns.isEmpty()) return;

    auto* row = new FilterConditionRow(_allColumns, _numericColumns,
                                        _booleanColumns, this);
    _filterRows.append(row);
    _filterRowsLayout->addWidget(row);

    connect(row, &FilterConditionRow::conditionChanged,
            this, &StarFilterWidget::onAnyConditionChanged);
    connect(row, &FilterConditionRow::removeRequested,
            this, [this, row]() { removeFilterRow(row); });

    // Show advanced panel if it was hidden
    if (!_advancedPanel->isVisible()) {
        _advancedPanel->setVisible(true);
        _toggleAdvancedButton->setChecked(true);
    }

    onAnyConditionChanged();
}

void StarFilterWidget::removeFilterRow(FilterConditionRow* row)
{
    _filterRows.removeAll(row);
    _filterRowsLayout->removeWidget(row);
    row->deleteLater();
    onAnyConditionChanged();
}

void StarFilterWidget::onAnyConditionChanged()
{
    applyFilters();
    updateFilterCountLabel();
    emit filtersChanged();
}

void StarFilterWidget::onLogicModeChanged(int index)
{
    if (_proxy) {
        _proxy->setLogicMode(index == 0 ? StarFilterProxyModel::And
                                        : StarFilterProxyModel::Or);
        updateFilterCountLabel();
        emit filtersChanged();
    }
}

void StarFilterWidget::applyFilters()
{
    if (!_proxy) return;

    QVector<FilterCondition> conditions;
    conditions.reserve(_filterRows.size());
    for (auto* row : _filterRows) {
        conditions.append(row->getCondition());
    }
    _proxy->setFilterConditions(conditions);
}

void StarFilterWidget::clearAllFilters()
{
    _searchEdit->clear();

    while (!_filterRows.isEmpty()) {
        auto* row = _filterRows.takeLast();
        _filterRowsLayout->removeWidget(row);
        row->deleteLater();
    }

    if (_obsEnableButton) _obsEnableButton->setChecked(false);
    if (_obsToggleButton) _obsToggleButton->setChecked(false);

    if (_proxy) {
        _proxy->setQuickSearchText(QString());
        _proxy->clearFilterConditions();
        _proxy->setObservabilityFilter({});
    }
    updateFilterCountLabel();
    emit filtersChanged();
}

void StarFilterWidget::updateFilterCountLabel()
{
    int count = _proxy ? _proxy->activeFilterCount() : 0;
    int matchingRows = _proxy ? _proxy->rowCount() : 0;

    if (count > 0) {
        _filterCountLabel->setText(
            QString("%1 filter%2 active · %3 star%4 shown")
                .arg(count).arg(count != 1 ? "s" : "")
                .arg(matchingRows).arg(matchingRows != 1 ? "s" : ""));
    } else {
        int totalRows = (_proxy && _proxy->sourceModel()) ?
            _proxy->sourceModel()->rowCount() : 0;
        _filterCountLabel->setText(QString("%1 stars").arg(totalRows));
    }
}

void StarFilterProxyModel::beginBatchFilter()
{
    ++_batchDepth;
}

void StarFilterProxyModel::endBatchFilter()
{
    if (--_batchDepth <= 0) {
        _batchDepth = 0;
        QSortFilterProxyModel::beginFilterChange();
        QSortFilterProxyModel::endFilterChange();
    }
}

void StarFilterProxyModel::setObservabilityFilter(const ObservabilityFilterSpec& spec)
{
    _obsFilter = spec;

    if (spec.enabled && spec.instrument && spec.date.isValid()) {
        Observability::Config cfg;
        cfg.minAltitudeDeg = spec.minAltitudeDeg;
        cfg.sunAltitudeDeg = spec.sunAltitudeDeg;
        _obsNight = Observability::computeNight(*spec.instrument, spec.date, cfg);
    } else {
        _obsNight = {};
    }

    beginBatchFilter();
    endBatchFilter();
}

bool StarFilterProxyModel::matchesObservability(int sourceRow, const QModelIndex& sourceParent) const
{
    Q_UNUSED(sourceParent);
    if (!_obsFilter.instrument || !_obsNight.valid)
        return true;   // misconfigured filter → pass everything

    auto* model = qobject_cast<StarTableModel*>(sourceModel());
    if (!model) return true;

    auto star = model->getStarAtRow(sourceRow);
    if (!star) return true;

    Observability::Config cfg;
    cfg.minAltitudeDeg = _obsFilter.minAltitudeDeg;
    cfg.sunAltitudeDeg = _obsFilter.sunAltitudeDeg;

    const double hours = Observability::observableHours(
        star->getRa(), star->getDec(),
        *_obsFilter.instrument, _obsNight, cfg);

    return _obsFilter.above ? (hours >= _obsFilter.thresholdHours)
                            : (hours <  _obsFilter.thresholdHours);
}

void StarFilterWidget::setInstruments(const std::vector<std::shared_ptr<Instrument>>& instruments)
{
    _obsInstruments.clear();
    if (!_obsInstrumentCombo) return;

    _obsInstrumentCombo->blockSignals(true);
    _obsInstrumentCombo->clear();
    for (const auto& inst : instruments) {
        if (!inst) continue;
        if (inst->isSpaceBased() || !inst->hasLocation()) continue;
        _obsInstruments.push_back(inst);
        QString label = inst->getFullName().isEmpty() ? inst->getName()
                                                       : inst->getFullName();
        _obsInstrumentCombo->addItem(label);
    }
    _obsInstrumentCombo->blockSignals(false);

    const bool any = !_obsInstruments.empty();
    if (_obsBody) _obsBody->setEnabled(any);
    if (!any && _obsToggleButton) {
        _obsToggleButton->setToolTip("Configure at least one ground-based instrument "
                                     "with a known location to use this filter.");
    } else if (_obsToggleButton) {
        _obsToggleButton->setToolTip(QString());
    }
}

void StarFilterWidget::onObservabilityChanged()
{
    if (!_proxy) return;

    ObservabilityFilterSpec spec;
    spec.enabled = _obsEnableButton && _obsEnableButton->isChecked();

    const int idx = _obsInstrumentCombo ? _obsInstrumentCombo->currentIndex() : -1;
    if (idx >= 0 && idx < static_cast<int>(_obsInstruments.size()))
        spec.instrument = _obsInstruments[idx];

    spec.date            = _obsDateEdit ? _obsDateEdit->date() : QDate::currentDate();
    spec.thresholdHours  = _obsThresholdSpin ? _obsThresholdSpin->value() : 4.0;
    spec.above           = _obsCompCombo ? (_obsCompCombo->currentIndex() == 0) : true;
    spec.minAltitudeDeg  = _obsAltSpin    ? _obsAltSpin->value()    : 30.0;
    spec.sunAltitudeDeg  = _obsSunAltSpin ? _obsSunAltSpin->value() : -18.0;

    _proxy->setObservabilityFilter(spec);
    updateFilterCountLabel();
    emit filtersChanged();
}