#include "app/HeadlessRuns.h"

#include "app/AppPaths.h"
#include "app/AppSettings.h"
#include "app/Logger.h"
#include "app/ServiceContext.h"
#include "core/Project.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "massfit/MassFitPlan.h"
#include "massfit/MassFitService.h"
#include "remote/RemoteBatchService.h"
#include "remote/RemoteFitService.h"
#include "spectra/SpectrumFetchService.h"
#include "spectra/fetch/LamostArchiveClient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTimer>
#include <QUuid>

#include <cstdio>
#include <cstring>
#include <csignal>

namespace {

class HeadlessContext : public ServiceContext {
public:
    DatabaseManager* databaseManager() const override { return dbm; }
    AppSettings*     settings() const override { return appSettings; }
    std::shared_ptr<Project> getCurrentProject() const override { return project; }

    DatabaseManager*         dbm = nullptr;
    AppSettings*             appSettings = nullptr;
    std::shared_ptr<Project> project;
};

QString argValue(int argc, char** argv, const char* name, const QString& def = {})
{
    for (int i = 2; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return QString::fromLocal8Bit(argv[i + 1]);
    return def;
}

bool hasFlag(int argc, char** argv, const char* name)
{
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

void say(const QString& line)
{
    std::printf("[%s] %s\n",
                qPrintable(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"))),
                qPrintable(line));
    std::fflush(stdout);
}

std::atomic<bool> g_interrupted{false};
extern "C" void onSignal(int) { g_interrupted.store(true); }

/*  Everything the window's ApplicationController sets up that these jobs
 *  touch: the database, the settings, and the remote services the mass
 *  fitter hands fits to.                                                    */
struct Headless {
    DatabaseManager dbm;
    AppSettings     settings;
    HeadlessContext ctx;
    std::unique_ptr<astra::remote::RemoteFitService>   remoteFits;
    std::unique_ptr<astra::remote::RemoteBatchService> remoteBatches;

    Headless()
    {
        ctx.dbm = &dbm;
        ctx.appSettings = &settings;
        remoteFits = std::make_unique<astra::remote::RemoteFitService>(&dbm);
        remoteFits->reattachAll();
        remoteBatches = std::make_unique<astra::remote::RemoteBatchService>(&dbm);
        remoteBatches->reattachAll();
    }
    ~Headless()
    {
        // Same order as ApplicationController: let remote work go first.
        remoteBatches->shutdown();
        remoteFits->beginShutdown();
    }

    bool openProject(const QString& nameOrId, int limit)
    {
        for (const auto& p : dbm.loadProjects())
            if (p->getId() == nameOrId || p->getName() == nameOrId) ctx.project = p;
        if (!ctx.project) {
            say(QStringLiteral("no project \"%1\" in %2").arg(nameOrId, AppPaths::database()));
            return false;
        }
        auto stars = dbm.loadStars(ctx.project->getId());
        if (limit > 0 && int(stars.size()) > limit) stars.resize(size_t(limit));
        ctx.project->setStars(stars, false);
        say(QStringLiteral("project %1: %2 star(s)").arg(ctx.project->getName()).arg(stars.size()));
        return true;
    }
};

int fetchSpectra(int argc, char** argv)
{
    Headless h;
    if (!h.openProject(QString::fromLocal8Bit(argv[2]),
                       argValue(argc, argv, "--limit", "0").toInt()))
        return 1;

    SpectrumFetchService::Options opt;
    const bool discoverOnly  = hasFlag(argc, argv, "--discover-only");
    opt.autoQueueAll         = !discoverOnly;
    opt.redownloadExisting   = hasFlag(argc, argv, "--redownload");
    opt.joinArms             = hasFlag(argc, argv, "--join-arms");
    opt.maxParallelDownloads = argValue(argc, argv, "--parallel", "4").toInt();
    for (const QString& a : argValue(argc, argv, "--archives").split(QLatin1Char(','),
                                                                   Qt::SkipEmptyParts)) {
        bool found = false;
        for (auto arch : { SpecFetch::Archive::LamostLRS, SpecFetch::Archive::LamostMRS,
                           SpecFetch::Archive::SdssOptical, SpecFetch::Archive::EsoPhase3,
                           SpecFetch::Archive::MastSSAP, SpecFetch::Archive::Apogee }) {
            if (SpecFetch::archiveKey(arch) != a.trimmed()) continue;
            found = true;
            opt.archives << arch;
            SpecFetch::ArchiveOptions ao;
            ao.fetchExposures = hasFlag(argc, argv, "--exposures");
            if (arch == SpecFetch::Archive::LamostLRS)
                ao.dataRelease = LamostArchiveClient::defaultDataRelease();
            opt.perArchive.insert(arch, ao);
        }
        if (!found) {
            QStringList keys;
            for (auto arch : { SpecFetch::Archive::LamostLRS, SpecFetch::Archive::LamostMRS,
                               SpecFetch::Archive::SdssOptical, SpecFetch::Archive::EsoPhase3,
                               SpecFetch::Archive::MastSSAP, SpecFetch::Archive::Apogee })
                keys << SpecFetch::archiveKey(arch);
            say(QStringLiteral("unknown archive \"%1\"; known: %2").arg(a, keys.join(", ")));
            return 2;
        }
    }
    if (opt.archives.isEmpty()) { say(QStringLiteral("--archives is required")); return 2; }

    SpectrumFetchService service(&h.ctx);
    const QString id = service.startSession(h.ctx.project->getAllStars(),
                                            h.ctx.project->getId(), opt);
    if (id.isEmpty()) { say(QStringLiteral("no star has usable coordinates")); return 1; }
    say(QStringLiteral("session %1 started").arg(id));

    int rc = 0;
    QTimer ticker;
    QObject::connect(&ticker, &QTimer::timeout, [&] {
        const auto info = service.sessionInfo(id);
        if (info.state == SpectrumFetchService::State::Discovering)
            say(QStringLiteral("discovering: %1/%2 queries, %3 product(s) found  %4")
                    .arg(info.discoveryDone).arg(info.discoveryTotal)
                    .arg(info.discovered).arg(info.discoveryDetail));
        else
            say(QStringLiteral("%1: %2/%3 downloads, %4 imported, %5 failed, %6 skipped")
                    .arg(SpectrumFetchService::stateLabel(info.state))
                    .arg(info.downloadsDone).arg(info.downloadsTotal)
                    .arg(info.importedSpectra).arg(info.failedItems).arg(info.skippedItems));
        if (discoverOnly && info.state == SpectrumFetchService::State::AwaitingSelection) {
            QHash<QString, int> byCollection;
            for (const auto& r : service.discoveredResults(id)) ++byCollection[r.collection];
            for (auto it = byCollection.cbegin(); it != byCollection.cend(); ++it)
                say(QStringLiteral("  %1: %2").arg(it.key()).arg(it.value()));
            say(QStringLiteral("discovered %1 product(s); not downloading").arg(info.discovered));
            QCoreApplication::quit();
            return;
        }
        if (!service.isSessionActive(id)) {
            say(QStringLiteral("finished: %1").arg(info.summary));
            QCoreApplication::quit();
        }
        if (g_interrupted.load()) {
            say(QStringLiteral("interrupted; stopping the session"));
            service.cancelSession(id);
            rc = 3;
        }
    });
    ticker.start(discoverOnly ? 5000 : 30000);
    QCoreApplication::exec();
    return rc;
}

void watchMassFit(MassFitService& service, const QString& runId)
{
    QTimer ticker;
    QObject::connect(&ticker, &QTimer::timeout, [&] {
        const auto info = service.runInfo(runId);
        QString remote;
        for (const auto& s : astra::remote::RemoteBatchService::instance()->status())
            remote += QStringLiteral("  [%1: %2 waiting, %3 in %4 batch(es)%5]")
                          .arg(s.hostName).arg(s.waiting).arg(s.inBatches).arg(s.batches)
                          .arg(s.lastError.isEmpty() ? QString() : QStringLiteral(", ") + s.lastError);
        say(QStringLiteral("%1: %2/%3 stars done, %4 failed, %5 running%6")
                .arg(MassFitService::runStateLabel(info.state))
                .arg(info.starDone).arg(info.starTotal).arg(info.starFailed)
                .arg(info.starRunning).arg(remote));
        if (MassFitService::isTerminal(info.state)) {
            say(QStringLiteral("run %1 %2: %3").arg(runId, MassFitService::runStateLabel(info.state), info.summary));
            QCoreApplication::quit();
        }
        if (g_interrupted.load()) {
            say(QStringLiteral("interrupted; remote fits keep running - resume with "
                               "--mass-fit-resume %1").arg(runId));
            QCoreApplication::quit();
        }
    });
    // The run's own log, as it grows.
    int printed = 0;
    QObject::connect(&service, &MassFitService::runLogUpdated, [&](const QString& id) {
        if (id != runId) return;
        const QStringList lines = QString::fromUtf8(service.runLog(runId)).split(QLatin1Char('\n'));
        for (int i = printed; i < lines.size() - 1; ++i) std::printf("    %s\n", qPrintable(lines[i]));
        printed = std::max(printed, int(lines.size()) - 1);
        std::fflush(stdout);
    });
    ticker.start(30000);
    QCoreApplication::exec();
}

int massFit(int argc, char** argv)
{
    namespace mf = astra::massfit;
    Headless h;
    if (!h.openProject(QString::fromLocal8Bit(argv[2]),
                       argValue(argc, argv, "--limit", "0").toInt()))
        return 1;

    const QString planPath = argValue(argc, argv, "--plan");
    QFile f(planPath);
    if (!f.open(QIODevice::ReadOnly)) { say(QStringLiteral("cannot read plan %1").arg(planPath)); return 2; }
    mf::MassFitPlan plan = mf::MassFitPlan::fromJsonString(QString::fromUtf8(f.readAll()));
    plan.id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
    plan.projectId = h.ctx.project->getId();
    if (const QStringList p = MassFitService::validateForRun(plan); !p.isEmpty()) {
        for (const QString& s : p) say(QStringLiteral("plan problem: %1").arg(s));
        return 1;
    }
    MassFitPlanRow row;
    row.id         = plan.id;
    row.projectId  = plan.projectId;
    row.name       = plan.name;
    row.configJson = plan.toJsonString();
    h.dbm.saveMassFitPlan(row);

    MassFitService::RunOptions opt;
    const QString existing = argValue(argc, argv, "--existing", "add");
    opt.existing = existing == QLatin1String("skip") ? mf::ExistingFitPolicy::SkipFitted
                 : existing == QLatin1String("poor") ? mf::ExistingFitPolicy::RefitPoor
                                                     : mf::ExistingFitPolicy::AddNew;
    opt.parallelStars = argValue(argc, argv, "--parallel",
                                 QString::number(plan.parallelStars)).toInt();

    MassFitService service(&h.ctx);
    service.setCaptureStdout(false);
    const QString runId = service.startRun(h.ctx.project->getAllStars(),
                                           h.ctx.project->getId(), plan, opt);
    if (runId.isEmpty()) { say(QStringLiteral("the run did not start")); return 1; }
    say(QStringLiteral("run %1 started with plan \"%2\" (%3 parallel)")
            .arg(runId, plan.name).arg(opt.parallelStars));
    watchMassFit(service, runId);
    return 0;
}

int massFitResume(int argc, char** argv)
{
    Headless h;
    const QString runId = QString::fromLocal8Bit(argv[2]);
    const auto row = h.dbm.loadMassFitRun(runId);
    if (!row) { say(QStringLiteral("no run %1").arg(runId)); return 1; }
    for (const auto& p : h.dbm.loadProjects())
        if (p->getId() == row->projectId) h.ctx.project = p;
    if (h.ctx.project) h.ctx.project->setStars(h.dbm.loadStars(h.ctx.project->getId()), false);

    MassFitService service(&h.ctx);
    service.setCaptureStdout(false);
    if (service.resumeRun(runId).isEmpty()) { say(QStringLiteral("run %1 cannot be resumed").arg(runId)); return 1; }
    say(QStringLiteral("run %1 resumed").arg(runId));
    watchMassFit(service, runId);
    return 0;
}

} // namespace

int runHeadless(int argc, char** argv)
{
    if (argc < 3) return -1;
    const char* mode = argv[1];
    if (std::strcmp(mode, "--fetch-spectra") != 0 && std::strcmp(mode, "--mass-fit") != 0 &&
        std::strcmp(mode, "--mass-fit-resume") != 0)
        return -1;

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    app.setOrganizationName(QStringLiteral("ASTRA"));
    AppPaths::initialize();
    // The window's log file, so a long headless run leaves the same record.
    Logger::initialize(QStringLiteral("ASTRA"));
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    // A closed pipe (a log reader gone, a detached terminal) must not end a
    // campaign that runs for days.
    std::signal(SIGPIPE, SIG_IGN);

    int rc = 0;
    if (std::strcmp(mode, "--fetch-spectra") == 0) rc = fetchSpectra(argc, argv);
    else if (std::strcmp(mode, "--mass-fit") == 0) rc = massFit(argc, argv);
    else rc = massFitResume(argc, argv);
    Logger::shutdown();
    return rc;
}
