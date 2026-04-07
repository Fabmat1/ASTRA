#include "InstrumentRepository.h"
#include "DBAccess.h"
#include <QUuid>
#include "models/Instrument.h"
#include "models/InstrumentMode.h"
#include <QSqlQuery>
#include <QSqlError>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>

static const QUuid ASTRA_INSTRUMENT_NS("f47ac10b-58cc-4372-a567-0e02b2c3d479");

InstrumentRepository::InstrumentRepository(DBAccess& db) : _db(db) {}

QString InstrumentRepository::instrumentUUID(const QString& name)
{
    return QUuid::createUuidV5(ASTRA_INSTRUMENT_NS, name)
        .toString(QUuid::WithoutBraces);
}

void InstrumentRepository::initializeInstruments()
{
    loadInstrumentsFromDatabase();
    if (_instrumentsById.isEmpty())
        loadDefaultInstruments();
}

void InstrumentRepository::loadDefaultInstruments()
{
    QFile file(QStringLiteral(":/data/default_instruments.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "DatabaseManager: cannot open default_instruments.json";
        return;
    }

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    if (doc.isNull()) {
        qWarning() << "DatabaseManager: JSON parse error:" << err.errorString();
        return;
    }

    QSqlDatabase db = _db.threadConnection();
    db.transaction();

    const QJsonArray instruments = doc.object().value("instruments").toArray();
    for (const auto& val : instruments) {
        Instrument inst = Instrument::fromJson(val.toObject());
        inst.setId(instrumentUUID(inst.getName()));
        inst.setBuiltin(true);

        if (writeInstrumentToDb(inst) && writeModesToDb(inst.getId(), inst.modes())) {
            auto ptr = std::make_shared<Instrument>(inst);
            cacheInstrument(ptr);
        }
    }

    db.commit();
}

void InstrumentRepository::loadInstrumentsFromDatabase()
{
    _instrumentsById.clear();
    _instrumentsByName.clear();

    QSqlQuery q(_db.threadConnection());
    q.exec(QStringLiteral("SELECT * FROM instruments"));

    while (q.next()) {
        auto inst = std::make_shared<Instrument>();
        inst->setId(q.value("id").toString());
        inst->setName(q.value("name").toString());
        inst->setFullName(q.value("full_name").toString());
        inst->setLatitude(q.value("latitude").toDouble());
        inst->setLongitude(q.value("longitude").toDouble());
        inst->setAltitude(q.value("altitude").toDouble());
        inst->setSpaceBased(q.value("space_based").toInt() != 0);
        inst->setBuiltin(q.value("is_builtin").toInt() != 0);

        QSqlQuery mq(_db.threadConnection());
        mq.prepare(QStringLiteral(
            "SELECT * FROM instrument_modes WHERE instrument_id = :id"));
        mq.bindValue(":id", inst->getId());
        mq.exec();

        while (mq.next()) {
            InstrumentMode mode;
            mode.setKey(mq.value("key").toString());
            mode.setDisplayName(mq.value("display_name").toString());
            mode.setDescription(mq.value("description").toString());
            mode.setDataType(InstrumentMode::dataTypeFromString(
                mq.value("data_type").toString()));

            QString spJson = mq.value("spectral_json").toString();
            if (!spJson.isEmpty())
                mode.setSpectralProperties(SpectralProperties::fromJson(
                    QJsonDocument::fromJson(spJson.toUtf8()).object()));

            QString phJson = mq.value("photometric_json").toString();
            if (!phJson.isEmpty())
                mode.setPhotometricProperties(PhotometricProperties::fromJson(
                    QJsonDocument::fromJson(phJson.toUtf8()).object()));

            QString exJson = mq.value("extras_json").toString();
            if (!exJson.isEmpty())
                mode.extras() = QJsonDocument::fromJson(exJson.toUtf8())
                                    .object().toVariantMap();

            inst->addMode(mode);
        }

        cacheInstrument(inst);
    }
}

bool InstrumentRepository::writeInstrumentToDb(const Instrument& inst)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare(R"(
        INSERT OR REPLACE INTO instruments
        (id, name, full_name, latitude, longitude, altitude, space_based, is_builtin)
        VALUES (:id, :name, :full_name, :lat, :lon, :alt, :space, :builtin)
    )");
    q.bindValue(":id",        inst.getId());
    q.bindValue(":name",      inst.getName());
    q.bindValue(":full_name", inst.getFullName());
    q.bindValue(":lat",       inst.getLatitude());
    q.bindValue(":lon",       inst.getLongitude());
    q.bindValue(":alt",       inst.getAltitude());
    q.bindValue(":space",     inst.isSpaceBased() ? 1 : 0);
    q.bindValue(":builtin",   inst.isBuiltin()    ? 1 : 0);

    if (!q.exec()) {
        qWarning() << "Failed to write instrument:" << q.lastError();
        return false;
    }
    return true;
}

bool InstrumentRepository::writeModesToDb(const QString& instrumentId,
                                     const QList<InstrumentMode>& modes)
{
    QSqlQuery dq(_db.threadConnection());
    dq.prepare(QStringLiteral(
        "DELETE FROM instrument_modes WHERE instrument_id = :id"));
    dq.bindValue(":id", instrumentId);
    dq.exec();

    auto compact = [](const QJsonObject& obj) -> QVariant {
        if (obj.isEmpty()) return QVariant();
        return QString::fromUtf8(
            QJsonDocument(obj).toJson(QJsonDocument::Compact));
    };

    QSqlQuery q(_db.threadConnection());
    q.prepare(R"(
        INSERT INTO instrument_modes
        (instrument_id, key, display_name, description, data_type,
         spectral_json, photometric_json, extras_json)
        VALUES (:inst_id, :key, :display, :desc, :dtype,
                :spectral, :photometric, :extras)
    )");

    for (const auto& mode : modes) {
        q.bindValue(":inst_id",     instrumentId);
        q.bindValue(":key",         mode.key());
        q.bindValue(":display",     mode.displayName());
        q.bindValue(":desc",        mode.description());
        q.bindValue(":dtype",       InstrumentMode::dataTypeToString(mode.dataType()));
        q.bindValue(":spectral",    mode.hasSpectralProperties()
                                        ? compact(mode.spectral().toJson())
                                        : QVariant());
        q.bindValue(":photometric", mode.hasPhotometricProperties()
                                        ? compact(mode.photometric().toJson())
                                        : QVariant());
        q.bindValue(":extras",      !mode.extras().isEmpty()
                                        ? compact(QJsonObject::fromVariantMap(mode.extras()))
                                        : QVariant());
        if (!q.exec()) {
            qWarning() << "Failed to write mode" << mode.key()
                       << ":" << q.lastError();
            return false;
        }
    }
    return true;
}

void InstrumentRepository::cacheInstrument(std::shared_ptr<Instrument> inst)
{
    _instrumentsById[inst->getId()] = inst;
    _instrumentsByName[inst->getName().toLower()] = inst;
}

void InstrumentRepository::uncacheInstrument(const QString& id)
{
    auto it = _instrumentsById.find(id);
    if (it != _instrumentsById.end()) {
        _instrumentsByName.remove(it.value()->getName().toLower());
        _instrumentsById.erase(it);
    }
}

std::shared_ptr<Instrument> InstrumentRepository::getInstrumentById(const QString& id) const
{
    return _instrumentsById.value(id);
}

std::shared_ptr<Instrument> InstrumentRepository::getInstrumentByName(const QString& name) const
{
    return _instrumentsByName.value(name.toLower());
}

std::vector<std::shared_ptr<Instrument>> InstrumentRepository::getAllInstruments() const
{
    auto values = _instrumentsById.values();
    return std::vector<std::shared_ptr<Instrument>>(values.begin(), values.end());
}

bool InstrumentRepository::saveInstrument(std::shared_ptr<Instrument> instrument)
{
    if (!instrument) return false;

    if (instrument->getId().isEmpty())
        instrument->setId(QUuid::createUuid().toString(QUuid::WithoutBraces));

    uncacheInstrument(instrument->getId());

    QSqlDatabase db = _db.threadConnection();
    db.transaction();

    if (!writeInstrumentToDb(*instrument) ||
        !writeModesToDb(instrument->getId(), instrument->modes())) {
        db.rollback();
        return false;
    }

    db.commit();
    cacheInstrument(instrument);
    return true;
}

bool InstrumentRepository::updateInstrument(std::shared_ptr<Instrument> instrument)
{
    if (!instrument || instrument->getId().isEmpty()) return false;
    return saveInstrument(instrument);
}

bool InstrumentRepository::deleteInstrument(const QString& id)
{
    QSqlQuery q(_db.threadConnection());
    q.prepare("DELETE FROM instruments WHERE id = :id");
    q.bindValue(":id", id);

    if (!q.exec()) {
        qWarning() << "Failed to delete instrument:" << q.lastError();
        return false;
    }

    uncacheInstrument(id);
    return true;
}

std::shared_ptr<Instrument> InstrumentRepository::resolveInstrumentString(
    const QString& input, QString* modeKey) const
{
    // Try exact instrument name first
    auto inst = getInstrumentByName(input);
    if (inst) {
        if (modeKey) modeKey->clear();
        return inst;
    }

    // Try "INSTRUMENT_MODE" pattern — split on underscore from left
    int sep = input.indexOf('_');
    while (sep > 0) {
        QString instPart = input.left(sep);
        QString modePart = input.mid(sep + 1);

        inst = getInstrumentByName(instPart);
        if (inst && inst->hasMode(modePart)) {
            if (modeKey) *modeKey = modePart;
            return inst;
        }

        // Handle instrument names containing underscores (e.g. "X-Shooter")
        sep = input.indexOf('_', sep + 1);
    }

    // Try every cached instrument to see if input matches a mode key
    for (auto it = _instrumentsById.constBegin();
         it != _instrumentsById.constEnd(); ++it) {
        if (it.value()->hasMode(input)) {
            if (modeKey) *modeKey = input;
            return it.value();
        }
    }

    return nullptr;
}

void InstrumentRepository::restoreDefaultInstruments()
{
    QSqlDatabase db = _db.threadConnection();
    db.transaction();

    QSqlQuery q(db);
    q.exec(QStringLiteral("DELETE FROM instrument_modes"));
    q.exec(QStringLiteral("DELETE FROM instruments"));

    db.commit();

    _instrumentsById.clear();
    _instrumentsByName.clear();

    loadDefaultInstruments();
}
