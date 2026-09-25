#ifndef APPPATHS_H
#define APPPATHS_H

#include <QString>
#include <QStringList>
#include <QDir>
#include <QStandardPaths>

class AppPaths
{
public:
    static void initialize()
    {
        // A runtime ASTRA_DATA_DIR wins over the compiled-in one, so a build
        // can be pointed at a scratch data root (tests, diagnostics, trying
        // a new version against a copy) without being rebuilt.
        const QString env = qEnvironmentVariable("ASTRA_DATA_DIR");
        QString compiled = env.isEmpty() ? QStringLiteral(ASTRA_DATA_DIR) : env;
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

    /// Removes a set of scratch directories, ignoring any that were never
    /// created. Fit backends stage their inputs in temporary trees and have to
    /// clear them on every exit path, successful or not.
    static void cleanupTempPaths(const QStringList& paths)
    {
        for (const QString& p : paths) {
            if (p.isEmpty()) continue;
            QDir d(p);
            if (d.exists()) d.removeRecursively();
        }
    }

private:
    static inline QString _root;
};

#endif // APPPATHS_H