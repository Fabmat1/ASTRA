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

private:
    QSqlDatabase _database;
    QString      _databasePath;
    QMutex       _connectionMutex;
};

#endif // DBACCESS_H
