#include "SpectrumRepository.h"
#include "DBAccess.h"
#include "models/Spectrum.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QFileInfo>
#include "utils/DataStore.h"

SpectrumRepository::SpectrumRepository(DBAccess& db) : _db(db) {}

bool SpectrumRepository::saveSpectrum(const QString& starId, std::shared_ptr<Spectrum> spectrum)
{
    if (spectrum->getId().isEmpty()) {
        spectrum->setId(_db.generateUUID());
    }

    QString dataDir = QFileInfo(_db.databasePath()).absolutePath() + "/data";
    QString dataFile = DataStore::spectrumPath(dataDir, starId, spectrum->getId());

    // Save compressed spectral data
    spectrum->saveDataToFile(dataFile);

    // Clean up old file if path changed (legacy migration)
    QString oldFile = spectrum->getDataFile();
    if (!oldFile.isEmpty() && oldFile != dataFile && QFile::exists(oldFile)) {
        QFile::remove(oldFile);
    }
    spectrum->setDataFile(dataFile);

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO spectra (
            id, star_id, file, instrument, mjd, bjd, exposure_time,
            data_file, barycentric_corrected, is_flagged
        ) VALUES (
            :id, :star_id, :file, :instrument, :mjd, :bjd, :exposure_time,
            :data_file, :barycentric_corrected, :is_flagged
        )
    )");

    query.bindValue(":id", spectrum->getId());
    query.bindValue(":star_id", starId);
    query.bindValue(":file", spectrum->getFile());
    query.bindValue(":instrument", spectrum->getInstrument());
    query.bindValue(":mjd", spectrum->getMJD());
    query.bindValue(":bjd", spectrum->getBJD());
    query.bindValue(":exposure_time", spectrum->getExposureTime());
    query.bindValue(":data_file", dataFile);
    query.bindValue(":barycentric_corrected",
                    spectrum->isBarycentricallyCorrected() ? 1 : 0);
    query.bindValue(":is_flagged",
                    spectrum->isFlagged() ? 1 : 0);

    if (!query.exec()) {
        qDebug() << "Failed to save spectrum:" << query.lastError();
        return false;
    }

    for (const auto& fit : spectrum->getSpectralFits()) {
        saveSpectralFit(starId, spectrum->getId(), fit);
    }
    return true;
}

bool SpectrumRepository::saveSpectralFit(const QString& starId,
                                      const QString& spectrumId,
                                      std::shared_ptr<SpectralFit> fit)
{
    if (fit->getId().isEmpty()) {
        fit->setId(_db.generateUUID());
    }

    QString dataDir  = QFileInfo(_db.databasePath()).absolutePath() + "/data";
    QString modelFile = DataStore::spectralFitPath(dataDir, starId,
                                                   spectrumId, fit->getId());
    fit->saveDataToFile(modelFile);

    QString oldFile = fit->getModelDataFile();
    if (!oldFile.isEmpty() && oldFile != modelFile && QFile::exists(oldFile)) {
        QFile::remove(oldFile);
    }

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO spectral_fits (
            id, spectrum_id, creation_date, model_id, is_best_fit, is_flagged,
            teff, teff_error, logg, logg_error, he, he_error,
            vsini, vsini_error, radial_velocity, radial_velocity_error,
            chi2, metallicity, metallicity_error,
            macroturbulence, macroturbulence_error,
            microturbulence, microturbulence_error,
            model_data_file
        ) VALUES (
            :id, :spectrum_id, :creation_date, :model_id, :is_best_fit, :is_flagged,
            :teff, :teff_error, :logg, :logg_error, :he, :he_error,
            :vsini, :vsini_error, :radial_velocity, :radial_velocity_error,
            :chi2, :metallicity, :metallicity_error,
            :macroturbulence, :macroturbulence_error,
            :microturbulence, :microturbulence_error,
            :model_data_file
        )
    )");

    query.bindValue(":id", fit->getId());
    query.bindValue(":spectrum_id", spectrumId);
    query.bindValue(":creation_date", fit->creationDate.toString(Qt::ISODate));
    query.bindValue(":model_id", fit->modelId);
    query.bindValue(":is_best_fit", fit->isBestFit ? 1 : 0);
    query.bindValue(":is_flagged", fit->isFlagged ? 1 : 0);
    query.bindValue(":teff", fit->teff);
    query.bindValue(":teff_error", fit->teffError);
    query.bindValue(":logg", fit->logg);
    query.bindValue(":logg_error", fit->loggError);
    query.bindValue(":he", fit->he);
    query.bindValue(":he_error", fit->heError);
    query.bindValue(":vsini", fit->vsini);
    query.bindValue(":vsini_error", fit->vsiniError);
    query.bindValue(":radial_velocity", fit->radialVelocity);
    query.bindValue(":radial_velocity_error", fit->radialVelocityError);
    query.bindValue(":chi2", fit->chi2);
    query.bindValue(":metallicity", fit->metallicity);
    query.bindValue(":metallicity_error", fit->metallicityError);
    query.bindValue(":macroturbulence", fit->macroturbulence);
    query.bindValue(":macroturbulence_error", fit->macroturbulenceError);
    query.bindValue(":microturbulence", fit->microturbulence);
    query.bindValue(":microturbulence_error", fit->microturbulenceError);
    query.bindValue(":model_data_file", modelFile);

    return query.exec();
}

std::vector<std::shared_ptr<Spectrum>> SpectrumRepository::loadSpectra(const QString& starId)
{
    std::vector<std::shared_ptr<Spectrum>> spectra;

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        SELECT * FROM spectra WHERE star_id = :star_id
    )");
    query.bindValue(":star_id", starId);

    if (!query.exec()) {
        qDebug() << "Failed to load spectra:" << query.lastError();
        return spectra;
    }

    while (query.next()) {
        auto spectrum = std::make_shared<Spectrum>();
        spectrum->setId(query.value("id").toString());
        spectrum->setFile(query.value("file").toString());
        spectrum->setInstrument(query.value("instrument").toString());
        double expTime = query.value("exposure_time").toDouble();
        spectrum->setTime(Time::fromMjdBjd(
            query.value("mjd").toDouble(),
            query.value("bjd").toDouble(),
            expTime > 0.0 ? expTime : -1.0));
        spectrum->setDataFile(query.value("data_file").toString());
        spectrum->setBarycentricallyCorrected(query.value("barycentric_corrected").toInt() != 0);
        spectrum->setFlagged(query.value("barycentric_corrected").toInt() != 0);

        // Load spectral fits
        auto fits = loadSpectralFits(spectrum->getId());
        for (const auto& fit : fits) {
            spectrum->addSpectralFit(fit);
        }

        spectra.push_back(spectrum);
    }

    return spectra;
}

std::vector<std::shared_ptr<SpectralFit>> SpectrumRepository::loadSpectralFits(const QString& spectrumId)
{
    std::vector<std::shared_ptr<SpectralFit>> fits;

    QSqlQuery query(_db.threadConnection());
    query.prepare("SELECT * FROM spectral_fits WHERE spectrum_id = :spectrum_id");
    query.bindValue(":spectrum_id", spectrumId);

    if (!query.exec()) {
        return fits;
    }

    while (query.next()) {
        auto fit = std::make_shared<SpectralFit>();
        fit->setId(query.value("id").toString());
        fit->creationDate = QDateTime::fromString(query.value("creation_date").toString(), Qt::ISODate);
        fit->modelId = query.value("model_id").toString();
        fit->isBestFit = query.value("is_best_fit").toInt() == 1;
        fit->isFlagged = query.value("is_flagged").toInt() == 1;
        fit->teff = query.value("teff").toDouble();
        fit->teffError = query.value("teff_error").toDouble();
        fit->logg = query.value("logg").toDouble();
        fit->loggError = query.value("logg_error").toDouble();
        fit->he = query.value("he").toDouble();
        fit->heError = query.value("he_error").toDouble();
        fit->vsini = query.value("vsini").toDouble();
        fit->vsiniError = query.value("vsini_error").toDouble();
        fit->radialVelocity = query.value("radial_velocity").toDouble();
        fit->radialVelocityError = query.value("radial_velocity_error").toDouble();
        fit->chi2 = query.value("chi2").toDouble();
        fit->metallicity = query.value("metallicity").toDouble();
        fit->metallicityError = query.value("metallicity_error").toDouble();
        fit->macroturbulence = query.value("macroturbulence").toDouble();
        fit->macroturbulenceError = query.value("macroturbulence_error").toDouble();
        fit->microturbulence = query.value("microturbulence").toDouble();
        fit->microturbulenceError = query.value("microturbulence_error").toDouble();
        fit->setModelDataFile(query.value("model_data_file").toString());

        fits.push_back(fit);
    }

    return fits;
}

bool SpectrumRepository::updateSpectrumFlag(const QString& spectrumId, bool flagged)
{
    QSqlQuery q(_db.database());
    q.prepare("UPDATE spectra SET is_flagged = :f WHERE id = :id");
    q.bindValue(":f", flagged ? 1 : 0);
    q.bindValue(":id", spectrumId);
    return q.exec();
}

bool SpectrumRepository::updateSpectralFitFlag(const QString& fitId, bool flagged)
{
    QSqlQuery q(_db.database());
    q.prepare("UPDATE spectral_fits SET is_flagged = :f WHERE id = :id");
    q.bindValue(":f", flagged ? 1 : 0);
    q.bindValue(":id", fitId);
    return q.exec();
}

bool SpectrumRepository::updateBestFit(const QString& spectrumId, const QString& bestFitId)
{
    QSqlQuery q(_db.database());
    // Clear for all fits of this spectrum
    q.prepare("UPDATE spectral_fits SET is_best_fit = 0 WHERE spectrum_id = :sid");
    q.bindValue(":sid", spectrumId);
    if (!q.exec()) return false;
    if (bestFitId.isEmpty()) return true;
    q.prepare("UPDATE spectral_fits SET is_best_fit = 1 WHERE id = :id");
    q.bindValue(":id", bestFitId);
    return q.exec();
}