#include "SpectrumRepository.h"
#include "DBAccess.h"
#include "SqlValue.h"
#include "models/Spectrum.h"
#include "utils/Logger.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <limits>
#include "utils/DataStore.h"

SpectrumRepository::SpectrumRepository(DBAccess& db) : _db(db) {}

// ── Fitted abundances ⇄ abundances_json ─────────────────────────────────────
// Both components share one column; a column per element per component would
// be 144 of them. Shape:
//   {"c1":{"FE":{"v":-4.77,"e":0.18,"b":0,"f":false}, …},"c2":{…}}
// "b" is the limit side (see FittedAbundance), "f" whether the parameter was
// frozen. "c2" is absent for a one-component fit, and an element without a
// value is left out rather than written as null.
static QJsonObject abundancesToJson(const QMap<QString, FittedAbundance>& abundances)
{
    QJsonObject out;
    for (auto it = abundances.constBegin(); it != abundances.constEnd(); ++it) {
        if (!it->isSet()) continue;
        QJsonObject entry;
        entry["v"] = it->value;
        entry["e"] = it->error;
        entry["b"] = it->limitSide;
        entry["f"] = it->frozen;
        out.insert(it.key(), entry);
    }
    return out;
}

static QMap<QString, FittedAbundance> abundancesFromJson(const QJsonObject& obj)
{
    QMap<QString, FittedAbundance> abundances;
    for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
        const QJsonObject entry = it->toObject();
        FittedAbundance a;
        a.value     = entry.value("v").toDouble(
                          std::numeric_limits<double>::quiet_NaN());
        a.error     = entry.value("e").toDouble(0.0);
        a.limitSide = entry.value("b").toInt(0);
        a.frozen    = entry.value("f").toBool(false);
        abundances.insert(it.key(), a);
    }
    return abundances;
}

static QString abundancesColumnValue(const SpectralFit& fit)
{
    QJsonObject root;
    const QJsonObject c1 = abundancesToJson(fit.abundances);
    if (!c1.isEmpty()) root.insert("c1", c1);
    if (fit.nComponents >= 2) {
        const QJsonObject c2 = abundancesToJson(fit.abundances2);
        if (!c2.isEmpty()) root.insert("c2", c2);
    }
    if (root.isEmpty()) return QString();
    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

bool SpectrumRepository::saveSpectrum(const QString            &starId,
                                    std::shared_ptr<Spectrum> spectrum,
                                    bool cascadeFits) {
    if (spectrum->getId().isEmpty()) {
        spectrum->setId(_db.generateUUID());
    }

    QString dataDir = QFileInfo(_db.databasePath()).absolutePath() + "/data";
    QString dataFile =
        DataStore::spectrumPath(dataDir, starId, spectrum->getId());
    const QString existingFile = spectrum->getDataFile();

    if (spectrum->hasData()) {
        if (!spectrum->saveDataToFile(dataFile)) {
            LOG_ERROR("SpectrumRepo", QString("Refused/failed to write data "
                                              "for spectrum %1; aborting save")
                                          .arg(spectrum->getId()));
            return false;
        }
        if (!existingFile.isEmpty() && existingFile != dataFile &&
            QFile::exists(existingFile))
            QFile::remove(existingFile);
        spectrum->setDataFile(dataFile);
    } else {
        if (!existingFile.isEmpty()) {
            dataFile = existingFile;
        } else {
            LOG_WARNING("SpectrumRepo",
                        QString("Spectrum %1 has no data and no existing file; "
                                "row will reference "
                                "a missing data file")
                            .arg(spectrum->getId()));
        }
    }

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        INSERT OR REPLACE INTO spectra (
            id, star_id, file, instrument, instrument_id, mode_key,
            mjd, bjd, exposure_time,
            data_file, barycentric_corrected, is_flagged,
            origin, origin_id, is_coadd, origin_meta
        ) VALUES (
            :id, :star_id, :file, :instrument, :instrument_id, :mode_key,
            :mjd, :bjd, :exposure_time,
            :data_file, :barycentric_corrected, :is_flagged,
            :origin, :origin_id, :is_coadd, :origin_meta
        )
    )");
    
    query.bindValue(":id", spectrum->getId());
    query.bindValue(":star_id", starId);
    query.bindValue(":file", spectrum->getFile());
    query.bindValue(":instrument", spectrum->getInstrument());
    query.bindValue(":instrument_id",
                    spectrum->getInstrumentId().isEmpty()
                        ? QVariant(QMetaType(QMetaType::QString))
                        : QVariant(spectrum->getInstrumentId()));
    query.bindValue(":mode_key",
                    spectrum->getModeKey().isEmpty()
                        ? QVariant(QMetaType(QMetaType::QString))
                        : QVariant(spectrum->getModeKey()));
    query.bindValue(":mjd", spectrum->getMJD());
    query.bindValue(":bjd", spectrum->getBJD());
    query.bindValue(":exposure_time", spectrum->getExposureTime());
    query.bindValue(":data_file", dataFile);
    query.bindValue(":barycentric_corrected",
                    spectrum->isBarycentricallyCorrected() ? 1 : 0);
    query.bindValue(":is_flagged",
                    spectrum->isFlagged() ? 1 : 0);
    query.bindValue(":origin",
                    spectrum->getOrigin().isEmpty()
                        ? QVariant(QMetaType(QMetaType::QString))
                        : QVariant(spectrum->getOrigin()));
    query.bindValue(":origin_id",
                    spectrum->getOriginId().isEmpty()
                        ? QVariant(QMetaType(QMetaType::QString))
                        : QVariant(spectrum->getOriginId()));
    query.bindValue(":is_coadd", spectrum->isCoadd() ? 1 : 0);
    query.bindValue(":origin_meta",
                    spectrum->getOriginMeta().isEmpty()
                        ? QVariant(QMetaType(QMetaType::QString))
                        : QVariant(spectrum->getOriginMeta()));

    if (!query.exec()) {
        LOG_ERROR("Spectra", QString("Failed to save spectrum: %1").arg(query.lastError().text()));
        return false;
    }

    if (cascadeFits) {
        for (const auto &fit : spectrum->getSpectralFits()) {
            saveSpectralFit(starId, spectrum->getId(), fit);
        }
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
            teff_error_up, teff_error_down,
            logg_error_up, logg_error_down,
            he_error_up, he_error_down,
            vsini_error_up, vsini_error_down,
            radial_velocity_error_up, radial_velocity_error_down,
            metallicity_error_up, metallicity_error_down,
            macroturbulence_error_up, macroturbulence_error_down,
            microturbulence_error_up, microturbulence_error_down,
            n_components,
            teff2, teff2_error, logg2, logg2_error, he2, he2_error,
            vsini2, vsini2_error, radial_velocity2, radial_velocity2_error,
            metallicity2, metallicity2_error,
            macroturbulence2, macroturbulence2_error,
            microturbulence2, microturbulence2_error,
            sur_ratio, sur_ratio_error,
            abundances_json,
            has_telluric, telluric_airmass, telluric_airmass_error,
            telluric_pwv, telluric_pwv_error, telluric_barycorr,
            model_data_file
        ) VALUES (
            :id, :spectrum_id, :creation_date, :model_id, :is_best_fit, :is_flagged,
            :teff, :teff_error, :logg, :logg_error, :he, :he_error,
            :vsini, :vsini_error, :radial_velocity, :radial_velocity_error,
            :chi2, :metallicity, :metallicity_error,
            :macroturbulence, :macroturbulence_error,
            :microturbulence, :microturbulence_error,
            :teff_error_up, :teff_error_down,
            :logg_error_up, :logg_error_down,
            :he_error_up, :he_error_down,
            :vsini_error_up, :vsini_error_down,
            :radial_velocity_error_up, :radial_velocity_error_down,
            :metallicity_error_up, :metallicity_error_down,
            :macroturbulence_error_up, :macroturbulence_error_down,
            :microturbulence_error_up, :microturbulence_error_down,
            :n_components,
            :teff2, :teff2_error, :logg2, :logg2_error, :he2, :he2_error,
            :vsini2, :vsini2_error, :radial_velocity2, :radial_velocity2_error,
            :metallicity2, :metallicity2_error,
            :macroturbulence2, :macroturbulence2_error,
            :microturbulence2, :microturbulence2_error,
            :sur_ratio, :sur_ratio_error,
            :abundances_json,
            :has_telluric, :telluric_airmass, :telluric_airmass_error,
            :telluric_pwv, :telluric_pwv_error, :telluric_barycorr,
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
    query.bindValue(":teff_error_up", SqlValue::fromDouble(fit->teffErrorUp));
    query.bindValue(":teff_error_down", SqlValue::fromDouble(fit->teffErrorDown));
    query.bindValue(":logg_error_up", SqlValue::fromDouble(fit->loggErrorUp));
    query.bindValue(":logg_error_down", SqlValue::fromDouble(fit->loggErrorDown));
    query.bindValue(":he_error_up", SqlValue::fromDouble(fit->heErrorUp));
    query.bindValue(":he_error_down", SqlValue::fromDouble(fit->heErrorDown));
    query.bindValue(":vsini_error_up", SqlValue::fromDouble(fit->vsiniErrorUp));
    query.bindValue(":vsini_error_down", SqlValue::fromDouble(fit->vsiniErrorDown));
    query.bindValue(":radial_velocity_error_up", SqlValue::fromDouble(fit->radialVelocityErrorUp));
    query.bindValue(":radial_velocity_error_down", SqlValue::fromDouble(fit->radialVelocityErrorDown));
    query.bindValue(":metallicity_error_up", SqlValue::fromDouble(fit->metallicityErrorUp));
    query.bindValue(":metallicity_error_down", SqlValue::fromDouble(fit->metallicityErrorDown));
    query.bindValue(":macroturbulence_error_up", SqlValue::fromDouble(fit->macroturbulenceErrorUp));
    query.bindValue(":macroturbulence_error_down", SqlValue::fromDouble(fit->macroturbulenceErrorDown));
    query.bindValue(":microturbulence_error_up", SqlValue::fromDouble(fit->microturbulenceErrorUp));
    query.bindValue(":microturbulence_error_down", SqlValue::fromDouble(fit->microturbulenceErrorDown));

    // Component 2 and the telluric parameters follow the NaN→NULL convention:
    // an unset value must not read back as a measured zero.
    query.bindValue(":n_components", fit->nComponents);
    query.bindValue(":teff2", SqlValue::fromDouble(fit->teff2));
    query.bindValue(":teff2_error", SqlValue::fromDouble(fit->teff2Error));
    query.bindValue(":logg2", SqlValue::fromDouble(fit->logg2));
    query.bindValue(":logg2_error", SqlValue::fromDouble(fit->logg2Error));
    query.bindValue(":he2", SqlValue::fromDouble(fit->he2));
    query.bindValue(":he2_error", SqlValue::fromDouble(fit->he2Error));
    query.bindValue(":vsini2", SqlValue::fromDouble(fit->vsini2));
    query.bindValue(":vsini2_error", SqlValue::fromDouble(fit->vsini2Error));
    query.bindValue(":radial_velocity2", SqlValue::fromDouble(fit->radialVelocity2));
    query.bindValue(":radial_velocity2_error", SqlValue::fromDouble(fit->radialVelocity2Error));
    query.bindValue(":metallicity2", SqlValue::fromDouble(fit->metallicity2));
    query.bindValue(":metallicity2_error", SqlValue::fromDouble(fit->metallicity2Error));
    query.bindValue(":macroturbulence2", SqlValue::fromDouble(fit->macroturbulence2));
    query.bindValue(":macroturbulence2_error", SqlValue::fromDouble(fit->macroturbulence2Error));
    query.bindValue(":microturbulence2", SqlValue::fromDouble(fit->microturbulence2));
    query.bindValue(":microturbulence2_error", SqlValue::fromDouble(fit->microturbulence2Error));
    query.bindValue(":sur_ratio", SqlValue::fromDouble(fit->surRatio));
    query.bindValue(":sur_ratio_error", SqlValue::fromDouble(fit->surRatioError));

    const QString abundancesJson = abundancesColumnValue(*fit);
    query.bindValue(":abundances_json",
                    abundancesJson.isEmpty() ? QVariant() : QVariant(abundancesJson));

    query.bindValue(":has_telluric", fit->hasTelluric ? 1 : 0);
    query.bindValue(":telluric_airmass", SqlValue::fromDouble(fit->telluricAirmass));
    query.bindValue(":telluric_airmass_error", SqlValue::fromDouble(fit->telluricAirmassError));
    query.bindValue(":telluric_pwv", SqlValue::fromDouble(fit->telluricPwv));
    query.bindValue(":telluric_pwv_error", SqlValue::fromDouble(fit->telluricPwvError));
    query.bindValue(":telluric_barycorr", SqlValue::fromDouble(fit->telluricBarycorr));

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
        LOG_ERROR("Spectra", QString("Failed to load spectra: %1").arg(query.lastError().text()));
        return spectra;
    }

    while (query.next()) {
        auto spectrum = std::make_shared<Spectrum>();
        spectrum->setId(query.value("id").toString());
        spectrum->setFile(query.value("file").toString());
        spectrum->setInstrument(query.value("instrument").toString());
        spectrum->setInstrumentId(query.value("instrument_id").toString());
        spectrum->setModeKey(query.value("mode_key").toString());
        double expTime = query.value("exposure_time").toDouble();
        spectrum->setTime(Time::fromMjdBjd(
            query.value("mjd").toDouble(),
            query.value("bjd").toDouble(),
            expTime > 0.0 ? expTime : -1.0));
        spectrum->setDataFile(query.value("data_file").toString());
        spectrum->setBarycentricallyCorrected(query.value("barycentric_corrected").toInt() != 0);
        spectrum->setFlagged(query.value("is_flagged").toInt() != 0);
        spectrum->setOrigin(query.value("origin").toString());
        spectrum->setOriginId(query.value("origin_id").toString());
        spectrum->setIsCoadd(query.value("is_coadd").isNull()
                                 || query.value("is_coadd").toInt() != 0);
        spectrum->setOriginMeta(query.value("origin_meta").toString());

        // Load spectral fits
        auto fits = loadSpectralFits(spectrum->getId());
        for (const auto& fit : fits) {
            spectrum->addSpectralFit(fit);
        }

        spectra.push_back(spectrum);
    }

    return spectra;
}

std::vector<SpectrumIndexRow>
SpectrumRepository::loadSpectraIndex(const QString &projectId) {
    std::vector<SpectrumIndexRow> rows;

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        SELECT s.id, s.star_id, s.file
        FROM spectra s
        JOIN stars st ON st.id = s.star_id
        WHERE st.project_id = :project_id
    )");
    query.bindValue(":project_id", projectId);

    if (!query.exec()) {
        LOG_ERROR("Spectra", QString("Failed to load spectra index: %1").arg(query.lastError().text()));
        return rows;
    }

    // Use positional indices: fastest, avoids column-name hashing per row
    const int idCol     = 0; // s.id
    const int starIdCol = 1; // s.star_id
    const int fileCol   = 2; // s.file

    while (query.next()) {
        SpectrumIndexRow row;
        row.spectrumId = query.value(idCol).toString();
        row.starId     = query.value(starIdCol).toString();
        row.file       = query.value(fileCol).toString();
        rows.push_back(std::move(row));
    }

    return rows;
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
        fit->teffErrorUp = SqlValue::toDoubleOrNaN(query, "teff_error_up");
        fit->teffErrorDown = SqlValue::toDoubleOrNaN(query, "teff_error_down");
        fit->loggErrorUp = SqlValue::toDoubleOrNaN(query, "logg_error_up");
        fit->loggErrorDown = SqlValue::toDoubleOrNaN(query, "logg_error_down");
        fit->heErrorUp = SqlValue::toDoubleOrNaN(query, "he_error_up");
        fit->heErrorDown = SqlValue::toDoubleOrNaN(query, "he_error_down");
        fit->vsiniErrorUp = SqlValue::toDoubleOrNaN(query, "vsini_error_up");
        fit->vsiniErrorDown = SqlValue::toDoubleOrNaN(query, "vsini_error_down");
        fit->radialVelocityErrorUp = SqlValue::toDoubleOrNaN(query, "radial_velocity_error_up");
        fit->radialVelocityErrorDown = SqlValue::toDoubleOrNaN(query, "radial_velocity_error_down");
        fit->metallicityErrorUp = SqlValue::toDoubleOrNaN(query, "metallicity_error_up");
        fit->metallicityErrorDown = SqlValue::toDoubleOrNaN(query, "metallicity_error_down");
        fit->macroturbulenceErrorUp = SqlValue::toDoubleOrNaN(query, "macroturbulence_error_up");
        fit->macroturbulenceErrorDown = SqlValue::toDoubleOrNaN(query, "macroturbulence_error_down");
        fit->microturbulenceErrorUp = SqlValue::toDoubleOrNaN(query, "microturbulence_error_up");
        fit->microturbulenceErrorDown = SqlValue::toDoubleOrNaN(query, "microturbulence_error_down");

        // Component 2: a fit written before these columns existed reads back
        // as a one-component fit, which is what it was.
        const QVariant nComponents = query.value("n_components");
        fit->nComponents = nComponents.isNull() || nComponents.toInt() < 1
                               ? 1 : nComponents.toInt();
        fit->teff2                 = SqlValue::toDoubleOrNaN(query, "teff2");
        fit->teff2Error            = query.value("teff2_error").toDouble();
        fit->logg2                 = SqlValue::toDoubleOrNaN(query, "logg2");
        fit->logg2Error            = query.value("logg2_error").toDouble();
        fit->he2                   = SqlValue::toDoubleOrNaN(query, "he2");
        fit->he2Error              = query.value("he2_error").toDouble();
        fit->vsini2                = SqlValue::toDoubleOrNaN(query, "vsini2");
        fit->vsini2Error           = query.value("vsini2_error").toDouble();
        fit->radialVelocity2       = SqlValue::toDoubleOrNaN(query, "radial_velocity2");
        fit->radialVelocity2Error  = query.value("radial_velocity2_error").toDouble();
        fit->metallicity2          = SqlValue::toDoubleOrNaN(query, "metallicity2");
        fit->metallicity2Error     = query.value("metallicity2_error").toDouble();
        fit->macroturbulence2      = SqlValue::toDoubleOrNaN(query, "macroturbulence2");
        fit->macroturbulence2Error = query.value("macroturbulence2_error").toDouble();
        fit->microturbulence2      = SqlValue::toDoubleOrNaN(query, "microturbulence2");
        fit->microturbulence2Error = query.value("microturbulence2_error").toDouble();
        fit->surRatio              = SqlValue::toDoubleOrNaN(query, "sur_ratio");
        fit->surRatioError         = query.value("sur_ratio_error").toDouble();

        const QString abundancesJson = query.value("abundances_json").toString();
        if (!abundancesJson.isEmpty()) {
            const QJsonObject root =
                QJsonDocument::fromJson(abundancesJson.toUtf8()).object();
            fit->abundances  = abundancesFromJson(root.value("c1").toObject());
            fit->abundances2 = abundancesFromJson(root.value("c2").toObject());
        }

        fit->hasTelluric          = query.value("has_telluric").toInt() == 1;
        fit->telluricAirmass      = SqlValue::toDoubleOrNaN(query, "telluric_airmass");
        fit->telluricAirmassError = query.value("telluric_airmass_error").toDouble();
        fit->telluricPwv          = SqlValue::toDoubleOrNaN(query, "telluric_pwv");
        fit->telluricPwvError     = query.value("telluric_pwv_error").toDouble();
        fit->telluricBarycorr     = SqlValue::toDoubleOrNaN(query, "telluric_barycorr");

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

bool SpectrumRepository::updateSpectrumInstrument(const QString& spectrumId,
                                                  const QString& instrument,
                                                  const QString& instrumentId,
                                                  const QString& modeKey)
{
    QSqlQuery q(_db.database());
    q.prepare("UPDATE spectra SET instrument = :inst, instrument_id = :iid, "
              "mode_key = :mk WHERE id = :id");
    q.bindValue(":inst", instrument);
    q.bindValue(":iid", instrumentId.isEmpty()
                            ? QVariant(QMetaType(QMetaType::QString))
                            : QVariant(instrumentId));
    q.bindValue(":mk", modeKey.isEmpty()
                           ? QVariant(QMetaType(QMetaType::QString))
                           : QVariant(modeKey));
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

bool SpectrumRepository::deleteSpectrum(const QString& spectrumId)
{
    auto fits = loadSpectralFits(spectrumId);

    QSqlQuery query(_db.threadConnection());

    query.prepare("DELETE FROM spectral_fits WHERE spectrum_id = :spectrum_id");
    query.bindValue(":spectrum_id", spectrumId);
    if (!query.exec()) {
        LOG_ERROR("Spectra", QString("Failed to delete spectral fits: %1").arg(query.lastError().text()));
        return false;
    }
    for (const auto& fit : fits) {
        const QString& modelFile = fit->getModelDataFile();
        if (!modelFile.isEmpty() && QFile::exists(modelFile))
            QFile::remove(modelFile);
    }

    query.prepare("SELECT data_file FROM spectra WHERE id = :id");
    query.bindValue(":id", spectrumId);
    if (!query.exec() || !query.next()) {
        LOG_ERROR("Spectra", QString("Failed to fetch spectrum for deletion: %1").arg(query.lastError().text()));
        return false;
    }
    QString dataFile = query.value("data_file").toString();

    query.prepare("DELETE FROM spectra WHERE id = :id");
    query.bindValue(":id", spectrumId);
    if (!query.exec()) {
        LOG_ERROR("Spectra", QString("Failed to delete spectrum: %1").arg(query.lastError().text()));
        return false;
    }

    if (!dataFile.isEmpty() && QFile::exists(dataFile))
        QFile::remove(dataFile);

    return true;
}

QSet<QString> SpectrumRepository::originIdsForProject(const QString& projectId)
{
    QSet<QString> ids;

    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        SELECT s.origin_id
        FROM spectra s
        JOIN stars st ON st.id = s.star_id
        WHERE st.project_id = :project_id AND s.origin_id IS NOT NULL
    )");
    query.bindValue(":project_id", projectId);

    if (!query.exec()) {
        LOG_ERROR("Spectra", QString("Failed to load spectrum origin ids: %1").arg(query.lastError().text()));
        return ids;
    }

    while (query.next()) {
        const QString id = query.value(0).toString();
        if (!id.isEmpty()) ids.insert(id);
    }
    return ids;
}

bool SpectrumRepository::deleteSpectraByOriginId(const QString& starId,
                                                 const QString& originId)
{
    if (originId.isEmpty()) return false;

    // Exposures carry the parent's origin_id with a "#expN" suffix; a forced
    // re-download of a product must clear those too.
    QSqlQuery query(_db.threadConnection());
    query.prepare(R"(
        SELECT id FROM spectra
        WHERE star_id = :star_id
          AND (origin_id = :origin_id OR origin_id LIKE :origin_prefix)
    )");
    query.bindValue(":star_id", starId);
    query.bindValue(":origin_id", originId);
    query.bindValue(":origin_prefix", originId + "#%");

    if (!query.exec()) {
        LOG_ERROR("Spectra", QString("Failed to look up spectra by origin: %1").arg(query.lastError().text()));
        return false;
    }

    QStringList spectrumIds;
    while (query.next())
        spectrumIds << query.value(0).toString();

    bool ok = true;
    for (const QString& id : spectrumIds)
        ok = deleteSpectrum(id) && ok;
    return ok;
}

bool SpectrumRepository::deleteSpectralFit(const QString& fitId)
{
    QSqlQuery query(_db.threadConnection());

    query.prepare("SELECT model_data_file FROM spectral_fits WHERE id = :id");
    query.bindValue(":id", fitId);
    if (!query.exec() || !query.next()) {
        LOG_ERROR("Spectra", QString("Failed to fetch spectral fit for deletion: %1").arg(query.lastError().text()));
        return false;
    }
    QString modelFile = query.value("model_data_file").toString();

    query.prepare("DELETE FROM spectral_fits WHERE id = :id");
    query.bindValue(":id", fitId);
    if (!query.exec()) {
        LOG_ERROR("Spectra", QString("Failed to delete spectral fit: %1").arg(query.lastError().text()));
        return false;
    }

    if (!modelFile.isEmpty() && QFile::exists(modelFile))
        QFile::remove(modelFile);

    return true;
}