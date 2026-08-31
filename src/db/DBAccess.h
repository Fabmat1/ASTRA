#ifndef DBACCESS_H
#define DBACCESS_H

#include <QSqlDatabase>
#include <QMutex>
#include <QString>

class DBAccess {
public:
    void setDatabase(const QSqlDatabase& db);
    void setDatabasePath(const QString& path);
    QSqlDatabase database() const;
    QString databasePath() const;
    QMutex& mutex();

    QSqlDatabase threadConnection();
    QString generateUUID();
    bool executeQuery(const QString& query);

    /// Bumped every time the main connection is replaced. Callers that cache a
    /// prepared QSqlQuery key it on this, so a reopened database re-prepares
    /// instead of running against a dead driver.
    quint64 connectionEpoch() const { return _connectionEpoch; }

private:
    QSqlDatabase _database;
    QString      _databasePath;
    QMutex       _connectionMutex;
    quint64      _connectionEpoch = 1;
};

#endif // DBACCESS_H
