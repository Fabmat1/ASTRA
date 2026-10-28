#include "ai/AiRuntime.h"

#include "ai/AiHost.h"
#include "ai/AiSettings.h"
#include "ai/AuditLog.h"
#include "ai/McpSession.h"
#include "ai/PermissionGate.h"
#include "ai/PythonRunner.h"
#include "ai/ToolRegistry.h"
#include "app/AppPaths.h"
#include "core/StarFieldRegistry.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace astra::ai {

namespace {

QString pythonPackageDir()
{
    return PythonEnvironment::packageDir();
}

class AstraResources : public ResourceProvider {
public:
    QJsonArray list() const override
    {
        auto res = [](const QString& uri, const QString& name, const QString& desc,
                      const QString& mime) {
            return QJsonObject{{QStringLiteral("uri"), uri}, {QStringLiteral("name"), name},
                               {QStringLiteral("description"), desc},
                               {QStringLiteral("mimeType"), mime}};
        };
        return {
            res(QStringLiteral("astra://conventions"), QStringLiteral("ASTRA conventions"),
                QStringLiteral("Units, error conventions, time scales and how ids work."),
                QStringLiteral("text/markdown")),
            res(QStringLiteral("astra://fields"), QStringLiteral("Star fields"),
                QStringLiteral("Every per-star field with unit, type and whether it is editable."),
                QStringLiteral("application/json")),
            res(QStringLiteral("astra://python"), QStringLiteral("Python access"),
                QStringLiteral("How to read ASTRA data from your own Python scripts."),
                QStringLiteral("text/markdown")),
        };
    }

    bool read(const QString& uri, QString* mime, QString* text) const override
    {
        if (uri == QLatin1String("astra://conventions")) {
            *mime = QStringLiteral("text/markdown");
            *text = astraInstructions(false);
            return true;
        }
        if (uri == QLatin1String("astra://fields")) {
            QJsonArray arr;
            for (const auto& f : StarFieldRegistry::instance().all()) {
                QJsonObject o{{QStringLiteral("key"), f.key},
                              {QStringLiteral("name"), f.displayName},
                              {QStringLiteral("category"), f.category},
                              {QStringLiteral("type"), StarField::typeName(f.type)},
                              {QStringLiteral("editable"), f.editable}};
                if (!f.unit.isEmpty()) o.insert(QStringLiteral("unit"), f.unit);
                if (!f.note.isEmpty()) o.insert(QStringLiteral("note"), f.note);
                arr.append(o);
            }
            *mime = QStringLiteral("application/json");
            *text = QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Indented));
            return true;
        }
        if (uri == QLatin1String("astra://python")) {
            *mime = QStringLiteral("text/markdown");
            *text = pythonHelp();
            return true;
        }
        return false;
    }

    static QString pythonHelp()
    {
        const QString pkg = pythonPackageDir();
        return QStringLiteral(
                   "# Reading ASTRA data from Python\n\n"
                   "ASTRA ships a Python package, `astra`, that reads the database and the "
                   "`.asd` data files directly (read-only, safe while ASTRA runs) and can talk "
                   "to the running ASTRA for changes.\n\n"
                   "```python\n"
                   "import sys; sys.path.insert(0, %1)\n"
                   "import astra\n"
                   "db = astra.open(%2)            # read-only\n"
                   "db.projects()                  # DataFrame (or list of dicts without pandas)\n"
                   "stars = db.stars(\"RVVD\", columns=[\"alias\", \"teff\", \"logg\", \"rv_k\"])\n"
                   "s = db.star(star_id)\n"
                   "for sp in s.spectra(): wl, flux, err = sp.wavelength, sp.flux, sp.error\n"
                   "rv = s.rv_curve()              # DataFrame: mjd, bjd, rv, rv_err, ...\n"
                   "lcs = s.lightcurves()          # {source: DataFrame}\n\n"
                   "live = astra.connect()         # the running ASTRA (writes, UI control)\n"
                   "live.call(\"set_star_fields\", star_ids=[star_id], fields={\"spec_class\": \"sdB\"})\n"
                   "```\n\n"
                   "Data root: `%3`\n"
                   "Needs numpy; pandas is optional; zstandard is needed for spectra and fits.\n"
                   "Large exports: the export_data tool writes csv, json or npy files; "
                   "astra.read_export(directory) loads them.\n")
            .arg(pkg.isEmpty() ? QStringLiteral("\"<ASTRA>/share/astra/python\"")
                               : QStringLiteral("r\"%1\"").arg(pkg),
                 QStringLiteral("r\"%1\"").arg(AppPaths::root()), AppPaths::root());
    }
};

} // namespace

AiRuntime::AiRuntime(AiHost* host, QObject* parent)
    : QObject(parent), _host(host),
      _settings(new AiSettings(this)),
      _tools(new ToolRegistry(this)),
      _gate(new PermissionGate(_settings, this)),
      _audit(new AuditLog(this)),
      _resources(std::make_unique<AstraResources>())
{
    _tools->setGate(_gate);
    _tools->setAudit(_audit);
}

AiRuntime::~AiRuntime() = default;

McpSession* AiRuntime::createMcpSession(QObject* parent)
{
    auto* s = new McpSession(_tools, _resources.get(), astraInstructions(_host->hasWindow()), parent);
    s->setWindowPresent(_host->hasWindow());
    return s;
}

void registerCoreTools(AiRuntime& rt)
{
    registerProjectStarTools(rt);
    registerSpectraTools(rt);
    registerRvTools(rt);
    registerPhotometryTools(rt);
    registerExportTools(rt);
    registerJobTools(rt);
    registerPythonTools(rt);
}

} // namespace astra::ai
