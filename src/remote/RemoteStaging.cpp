#include "remote/RemoteStaging.h"

#include "fitting/GaelMapping.h"

#include <specfit/GaelAPI.hpp>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>

namespace astra::remote {

using astra::fitting::SpectralFitJob;

bool writeFitDirectory(const SpectralFitJob& job,
                       const QStringList& gridBasePaths, const QString& dir,
                       QString* err)
{
    if (!QDir().mkpath(dir)) {
        if (err) *err = QStringLiteral("cannot create %1").arg(dir);
        return false;
    }

    // The spectra are the files the job already wrote for a local fit; they
    // travel next to the config and are referred to by bare name, so the
    // input is valid wherever the directory ends up.
    QHash<QString, QString> remap;
    int index = 0;
    for (const auto& obs : job.observations) {
        for (const auto& f : obs.files) {
            if (f.filename.isEmpty()) continue;
            const QString suffix = QFileInfo(f.filename).suffix();
            const QString base =
                QStringLiteral("spec_%1.%2")
                    .arg(index++, 4, 10, QLatin1Char('0'))
                    .arg(suffix.isEmpty() ? QStringLiteral("txt") : suffix);
            if (!QFile::copy(f.filename, QDir(dir).filePath(base))) {
                if (err) *err = QStringLiteral("cannot stage %1").arg(f.filename);
                return false;
            }
            remap.insert(f.filename, base);
        }
    }

    SpectralFitJob remoteJob = job;
    remoteJob.basePaths = gridBasePaths;

    auto gs = astra::fitting::gaelmap::toGaelSettings(remoteJob);
    auto fi = astra::fitting::gaelmap::toGaelInput(
        remoteJob, [&remap](const QString& p) -> QString {
            if (const QString b = remap.value(p); !b.isEmpty()) return b;
            return QStringLiteral("results");     // the output path
        });

    try {
        specfit::api::global_settings_to_json_file(
            gs, QDir(dir).filePath(QStringLiteral("global_settings.json")).toStdString());
        specfit::api::fit_input_to_json_file(
            fi, QDir(dir).filePath(QStringLiteral("fit_input.json")).toStdString());
    } catch (const std::exception& e) {
        if (err) *err = QStringLiteral("could not write the fit input: %1")
                            .arg(QString::fromUtf8(e.what()));
        return false;
    }
    return true;
}

QStringList gridsOf(const SpectralFitJob& job)
{
    QStringList out;
    for (const auto& c : job.components)
        if (!c.gridPath.isEmpty() && !out.contains(c.gridPath))
            out << c.gridPath;
    return out;
}

} // namespace astra::remote
