#include "../importWizard/StarImportWizard.h"
#include "controllers/ApplicationController.h"
#include "../utils/BackgroundTaskManager.h"
#include "../db/DatabaseManager.h"
#include "models/Project.h"
#include "../utils/Logger.h"

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
    auto* sedPage = new SEDImportPage;
    sedPage->setStagingArea(&_staging);
    setPage(Page_SED, sedPage);
    auto* lcPage = new LightcurveImportPage;
    lcPage->setStagingArea(&_staging);
    setPage(Page_LightcurveImport, lcPage);

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

void StarImportWizard::accept() {
    // Wait for any in-flight background tasks
    waitForBackgroundTasks();

    if (_staging.isEmpty()) {
        LOG_INFO("ImportWizard", "Nothing to commit - staging area is empty");
        QWizard::accept();
        return;
    }

    // Capture counts BEFORE commitAll clears them
    const int nStars   = _staging.newStarCount();
    const int nSpectra = _staging.newSpectrumCount();
    const int nFits    = _staging.newFitCount();
    const int nRV      = _staging.newRVCurveCount();
    const int nSED     = _staging.newSEDModelCount();
    const int nLC      = _staging.dirtyLightcurveStarCount();

    LOG_INFO("ImportWizard", QString("Committing staged data: %1 stars, %2 "
                                     "spectra, %3 fits, %4 RV results")
                                 .arg(nStars)
                                 .arg(nSpectra)
                                 .arg(nFits)
                                 .arg(nRV));

    // Progress dialog on the HEAP - so it survives until the async callback
    auto *progress = new QProgressDialog("Saving imported data to database…",
                                         QString(), 0, 100, this);
    progress->setWindowModality(Qt::WindowModal);
    progress->setMinimumDuration(0);
    progress->setCancelButton(nullptr);
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->show();

    // Guard the dialog with a QPointer so stale, late-delivered progress
    // updates become a safe no-op instead of a use-after-free.
    QPointer<QProgressDialog> progressGuard(progress);

    // shared_ptr keeps the timer alive as long as any pending lambda captures
    // it.
    auto elapsed = std::make_shared<QElapsedTimer>();
    elapsed->start();

    // Lambda runs on the WORKER thread → must marshal to GUI thread.
    auto progressCb = [progressGuard, elapsed, this](int done, int total) {
        QMetaObject::invokeMethod(
            this,
            [progressGuard, elapsed, done, total]() {
                // NOTE: QProgressDialog::setValue() on a modal dialog calls
                // QApplication::processEvents() internally. That can re-enter
                // the event loop and run the QFutureWatcher::finished handler,
                // which closes (WA_DeleteOnClose → deletes) the dialog. After
                // that the QPointer becomes null mid-lambda, so we must
                // re-check the guard before EVERY use, not just once at the
                // top.
                if (!progressGuard)
                    return;
                progressGuard->setMaximum(total);

                if (!progressGuard)
                    return;
                progressGuard->setValue(done); // may re-enter the event loop

                if (!progressGuard)
                    return;

                QString label =
                    QStringLiteral("Saving imported data… (%1 / %2)")
                        .arg(done)
                        .arg(total);

                if (done > 0 && done < total) {
                    const qint64 ms    = elapsed->elapsed();
                    const double rate  = double(done) / double(ms); // items/ms
                    const qint64 etaMs = qint64(double(total - done) / rate);
                    const qint64 etaS  = etaMs / 1000;
                    label += QStringLiteral("\nETA: %1m %2s")
                                 .arg(etaS / 60)
                                 .arg(etaS % 60, 2, 10, QLatin1Char('0'));
                } else if (done == total) {
                    label = QStringLiteral("Finalising…");
                }

                if (!progressGuard)
                    return;
                progressGuard->setLabelText(label);
            },
            Qt::QueuedConnection);
    };

    DatabaseManager   *dbm       = _controller->databaseManager();
    ImportStagingArea *staging   = &_staging;
    QString            projectId = _project->getId();

    auto future =
        QtConcurrent::run([dbm, projectId, staging, progressCb]() -> bool {
            return staging->commitAll(dbm, projectId, progressCb);
        });

    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, progressGuard, nStars, nSpectra, nFits, nRV, nSED,
             nLC]() {
                bool ok = watcher->result();
                watcher->deleteLater();

                if (progressGuard) {
                    progressGuard->reset();
                    progressGuard->close(); // WA_DeleteOnClose deletes it;
                                            // QPointer auto-nulls
                }

                if (!ok) {
                    QString detail = "Please check the log for details.";
                    // The usual cause of a whole commit rolling back.
                    DatabaseManager* dbm = _controller
                                               ? _controller->databaseManager()
                                               : nullptr;
                    if (dbm && !dbm->isHealthy()) {
                        detail = QString(
                            "The database file failed its integrity check:\n%1\n"
                            "It has to be repaired or restored before anything "
                            "can be saved.").arg(dbm->integrityError());
                    }
                    QMessageBox::critical(
                        this, "Import Error",
                        "Failed to save imported data to the database.\n"
                        "Your in-memory changes have been preserved, but "
                        "nothing was written to disk.\n\n" + detail);
                    return;
                }

                QMessageBox::information(this, "Import Complete",
                                         QString("Successfully imported:\n"
                                                 "• %1 stars\n"
                                                 "• %2 spectra\n"
                                                 "• %3 spectral fits\n"
                                                 "• %4 RV curves\n"
                                                 "• %5 SED fits\n"
                                                 "• %6 Lightcurves")
                                             .arg(nStars)
                                             .arg(nSpectra)
                                             .arg(nFits)
                                             .arg(nRV)
                                             .arg(nSED)
                                             .arg(nLC));

                emit importCompleted(_project->getId());
                QWizard::accept();
            });
    watcher->setFuture(future);
}

void StarImportWizard::reject()
{
    if (!_staging.isEmpty()) {
        auto answer = QMessageBox::question(this, "Cancel Import",
            "You have unsaved import data. Are you sure you want to cancel?\n"
            "All imported data will be discarded.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);

        if (answer != QMessageBox::Yes)
            return;
    }

    waitForBackgroundTasks();
    _staging.clear();

    QWizard::reject();
}