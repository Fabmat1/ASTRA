#include "StarImportWizard.h"
#include "controllers/ApplicationController.h"
#include "utils/BackgroundTaskManager.h"
#include "utils/DatabaseManager.h"
#include "models/Project.h"
#include "utils/Logger.h"

#include <QApplication>
#include <QProgressDialog>
#include <QtConcurrent>

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

void StarImportWizard::waitForBackgroundTasks()
{
    auto* taskMgr = _controller->backgroundTaskManager();
    if (!taskMgr || !taskMgr->hasActiveTasks())
        return;

    QProgressDialog progress("Waiting for background tasks to finish...",
                             QString(), 0, 0, this);
    progress.setWindowModality(Qt::WindowModal);
    progress.setMinimumDuration(0);
    progress.setCancelButton(nullptr);
    progress.show();

    while (taskMgr->hasActiveTasks()) {
        QApplication::processEvents();
        QThread::msleep(100);
    }

    progress.close();
}

void StarImportWizard::accept()
{
    // Wait for any in-flight background tasks
    waitForBackgroundTasks();

    if (_staging.isEmpty()) {
        LOG_INFO("ImportWizard", "Nothing to commit — staging area is empty");
        QWizard::accept();
        return;
    }

    // Capture counts BEFORE commitAll clears them
    const int nStars   = _staging.newStarCount();
    const int nSpectra = _staging.newSpectraCount();
    const int nFits    = _staging.newFitCount();
    const int nRV      = _staging.rvResultCount();

    LOG_INFO("ImportWizard", QString("Committing staged data: %1 stars, %2 spectra, %3 fits, %4 RV results")
             .arg(nStars).arg(nSpectra).arg(nFits).arg(nRV));

    // Progress dialog on the HEAP — so it survives until the async callback
    auto* progress = new QProgressDialog("Saving imported data to database...",
                                         QString(), 0, 0, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setCancelButton(nullptr);
    progress->setAttribute(Qt::WA_DeleteOnClose);  // auto-delete when closed
    progress->show();

    DatabaseManager* dbm = _controller->databaseManager();
    std::shared_ptr<Project> project = _project;
    ImportStagingArea* staging = &_staging;

    auto future = QtConcurrent::run([dbm, project, staging]() -> bool {
        return staging->commitAll(dbm, project);
    });

    QFutureWatcher<bool>* watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, progress, nStars, nSpectra, nFits, nRV]()
    {
        bool ok = watcher->result();
        watcher->deleteLater();
        progress->close();  // triggers deleteLater via WA_DeleteOnClose

        if (!ok) {
            QMessageBox::critical(this, "Import Error",
                "Failed to save imported data to the database.\n"
                "Your in-memory changes have been preserved, but nothing was written to disk.\n"
                "Please check the log for details.");
            return;
        }

        QMessageBox::information(this, "Import Complete",
            QString("Successfully imported:\n"
                    "• %1 stars\n"
                    "• %2 spectra\n"
                    "• %3 spectral fits\n"
                    "• %4 RV curves")
                .arg(nStars).arg(nSpectra).arg(nFits).arg(nRV));

        QWizard::accept();
    });

    watcher->setFuture(future);
}

void StarImportWizard::reject()
{
    // If there's staged data, confirm cancellation
    if (!_staging.isEmpty()) {
        auto answer = QMessageBox::question(this, "Cancel Import",
            "You have unsaved import data. Are you sure you want to cancel?\n"
            "All imported data will be discarded.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

        if (answer != QMessageBox::Yes)
            return;
    }

    waitForBackgroundTasks();
    _staging.rollbackAll(_project);

    QWizard::reject();
}