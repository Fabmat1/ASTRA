#include "views/tools/MassFitRuleEditor.h"

#include "utils/UiIcons.h"
#include "utils/WheelGuard.h"
#include "utils/WindowSizing.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace mf = astra::massfit;

namespace {

// The operators, in the order the combo lists them. Between needs a second
// value; the two boolean tests need none.
struct OpEntry { mf::Condition::Op op; const char* label; };

const QVector<OpEntry>& opEntries()
{
    static const QVector<OpEntry> ops = {
        { mf::Condition::Op::Lt,      "<"          },
        { mf::Condition::Op::Le,      "<="         },
        { mf::Condition::Op::Gt,      ">"          },
        { mf::Condition::Op::Ge,      ">="         },
        { mf::Condition::Op::Eq,      "="          },
        { mf::Condition::Op::Ne,      "!="         },
        { mf::Condition::Op::Between, "between"    },
        { mf::Condition::Op::IsTrue,  "is true"    },
        { mf::Condition::Op::IsFalse, "is false"   },
    };
    return ops;
}

QDoubleSpinBox* makeValueSpin(double value)
{
    auto* s = new QDoubleSpinBox;
    // One spin for teff (tens of thousands) and chi2r (order 1) alike, so the
    // range is wide and the step small; decimals are trimmed for readability.
    s->setRange(-1e9, 1e9);
    s->setDecimals(4);
    s->setSingleStep(1.0);
    s->setValue(value);
    s->setKeyboardTracking(false);
    s->setMinimumWidth(110);
    astra::blockWheelScrolling(s);
    return s;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────

MassFitRuleEditor::MassFitRuleEditor(const mf::RuleGroup& rule,
                                     const QString& title, QWidget* parent)
    : QDialog(parent), _rule(rule)
{
    setWindowTitle(title.isEmpty() ? tr("Edit rule") : title);
    setModal(true);

    auto* outer = new QVBoxLayout(this);

    auto* intro = new QLabel(
        tr("The rule is tested against what the node's fit produced. A rule "
           "with no conditions is always true, which is how an unconditional "
           "branch is written."), this);
    intro->setWordWrap(true);
    outer->addWidget(intro);

    auto* topRow = new QHBoxLayout;
    topRow->addWidget(new QLabel(tr("Match:"), this));
    _combineCombo = new QComboBox(this);
    _combineCombo->addItem(tr("All of the conditions (AND)"),
                           int(mf::RuleGroup::Combine::All));
    _combineCombo->addItem(tr("Any of the conditions (OR)"),
                           int(mf::RuleGroup::Combine::Any));
    _combineCombo->setCurrentIndex(
        _rule.combine == mf::RuleGroup::Combine::Any ? 1 : 0);
    connect(_combineCombo, &QComboBox::currentIndexChanged,
            this, [this]{ updatePreview(); });
    topRow->addWidget(_combineCombo, 1);
    outer->addLayout(topRow);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget;
    _rowsLayout = new QVBoxLayout(host);
    _rowsLayout->setContentsMargins(0, 0, 0, 0);
    _rowsLayout->setSpacing(2);
    scroll->setWidget(host);
    scroll->setMinimumHeight(180);
    outer->addWidget(scroll, 1);

    auto* addBtn = new QPushButton(tr("Add condition"), this);
    UiIcons::apply(addBtn, UiIcons::Role::TransferAdd);
    connect(addBtn, &QPushButton::clicked, this, [this]{
        mf::Condition c;
        c.field = mf::AttemptSummary::fieldNames().value(0);
        addRow(c);
        updatePreview();
    });
    auto* addRow2 = new QHBoxLayout;
    addRow2->addWidget(addBtn);
    addRow2->addStretch();
    outer->addLayout(addRow2);

    _nestedNote = new QLabel(this);
    _nestedNote->setWordWrap(true);
    _nestedNote->setVisible(!_rule.groups.isEmpty());
    if (!_rule.groups.isEmpty())
        _nestedNote->setText(
            tr("This rule also carries %1 nested sub-group(s). They are kept "
               "exactly as they are, but this editor only edits the conditions "
               "above.").arg(_rule.groups.size()));
    outer->addWidget(_nestedNote);

    _preview = new QLabel(this);
    _preview->setWordWrap(true);
    _preview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(_preview);

    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok |
                                    QDialogButtonBox::Cancel, this);
    connect(bb, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
    UiIcons::applyDialogButtons(bb);
    outer->addWidget(bb);

    for (const mf::Condition& c : _rule.conditions) addRow(c);
    updatePreview();

    astra::blockWheelScrollingRecursive(this);
    resize(620, 460);
    WindowSizing::fitToScreen(this);
}

mf::RuleGroup MassFitRuleEditor::rule() const
{
    mf::RuleGroup out;
    out.combine = _combineCombo->currentIndex() == 1
                      ? mf::RuleGroup::Combine::Any
                      : mf::RuleGroup::Combine::All;
    out.groups = _rule.groups;   // preserved, never edited here

    for (const Row& r : _rows) {
        // keyboardTracking is off, so a typed-but-uncommitted value would be
        // lost without this.
        r.lo->interpretText();
        r.hi->interpretText();
        r.component->interpretText();

        mf::Condition c;
        c.field     = r.field->currentData().toString();
        c.op        = mf::Condition::Op(r.op->currentData().toInt());
        c.lo        = r.lo->value();
        c.hi        = r.hi->value();
        c.component = r.component->value() - 1;   // shown 1-based
        out.conditions.append(c);
    }
    return out;
}

void MassFitRuleEditor::addRow(const mf::Condition& c)
{
    Row r;
    r.host = new QWidget;
    auto* h = new QHBoxLayout(r.host);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(4);

    r.field = new QComboBox;
    for (const QString& f : mf::AttemptSummary::fieldNames())
        r.field->addItem(f, f);
    const int fi = r.field->findData(c.field);
    if (fi >= 0) r.field->setCurrentIndex(fi);
    r.field->setMinimumWidth(140);
    h->addWidget(r.field);

    r.op = new QComboBox;
    for (const OpEntry& e : opEntries())
        r.op->addItem(QString::fromLatin1(e.label), int(e.op));
    const int oi = r.op->findData(int(c.op));
    if (oi >= 0) r.op->setCurrentIndex(oi);
    h->addWidget(r.op);

    r.lo = makeValueSpin(c.lo);
    h->addWidget(r.lo);

    r.andLabel = new QLabel(tr("and"));
    h->addWidget(r.andLabel);

    r.hi = makeValueSpin(c.hi);
    h->addWidget(r.hi);

    r.compLabel = new QLabel(tr("component"));
    h->addWidget(r.compLabel);
    r.component = new QSpinBox;
    r.component->setRange(1, 9);
    r.component->setValue(c.component + 1);
    r.component->setKeyboardTracking(false);
    astra::blockWheelScrolling(r.component);
    h->addWidget(r.component);

    auto* rm = new QPushButton;
    UiIcons::apply(rm, UiIcons::Role::Remove);
    rm->setMaximumWidth(28);
    rm->setToolTip(tr("Remove this condition"));
    h->addWidget(rm);

    QWidget* hostPtr = r.host;
    connect(rm, &QPushButton::clicked, this, [this, hostPtr]{
        for (int i = 0; i < _rows.size(); ++i) {
            if (_rows[i].host == hostPtr) { _rows.remove(i); break; }
        }
        hostPtr->setParent(nullptr);
        hostPtr->deleteLater();
        updatePreview();
    });

    connect(r.op, &QComboBox::currentIndexChanged, this, [this, hostPtr]{
        for (const Row& row : _rows)
            if (row.host == hostPtr) { syncRowVisibility(row); break; }
        updatePreview();
    });
    connect(r.field, &QComboBox::currentIndexChanged, this, [this, hostPtr]{
        for (const Row& row : _rows)
            if (row.host == hostPtr) { syncRowVisibility(row); break; }
        updatePreview();
    });
    connect(r.lo, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this]{ updatePreview(); });
    connect(r.hi, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this]{ updatePreview(); });
    connect(r.component, QOverload<int>::of(&QSpinBox::valueChanged),
            this, [this]{ updatePreview(); });

    _rows.append(r);
    _rowsLayout->addWidget(r.host);
    syncRowVisibility(r);
    astra::blockWheelScrollingRecursive(r.host);
}

void MassFitRuleEditor::syncRowVisibility(const Row& r)
{
    const auto op = mf::Condition::Op(r.op->currentData().toInt());
    const bool boolOp = op == mf::Condition::Op::IsTrue
                        || op == mf::Condition::Op::IsFalse;
    const bool between = op == mf::Condition::Op::Between;

    r.lo->setVisible(!boolOp);
    r.andLabel->setVisible(between);
    r.hi->setVisible(between);

    // chi2, the counters and the two flags belong to the fit as a whole, so
    // asking which component they refer to would be meaningless.
    const QString field = r.field->currentData().toString();
    const bool perComponent = field == QLatin1String("teff")
                              || field == QLatin1String("logg")
                              || field == QLatin1String("he")
                              || field == QLatin1String("vsini")
                              || field == QLatin1String("z");
    r.compLabel->setVisible(perComponent);
    r.component->setVisible(perComponent);
}

void MassFitRuleEditor::updatePreview()
{
    const mf::RuleGroup g = rule();
    _preview->setText(tr("Reads as: %1").arg(g.describe()));
}
