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
#include <QFileDialog>
#include <QMessageBox>
#include <QRegularExpression>
#include <QString>
#include <QUuid>

namespace {

inline QString freshId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
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

    auto result = StarPackage::readFromFile(path);
    if (!result.success) {
        QMessageBox::critical(
            parent, "Import Failed",
            QString("Could not read '%1':\n%2").arg(path, result.error));
        return -1;
    }
    if (result.stars.empty()) {
        QMessageBox::information(parent, "Receive Stars",
                                 "The package contained no stars.");
        return 0;
    }

    DatabaseManager *dbm = controller->databaseManager();

    // Ensure referenced instruments exist (never clobber the user's own).
    for (auto &inst : result.instruments) {
        if (!inst)
            continue;
        if (!dbm->getInstrumentById(inst->getId()) &&
            !dbm->getInstrumentByName(inst->getName()))
            dbm->saveInstrument(inst);
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    dbm->beginTransaction();

    bool ok    = true;
    int  saved = 0;
    for (auto &star : result.stars) {
        if (!star)
            continue;
        remapIdsForImport(star);
        if (!persistImportedStar(dbm, project->getId(), star)) {
            ok = false;
            break;
        }
        ++saved;
    }

    if (ok)
        dbm->commitTransaction();
    else
        dbm->rollbackTransaction();
    QApplication::restoreOverrideCursor();

    if (!ok) {
        QMessageBox::critical(
            parent, "Import Failed",
            "An error occurred while saving. No changes were made.");
        return -1;
    }

    for (auto &star : result.stars)
        if (star)
            project->addStar(star);

    QString extra;
    if (!result.creatorNote.isEmpty())
        extra = QString("\n\nNote from sender:\n%1").arg(result.creatorNote);
    QMessageBox::information(parent, "Stars Received",
                             QString("Imported %1 star%2 from:\n%3%4")
                                 .arg(saved)
                                 .arg(saved != 1 ? "s" : "")
                                 .arg(path, extra));
    return saved;
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

    QString err;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const bool ok = StarPackage::writeToFile(path, stars, opts, &err);
    QApplication::restoreOverrideCursor();

    if (!ok) {
        QMessageBox::critical(
            parent, "Share Failed",
            QString("Could not write the package:\n%1").arg(err));
        return;
    }

    QMessageBox::information(parent, "Stars Shared",
                             QString("Exported %1 star%2 to:\n%3")
                                 .arg(stars.size())
                                 .arg(stars.size() != 1 ? "s" : "")
                                 .arg(path));
}

} // namespace StarShare