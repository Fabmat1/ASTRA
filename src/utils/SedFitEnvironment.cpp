#include "SedFitEnvironment.h"

#include "AppSettings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#ifndef ASTRA_SEDFIT_BUNDLE_RELDIR
#  define ASTRA_SEDFIT_BUNDLE_RELDIR ""
#endif
#ifndef ASTRA_SEDFIT_REFDATA_RELDIR
#  define ASTRA_SEDFIT_REFDATA_RELDIR ""
#endif
#ifndef ASTRA_SEDFIT_BUILD_DIR
#  define ASTRA_SEDFIT_BUILD_DIR ""
#endif
#ifndef ASTRA_SEDFIT_REFDATA_SOURCE_DIR
#  define ASTRA_SEDFIT_REFDATA_SOURCE_DIR ""
#endif

namespace {

QString executableIn(const QString& dir)
{
    if (dir.isEmpty()) return {};
    const QString base = QDir(dir).absoluteFilePath(QStringLiteral("sedfit"));
    const QFileInfo fi(base);
    if (fi.exists() && fi.isExecutable())
        return fi.absoluteFilePath();
#ifdef Q_OS_WIN
    // The bundled helper is sedfit.exe there, and QFileInfo does not append the
    // extension for us -- without this every lookup below misses and SED
    // fitting falls back to "no sedfit found" on a package that ships one.
    // Same shape as AppSettings::lcurveBinary().
    const QFileInfo fiExe(base + QStringLiteral(".exe"));
    if (fiExe.exists())
        return fiExe.absoluteFilePath();
#endif
    return {};
}

bool looksLikeRefdata(const QString& dir)
{
    if (dir.isEmpty()) return false;
    const QDir d(dir);
    return QFileInfo::exists(d.filePath(QStringLiteral("filter_passbands.fits.gz")))
        || QFileInfo::exists(d.filePath(QStringLiteral("filter_passbands.fits")));
}

} // namespace

QString SedFitEnvironment::resolveBinary()
{
    AppSettings settings;
    const QString custom = settings.sedFitBinaryPath().trimmed();
    if (!custom.isEmpty() && QFileInfo(custom).isExecutable())
        return QFileInfo(custom).absoluteFilePath();

    const QString appDir = QCoreApplication::applicationDirPath();

    // Next to the ASTRA executable (linuxdeploy drops helpers into usr/bin).
    if (QString p = executableIn(appDir); !p.isEmpty())
        return p;

    // Installed tree: <prefix>/libexec/astra/sedfit relative to bindir.
    {
        const QString rel = QStringLiteral(ASTRA_SEDFIT_BUNDLE_RELDIR);
        if (!rel.isEmpty())
            if (QString p = executableIn(QDir(appDir).absoluteFilePath(rel));
                !p.isEmpty())
                return p;
    }

    // Dev convenience: sedfit's location inside the ASTRA build tree.
    if (QString p = executableIn(QStringLiteral(ASTRA_SEDFIT_BUILD_DIR));
        !p.isEmpty())
        return p;

    return QStandardPaths::findExecutable(QStringLiteral("sedfit"));
}

QString SedFitEnvironment::refdataDir()
{
    AppSettings settings;
    const QString custom = settings.sedFitRefdataDir().trimmed();
    if (looksLikeRefdata(custom))
        return QDir(custom).absolutePath();

    const QString appDir = QCoreApplication::applicationDirPath();
    {
        const QString rel = QStringLiteral(ASTRA_SEDFIT_REFDATA_RELDIR);
        if (!rel.isEmpty()) {
            const QString cand = QDir(appDir).absoluteFilePath(rel);
            if (looksLikeRefdata(cand))
                return QDir(cand).absolutePath();
        }
    }
    // Dev run from the build tree: use the refdata in the source checkout.
    {
        const QString src = QStringLiteral(ASTRA_SEDFIT_REFDATA_SOURCE_DIR);
        if (looksLikeRefdata(src))
            return QDir(src).absolutePath();
    }
    return {};
}

bool SedFitEnvironment::isBundled(const QString& binaryPath)
{
    if (binaryPath.isEmpty()) return false;
    const QString resolved = QFileInfo(binaryPath).absoluteFilePath();
    const QString appDir   = QCoreApplication::applicationDirPath();

    if (resolved == executableIn(appDir))
        return true;
    const QString rel = QStringLiteral(ASTRA_SEDFIT_BUNDLE_RELDIR);
    return !rel.isEmpty()
        && resolved == executableIn(QDir(appDir).absoluteFilePath(rel));
}

QProcessEnvironment SedFitEnvironment::environmentFor(const QString& binaryPath)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!isBundled(binaryPath))
        return env;

    // Bundled copy inside an AppImage/installed tree: make sure it prefers
    // the shipped shared libraries (cfitsio, gsl, curl, ...).
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString libDir =
        QDir(appDir).absoluteFilePath(QStringLiteral("../lib"));
    QStringList parts{QDir(libDir).absolutePath()};
    const QString existing = env.value(QStringLiteral("LD_LIBRARY_PATH"));
    if (!existing.isEmpty()) parts << existing;
    env.insert(QStringLiteral("LD_LIBRARY_PATH"), parts.join(':'));
    return env;
}
