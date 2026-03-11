// src/utils/StarImportWizard.cpp

#include "StarImportWizard.h"
#include "controllers/ApplicationController.h"
#include "models/Project.h"

StarImportWizard::StarImportWizard(ApplicationController* controller,
    std::shared_ptr<Project> project,
    QWidget* parent)
    : QWizard(parent)
    , _controller(controller)
    , _project(project)
{
    setWindowTitle("Star Import Wizard");
    setWizardStyle(QWizard::ModernStyle);

    setPage(Page_GeneralImport, new GeneralImportPage);
    setPage(Page_Spectra, new SpectraImportPage);
    setPage(Page_SpectralFits, new SpectralFitImportPage);  
    setPage(Page_RadialVelocity, new RadialVelocityImportPage);
    setPage(Page_Photometry, new PhotometryImportPage);

    setOptions(QWizard::NoBackButtonOnStartPage | 
               QWizard::NoCancelButtonOnLastPage);

    resize(900, 700);
}