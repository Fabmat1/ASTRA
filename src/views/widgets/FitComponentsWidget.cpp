#include "views/widgets/FitComponentsWidget.h"

#include "dialogs/SettingsDialog.h"
#include "models/ElementAbundances.h"
#include "utils/AppSettings.h"
#include "utils/CheckBoxDragger.h"
#include "utils/WheelGuard.h"
#include "views/widgets/GridSelectorWidget.h"

#include <QAbstractSpinBox>
#include <QCheckBox>
#include <QDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLayout>
#include <QPushButton>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

#include <vector>

namespace fit = astra::fitting;

namespace {

QDoubleSpinBox* makeDoubleSpin(double min, double max, int decimals,
                               double val, double step = 1.0,
                               const QString& suffix = {})
{
    auto* s = new QDoubleSpinBox;
    s->setRange(min, max);
    s->setDecimals(decimals);
    s->setSingleStep(step);
    s->setValue(val);
    if (!suffix.isEmpty()) s->setSuffix(" " + suffix);
    s->setKeyboardTracking(false);
    s->setMaximumWidth(110);
    astra::blockWheelScrolling(s);
    return s;
}

void clearLayout(QLayout* l)
{
    if (!l) return;
    while (auto* it = l->takeAt(0)) {
        if (auto* w = it->widget()) { w->setParent(nullptr); delete w; }
        if (auto* c = it->layout())  { clearLayout(c); delete c; }
        delete it;
    }
}

// An abundance spin box parked at its minimum reads "grid default" and means
// "untouched": the element stays out of the job's map, so the backend models it
// at the middle of its grid axis instead of at a value we invented.
constexpr double kAbundanceUnset = -30.0;
// Nothing between the sentinel and here is a physical abundance, so a value
// that lands in the gap (stepping up out of "grid default") is snapped to the
// element's solar value instead of being taken literally.
constexpr double kAbundanceFloor = -20.0;

} // namespace

// =====================================================================

FitComponentsWidget::FitComponentsWidget(QWidget* parent)
    : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(4);

    _componentsLayout = new QVBoxLayout;
    _componentsLayout->setSpacing(6);
    v->addLayout(_componentsLayout);

    auto* row = new QHBoxLayout;
    _addComponentBtn = new QPushButton("+ Add component");
    connect(_addComponentBtn, &QPushButton::clicked, this, [this]{
        fit::StellarComponent c;
        _components.append(c);
        rebuildComponentRows();
        emit componentsChanged();
    });
    row->addWidget(_addComponentBtn);
    row->addStretch();
    v->addLayout(row);

    // A fit without a component cannot be configured, so the editor always
    // starts with one.
    _components.append(fit::StellarComponent{});
    rebuildComponentRows();
}

void FitComponentsWidget::setComponents(
    const QVector<fit::StellarComponent>& comps)
{
    // Tear the old rows down before the vector they point into is replaced.
    // The parameter spins hold raw pointers into the current buffer, and
    // deleting a focused one can make it interpret its text a last time.
    clearLayout(_componentsLayout);
    _componentSelectors.clear();

    _components = comps;
    if (_components.isEmpty())
        _components.append(fit::StellarComponent{});
    rebuildComponentRows();
}

QVector<fit::StellarComponent> FitComponentsWidget::components() const
{
    // keyboardTracking is off on every numeric field here, so a value the user
    // typed without pressing Return has not reached _components yet. Reading
    // the vector without this would silently drop the edit.
    const auto spins = findChildren<QAbstractSpinBox*>();
    for (auto* s : spins) s->interpretText();
    return _components;
}

void FitComponentsWidget::setMaxComponents(int n)
{
    _maxComponents = n;
    rebuildComponentRows();
}

// ────────────────────────────────────────────────────────────────────
// Component rows
// ────────────────────────────────────────────────────────────────────
void FitComponentsWidget::rebuildComponentRows()
{
    clearLayout(_componentsLayout);
    _componentSelectors.clear();

    AppSettings settings;
    const QStringList basePaths = settings.gridBasePaths();

    for (int i = 0; i < _components.size(); ++i) {
        auto& c = _components[i];

        auto* frame = new QGroupBox(QString("Component %1").arg(i + 1));
        auto* form  = new QFormLayout(frame);
        form->setLabelAlignment(Qt::AlignRight);

        auto* selector = new GridSelectorWidget;
        selector->setBasePaths(basePaths);
        selector->setShowConfigureButton(true);
        if (!c.gridPath.isEmpty())
            selector->setSelection({}, c.gridPath);
        // Seed from whatever the selector defaulted to, but only when this
        // component has no grid of its own yet. A setSelection issued while
        // the background grid scan is still running is deferred, so the
        // selector reads back empty for a moment - writing that back would
        // erase a grid path a saved plan carries.
        if (c.gridPath.isEmpty())
            c.gridPath = selector->selectedRelativePath();

        connect(selector, &GridSelectorWidget::selectionChanged,
                this, [this, i, selector]{
            if (i < _components.size())
                _components[i].gridPath = selector->selectedRelativePath();
            emit componentsChanged();
        });
        connect(selector, &GridSelectorWidget::configurePathsRequested,
                this, [this]{
            AppSettings s;
            SettingsDialog dlg(&s, this);
            if (dlg.exec() == QDialog::Accepted) {
                AppSettings fresh;
                const auto paths = fresh.gridBasePaths();
                for (auto* sel : _componentSelectors) sel->setBasePaths(paths);
            }
        });

        _componentSelectors.append(selector);
        astra::blockWheelScrollingRecursive(selector);   // its grid combos
        form->addRow("Grid:", selector);

        struct P { const char* label; double* val; bool* freeze;
                   double min, max; int decimals; double step; };
        std::vector<P> params = {
            { "Teff [K]",     &c.teff,  &c.freezeTeff,  1000.0, 200000.0, 0, 100.0 },
            { "log g",        &c.logg,  &c.freezeLogg,     0.0,      7.0, 2,  0.05 },
            { "vsini [km/s]", &c.vsini, &c.freezeVsini,    0.0,   2000.0, 2,  1.0  },
            { "log(He/H)",    &c.he,    &c.freezeHe,      -5.0,      2.0, 3,  0.05 },
            { "ζ",            &c.zeta,  &c.freezeZeta,    -5.0,     50.0, 3,  0.1  },
            { "ξ",            &c.xi,    &c.freezeXi,      -5.0,     50.0, 3,  0.1  },
            { "[M/H]",        &c.z,     &c.freezeZ,       -5.0,      5.0, 3,  0.05 },
        };

        for (auto& p : params) {
            auto* spin = makeDoubleSpin(p.min, p.max, p.decimals, *p.val, p.step);
            auto* cb   = new QCheckBox("freeze");
            cb->setChecked(*p.freeze);
            auto* row2 = new QHBoxLayout;
            row2->addWidget(spin, 1);
            row2->addWidget(cb);
            form->addRow(p.label, row2);

            double* vPtr = p.val;
            bool*   fPtr = p.freeze;
            connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                    this, [this, vPtr](double v){
                *vPtr = v;
                emit componentsChanged();
            });
            connect(cb, &QCheckBox::toggled, this,
                    [this, fPtr](bool b){
                *fPtr = b;
                emit componentsChanged();
            });
        }

        // Component 1's surface ratio is 1 and frozen by definition, so only
        // the later components get an editable one.
        if (i > 0) {
            auto* srSpin = makeDoubleSpin(0.0, 1e6, 4, c.surRatio, 0.01);
            srSpin->setToolTip(
                "Effective surface area of this component relative to "
                "component 1's.");
            auto* srFreeze = new QCheckBox("freeze");
            srFreeze->setChecked(c.freezeSurRatio);
            auto* srRow = new QHBoxLayout;
            srRow->addWidget(srSpin, 1);
            srRow->addWidget(srFreeze);
            form->addRow("Surface ratio", srRow);

            connect(srSpin, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
                    this, [this, i](double v){
                if (i < _components.size()) _components[i].surRatio = v;
                emit componentsChanged();
            });
            connect(srFreeze, &QCheckBox::toggled, this, [this, i](bool b){
                if (i < _components.size()) _components[i].freezeSurRatio = b;
                emit componentsChanged();
            });
        }

        form->addRow(buildAbundanceSection(i));

        if (_components.size() > 1) {
            auto* rm = new QPushButton("Remove component");
            connect(rm, &QPushButton::clicked, this, [this, i]{
                _components.removeAt(i);
                rebuildComponentRows();
                emit componentsChanged();
            });
            form->addRow("", rm);
        }

        _componentsLayout->addWidget(frame);
    }

    if (_addComponentBtn) {
        const bool room = _maxComponents <= 0
                          || _components.size() < _maxComponents;
        _addComponentBtn->setEnabled(room);
        _addComponentBtn->setToolTip(
            room ? QString()
                 : QString("This editor is limited to %1 components.")
                       .arg(_maxComponents));
    }
}

QWidget* FitComponentsWidget::buildAbundanceSection(int componentIndex)
{
    const int ci = componentIndex;

    auto* host = new QWidget;
    auto* v    = new QVBoxLayout(host);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);

    // Two dozen elements would dwarf the seven stellar parameters above them,
    // so the list lives behind a header button and starts closed.
    auto* header = new QToolButton;
    header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    header->setArrowType(Qt::RightArrow);
    header->setCheckable(true);
    header->setAutoRaise(true);
    header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    auto* body = new QWidget;
    body->setVisible(false);
    connect(header, &QToolButton::toggled, body, [header, body](bool on){
        body->setVisible(on);
        header->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
    });

    auto summarise = [this, ci, header]{
        if (ci >= _components.size()) return;
        const auto& c = _components[ci];
        int fitted = 0;
        for (auto it = c.freezeAbundances.cbegin();
             it != c.freezeAbundances.cend(); ++it)
            if (!it.value()) ++fitted;
        header->setText(QString("Element abundances  (%1 fitted, %2 seeded)")
                            .arg(fitted).arg(c.abundances.size()));
    };
    summarise();

    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(12, 2, 0, 2);
    bodyLayout->setSpacing(2);

    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(2);

    QVector<QCheckBox*>      fitBoxes;
    QVector<QDoubleSpinBox*> valueSpins;

    const auto& els  = astra::elements::all();
    const auto& comp = _components[ci];
    for (int e = 0; e < els.size(); ++e) {
        const QString sym = els[e].symbol;

        auto* fitBox = new QCheckBox(els[e].display);
        fitBox->setMinimumWidth(48);
        fitBox->setToolTip("Fit this element; unchecked it is still modelled, "
                           "just held fixed.");
        fitBox->setChecked(!comp.freezeAbundances.value(sym, true));

        auto* spin = makeDoubleSpin(kAbundanceUnset, 12.0, 3,
                                    comp.abundances.value(sym, kAbundanceUnset),
                                    0.05);
        spin->setSpecialValueText("grid default");
        spin->setMaximumWidth(130);          // room for the special-value text
        spin->setToolTip(
            QString("Starting log10 n(%1)/n_total (solar: %2). "
                    "10 or more removes the element from the model.")
                .arg(els[e].display).arg(els[e].solarLogN, 0, 'f', 2));

        connect(fitBox, &QCheckBox::toggled, this,
                [this, ci, sym, summarise](bool on){
            if (ci >= _components.size()) return;
            // Absent means frozen, so unticking removes the entry rather than
            // writing the default back.
            if (on) _components[ci].freezeAbundances[sym] = false;
            else    _components[ci].freezeAbundances.remove(sym);
            summarise();
            emit componentsChanged();
        });
        const double solar = els[e].solarLogN;
        connect(spin, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this,
                [this, ci, sym, solar, spin, summarise](double val){
            if (ci >= _components.size()) return;
            if (val > kAbundanceUnset && val < kAbundanceFloor) {
                spin->setValue(solar);   // re-enters and stores the solar seed
                return;
            }
            if (val <= kAbundanceUnset) _components[ci].abundances.remove(sym);
            else                        _components[ci].abundances[sym] = val;
            summarise();
            emit componentsChanged();
        });

        grid->addWidget(fitBox, e / 2, (e % 2) * 2);
        grid->addWidget(spin,   e / 2, (e % 2) * 2 + 1);
        fitBoxes.append(fitBox);
        valueSpins.append(spin);
    }
    bodyLayout->addLayout(grid);

    // Two dozen elements are tedious to tick one at a time, so a press can be
    // dragged down/across the column to sweep a run of them.
    new CheckBoxDragger(fitBoxes, body);

    auto* clearRow = new QHBoxLayout;
    auto* selectAllBtn = new QPushButton("Select all");
    selectAllBtn->setToolTip("Fit every element (leaves the starting values "
                             "untouched).");
    auto* clearBtn = new QPushButton("Clear all");
    clearBtn->setToolTip("Drop every seed and fit flag for this component.");
    // Driving the widgets rather than the maps lets their own signals do the
    // clearing, so no row is rebuilt from underneath the button.
    connect(selectAllBtn, &QPushButton::clicked, this, [fitBoxes]{
        for (auto* b : fitBoxes) b->setChecked(true);
    });
    connect(clearBtn, &QPushButton::clicked, this, [fitBoxes, valueSpins]{
        for (auto* b : fitBoxes)   b->setChecked(false);
        for (auto* s : valueSpins) s->setValue(kAbundanceUnset);
    });
    clearRow->addStretch();
    clearRow->addWidget(selectAllBtn);
    clearRow->addWidget(clearBtn);
    bodyLayout->addLayout(clearRow);

    v->addWidget(header);
    v->addWidget(body);
    return host;
}
