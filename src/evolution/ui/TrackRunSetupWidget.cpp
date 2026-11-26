#include "evolution/ui/TrackRunSetupWidget.h"

#include "app/ui/UiIcons.h"
#include "ui/widgets/FlowLayout.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace astra::evolution::ui {

namespace {

constexpr int kKeyRole = Qt::UserRole + 1;

QString massText(double m) { return QString::number(m, 'f', m < 0.1 ? 4 : 3); }

QJsonObject jsonFrom(const QString& s) { return QJsonDocument::fromJson(s.toUtf8()).object(); }
QString jsonText(const QJsonObject& o) { return QString::fromUtf8(QJsonDocument(o).toJson(QJsonDocument::Compact)); }

}  // namespace

// ── TrackSubsetWidget ───────────────────────────────────────────────────────

TrackSubsetWidget::TrackSubsetWidget(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    _stack = new QStackedWidget;
    lay->addWidget(_stack);

    // Cores: tick core masses
    _coresPage = new QWidget;
    {
        auto* v = new QVBoxLayout(_coresPage);
        v->setContentsMargins(0, 0, 0, 0);
        auto* caption = new QLabel(tr("Core masses (M☉)"));
        v->addWidget(caption);
        auto* host = new QWidget;
        _coresFlow = new FlowLayout(host, 0, 8, 4);
        v->addWidget(host);
    }
    _stack->addWidget(_coresPage);

    // MassRange
    auto* rangePage = new QWidget;
    {
        auto* h = new QHBoxLayout(rangePage);
        h->setContentsMargins(0, 0, 0, 0);
        _lo = new QDoubleSpinBox;
        _hi = new QDoubleSpinBox;
        for (QDoubleSpinBox* s : {_lo, _hi}) {
            s->setDecimals(3);
            s->setSingleStep(0.01);
            s->setRange(0.0, 5.0);
            s->setSuffix(QStringLiteral(" M☉"));
            s->setKeyboardTracking(false);
        }
        h->addWidget(new QLabel(tr("Mass from")));
        h->addWidget(_lo);
        h->addWidget(new QLabel(tr("to")));
        h->addWidget(_hi);
        h->addStretch(1);
        connect(_lo, &QDoubleSpinBox::valueChanged, this, [this] { if (!_updating) refreshStatus(); });
        connect(_hi, &QDoubleSpinBox::valueChanged, this, [this] { if (!_updating) refreshStatus(); });
    }
    _stack->addWidget(rangePage);

    // Groups: compositions + envelopes
    _groupsPage = new QWidget;
    {
        auto* v = new QVBoxLayout(_groupsPage);
        v->setContentsMargins(0, 0, 0, 0);
        v->addWidget(new QLabel(tr("Compositions")));
        auto* host = new QWidget;
        _groupsFlow = new FlowLayout(host, 0, 8, 4);
        v->addWidget(host);
        auto* h = new QHBoxLayout;
        h->addWidget(new QLabel(tr("Envelopes per composition")));
        _envelopes = new QSpinBox;
        _envelopes->setRange(0, 99);
        _envelopes->setSpecialValueText(tr("all"));
        _envelopes->setKeyboardTracking(false);
        h->addWidget(_envelopes);
        h->addStretch(1);
        v->addLayout(h);
        connect(_envelopes, &QSpinBox::valueChanged, this, [this] { if (!_updating) refreshStatus(); });
    }
    _stack->addWidget(_groupsPage);

    auto* bottom = new QHBoxLayout;
    _status = new QLabel;
    _status->setWordWrap(true);
    _status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    _status->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    _resetButton = new QPushButton(tr("Default"));
    _resetButton->setToolTip(tr("sdfit's default"));
    _resetButton->setAutoDefault(false);
    UiIcons::apply(_resetButton, UiIcons::Role::Refresh);
    bottom->addWidget(_status, 1);
    bottom->addWidget(_resetButton, 0, Qt::AlignTop);
    lay->addLayout(bottom);
    connect(_resetButton, &QPushButton::clicked, this, [this] {
        setFamily(_family, TrackSetLibrary::defaultSubset(_family));
        emit changed();
    });
}

void TrackSubsetWidget::clearChecks()
{
    for (QCheckBox* c : _coreChecks) delete c;
    for (QCheckBox* c : _groupChecks) delete c;
    _coreChecks.clear();
    _groupChecks.clear();
}

void TrackSubsetWidget::setFamily(const TrackFamilyInfo& family, const TrackSubset& subset)
{
    _updating = true;
    _family = family;
    clearChecks();
    switch (family.subsetKind) {
    case SubsetKind::Cores:
        for (double m : family.coreMasses) {
            auto* c = new QCheckBox(massText(m));
            c->setProperty("mass", m);
            bool on = subset.cores.isEmpty();
            for (double s : subset.cores) on = on || std::fabs(s - m) < 1e-6;
            c->setChecked(on);
            connect(c, &QCheckBox::toggled, this, [this] { if (!_updating) refreshStatus(); });
            _coresFlow->addWidget(c);
            _coreChecks << c;
        }
        _stack->setCurrentIndex(0);
        break;
    case SubsetKind::MassRange: {
        const QVector<double>& m = family.coreMasses;
        const double lo = m.isEmpty() ? 0.0 : m.first();
        const double hi = m.isEmpty() ? 1.0 : m.last();
        _lo->setRange(0.0, std::max(5.0, hi));
        _hi->setRange(0.0, std::max(5.0, hi));
        _lo->setValue(std::isfinite(subset.massLo) ? subset.massLo : lo);
        _hi->setValue(std::isfinite(subset.massHi) ? subset.massHi : hi);
        _stack->setCurrentIndex(1);
        break;
    }
    case SubsetKind::Groups:
        for (int i = 0; i < family.groups.size(); ++i) {
            const QString& g = family.groups[i];
            auto* c = new QCheckBox(g);
            const double feh = i < family.groupFeh.size() ? family.groupFeh[i] : NAN;
            if (std::isfinite(feh)) c->setToolTip(tr("[Fe/H] = %1").arg(feh, 0, 'f', 2));
            c->setProperty("group", g);
            c->setChecked(subset.groups.isEmpty() || subset.groups.contains(g));
            connect(c, &QCheckBox::toggled, this, [this] { if (!_updating) refreshStatus(); });
            _groupsFlow->addWidget(c);
            _groupChecks << c;
        }
        _envelopes->setMaximum(std::max(1, family.maxEnvelopesPerGroup));
        _envelopes->setValue(subset.envelopes);
        _stack->setCurrentIndex(2);
        break;
    }
    // only the shown page counts for the height (a stacked widget otherwise keeps
    // the tallest page's height)
    for (int i = 0; i < _stack->count(); ++i)
        _stack->widget(i)->setSizePolicy(QSizePolicy::Preferred,
                                         i == _stack->currentIndex() ? QSizePolicy::Preferred : QSizePolicy::Ignored);
    _stack->adjustSize();
    _updating = false;
    refreshStatus();
}

TrackSubset TrackSubsetWidget::subset() const
{
    TrackSubset s;
    switch (_family.subsetKind) {
    case SubsetKind::Cores: {
        bool all = true;
        for (QCheckBox* c : _coreChecks) {
            if (c->isChecked()) s.cores << c->property("mass").toDouble();
            else all = false;
        }
        if (all) s.cores.clear();
        break;
    }
    case SubsetKind::MassRange:
        s.massLo = _lo->value();
        s.massHi = _hi->value();
        break;
    case SubsetKind::Groups: {
        bool all = true;
        for (QCheckBox* c : _groupChecks) {
            if (c->isChecked()) s.groups << c->property("group").toString();
            else all = false;
        }
        if (all) s.groups.clear();
        s.envelopes = _envelopes->value();
        break;
    }
    }
    return s;
}

void TrackSubsetWidget::setModelSettings(const TrackModelSettings& model)
{
    _model = model;
    refreshStatus();
}

void TrackSubsetWidget::refreshStatus()
{
    if (_family.key.isEmpty()) {
        _status->clear();
        return;
    }
    QString why;
    const auto p = TrackSetLibrary::shared().prepare(_family.key, subset(), _model, &why);
    if (p) {
        _status->setText(tr("%1 of %2 tracks, %3").arg(p->grid->n_nodes()).arg(p->full->n_nodes()).arg(p->interpolation));
        _status->setProperty("problem", false);
    } else {
        _status->setText(why);
        _status->setProperty("problem", true);
    }
    _status->setStyleSheet(p ? QString() : QStringLiteral("color: palette(link);"));
    if (!_updating) emit changed();
}

// ── TrackRunSetupWidget ─────────────────────────────────────────────────────

TrackRunSetupWidget::TrackRunSetupWidget(QWidget* parent) : QWidget(parent)
{
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    auto* listRow = new QHBoxLayout;
    _list = new QListWidget;
    _list->setDragDropMode(QAbstractItemView::InternalMove);
    _list->setDefaultDropAction(Qt::MoveAction);
    _list->setSelectionMode(QAbstractItemView::SingleSelection);
    _list->setMaximumHeight(130);
    listRow->addWidget(_list, 1);
    auto* arrows = new QVBoxLayout;
    _up = new QToolButton;
    _down = new QToolButton;
    _up->setToolTip(tr("Move up"));
    _down->setToolTip(tr("Move down"));
    UiIcons::apply(_up, UiIcons::Role::ReorderUp);
    UiIcons::apply(_down, UiIcons::Role::ReorderDown);
    arrows->addWidget(_up);
    arrows->addWidget(_down);
    arrows->addStretch(1);
    listRow->addLayout(arrows);
    lay->addLayout(listRow);

    _empty = new QLabel;
    _empty->setWordWrap(true);
    _empty->hide();
    lay->addWidget(_empty);

    _subset = new TrackSubsetWidget;
    lay->addWidget(_subset);

    // Advanced
    _advToggle = new QToolButton;
    _advToggle->setText(tr("Advanced"));
    _advToggle->setCheckable(true);
    _advToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    _advToggle->setAutoRaise(true);
    UiIcons::apply(_advToggle, UiIcons::Role::DisclosureCollapsed);
    lay->addWidget(_advToggle);

    _advanced = new QWidget;
    auto* form = new QFormLayout(_advanced);
    form->setContentsMargins(12, 0, 0, 0);
    _eepPrior = new QComboBox;
    _eepPrior->addItem(tr("None (flat in EEP)"), int(EepPriorChoice::None));
    _eepPrior->addItem(tr("Uniform in age"), int(EepPriorChoice::Age));
    _eepPrior->addItem(tr("sdB (sdfit --sdB_Prior)"), int(EepPriorChoice::SdB));
    _eepPrior->addItem(tr("sdO (sdfit --sdO_Prior)"), int(EepPriorChoice::SdO));
    form->addRow(tr("EEP prior:"), _eepPrior);
    _massPrior = new QCheckBox(tr("Core mass prior from the SED mass"));
    form->addRow(QString(), _massPrior);
    _walkers = new QSpinBox;
    _walkers->setRange(8, 100000);
    _burn = new QSpinBox;
    _burn->setRange(0, 1000000);
    _steps = new QSpinBox;
    _steps->setRange(10, 1000000);
    for (QSpinBox* s : {_walkers, _burn, _steps}) {
        s->setSingleStep(100);
        s->setKeyboardTracking(false);
    }
    auto* sampler = new QHBoxLayout;
    sampler->addWidget(new QLabel(tr("walkers")));
    sampler->addWidget(_walkers);
    sampler->addWidget(new QLabel(tr("burn-in")));
    sampler->addWidget(_burn);
    sampler->addWidget(new QLabel(tr("steps")));
    sampler->addWidget(_steps);
    sampler->addStretch(1);
    form->addRow(tr("Sampler:"), sampler);
    _seed = new QLineEdit;
    _seed->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\d{1,20}")), _seed));
    _seed->setMaximumWidth(200);
    form->addRow(tr("Seed:"), _seed);
    _hanInterp = new QComboBox;
    _hanInterp->addItem(tr("Bilinear (rectangular grid)"), false);
    _hanInterp->addItem(tr("scipy's triangles (as sdfit)"), true);
    form->addRow(tr("Han interpolation:"), _hanInterp);
    _advanced->hide();
    lay->addWidget(_advanced);

    connect(_advToggle, &QToolButton::toggled, this, [this](bool on) {
        _advanced->setVisible(on);
        UiIcons::apply(_advToggle, on ? UiIcons::Role::DisclosureExpanded : UiIcons::Role::DisclosureCollapsed);
    });
    connect(_list, &QListWidget::currentRowChanged, this, &TrackRunSetupWidget::onCurrentRowChanged);
    connect(_list, &QListWidget::itemChanged, this, [this] { emit changed(); });
    connect(_list->model(), &QAbstractItemModel::rowsMoved, this, [this] { emit changed(); });
    connect(_up, &QToolButton::clicked, this, [this] { moveCurrent(-1); });
    connect(_down, &QToolButton::clicked, this, [this] { moveCurrent(+1); });
    connect(_subset, &TrackSubsetWidget::changed, this, [this] {
        if (!_subset->familyKey().isEmpty()) _subsets.insert(_subset->familyKey(), _subset->subset());
        emit changed();
    });
    auto modelChanged = [this] {
        _subset->setModelSettings(modelSettings());
        emit changed();
    };
    connect(_eepPrior, &QComboBox::currentIndexChanged, this, modelChanged);
    connect(_massPrior, &QCheckBox::toggled, this, modelChanged);
    connect(_hanInterp, &QComboBox::currentIndexChanged, this, modelChanged);
    for (QSpinBox* s : {_walkers, _burn, _steps}) connect(s, &QSpinBox::valueChanged, this, modelChanged);
    connect(_seed, &QLineEdit::editingFinished, this, modelChanged);

    reloadFamilies();
}

void TrackRunSetupWidget::reloadFamilies()
{
    _infos = TrackSetLibrary::shared().families();
    restoreChoices();
}

void TrackRunSetupWidget::restoreChoices()
{
    QSettings st;
    const QStringList order = st.value(QStringLiteral("evolution/run/order")).toStringList();
    const QStringList checked = st.value(QStringLiteral("evolution/run/checked")).toStringList();
    const bool haveChoices = st.contains(QStringLiteral("evolution/run/checked"));

    // Families: remembered order first, then the rest as found.
    QVector<TrackFamilyInfo> sorted;
    for (const QString& k : order)
        for (const TrackFamilyInfo& f : _infos)
            if (f.key == k && f.usable()) sorted << f;
    for (const TrackFamilyInfo& f : _infos) {
        if (!f.usable()) continue;
        bool seen = false;
        for (const TrackFamilyInfo& g : sorted) seen = seen || g.key == f.key;
        if (!seen) sorted << f;
    }
    _usable = sorted.size();

    {
        QSignalBlocker block(_list);
        _list->clear();
        for (const TrackFamilyInfo& f : sorted) {
            auto* it = new QListWidgetItem(f.displayName);
            it->setData(kKeyRole, f.key);
            it->setToolTip(f.summary());
            it->setFlags(it->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsDragEnabled);
            it->setFlags(it->flags() & ~Qt::ItemIsDropEnabled);
            const bool on = haveChoices ? checked.contains(f.key) : f.key == QLatin1String("han");
            it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
            _list->addItem(it);
            const QString saved = st.value(QStringLiteral("evolution/run/subset/") + f.key).toString();
            _subsets.insert(f.key, saved.isEmpty() ? TrackSetLibrary::defaultSubset(f)
                                                   : TrackSubset::fromJson(jsonFrom(saved)));
        }
        // nothing ticked at all: the first family
        bool any = false;
        for (int i = 0; i < _list->count(); ++i) any = any || _list->item(i)->checkState() == Qt::Checked;
        if (!any && _list->count() > 0) _list->item(0)->setCheckState(Qt::Checked);
    }

    const TrackModelSettings m =
        TrackModelSettings::fromJson(jsonFrom(st.value(QStringLiteral("evolution/run/model")).toString()));
    {
        const QSignalBlocker b1(_eepPrior), b2(_massPrior), b3(_walkers), b4(_burn), b5(_steps), b6(_seed),
            b7(_hanInterp);
        _eepPrior->setCurrentIndex(std::max(0, _eepPrior->findData(int(m.eepPrior))));
        _massPrior->setChecked(m.massPrior);
        _walkers->setValue(m.walkers);
        _burn->setValue(m.burn);
        _steps->setValue(m.steps);
        _seed->setText(QString::number(m.seed));
        _hanInterp->setCurrentIndex(m.hanScipyInterpolation ? 1 : 0);
    }
    bool hasRect = false;
    for (const TrackFamilyInfo& f : sorted) hasRect = hasRect || f.rectangular;
    _hanInterp->setEnabled(hasRect);

    _empty->setVisible(_usable == 0);
    _list->setVisible(_usable > 0);
    _up->setVisible(_usable > 1);
    _down->setVisible(_usable > 1);
    _subset->setVisible(_usable > 0);
    if (_usable == 0) {
        const QString folder = TrackSetLibrary::shared().folder();
        _empty->setText(folder.isEmpty()
                            ? tr("No tracks folder set (Settings > Evolutionary Tracks).")
                            : tr("No usable track files in %1.").arg(folder));
    }
    if (_list->count() > 0) _list->setCurrentRow(0);
    onCurrentRowChanged();
}

void TrackRunSetupWidget::saveChoices() const
{
    QSettings st;
    QStringList order, checked;
    for (int i = 0; i < _list->count(); ++i) {
        const QString k = _list->item(i)->data(kKeyRole).toString();
        order << k;
        if (_list->item(i)->checkState() == Qt::Checked) checked << k;
    }
    st.setValue(QStringLiteral("evolution/run/order"), order);
    st.setValue(QStringLiteral("evolution/run/checked"), checked);
    for (auto it = _subsets.constBegin(); it != _subsets.constEnd(); ++it) {
        SubsetKind kind = SubsetKind::MassRange;
        for (const TrackFamilyInfo& f : _infos)
            if (f.key == it.key()) kind = f.subsetKind;
        st.setValue(QStringLiteral("evolution/run/subset/") + it.key(), jsonText(it.value().toJson(kind)));
    }
    st.setValue(QStringLiteral("evolution/run/model"), jsonText(modelSettings().toJson()));
}

QStringList TrackRunSetupWidget::families() const
{
    QStringList out;
    for (int i = 0; i < _list->count(); ++i)
        if (_list->item(i)->checkState() == Qt::Checked) out << _list->item(i)->data(kKeyRole).toString();
    return out;
}

QHash<QString, TrackSubset> TrackRunSetupWidget::subsets() const { return _subsets; }

TrackModelSettings TrackRunSetupWidget::modelSettings() const
{
    TrackModelSettings m;
    m.eepPrior = EepPriorChoice(_eepPrior->currentData().toInt());
    m.massPrior = _massPrior->isChecked();
    m.walkers = _walkers->value();
    m.burn = _burn->value();
    m.steps = _steps->value();
    bool ok = false;
    const quint64 seed = _seed->text().toULongLong(&ok);
    m.seed = ok ? seed : 1;
    m.hanScipyInterpolation = _hanInterp->currentData().toBool();
    return m;
}

QString TrackRunSetupWidget::currentFamily() const
{
    const QListWidgetItem* it = _list->currentItem();
    return it ? it->data(kKeyRole).toString() : QString();
}

void TrackRunSetupWidget::onCurrentRowChanged()
{
    const QString key = currentFamily();
    _up->setEnabled(_list->currentRow() > 0);
    _down->setEnabled(_list->currentRow() >= 0 && _list->currentRow() < _list->count() - 1);
    for (const TrackFamilyInfo& f : _infos)
        if (f.key == key) {
            _subset->setModelSettings(modelSettings());
            _subset->setFamily(f, _subsets.value(key, TrackSetLibrary::defaultSubset(f)));
        }
    emit currentFamilyChanged(key);
}

void TrackRunSetupWidget::moveCurrent(int delta)
{
    const int row = _list->currentRow();
    const int to = row + delta;
    if (row < 0 || to < 0 || to >= _list->count()) return;
    QListWidgetItem* it = _list->takeItem(row);
    _list->insertItem(to, it);
    _list->setCurrentRow(to);
    emit changed();
}


}  // namespace astra::evolution::ui
