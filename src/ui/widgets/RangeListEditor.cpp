#include "ui/widgets/RangeListEditor.h"

#include "app/ui/UiIcons.h"
#include "ui/widgets/WheelGuard.h"
#include "ui/widgets/WidgetFactory.h"

#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace astra {

namespace {
// Wide enough for "100000.00 Å" so no column is narrower than its values.
constexpr int kSpinWidth = 128;
}

RangeListEditor::RangeListEditor(QVector<Column> columns, const QString& addLabel,
                                 QWidget* parent)
    : QWidget(parent)
    , _columns(std::move(columns))
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(6);

    _gridHost = new QWidget;
    _grid = new QGridLayout(_gridHost);
    _grid->setContentsMargins(0, 0, 0, 0);
    _grid->setHorizontalSpacing(8);
    _grid->setVerticalSpacing(4);
    v->addWidget(_gridHost);

    _emptyLbl = new QLabel;
    _emptyLbl->setStyleSheet("color: palette(mid);");
    _emptyLbl->setVisible(false);
    v->addWidget(_emptyLbl);

    _buttonRow = new QWidget;
    auto* h = new QHBoxLayout(_buttonRow);
    h->setContentsMargins(0, 0, 0, 0);
    auto* add = new QPushButton(addLabel);
    h->addWidget(add);
    h->addStretch();
    v->addWidget(_buttonRow);

    _newRow = [this](const QVector<Row>& rows) {
        if (rows.isEmpty()) {
            Row r;
            for (const auto& c : _columns) r << c.min;
            return r;
        }
        Row r = rows.last();
        // a range in the first two columns moves past its own end
        if (r.size() >= 2) {
            const double w = r[1] - r[0];
            r[0] = r[1] + w;
            r[1] = r[0] + w;
        }
        return r;
    };

    connect(add, &QPushButton::clicked, this, [this] {
        const QVector<Row> current = rows();
        _rows = current;
        _rows.append(_newRow(current));
        rebuild();
        emit rowsChanged();
    });

    rebuild();
}

void RangeListEditor::setRows(const QVector<Row>& rows)
{
    _rows = rows;
    rebuild();
}

QVector<RangeListEditor::Row> RangeListEditor::rows() const
{
    // keyboard tracking is off, so a typed value reaches _rows on Return
    // only; interpretText() pushes it through valueChanged first
    for (auto* s : _gridHost->findChildren<QDoubleSpinBox*>())
        s->interpretText();
    return _rows;
}

void RangeListEditor::setNewRowFactory(std::function<Row(const QVector<Row>&)> f)
{
    if (f) _newRow = std::move(f);
}

void RangeListEditor::setEmptyText(const QString& text)
{
    _emptyLbl->setText(text);
    _emptyLbl->setVisible(_rows.isEmpty() && !text.isEmpty());
}

void RangeListEditor::addButton(QPushButton* button)
{
    auto* h = static_cast<QHBoxLayout*>(_buttonRow->layout());
    h->insertWidget(h->count() - 1, button);   // before the stretch
}

void RangeListEditor::rebuild()
{
    clearLayout(_grid);

    const int nCol = _columns.size();
    _gridHost->setVisible(!_rows.isEmpty());
    _emptyLbl->setVisible(_rows.isEmpty() && !_emptyLbl->text().isEmpty());
    if (_rows.isEmpty()) return;

    for (int c = 0; c < nCol; ++c) {
        auto* h = new QLabel(_columns[c].header);
        h->setStyleSheet("color: palette(mid);");
        _grid->addWidget(h, 0, c, Qt::AlignLeft | Qt::AlignBottom);
    }

    for (int r = 0; r < _rows.size(); ++r) {
        QDoubleSpinBox* first = nullptr;
        for (int c = 0; c < nCol; ++c) {
            const auto& col = _columns[c];
            auto* s = makeDoubleSpin(col.min, col.max, col.decimals,
                                     _rows[r].value(c, col.min), col.step,
                                     col.suffix, kSpinWidth);
            s->setFixedWidth(kSpinWidth);
            s->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            connect(s, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                    this, [this, r, c](double val) {
                if (r >= _rows.size()) return;
                if (_rows[r].size() <= c) _rows[r].resize(c + 1);
                _rows[r][c] = val;
                emit rowsChanged();
            });
            _grid->addWidget(s, r + 1, c);
            if (!first) first = s;
        }
        auto* rm = new QPushButton;
        UiIcons::apply(rm, UiIcons::Role::Remove);
        rm->setToolTip(tr("Remove this row"));
        const int hgt = first ? first->sizeHint().height() : 24;
        rm->setFixedSize(hgt, hgt);
        connect(rm, &QPushButton::clicked, this, [this, r] {
            _rows = rows();
            if (r < _rows.size()) _rows.removeAt(r);
            // rebuild() deletes this button; let its click finish first
            QMetaObject::invokeMethod(this, [this] {
                rebuild();
                emit rowsChanged();
            }, Qt::QueuedConnection);
        });
        _grid->addWidget(rm, r + 1, nCol);
    }
    _grid->setColumnStretch(nCol + 1, 1);
    blockWheelScrollingRecursive(_gridHost);
}

}   // namespace astra
