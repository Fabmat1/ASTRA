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
#include <cmath>
#include <algorithm>
#include <QRegularExpression>

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

InstrumentModeMatch InstrumentRepository::matchSpectralProperties(
    const std::vector<std::shared_ptr<Instrument>>& instruments,
    const QString& instrumentHint,
    double wlMin, double wlMax, int numPoints)
{
    InstrumentModeMatch best;

    if (wlMin >= wlMax || numPoints < 2)
        return best;

    double deltaLambda = (wlMax - wlMin) / (numPoints - 1);
    double centralWl   = (wlMin + wlMax) / 2.0;
    double samplingRes = (deltaLambda > 0) ? centralWl / (2.0 * deltaLambda) : 0.0;

    // Build hint variants for flexible name matching
    QStringList hintParts;
    if (!instrumentHint.isEmpty()) {
        QString h = instrumentHint.trimmed();
        hintParts << h;
        if (h.contains('/')) {
            hintParts << h.section('/', 0, 0).trimmed();
            hintParts << h.section('/', 1).trimmed();
        }
        static QRegularExpression sepRe("[/\\-_\\s]+");
        for (const auto& part : h.split(sepRe, Qt::SkipEmptyParts))
            hintParts << part.trimmed();
        hintParts.removeDuplicates();
    }

    // Find candidate instruments that match the hint
    std::vector<std::shared_ptr<Instrument>> candidates;
    bool hintResolved = false;

    if (!hintParts.isEmpty()) {
        for (const auto& inst : instruments) {
            for (const QString& hint : hintParts) {
                if (hint.length() < 2) continue;
                if (inst->getName().compare(hint, Qt::CaseInsensitive) == 0 ||
                    inst->getFullName().contains(hint, Qt::CaseInsensitive))
                {
                    candidates.push_back(inst);
                    hintResolved = true;
                    break;
                }
            }
        }
    }

    if (!hintResolved)
        candidates = instruments;

    // Wavelength-overlap scoring helper
    auto scoreOverlap = [&](double mMin, double mMax) -> double {
        if (mMin >= mMax) return 0.0;
        double oMin = std::max(wlMin, mMin);
        double oMax = std::min(wlMax, mMax);
        if (oMin >= oMax) return 0.0;

        double overlap   = oMax - oMin;
        double specRange = wlMax - wlMin;
        double modeRange = mMax - mMin;
        double specCov = (specRange > 0) ? overlap / specRange : 0.0;
        double modeCov = (modeRange > 0) ? overlap / modeRange : 0.0;
        return std::sqrt(specCov * modeCov);
    };

    double bestScore = -1.0;

    for (const auto& inst : candidates) {
        for (const auto& mode : inst->modes()) {
            if (!mode.hasSpectralProperties()) continue;
            const auto& sp = mode.spectral();

            // Wavelength overlap — check broad range and each setup
            double wlScore = scoreOverlap(sp.wavelengthMin, sp.wavelengthMax);
            if (wlScore < 0.05) continue;

            for (const auto& setup : sp.commonSetups) {
                if (setup.wavelengthMin > 0 && setup.wavelengthMax > setup.wavelengthMin)
                    wlScore = std::max(wlScore,
                                       scoreOverlap(setup.wavelengthMin, setup.wavelengthMax));
            }

            // Resolution compatibility from sampling
            double resScore = 0.5;
            if (samplingRes > 0 && sp.resolution.isValid()) {
                double modeRes = sp.resolution.at(centralWl);
                if (modeRes > 0) {
                    if (modeRes > samplingRes * 20.0)
                        resScore = 0.05;           // mode far beyond sampling capability
                    else if (modeRes > samplingRes)
                        resScore = 0.3 + 0.7 * (samplingRes / modeRes);
                    else
                        resScore = 1.0;            // oversampled — fine
                }
            }

            double hintBonus = hintResolved ? 0.15 : 0.0;
            double total = wlScore * 0.55 + resScore * 0.30 + hintBonus;

            if (total > bestScore) {
                bestScore        = total;
                best.instrument  = inst;
                best.modeKey     = mode.key();
                best.displayString = inst->getName() + "/" + mode.key();
                best.confidence  = std::min(1.0, total);
            }
        }
    }

    return best;
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

std::shared_ptr<Instrument>
InstrumentRepository::resolveInstrumentString(const QString& input,
                                               QString* modeKey) const
{
    if (modeKey) modeKey->clear();
    QString s = input.trimmed();
    if (s.isEmpty()) return nullptr;

    // ── Hardcoded aliases for known legacy strings ──
    // Keyed on case-insensitive exact match of the input.
    struct Alias { const char* from; const char* instr; const char* mode; };
    static const Alias kAliases[] = {
        { "LAMOST_LORES", "LAMOST", "LRS" },
        { "LAMOST_HIRES", "LAMOST", "MRS_blue" },  // ← confirm / split if needed
        { "EFOSC",        "EFOSC2", ""          },
    };
    for (const auto& a : kAliases) {
        if (s.compare(a.from, Qt::CaseInsensitive) == 0) {
            auto inst = getInstrumentByName(a.instr);
            if (inst) {
                if (modeKey && a.mode[0]) *modeKey = a.mode;
                return inst;
            }
        }
    }

    // ── Split into candidate (name, mode) tokens on / _ - whitespace ──
    static const QRegularExpression sep("[/\\-_\\s]+");
    const QStringList parts = s.split(sep, Qt::SkipEmptyParts);

    auto tryMatch = [&](const QString& namePart, const QString& modePart)
        -> std::shared_ptr<Instrument>
    {
        if (namePart.length() < 2) return nullptr;

        // Case-insensitive name match: exact > prefix > contains
        std::shared_ptr<Instrument> exact, prefix, contains;
        for (const auto& inst : _instrumentsById) {
            const QString n = inst->getName();
            if (n.compare(namePart, Qt::CaseInsensitive) == 0)          { exact = inst; break; }
            if (!prefix && n.startsWith(namePart, Qt::CaseInsensitive)) prefix = inst;
            if (!contains && n.contains(namePart, Qt::CaseInsensitive)) contains = inst;
        }
        auto inst = exact ? exact : (prefix ? prefix : contains);
        if (!inst) return nullptr;

        if (modeKey && !modePart.isEmpty()) {
            // Try case-insensitive match on mode key, then display name
            for (const auto& m : inst->modes()) {
                if (m.key().compare(modePart, Qt::CaseInsensitive) == 0 ||
                    m.displayName().compare(modePart, Qt::CaseInsensitive) == 0)
                {
                    *modeKey = m.key();
                    return inst;
                }
            }
            // Partial: input mode is a prefix/contains of any mode key
            for (const auto& m : inst->modes()) {
                if (m.key().startsWith(modePart, Qt::CaseInsensitive) ||
                    m.key().contains(modePart, Qt::CaseInsensitive))
                {
                    *modeKey = m.key();
                    return inst;
                }
            }
        }
        return inst;
    };

    // 1. Try the whole string as an instrument name.
    if (auto inst = tryMatch(s, {})) return inst;

    // 2. Try (first token) as instrument, (rest) as mode.
    if (parts.size() >= 2) {
        QString rest = parts.mid(1).join('_');   // preserve multi-word modes
        if (auto inst = tryMatch(parts[0], rest))    return inst;
        if (auto inst = tryMatch(parts[0], parts[1]))return inst;
    }

    // 3. Try each token as an instrument name (handles "ESO/UVES" → UVES).
    for (const QString& p : parts)
        if (auto inst = tryMatch(p, {})) return inst;

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
