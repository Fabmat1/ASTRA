// src/utils/RVPhotometryPages.h

#ifndef RVPHOTOMETRYPAGES_H
#define RVPHOTOMETRYPAGES_H

#include <QWizardPage>

class RadialVelocityImportPage : public QWizardPage
{
    Q_OBJECT
public:
    RadialVelocityImportPage(QWidget* parent = nullptr);
    int nextId() const override;
};

class PhotometryImportPage : public QWizardPage
{
    Q_OBJECT
public:
    PhotometryImportPage(QWidget* parent = nullptr);
    int nextId() const override;
};

#endif // RVPHOTOMETRYPAGES_H