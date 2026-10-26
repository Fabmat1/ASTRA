#include "spectra/ui/SpectrumPanelsEditor.h"

#include "app/ui/UiIcons.h"
#include "spectra/SpectrumPanelPlan.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

enum Col { ColLabel = 0, ColFrom, ColTo, ColScale, ColCount };

using Scale = SpectrumViewPanel::Scale;

}   // namespace

SpectrumPanelsEditor::SpectrumPanelsEditor(QWidget* parent)
    : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);

    _table = new QTableWidget(0, ColCount);
    _table->setHorizontalHeaderLabels(
        {tr("Title"), tr("From (Å)"), tr("To (Å)"), tr("Shown as")});
    _table->verticalHeader()->setVisible(false);
    _table->setSelectionBehavior(QAbstractItemView::SelectRows);
    _table->setSelectionMode(QAbstractItemView::SingleSelection);
    auto* hh = _table->horizontalHeader();
    hh->setSectionResizeMode(ColLabel, QHeaderView::Stretch);
    hh->setSectionResizeMode(ColFrom, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(ColTo, QHeaderView::ResizeToContents);
    hh->setSectionResizeMode(ColScale, QHeaderView::ResizeToContents);
    v->addWidget(_table);

    auto* buttons = new QHBoxLayout;
    auto* add = new QPushButton(tr("+ Add panel"));
    auto* remove = new QPushButton(tr("Remove"));
    UiIcons::apply(remove, UiIcons::Role::Remove);
    auto* up = new QPushButton;
    UiIcons::apply(up, UiIcons::Role::ReorderUp);
    up->setToolTip(tr("Move the selected panel up"));
    auto* down = new QPushButton;
    UiIcons::apply(down, UiIcons::Role::ReorderDown);
    down->setToolTip(tr("Move the selected panel down"));
    auto* builtin = new QPushButton(tr("Load built-in list"));
    buttons->addWidget(add);
    buttons->addWidget(remove);
    buttons->addWidget(up);
    buttons->addWidget(down);
    buttons->addStretch();
    buttons->addWidget(builtin);
    v->addLayout(buttons);

    connect(add, &QPushButton::clicked, this, [this] {
        // A new line cut-out just past the last panel, 50 A wide.
        const auto all = panels();
        const double lo = all.isEmpty() ? 4000.0 : all.last().wlHigh + 10.0;
        const int at = _table->currentRow() >= 0 ? _table->currentRow() + 1 : -1;
        addRow({tr("New panel"), lo, lo + 50.0, Scale::Normalized}, at);
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (_table->currentRow() >= 0) _table->removeRow(_table->currentRow());
    });
    connect(up, &QPushButton::clicked, this, [this] { moveCurrent(-1); });
    connect(down, &QPushButton::clicked, this, [this] { moveCurrent(+1); });
    connect(builtin, &QPushButton::clicked, this,
            [this] { setPanels(astra::spectra::builtInViewPanels()); });
}

void SpectrumPanelsEditor::setPanels(const QVector<SpectrumViewPanel>& panels)
{
    _table->setRowCount(0);
    for (const auto& p : panels) addRow(p);
}

void SpectrumPanelsEditor::addRow(const SpectrumViewPanel& p, int at)
{
    const int row = at < 0 ? _table->rowCount() : at;
    _table->insertRow(row);
    _table->setItem(row, ColLabel, new QTableWidgetItem(p.label));
    // Doubles get the item views' spin box editor.
    auto* from = new QTableWidgetItem;
    from->setData(Qt::EditRole, p.wlLow);
    _table->setItem(row, ColFrom, from);
    auto* to = new QTableWidgetItem;
    to->setData(Qt::EditRole, p.wlHigh);
    _table->setItem(row, ColTo, to);

    auto* scale = new QComboBox;
    scale->addItem(tr("Normalized"), int(Scale::Normalized));
    scale->addItem(tr("Flux"), int(Scale::Flux));
    scale->addItem(tr("As switched"), int(Scale::Auto));
    scale->setToolTip(tr("Normalized: divided by the fitted continuum (or a "
                         "straight line under the window without a fit).\n"
                         "Flux: undivided, with the continuum drawn.\n"
                         "As switched: follows the panel's Normalized / Flux "
                         "switch."));
    scale->setCurrentIndex(scale->findData(int(p.scale)));
    _table->setCellWidget(row, ColScale, scale);
    _table->setCurrentCell(row, ColLabel);
}

QVector<SpectrumViewPanel> SpectrumPanelsEditor::panels() const
{
    QVector<SpectrumViewPanel> out;
    for (int r = 0; r < _table->rowCount(); ++r) {
        SpectrumViewPanel p;
        if (auto* it = _table->item(r, ColLabel)) p.label = it->text().trimmed();
        if (auto* it = _table->item(r, ColFrom))  p.wlLow = it->data(Qt::EditRole).toDouble();
        if (auto* it = _table->item(r, ColTo))    p.wlHigh = it->data(Qt::EditRole).toDouble();
        if (auto* c = qobject_cast<QComboBox*>(_table->cellWidget(r, ColScale)))
            p.scale = static_cast<Scale>(c->currentData().toInt());
        if (p.wlHigh > p.wlLow) out.append(p);
    }
    return out;
}

void SpectrumPanelsEditor::moveCurrent(int delta)
{
    const int r = _table->currentRow();
    const int to = r + delta;
    if (r < 0 || to < 0 || to >= _table->rowCount()) return;
    auto all = panels();
    // panels() drops rows without a window; only reorder when none was dropped
    if (all.size() != _table->rowCount()) return;
    all.move(r, to);
    setPanels(all);
    _table->setCurrentCell(to, ColLabel);
}
