#include "fitting/ui/SpectralFitDefaultsPage.h"

#include "app/AppSettings.h"
#include "fitting/UserFitDefaults.h"
#include "fitting/ui/FitComponentsWidget.h"
#include "ui/widgets/WheelGuard.h"

#include <QCheckBox>
#include <QGroupBox>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>

namespace fit = astra::fitting;

SpectralFitDefaultsPage::SpectralFitDefaultsPage(QWidget* parent)
    : QWidget(parent)
{
    const auto saved = fit::UserFitDefaults::load(AppSettings());

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* host = new QWidget;
    auto* v = new QVBoxLayout(host);
    v->setContentsMargins(16, 16, 16, 16);

    auto* intro = new QLabel(
        "The stellar components every new spectrum fit window starts from. "
        "\"Save as default\" in the fit window stores the same setting.<br><br>"
        "Fit range, ignore regions and continuum-spline anchors depend on the "
        "spectrograph, so they are set per instrument mode: <b>Data / "
        "Instruments...</b>, or \"Save as mode default\" in the fit window.");
    intro->setWordWrap(true);
    v->addWidget(intro);

    auto* compBox = new QGroupBox("Stellar components");
    auto* cv = new QVBoxLayout(compBox);
    _ownComponentsCheck = new QCheckBox(
        "Use these grids and parameters (unticked: ASTRA's built-in default)");
    _ownComponentsCheck->setChecked(!saved.isEmpty());
    cv->addWidget(_ownComponentsCheck);
    _components = new FitComponentsWidget;
    if (!saved.isEmpty()) _components->setComponents(saved.components);
    _components->setEnabled(_ownComponentsCheck->isChecked());
    cv->addWidget(_components);
    v->addWidget(compBox);
    v->addStretch();

    scroll->setWidget(host);
    outer->addWidget(scroll);

    connect(_ownComponentsCheck, &QCheckBox::toggled,
            _components, &QWidget::setEnabled);

    astra::blockWheelScrollingRecursive(this);
}

void SpectralFitDefaultsPage::apply()
{
    fit::UserFitDefaults d;
    if (_ownComponentsCheck->isChecked())
        d.components = _components->components();
    AppSettings settings;
    d.save(settings);
}
