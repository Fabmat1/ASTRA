#ifndef APPPATHS_H
#define APPPATHS_H

#include <QString>
#include <QDir>
#include <QStandardPaths>

class AppPaths
{
public:
    static void initialize()
    {
        QString compiled = QStringLiteral(ASTRA_DATA_DIR);
        if (compiled.isEmpty()) {
            _root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        } else {
            _root = compiled;
        }
        QDir().mkpath(_root);
    }

    static QString root()     { return _root; }
    static QString database() { return _root + "/astra.db"; }
    static QString logs()     { return _root + "/logs"; }
    static QString media()    { return _root + "/media"; }

private:
    static inline QString _root;
};

#endif // APPPATHS_H