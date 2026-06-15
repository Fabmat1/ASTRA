#include "io/StarShare.h"
#include "io/StarPackage.h"
#include "models/Star.h"

#include "controllers/ApplicationController.h"
#include "db/DatabaseManager.h"
#include "models/Instrument.h"
#include "models/Photometry.h"
#include "models/Project.h"
#include "models/RadialVelocity.h"
#include "models/Spectrum.h"
#include <QApplication>
#include <QEventLoop>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QString>
#include <QUuid>
#include <QtConcurrent>

#include <algorithm>
#include <functional>

namespace {

inline QString freshId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// Runs `job` on a worker thread while a modal progress dialog blocks the UI.
// `job` is handed a thread-safe reporter `report(percent, phase)` that it may
// call from the worker thread to drive the dialog. The dialog is not
// cancellable (the work writes files / a DB transaction and must run to
// completion). Returns the job's result once it finishes.
template <class Result>
Result runWithProgress(
    QWidget *parent, const QString &title,
    const std::function<Result(const StarPackage::ProgressFn &)> &job) {
    QProgressDialog dlg(title, QString(), 0, 100, parent);
    dlg.setWindowTitle(title);
    dlg.setWindowModality(Qt::ApplicationModal);
    dlg.setMinimumDuration(0);
    dlg.setAutoClose(false);
    dlg.setAutoReset(false);
    dlg.setCancelButton(nullptr); // not cancellable
    dlg.setValue(0);

    // Thread-safe reporter: marshals updates onto the GUI thread.
    QPointer<QProgressDialog> dlgPtr(&dlg);
    StarPackage::ProgressFn   report = [dlgPtr](int pct, const QString &phase) {
        if (!dlgPtr)
            return;
        QMetaObject::invokeMethod(
            dlgPtr.data(),
            [dlgPtr, pct, phase]() {
                if (!dlgPtr)
                    return;
                dlgPtr->setLabelText(phase);
                dlgPtr->setValue(qBound(0, pct, 100));
            },
            Qt::QueuedConnection);
    };

    QFutureWatcher<Result> watcher;
    QEventLoop             loop;
    QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop,
                     &QEventLoop::quit);
    watcher.setFuture(
        QtConcurrent::run([job, report]() -> Result { return job(report); }));

    dlg.show();
    if (!watcher.isFinished())
        loop.exec(); // pumps GUI events; modal dialog blocks interaction
    dlg.close();
    return watcher.result();
}

// Regenerate every ID so imported objects never collide with existing DB rows,
// while preserving internal cross-references (best-fit, RV→spectrum/fit links).
// Also clears stale data-file paths so repositories write fresh DataStore
// files.
void remapIdsForImport(const std::shared_ptr<Star> &star) {
    star->setId(freshId());

    std::map<QString, QString> specMap; // old spectrum id → new
    std::map<QString, QString> fitMap;  // old fit id      → new

    auto spectra = star->getSpectra();
    for (auto &sp : spectra) {
        if (!sp)
            continue;
        const QString oldSpec = sp->getId();
        const QString newSpec = freshId();
        sp->setId(newSpec);
        sp->setDataFile({}); // force fresh DataStore path
        specMap[oldSpec] = newSpec;

        QString newBest;
        for (auto &fit : sp->getSpectralFits()) {
            if (!fit)
                continue;
            const QString oldFit = fit->getId();
            const QString newFit = freshId();
            fit->setId(newFit);
            fit->setModelDataFile({});
            fitMap[oldFit] = newFit;
            if (fit->isBestFit)
                newBest = newFit;
        }
        if (!newBest.isEmpty())
            sp->setBestFitById(newBest);
    }

    if (auto ph = star->getPhotometry()) {
        ph->setId({}); // savePhotometry will allocate
        for (auto &m : ph->getSEDModels())
            if (m) {
                m->setId({});
                m->setModelDataFile({});
            }
        for (const auto &src : ph->getLightcurveSources())
            for (auto &f : ph->getLCFits(src))
                if (f) {
                    f->setId({});
                    f->setModelDataFile({});
                }
    }

    if (auto curve = star->getRVCurve()) {
        const QString newCurve = freshId();
        curve->setId(newCurve);
        for (auto &p : curve->getRVPoints()) {
            if (!p)
                continue;
            p->setId(freshId());
            p->setCurveId(newCurve);
            if (!p->getSpectrumId().isEmpty()) {
                auto it = specMap.find(p->getSpectrumId());
                if (it != specMap.end())
                    p->setSpectrumId(it->second);
            }
            if (!p->getSpectralFitId().isEmpty()) {
                auto it = fitMap.find(p->getSpectralFitId());
                if (it != fitMap.end())
                    p->setSpectralFitId(it->second);
            }
        }
        for (auto &f : curve->getRVFits()) {
            if (!f)
                continue;
            f->setId(freshId());
            f->setCurveId(newCurve);
        }
    }
}

bool persistImportedStar(DatabaseManager *dbm, const QString &projectId,
                         const std::shared_ptr<Star> &star) {
    const QString starId = star->getId();
    if (!dbm->saveStar(projectId, star))
        return false;

    for (auto &sp : star->getSpectra())
        if (sp && !dbm->saveSpectrum(starId, sp, true))
            return false;

    if (auto ph = star->getPhotometry())
        if (!dbm->savePhotometry(starId, ph))
            return false;

    if (auto curve = star->getRVCurve()) {
        if (!dbm->saveRadialVelocityCurve(curve, starId))
            return false;
        for (auto &p : curve->getRVPoints())
            if (p)
                dbm->saveRadialVelocityPoint(p, curve->getId());
        for (auto &f : curve->getRVFits())
            if (f)
                dbm->saveRVFit(f, curve->getId());
    }
    return true;
}

} // anonymous namespace

int StarShare::importFileInteractive(QWidget                 *parent,
                                     ApplicationController   *controller,
                                     std::shared_ptr<Project> project,
                                     const QString           &pathIn) {
    if (!controller || !project)
        return -1;

    QString path = pathIn;
    if (path.isEmpty()) {
        const QString filter =
            QStringLiteral("ASTRA Star Package (*%1);;All Files (*)")
                .arg(StarPackage::FILE_EXTENSION);
        path = QFileDialog::getOpenFileName(parent, "Receive / Import Stars",
                                            QString(), filter);
        if (path.isEmpty())
            return 0; // cancelled
    }

    DatabaseManager *dbm       = controller->databaseManager();
    const QString    projectId = project->getId();

    // Outcome of the off-thread read + persist.
    struct ImportOutcome {
        bool        readOk    = false;
        bool        saveOk    = true;
        int         saved     = 0;
        bool        empty     = false;
        QString     readError;
        QString     creatorNote;
        std::vector<std::shared_ptr<Star>> stars;
    };

    // Read the package and persist its stars on a worker thread. Reading
    // (decompress + JSON + blobs) and saving (DB transaction + side-file
    // writes) are both heavy; doing them here keeps the UI responsive behind a
    // modal progress dialog. All DB work runs on this single worker thread, so
    // the per-thread connection and the surrounding transaction stay coherent.
    const ImportOutcome out = runWithProgress<ImportOutcome>(
        parent, QStringLiteral("Receiving stars…"),
        [dbm, projectId, path](const StarPackage::ProgressFn &report)
            -> ImportOutcome {
            ImportOutcome o;

            // Read → progress 0..40 of the overall bar.
            auto pkg = StarPackage::readFromFile(
                path, [&report](int p, const QString &ph) {
                    report(int(p * 0.40), ph);
                });
            o.readOk      = pkg.success;
            o.readError   = pkg.error;
            o.creatorNote = pkg.creatorNote;
            if (!pkg.success)
                return o;
            if (pkg.stars.empty()) {
                o.empty = true;
                return o;
            }

            // Ensure referenced instruments exist (never clobber the user's).
            report(42, QStringLiteral("Saving instruments…"));
            for (auto &inst : pkg.instruments) {
                if (!inst)
                    continue;
                if (!dbm->getInstrumentById(inst->getId()) &&
                    !dbm->getInstrumentByName(inst->getName()))
                    dbm->saveInstrument(inst);
            }

            // Persist stars → progress 45..100.
            dbm->beginTransaction();
            const int n = int(pkg.stars.size());
            int       i = 0;
            for (auto &star : pkg.stars) {
                ++i;
                if (!star)
                    continue;
                remapIdsForImport(star);
                if (!persistImportedStar(dbm, projectId, star)) {
                    o.saveOk = false;
                    break;
                }
                ++o.saved;
                report(45 + int(55.0 * i / n),
                       QStringLiteral("Saving star %1 of %2").arg(i).arg(n));
            }
            if (o.saveOk)
                dbm->commitTransaction();
            else
                dbm->rollbackTransaction();

            if (o.saveOk)
                o.stars = std::move(pkg.stars);
            return o;
        });

    if (!out.readOk) {
        QMessageBox::critical(
            parent, "Import Failed",
            QString("Could not read '%1':\n%2").arg(path, out.readError));
        return -1;
    }
    if (out.empty) {
        QMessageBox::information(parent, "Receive Stars",
                                 "The package contained no stars.");
        return 0;
    }
    if (!out.saveOk) {
        QMessageBox::critical(
            parent, "Import Failed",
            "An error occurred while saving. No changes were made.");
        return -1;
    }

    for (auto &star : out.stars)
        if (star)
            project->addStar(star);

    QString extra;
    if (!out.creatorNote.isEmpty())
        extra = QString("\n\nNote from sender:\n%1").arg(out.creatorNote);
    QMessageBox::information(parent, "Stars Received",
                             QString("Imported %1 star%2 from:\n%3%4")
                                 .arg(out.saved)
                                 .arg(out.saved != 1 ? "s" : "")
                                 .arg(path, extra));
    return out.saved;
}

namespace StarShare {

void exportStarsInteractive(QWidget *parent,
                            ApplicationController * /*controller*/,
                            const std::vector<std::shared_ptr<Star>> &stars) {
    if (stars.empty()) {
        QMessageBox::information(parent, "Share Stars",
                                 "There are no stars to share.");
        return;
    }

    // Suggest a sensible default filename.
    QString suggested;
    if (stars.size() == 1 && stars.front()) {
        const auto &s    = stars.front();
        QString     base = !s->getAlias().isEmpty()      ? s->getAlias()
                           : !s->getSourceId().isEmpty() ? s->getSourceId()
                           : !s->getJName().isEmpty() ? s->getJName()
                                                      : QStringLiteral("star");
        base.replace(QRegularExpression("[^A-Za-z0-9._-]+"), "_");
        suggested = base + StarPackage::FILE_EXTENSION;
    } else {
        suggested = QStringLiteral("astra_%1_stars%2")
                        .arg(stars.size())
                        .arg(StarPackage::FILE_EXTENSION);
    }

    const QString filter =
        QStringLiteral("ASTRA Star Package (*%1);;All Files (*)")
            .arg(StarPackage::FILE_EXTENSION);

    QString path = QFileDialog::getSaveFileName(parent, "Share / Export Stars",
                                                suggested, filter);
    if (path.isEmpty())
        return;
    if (!path.endsWith(StarPackage::FILE_EXTENSION))
        path += StarPackage::FILE_EXTENSION;

    StarPackage::ExportOptions opts; // defaults: include everything

    // Serialize, compress and write on a worker thread (loading lazy spectrum /
    // fit side-files, JSON building and zlib compression can all be heavy) so
    // the UI stays responsive behind a modal progress dialog.
    struct ExportOutcome {
        bool    ok = false;
        QString error;
    };
    const ExportOutcome out = runWithProgress<ExportOutcome>(
        parent, QStringLiteral("Sharing stars…"),
        [path, stars, opts](const StarPackage::ProgressFn &report)
            -> ExportOutcome {
            ExportOutcome o;
            o.ok = StarPackage::writeToFile(path, stars, opts, &o.error,
                                            StarPackage::InstrumentResolver{},
                                            report);
            return o;
        });

    if (!out.ok) {
        QMessageBox::critical(
            parent, "Share Failed",
            QString("Could not write the package:\n%1").arg(out.error));
        return;
    }

    QMessageBox::information(parent, "Stars Shared",
                             QString("Exported %1 star%2 to:\n%3")
                                 .arg(stars.size())
                                 .arg(stars.size() != 1 ? "s" : "")
                                 .arg(path));
}

int copyStarsToProject(QWidget *parent, ApplicationController *controller,
                       const std::vector<std::shared_ptr<Star>> &stars,
                       std::shared_ptr<Project>                  target) {
    if (!controller || !target || stars.empty())
        return 0;

    DatabaseManager *dbm      = controller->databaseManager();
    const QString    targetId = target->getId();

    struct CopyOutcome {
        bool    ok    = false;
        int     saved = 0;
        QString error;
    };

    // The whole pipeline runs off the GUI thread behind a modal progress bar:
    // serializing to a buffer deep-loads lazy side-files, reading it back yields
    // detached star objects holding all their data in memory, and persisting
    // (with remapped IDs + cleared data-file paths) writes fresh DataStore
    // files so the copies never share storage with the originals.
    const CopyOutcome out = runWithProgress<CopyOutcome>(
        parent, QStringLiteral("Copying stars…"),
        [dbm, targetId, stars](const StarPackage::ProgressFn &report)
            -> CopyOutcome {
            CopyOutcome o;

            // Serialize → progress 0..45.
            QString          err;
            const QByteArray buf = StarPackage::writeToBuffer(
                stars, StarPackage::ExportOptions{}, &err,
                StarPackage::InstrumentResolver{},
                [&report](int p, const QString &ph) {
                    report(int(p * 0.45), ph);
                });
            if (buf.isEmpty()) {
                o.error = err.isEmpty()
                              ? QStringLiteral("Failed to serialize stars.")
                              : err;
                return o;
            }

            // Read back → progress 45..55.
            auto pkg = StarPackage::readFromBuffer(
                buf, [&report](int p, const QString &ph) {
                    report(45 + int(p * 0.10), ph);
                });
            if (!pkg.success) {
                o.error = pkg.error;
                return o;
            }

            // Persist into the target project → progress 55..100. Instruments
            // are referenced by id and live in the same database, so there is
            // nothing to import for them here.
            dbm->beginTransaction();
            const int n = int(pkg.stars.size());
            int       i = 0;
            o.ok        = true;
            for (auto &star : pkg.stars) {
                ++i;
                if (!star)
                    continue;
                remapIdsForImport(star);
                if (!persistImportedStar(dbm, targetId, star)) {
                    o.ok = false;
                    break;
                }
                ++o.saved;
                report(55 + int(45.0 * i / std::max(1, n)),
                       QStringLiteral("Saving star %1 of %2").arg(i).arg(n));
            }
            if (o.ok)
                dbm->commitTransaction();
            else
                dbm->rollbackTransaction();
            return o;
        });

    if (!out.ok) {
        QMessageBox::critical(
            parent, "Copy Failed",
            QString("Could not copy stars:\n%1").arg(out.error));
        return -1;
    }

    // Drop any in-memory star cache for the target so the next time it is
    // opened it reloads from the database (with proper lazy loaders) and shows
    // the freshly copied stars.
    target->setStars({}, false);

    QMessageBox::information(
        parent, "Stars Copied",
        QString("Copied %1 star%2 to project \"%3\".")
            .arg(out.saved)
            .arg(out.saved != 1 ? "s" : "")
            .arg(target->getName()));
    return out.saved;
}

int moveStarsToProject(QWidget *parent, ApplicationController *controller,
                       const std::vector<std::shared_ptr<Star>> &stars,
                       std::shared_ptr<Project>                  source,
                       std::shared_ptr<Project>                  target) {
    if (!controller || !target || stars.empty())
        return 0;

    DatabaseManager     *dbm = controller->databaseManager();
    std::vector<QString> ids;
    ids.reserve(stars.size());
    for (const auto &s : stars)
        if (s && !s->getId().isEmpty())
            ids.push_back(s->getId());
    if (ids.empty())
        return 0;

    if (!dbm->moveStarsToProject(ids, target->getId())) {
        QMessageBox::critical(
            parent, "Move Failed",
            "Could not move the stars. No changes were made.");
        return -1;
    }

    // Detach the moved stars from the source's in-memory list (the caller
    // refreshes its view) and invalidate the target cache so it reloads fresh.
    if (source)
        source->removeStars(stars);
    target->setStars({}, false);

    QMessageBox::information(
        parent, "Stars Moved",
        QString("Moved %1 star%2 to project \"%3\".")
            .arg(ids.size())
            .arg(ids.size() != 1 ? "s" : "")
            .arg(target->getName()));
    return int(ids.size());
}

} // namespace StarShare