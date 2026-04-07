#include "DBAccess.h"
#include <QCoreApplication>
#include <QSqlQuery>
#include <QSqlError>
#include <QUuid>
#include <QThread>

void DBAccess::setDatabase(const QSqlDatabase& db) { _database = db; }
void DBAccess::setDatabasePath(const QString& path) { _databasePath = path; }
QSqlDatabase DBAccess::database() const { return _database; }
QString DBAccess::databasePath() const { return _databasePath; }
QMutex& DBAccess::mutex() { return _connectionMutex; }

QSqlDatabase DBAccess::threadConnection()
{
    // Main thread: reuse the original connection
    if (QThread::currentThread() == QCoreApplication::instance()->thread())
        return _database;

    // Worker threads: per-thread named connection
    QString connName = QStringLiteral("worker_%1")
        .arg(reinterpret_cast<quintptr>(QThread::currentThread()), 0, 16);

    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase db = QSqlDatabase::database(connName, /*open=*/false);
        if (db.isOpen())
            return db;
    }

    QMutexLocker lock(&_connectionMutex);

    // Double-check after acquiring lock
    if (QSqlDatabase::contains(connName)) {
        QSqlDatabase db = QSqlDatabase::database(connName, /*open=*/false);
        if (db.isOpen())
            return db;
    }

    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
    db.setDatabaseName(_databasePath);
    if (!db.open()) {
        qWarning() << "Failed to open worker DB connection:" << db.lastError();
    } else {
        QSqlQuery walQuery(db);
        walQuery.exec("PRAGMA journal_mode=WAL");
        walQuery.exec("PRAGMA synchronous=NORMAL");
        walQuery.exec("PRAGMA cache_size=10000");
    }
    return db;
}

QString DBAccess::generateUUID()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

bool DBAccess::executeQuery(const QString& query)
{
    QSqlQuery sqlQuery(threadConnection());
    if (!sqlQuery.exec(query)) {
        qDebug() << "Query execution failed:" << sqlQuery.lastError();
        qDebug() << "Query was:" << query;
        return false;
    }
    return true;
}
