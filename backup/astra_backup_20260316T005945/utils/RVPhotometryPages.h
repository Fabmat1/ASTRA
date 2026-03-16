#ifndef RVPHOTOMETRYPAGES_H
#define RVPHOTOMETRYPAGES_H

#include <QWizardPage>
#include <QHash>
#include <memory>
#include <vector>

class Star;
class Spectrum;
class SpectralFit;
class RadialVelocityCurve;
class RadialVelocityPoint;
class RVFit;
class ApplicationController;
class DatabaseManager;

class QLineEdit;
class QRadioButton;
class QStackedWidget;
class QPushButton;
class QProgressBar;
class QTreeWidget;
class QLabel;
class QCheckBox;
class QComboBox;
class QGroupBox;
class QVBoxLayout; 


class PhotometryImportPage : public QWizardPage
{
    Q_OBJECT
public:
    PhotometryImportPage(QWidget* parent = nullptr);
    int nextId() const override;
};

#endif // RVPHOTOMETRYPAGES_H