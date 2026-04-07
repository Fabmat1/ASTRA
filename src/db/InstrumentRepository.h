#ifndef INSTRUMENTREPOSITORY_H
#define INSTRUMENTREPOSITORY_H

#include <memory>
#include <vector>
#include <QString>
#include "models/Instrument.h"
#include "models/InstrumentMode.h"

class DBAccess;

class InstrumentRepository {
public:
    explicit InstrumentRepository(DBAccess& db);

    void initializeInstruments();
    std::shared_ptr<Instrument> getInstrumentById(const QString& id) const;
    std::shared_ptr<Instrument> getInstrumentByName(const QString& name) const;
    std::vector<std::shared_ptr<Instrument>> getAllInstruments() const;
    bool saveInstrument(std::shared_ptr<Instrument> instrument);
    bool updateInstrument(std::shared_ptr<Instrument> instrument);
    bool deleteInstrument(const QString& id);
    std::shared_ptr<Instrument> resolveInstrumentString( const QString& input, QString* modeKey = nullptr) const;
    void restoreDefaultInstruments();
    void loadDefaultInstruments();
    void loadInstrumentsFromDatabase();
    bool writeInstrumentToDb(const Instrument& inst);
    bool writeModesToDb(const QString& instrumentId, const QList<InstrumentMode>& modes);
    void cacheInstrument(std::shared_ptr<Instrument> inst);
    void uncacheInstrument(const QString& id);
    QString instrumentUUID(const QString& name);

private:
    DBAccess& _db;
    QHash<QString, std::shared_ptr<Instrument>> _instrumentsById;
    QHash<QString, std::shared_ptr<Instrument>> _instrumentsByName;
};

#endif // INSTRUMENTREPOSITORY_H
