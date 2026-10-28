#include "app/HeadlessRuns.h"

#include "app/AppPaths.h"
#include "app/AppSettings.h"
#include "app/HeadlessContext.h"
#include "app/Logger.h"
#include "app/ServiceContext.h"
#include "core/Project.h"
#include "core/Star.h"
#include "db/DatabaseManager.h"
#include "massfit/MassFitPlan.h"
#include "massfit/MassFitService.h"
#include "remote/RemoteBatchService.h"
#include "remote/RemoteFitService.h"
#include "remote/SshGridProvider.h"
#include "spectra/SpectrumFetchService.h"
#include "spectra/fetch/LamostArchiveClient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>
#include <QUuid>

#include <cstdio>
#include <cstring>
#include <csignal>

namespace {

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
    std::shared_ptr<astra::remote::SshGridProvider>    gridProvider;

    Headless()
    {
        ctx.dbm = &dbm;
        ctx.appSettings = &settings;
        // As in ApplicationController: grid paths of the form ssh://<host>/...
        // (hosts that stream their grids) are served by this provider from
        // inside GAEL. Without it a local fit on a streamed grid fails as soon
        // as GAEL opens the grid.
        gridProvider = std::make_shared<astra::remote::SshGridProvider>(
            std::make_shared<astra::remote::GridDiskCache>(
                settings.effectiveRemoteGridCacheDir(),
                qint64(settings.remoteGridCacheCapGb()) << 30));
        gridProvider->setLogCallback([](const QString& m) { LOG_INFO("GridStream", m); });
        specfit::set_grid_data_provider(gridProvider);
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
        specfit::set_grid_data_provider(nullptr);
    }

    // starsFile: one star id per line; the run then covers only those stars.
    bool openProject(const QString& nameOrId, int limit, const QString& starsFile = {})
    {
        for (const auto& p : dbm.loadProjects())
            if (p->getId() == nameOrId || p->getName() == nameOrId) ctx.project = p;
        if (!ctx.project) {
            say(QStringLiteral("no project \"%1\" in %2").arg(nameOrId, AppPaths::database()));
            return false;
        }
        auto stars = dbm.loadStars(ctx.project->getId());
        if (!starsFile.isEmpty()) {
            QFile f(starsFile);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                say(QStringLiteral("cannot read %1").arg(starsFile));
                return false;
            }
            QSet<QString> wanted;
            while (!f.atEnd()) {
                const QString id = QString::fromUtf8(f.readLine()).trimmed();
                if (!id.isEmpty()) wanted.insert(id);
            }
            std::erase_if(stars, [&](const auto& s) { return !wanted.contains(s->getId()); });
            if (int(stars.size()) < wanted.size())
                say(QStringLiteral("%1 of the %2 listed star(s) are not in the project")
                        .arg(wanted.size() - int(stars.size())).arg(wanted.size()));
        }
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
                       argValue(argc, argv, "--limit", "0").toInt(),
                       argValue(argc, argv, "--stars")))
        return 1;

    SpectrumFetchService::Options opt;
    const bool discoverOnly  = hasFlag(argc, argv, "--discover-only");
    opt.autoQueueAll         = !discoverOnly;
    opt.redownloadExisting   = hasFlag(argc, argv, "--redownload");
    opt.joinArms             = hasFlag(argc, argv, "--join-arms");
    opt.maxParallelDownloads = argValue(argc, argv, "--parallel", "4").toInt();
    opt.radiusArcsec         = argValue(argc, argv, "--radius", "3").toDouble();

    // --only <file>: download just the discovered products whose origin id is
    // listed (one per line). --lamost-sexp <tsv>: also queue LAMOST LRS single
    // exposures that no catalogue lists, one "<star id> <UT date> <plan>
    // <spectrograph> <fiber>" per line, tab- or space-separated.
    QSet<QString> only;
    QList<SpecFetch::RemoteSpectrum> extra;
    const QString onlyFile = argValue(argc, argv, "--only");
    const QString sexpFile = argValue(argc, argv, "--lamost-sexp");
    for (const QString& path : { onlyFile, sexpFile }) {
        if (path.isEmpty()) continue;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            say(QStringLiteral("cannot read %1").arg(path));
            return 2;
        }
        while (!f.atEnd()) {
            const QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
            if (path == onlyFile) { only.insert(line); continue; }
            const QStringList c = line.split(QRegularExpression(QStringLiteral("\\s+")));
            const auto r = c.size() == 5
                ? LamostArchiveClient::uncataloguedExposure(c[0], c[1], c[2], c[3].toInt(), c[4].toInt())
                : SpecFetch::RemoteSpectrum{};
            if (r.originId.isEmpty()) {
                say(QStringLiteral("bad --lamost-sexp line: %1").arg(line));
                return 2;
            }
            extra << r;
        }
    }
    const bool picking = !onlyFile.isEmpty() || !sexpFile.isEmpty();
    if (picking) opt.autoQueueAll = false;

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
            // --release picks an older LAMOST release, e.g. DR7 for single
            // exposures the newer catalogues no longer list.
            if (arch == SpecFetch::Archive::LamostLRS)
                ao.dataRelease = argValue(argc, argv, "--release",
                                          LamostArchiveClient::defaultDataRelease());
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

    if (picking && !discoverOnly) {
        QObject::connect(&service, &SpectrumFetchService::discoveryFinished, &service,
                         [&](const QString& sid, const QList<SpecFetch::RemoteSpectrum>& found) {
            if (sid != id) return;
            QList<SpecFetch::RemoteSpectrum> picks = extra;
            QSet<QString> matched;
            for (const auto& r : found)
                if (only.contains(r.originId)) { picks << r; matched.insert(r.originId); }
            for (const QString& o : only)
                if (!matched.contains(o)) say(QStringLiteral("not found by the search: %1").arg(o));
            say(QStringLiteral("queueing %1 of %2 found product(s) and %3 listed exposure(s)")
                    .arg(matched.size()).arg(found.size()).arg(extra.size()));
            service.queueDownloads(id, picks);
        });
    }

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
                       argValue(argc, argv, "--limit", "0").toInt(),
                       argValue(argc, argv, "--stars")))
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
    // campaign that runs for days. Windows has no SIGPIPE.
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);
#endif

    int rc = 0;
    if (std::strcmp(mode, "--fetch-spectra") == 0) rc = fetchSpectra(argc, argv);
    else if (std::strcmp(mode, "--mass-fit") == 0) rc = massFit(argc, argv);
    else rc = massFitResume(argc, argv);
    Logger::shutdown();
    return rc;
}
