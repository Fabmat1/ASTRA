#include "ColumnConfigDialog.h"
#include "models/ColumnPreset.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QHeaderView>
#include <QInputDialog>
#include <QMessageBox>
#include <QUuid>
#include <QSplitter>
#include <algorithm>

// helper role to carry the column key on every item
static constexpr int KeyRole = Qt::UserRole + 1;

ColumnConfigDialog::ColumnConfigDialog(const std::vector<QString>& currentColumns,
                                         QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Configure Columns");
    setMinimumSize(720, 520);
    resize(820, 580);
    setupUi();
    populateAvailableTree();
    loadColumnsIntoSelected(currentColumns);
    syncPresetCombo();
}

// ─────────────────────────────────────────────────────────────────────────────
void ColumnConfigDialog::setupUi()
{
    auto* root = new QVBoxLayout(this);

    // ── Preset row ──────────────────────────────────────────────────────────
    auto* presetRow = new QHBoxLayout;
    presetRow->addWidget(new QLabel("Preset:"));

    _presetCombo = new QComboBox;
    _presetCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    presetRow->addWidget(_presetCombo, 1);

    _savePresetBtn = new QPushButton("Save as…");
    _savePresetBtn->setToolTip("Save current column selection as a custom preset");
    presetRow->addWidget(_savePresetBtn);

    _deletePresetBtn = new QPushButton("Delete");
    _deletePresetBtn->setToolTip("Delete the selected custom preset");
    presetRow->addWidget(_deletePresetBtn);

    root->addLayout(presetRow);

    // ── Two-panel area ──────────────────────────────────────────────────────
    auto* splitter = new QSplitter(Qt::Horizontal);

    // Left: available columns (tree grouped by category)
    auto* leftGroup = new QGroupBox("Available Columns");
    auto* leftLay = new QVBoxLayout(leftGroup);
    _availableTree = new QTreeWidget;
    _availableTree->setHeaderHidden(true);
    _availableTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _availableTree->setRootIsDecorated(true);
    leftLay->addWidget(_availableTree);

    // Centre: add/remove buttons
    auto* centreWidget = new QWidget;
    auto* centreLay = new QVBoxLayout(centreWidget);
    centreLay->setAlignment(Qt::AlignCenter);
    _addBtn      = new QPushButton("→");
    _removeBtn   = new QPushButton("←");
    _addAllBtn   = new QPushButton("⇒");
    _removeAllBtn= new QPushButton("⇐");
    for (auto* b : {_addBtn, _removeBtn, _addAllBtn, _removeAllBtn})
        b->setFixedWidth(44);
    centreLay->addStretch();
    centreLay->addWidget(_addAllBtn);
    centreLay->addWidget(_addBtn);
    centreLay->addWidget(_removeBtn);
    centreLay->addWidget(_removeAllBtn);
    centreLay->addStretch();

    // Right: selected columns (ordered list) + up/down
    auto* rightGroup = new QGroupBox("Selected Columns (display order)");
    auto* rightLay = new QHBoxLayout(rightGroup);
    _selectedList = new QListWidget;
    _selectedList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    _selectedList->setDragDropMode(QAbstractItemView::InternalMove);
    rightLay->addWidget(_selectedList, 1);

    auto* orderLay = new QVBoxLayout;
    orderLay->setAlignment(Qt::AlignCenter);
    _upBtn   = new QPushButton("▲");
    _downBtn = new QPushButton("▼");
    _upBtn->setFixedWidth(36);
    _downBtn->setFixedWidth(36);
    orderLay->addStretch();
    orderLay->addWidget(_upBtn);
    orderLay->addWidget(_downBtn);
    orderLay->addStretch();
    rightLay->addLayout(orderLay);

    splitter->addWidget(leftGroup);
    splitter->addWidget(centreWidget);
    splitter->addWidget(rightGroup);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 0);
    splitter->setStretchFactor(2, 3);
    root->addWidget(splitter, 1);

    // ── Bottom buttons ──────────────────────────────────────────────────────
    auto* btnRow = new QHBoxLayout;
    btnRow->addStretch();
    _applyBtn  = new QPushButton("Apply");
    _okBtn     = new QPushButton("OK");
    _cancelBtn = new QPushButton("Cancel");
    _okBtn->setDefault(true);
    btnRow->addWidget(_applyBtn);
    btnRow->addWidget(_okBtn);
    btnRow->addWidget(_cancelBtn);
    root->addLayout(btnRow);

    // ── Connections ─────────────────────────────────────────────────────────
    connect(_presetCombo, QOverload<int>::of(&QComboBox::activated),
            this, &ColumnConfigDialog::onPresetSelected);
    connect(_addBtn,       &QPushButton::clicked, this, &ColumnConfigDialog::onAddColumns);
    connect(_removeBtn,    &QPushButton::clicked, this, &ColumnConfigDialog::onRemoveColumns);
    connect(_addAllBtn,    &QPushButton::clicked, this, &ColumnConfigDialog::onAddAll);
    connect(_removeAllBtn, &QPushButton::clicked, this, &ColumnConfigDialog::onRemoveAll);
    connect(_upBtn,        &QPushButton::clicked, this, &ColumnConfigDialog::onMoveUp);
    connect(_downBtn,      &QPushButton::clicked, this, &ColumnConfigDialog::onMoveDown);
    connect(_savePresetBtn,  &QPushButton::clicked, this, &ColumnConfigDialog::onSaveAsPreset);
    connect(_deletePresetBtn,&QPushButton::clicked, this, &ColumnConfigDialog::onDeletePreset);

    connect(_okBtn, &QPushButton::clicked, this, [this]() {
        emit columnsChanged(selectedColumns());
        accept();
    });
    connect(_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(_applyBtn,  &QPushButton::clicked, this, [this]() {
        emit columnsChanged(selectedColumns());
    });

    // Double-click in available → add
    connect(_availableTree, &QTreeWidget::itemDoubleClicked,
            this, [this](QTreeWidgetItem* item, int) {
        if (item && !item->data(0, KeyRole).toString().isEmpty())
            onAddColumns();
    });
    // Double-click in selected → remove
    connect(_selectedList, &QListWidget::itemDoubleClicked,
            this, [this](QListWidgetItem*) { onRemoveColumns(); });
}

// ─────────────────────────────────────────────────────────────────────────────
void ColumnConfigDialog::populateAvailableTree()
{
    _availableTree->clear();
    auto& mgr = ColumnPresetManager::instance();

    for (const auto& cat : mgr.categories()) {
        auto* catItem = new QTreeWidgetItem(_availableTree);
        catItem->setText(0, cat);
        catItem->setFlags(Qt::ItemIsEnabled);
        QFont f = catItem->font(0);
        f.setBold(true);
        catItem->setFont(0, f);

        for (const auto& col : mgr.columnsForCategory(cat)) {
            auto* child = new QTreeWidgetItem(catItem);
            child->setText(0, col.displayName);
            child->setData(0, KeyRole, col.key);
            child->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
            if (col.isBoolFlag)
                child->setToolTip(0, "Boolean flag (✓ / ✗)");
        }
        catItem->setExpanded(true);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void ColumnConfigDialog::loadColumnsIntoSelected(const std::vector<QString>& keys)
{
    _selectedList->clear();
    auto& mgr = ColumnPresetManager::instance();
    for (const auto& k : keys) {
        auto* item = new QListWidgetItem(mgr.displayName(k));
        item->setData(KeyRole, k);
        _selectedList->addItem(item);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
std::vector<QString> ColumnConfigDialog::selectedColumns() const
{
    std::vector<QString> cols;
    cols.reserve(_selectedList->count());
    for (int i = 0; i < _selectedList->count(); ++i)
        cols.push_back(_selectedList->item(i)->data(KeyRole).toString());
    return cols;
}

// ─────────────────────────────────────────────────────────────────────────────
// Preset combo
// ─────────────────────────────────────────────────────────────────────────────
void ColumnConfigDialog::syncPresetCombo()
{
    _presetCombo->blockSignals(true);
    _presetCombo->clear();

    _presetCombo->addItem("— Custom —", QString());          // index 0

    auto& mgr = ColumnPresetManager::instance();
    _presetCombo->insertSeparator(_presetCombo->count());

    for (const auto& p : mgr.builtInPresets())
        _presetCombo->addItem("⬒ " + p.name, p.id);         // built-in icon hint

    auto custom = mgr.customPresets();
    if (!custom.empty()) {
        _presetCombo->insertSeparator(_presetCombo->count());
        for (const auto& p : custom)
            _presetCombo->addItem("★ " + p.name, p.id);
    }

    _presetCombo->setCurrentIndex(0);
    _deletePresetBtn->setEnabled(false);
    _presetCombo->blockSignals(false);
}

void ColumnConfigDialog::onPresetSelected(int index)
{
    QString presetId = _presetCombo->itemData(index).toString();
    if (presetId.isEmpty()) {
        _deletePresetBtn->setEnabled(false);
        return;  // "Custom" row — don't change anything
    }

    auto& mgr = ColumnPresetManager::instance();
    const auto* p = mgr.preset(presetId);
    if (p) {
        loadColumnsIntoSelected(p->columnKeys);
        _deletePresetBtn->setEnabled(!p->isBuiltIn);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Add / Remove columns
// ─────────────────────────────────────────────────────────────────────────────
void ColumnConfigDialog::onAddColumns()
{
    // Collect keys already selected
    QSet<QString> existing;
    for (int i = 0; i < _selectedList->count(); ++i)
        existing.insert(_selectedList->item(i)->data(KeyRole).toString());

    auto& mgr = ColumnPresetManager::instance();
    for (auto* treeItem : _availableTree->selectedItems()) {
        QString key = treeItem->data(0, KeyRole).toString();
        if (key.isEmpty() || existing.contains(key))
            continue;
        auto* item = new QListWidgetItem(mgr.displayName(key));
        item->setData(KeyRole, key);
        _selectedList->addItem(item);
        existing.insert(key);
    }
    _presetCombo->setCurrentIndex(0);   // mark as "Custom"
}

void ColumnConfigDialog::onRemoveColumns()
{
    auto selected = _selectedList->selectedItems();
    for (auto* item : selected)
        delete item;
    _presetCombo->setCurrentIndex(0);
}

void ColumnConfigDialog::onAddAll()
{
    auto& mgr = ColumnPresetManager::instance();
    _selectedList->clear();
    for (const auto& col : mgr.allColumns()) {
        auto* item = new QListWidgetItem(col.displayName);
        item->setData(KeyRole, col.key);
        _selectedList->addItem(item);
    }
    _presetCombo->setCurrentIndex(0);
}

void ColumnConfigDialog::onRemoveAll()
{
    _selectedList->clear();
    _presetCombo->setCurrentIndex(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reorder
// ─────────────────────────────────────────────────────────────────────────────
void ColumnConfigDialog::onMoveUp()
{
    auto items = _selectedList->selectedItems();
    // Collect rows, sort ascending
    QList<int> rows;
    for (auto* it : items)
        rows << _selectedList->row(it);
    std::sort(rows.begin(), rows.end());

    for (int r : rows) {
        if (r <= 0) continue;
        auto* taken = _selectedList->takeItem(r);
        _selectedList->insertItem(r - 1, taken);
        taken->setSelected(true);
    }
    _presetCombo->setCurrentIndex(0);
}

void ColumnConfigDialog::onMoveDown()
{
    auto items = _selectedList->selectedItems();
    QList<int> rows;
    for (auto* it : items)
        rows << _selectedList->row(it);
    std::sort(rows.begin(), rows.end(), std::greater<int>());

    int maxRow = _selectedList->count() - 1;
    for (int r : rows) {
        if (r >= maxRow) continue;
        auto* taken = _selectedList->takeItem(r);
        _selectedList->insertItem(r + 1, taken);
        taken->setSelected(true);
    }
    _presetCombo->setCurrentIndex(0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Save / Delete custom presets
// ─────────────────────────────────────────────────────────────────────────────
void ColumnConfigDialog::onSaveAsPreset()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, "Save Preset",
                                          "Preset name:", QLineEdit::Normal,
                                          QString(), &ok);
    if (!ok || name.trimmed().isEmpty()) return;

    ColumnPreset preset;
    preset.id         = QUuid::createUuid().toString(QUuid::WithoutBraces);
    preset.name       = name.trimmed();
    preset.columnKeys = selectedColumns();
    preset.isBuiltIn  = false;

    ColumnPresetManager::instance().saveCustomPreset(preset);
    syncPresetCombo();

    // Select the newly saved preset
    for (int i = 0; i < _presetCombo->count(); ++i) {
        if (_presetCombo->itemData(i).toString() == preset.id) {
            _presetCombo->setCurrentIndex(i);
            break;
        }
    }
    _deletePresetBtn->setEnabled(true);
}

void ColumnConfigDialog::onDeletePreset()
{
    QString presetId = _presetCombo->currentData().toString();
    if (presetId.isEmpty()) return;

    auto& mgr = ColumnPresetManager::instance();
    const auto* p = mgr.preset(presetId);
    if (!p || p->isBuiltIn) return;

    auto reply = QMessageBox::question(
        this, "Delete Preset",
        QString("Delete custom preset \"%1\"?").arg(p->name),
        QMessageBox::Yes | QMessageBox::No);

    if (reply == QMessageBox::Yes) {
        mgr.deleteCustomPreset(presetId);
        syncPresetCombo();
    }
}