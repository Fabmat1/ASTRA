#pragma once

#include <QVector>
#include <QWidget>

#include "fitting/FitTypes.h"

class GridSelectorWidget;

class QPushButton;
class QVBoxLayout;

// ─────────────────────────────────────────────────────────────────────────────
// The stellar-component editor: one card per component with its model grid,
// the seven stellar parameters and their freeze flags, the surface ratio from
// the second component onwards, and a collapsible per-element abundance
// editor.
//
// This used to live inside FitSetupWidget, which made it reachable only from
// the single-star fit dialog. The mass fitter needs exactly the same editor
// for every fit setup in a plan, so it moved here whole rather than being
// written a second time: the widget owns a QVector<StellarComponent> and
// nothing else, and both callers drive it through that vector.
// ─────────────────────────────────────────────────────────────────────────────
class FitComponentsWidget : public QWidget
{
    Q_OBJECT
public:
    explicit FitComponentsWidget(QWidget* parent = nullptr);

    /// Replaces the edited set. An empty vector still yields one default
    /// component, because a fit without one cannot be configured at all.
    void setComponents(const QVector<astra::fitting::StellarComponent>& comps);

    /// The edited set. Spin boxes here have keyboard tracking off, so a value
    /// the user typed but has not committed would otherwise be lost; this
    /// interprets every pending editor first.
    QVector<astra::fitting::StellarComponent> components() const;

    /// Upper bound on the number of components the user may add; 0 (the
    /// default) leaves it unbounded, which is what the single-star dialog
    /// has always allowed.
    void setMaxComponents(int n);

signals:
    void componentsChanged();

private:
    void rebuildComponentRows();
    /// Collapsed "Element abundances" editor for _components[componentIndex].
    QWidget* buildAbundanceSection(int componentIndex);

    QVector<astra::fitting::StellarComponent> _components;
    QVector<GridSelectorWidget*>              _componentSelectors;

    QVBoxLayout* _componentsLayout = nullptr;
    QPushButton* _addComponentBtn  = nullptr;

    int _maxComponents = 0;   ///< 0 = unlimited
};
