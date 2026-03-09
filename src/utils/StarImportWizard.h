// src/utils/StarImportWizard.h

#ifndef STARIMPORTWIZARD_H
#define STARIMPORTWIZARD_H

#include <QWizard>
#include <memory>
#include <vector>

class ApplicationController;
class Project;
class Star;

class StarImportWizard : public QWizard
{
    Q_OBJECT

public:
    StarImportWizard(ApplicationController* controller, 
                     std::shared_ptr<Project> project,
                     QWidget* parent = nullptr);

    enum { Page_GeneralImport, Page_ColumnMapping, Page_Spectra, Page_RadialVelocity, Page_Photometry };
    
    // Public getters for access by wizard pages
    ApplicationController* controller() const { return _controller; }
    std::shared_ptr<Project> project() const { return _project; }
    
    // Store imported stars for later steps
    void setImportedStars(const std::vector<std::shared_ptr<Star>>& stars) { _importedStars = stars; }
    std::vector<std::shared_ptr<Star>> importedStars() const { return _importedStars; }
    std::vector<std::shared_ptr<Star>> getImportedStars() const { return _importedStars; }

private:
    ApplicationController* _controller;
    std::shared_ptr<Project> _project;
    std::vector<std::shared_ptr<Star>> _importedStars;
};

// Include all sub-components so users only need to include StarImportWizard.h
#include "GeneralImportPage.h"
#include "SpectraImportPage.h"
#include "RVPhotometryPages.h"
#include "CatalogQueryWorkers.h"

#endif // STARIMPORTWIZARD_H