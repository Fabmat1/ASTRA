// src/utils/RVPhotometryPages.cpp

#include "RVPhotometryPages.h"
#include "StarImportWizard.h"

#include <QVBoxLayout>
#include <QLabel>

// ============================================================================
// RadialVelocityImportPage Implementation
// ============================================================================

RadialVelocityImportPage::RadialVelocityImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import Radial Velocity Data");
    setSubTitle("Import RV measurements and time series");
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel("Radial velocity import functionality to be implemented.\n\n"
                               "This page will allow you to:\n"
                               "• Import RV measurements from tables\n"
                               "• Import RV time series/curves\n"
                               "• Associate RV data with imported stars");
    label->setWordWrap(true);
    layout->addWidget(label);
    layout->addStretch();
}

int RadialVelocityImportPage::nextId() const
{
    return StarImportWizard::Page_Photometry;
}

// ============================================================================
// PhotometryImportPage Implementation
// ============================================================================

PhotometryImportPage::PhotometryImportPage(QWidget* parent)
    : QWizardPage(parent)
{
    setTitle("Import Photometry");
    setSubTitle("Import lightcurves and photometric measurements");
    setFinalPage(true);
    
    QVBoxLayout* layout = new QVBoxLayout(this);
    QLabel* label = new QLabel("Photometry import functionality to be implemented.\n\n"
                               "This page will allow you to:\n"
                               "• Import photometric measurements\n"
                               "• Import lightcurve data (TESS, Kepler, etc.)\n"
                               "• Associate photometry with imported stars");
    label->setWordWrap(true);
    layout->addWidget(label);
    layout->addStretch();
}

int PhotometryImportPage::nextId() const
{
    return -1;
}