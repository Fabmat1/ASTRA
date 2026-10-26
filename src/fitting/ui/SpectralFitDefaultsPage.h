#pragma once

#include <QWidget>

class FitComponentsWidget;

class QCheckBox;

/// Settings page: the stellar components a new spectrum fit window starts
/// from, in the same editor as the fit dialog. The fit dialog's "Save as
/// default" button writes the same setting. Edits stay on the page until
/// apply().
class SpectralFitDefaultsPage : public QWidget
{
    Q_OBJECT
public:
    explicit SpectralFitDefaultsPage(QWidget* parent = nullptr);

    void apply();

private:
    QCheckBox*           _ownComponentsCheck = nullptr;
    FitComponentsWidget* _components         = nullptr;
};
