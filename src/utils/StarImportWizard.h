#ifndef STARIMPORTWIZARD_H
#define STARIMPORTWIZARD_H

#include <QWizard>
#include <QMessageBox>
#include <memory>
#include <vector>

#include "ImportStagingArea.h"

class ApplicationController;
class Project;
class Star;
class BackgroundTaskManager;

class StarImportWizard : public QWizard
{
    Q_OBJECT

public:
    StarImportWizard(ApplicationController* controller,
                     std::shared_ptr<Project> project,
                     QWidget* parent = nullptr);

    enum { Page_GeneralImport, Page_ColumnMapping, Page_Spectra,
           Page_SpectralFits, Page_RadialVelocity, Page_Photometry };

    // Public getters for access by wizard pages
    ApplicationController* controller() const { return _controller; }
    std::shared_ptr<Project> project() const { return _project; }

    // Store imported stars for later steps
    void setImportedStars(const std::vector<std::shared_ptr<Star>>& stars) { _importedStars = stars; }
    std::vector<std::shared_ptr<Star>> importedStars() const { return _importedStars; }
    std::vector<std::shared_ptr<Star>> getImportedStars() const { return _importedStars; }

    // Staging area access
    ImportStagingArea* stagingArea() { return &_staging; }

public slots:
    void accept() override;
    void reject() override;

private:
    void waitForBackgroundTasks();

    ApplicationController* _controller;
    std::shared_ptr<Project> _project;
    std::vector<std::shared_ptr<Star>> _importedStars;
    ImportStagingArea _staging;
};

// Include all sub-components so users only need to include StarImportWizard.h
#include "GeneralImportPage.h"
#include "SpectraImportPage.h"
#include "SpectralFitImportPage.h"
#include "RadialVelocityImportPage.h"
#include "RVPhotometryPages.h"
#include "CatalogQueryWorkers.h"

#endif // STARIMPORTWIZARD_H