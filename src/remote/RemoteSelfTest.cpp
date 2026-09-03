#include "remote/RemoteSelfTest.h"

#include "models/RemoteHost.h"
#include "remote/GridDiskCache.h"
#include "remote/RemoteHostRegistry.h"
#include "remote/SshConnection.h"
#include "remote/SshFileStreamChannel.h"
#include "remote/RemoteFitService.h"
#include "remote/SshGridProvider.h"

#include "fitting/FitTypes.h"
#include "db/DatabaseManager.h"
#include "models/Project.h"
#include "models/Spectrum.h"
#include "models/Star.h"
#include "utils/AppPaths.h"

#include "views/widgets/GridSelectorWidget.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
#include <QtConcurrent>

#include <specfit/GridDataProvider.hpp>
#include <specfit/ModelGrid.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QUuid>

#include <cstdio>
#include <cstring>

namespace astra::remote {

namespace {
bool step(const char* name, bool ok, const QString& detail = {})
{
    std::printf("%-28s %s%s%s\n", name, ok ? "OK" : "FAIL",
                detail.isEmpty() ? "" : "  ", qPrintable(detail));
    return ok;
}
} // namespace

int runRemoteSelfTest(int argc, char** argv)
{
    if (argc < 3 || std::strcmp(argv[1], "--remote-selftest") != 0) return -1;

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    app.setOrganizationName(QStringLiteral("ASTRA"));

    RemoteHost host;
    host.id          = QUuid::createUuid().toString(QUuid::WithoutBraces);
    host.name        = QString::fromLocal8Bit(argv[2]);
    host.destination = host.name;
    const QString gridBase =
        argc >= 4 ? QString::fromLocal8Bit(argv[3]) : QString();

    bool allOk = true;
    SshConnection conn(host);
    QString err;

    QElapsedTimer t;
    t.start();
    allOk &= step("ensureMaster", conn.ensureMaster(&err), err);
    if (!allOk) return 1;
    std::printf("%-28s %lld ms\n", "  (time)", (long long)t.elapsed());

    t.restart();
    auto r = conn.exec(QStringLiteral("uname -sm"));
    allOk &= step("exec uname", r.ok(),
                  QString::fromUtf8(r.out).trimmed() +
                      QStringLiteral("  [%1 ms]").arg(t.elapsed()));

    /*  shellCommand through a possibly-tcsh login shell.                    */
    r = conn.exec(SshConnection::shellCommand(
        QStringLiteral("echo $((6 * 7))")));
    allOk &= step("exec sh -c arithmetic",
                  r.ok() && QString::fromUtf8(r.out).trimmed() ==
                                QLatin1String("42"),
                  QString::fromUtf8(r.out).trimmed());

    /*  Round-trip a file upload + download.                                 */
    QTemporaryDir tmp;
    {
        QFile f(tmp.filePath(QStringLiteral("up.bin")));
        f.open(QIODevice::WriteOnly);
        QByteArray blob(256 * 1024, Qt::Uninitialized);
        for (int i = 0; i < blob.size(); ++i)
            blob[i] = char((i * 31) & 0xff);
        f.write(blob);
        f.close();
        const QString remote =
            QStringLiteral("$HOME/.astra/selftest.bin");
        conn.exec(SshConnection::shellCommand(
            QStringLiteral("mkdir -p $HOME/.astra")));
        bool up = conn.uploadFile(f.fileName(), remote, &err);
        allOk &= step("uploadFile 256k", up, err);
        const QString back = tmp.filePath(QStringLiteral("down.bin"));
        bool down = up && conn.downloadFile(remote, back, &err);
        allOk &= step("downloadFile", down, err);
        if (down) {
            QFile g(back);
            g.open(QIODevice::ReadOnly);
            allOk &= step("content identical", g.readAll() == blob);
        }
        conn.exec(SshConnection::shellCommand(
            QStringLiteral("rm -f %1").arg(remote)));
    }

    /*  Streaming channel.                                                   */
    SshFileStreamChannel chan(&conn, QStringLiteral("$HOME/.astra/bin"));
    t.restart();
    allOk &= step("channel handshake", chan.ping(&err),
                  QStringLiteral("[%1 ms]").arg(t.elapsed()));

    if (!gridBase.isEmpty()) {
        QStringList grids;
        t.restart();
        bool ok = chan.list(gridBase, 4, &grids, &err);
        allOk &= step("LIST grids", ok,
                      QStringLiteral("%1 grids [%2 ms]  e.g. %3")
                          .arg(grids.size()).arg(t.elapsed())
                          .arg(grids.value(0)));

        if (ok && !grids.isEmpty()) {
            /*  Fetch the first grid.fits plus whatever sits next to it.     */
            const QString gridFits = grids.first();
            const qint64 sz = chan.stat(gridFits, &err);
            allOk &= step("STAT grid.fits", sz > 0,
                          QStringLiteral("%1 bytes").arg(sz));

            QVector<SshFileStreamChannel::FileRequest> reqs;
            for (int i = 0; i < qMin(grids.size(), 8); ++i)
                reqs << SshFileStreamChannel::FileRequest{
                    grids[i],
                    tmp.filePath(QStringLiteral("g%1.fits").arg(i))};
            t.restart();
            QStringList failed;
            ok = chan.getFiles(reqs, &err, &failed);
            const double secs = t.elapsed() / 1000.0;
            allOk &= step("GET batch", ok,
                          QStringLiteral("%1 files, %2 kB, %3 s (%4 kB/s)%5")
                              .arg(chan.filesFetched())
                              .arg(chan.bytesFetched() / 1024)
                              .arg(secs, 0, 'f', 2)
                              .arg(secs > 0 ? chan.bytesFetched() / 1024 / secs
                                            : 0, 0, 'f', 0)
                              .arg(err.isEmpty() ? QString()
                                                 : QStringLiteral("  ") + err));
        }
    }

    /*  Directory round trip via tar.                                        */
    {
        QDir(tmp.path()).mkpath(QStringLiteral("dir/sub"));
        QFile f(tmp.filePath(QStringLiteral("dir/sub/x.txt")));
        f.open(QIODevice::WriteOnly);
        f.write("selftest\n");
        f.close();
        const QString rdir = QStringLiteral("$HOME/.astra/selftest_dir");
        bool up = conn.uploadDirTar(tmp.filePath(QStringLiteral("dir")),
                                    rdir, &err);
        allOk &= step("uploadDirTar", up, err);
        const QString ldir = tmp.filePath(QStringLiteral("dir_back"));
        bool down = up && conn.downloadDirTar(rdir, ldir, &err);
        allOk &= step("downloadDirTar", down, err);
        if (down) {
            QFile g(ldir + QStringLiteral("/sub/x.txt"));
            allOk &= step("dir content identical",
                          g.open(QIODevice::ReadOnly) &&
                              g.readAll() == QByteArray("selftest\n"));
        }
        conn.exec(SshConnection::shellCommand(
            QStringLiteral("rm -rf %1").arg(rdir)));
    }

    chan.shutdown();
    std::printf("\n%s\n", allOk ? "ALL PASSED" : "FAILURES ABOVE");
    return allOk ? 0 : 1;
}

int runRemoteGridTest(int argc, char** argv)
{
    if (argc < 6 || std::strcmp(argv[1], "--remote-gridtest") != 0) return -1;

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    // A separate settings store: this must not touch the user's hosts.
    app.setOrganizationName(QStringLiteral("ASTRA-remotetest"));

    const QString dest      = QString::fromLocal8Bit(argv[2]);
    const QString remoteBase= QString::fromLocal8Bit(argv[3]);
    const QString relGrid   = QString::fromLocal8Bit(argv[4]);
    const QString localBase = QString::fromLocal8Bit(argv[5]);

    RemoteHost host;
    host.id   = QStringLiteral("gridtest");
    host.name = QStringLiteral("gridtest");
    host.destination = dest;
    host.gridBasePaths = {remoteBase};
    host.useGridsForStreaming = true;
    RemoteHostRegistry::instance().setHosts({host});

    QTemporaryDir cacheDir;
    auto cache = std::make_shared<GridDiskCache>(cacheDir.path(),
                                                 qint64(8) << 30);
    auto provider = std::make_shared<SshGridProvider>(cache);
    provider->setLogCallback([](const QString& m) {
        std::printf("  [stream] %s\n", qPrintable(m));
    });
    specfit::set_grid_data_provider(provider);

    bool allOk = true;
    try {
        std::printf("Loading local  %s/%s\n", qPrintable(localBase),
                    qPrintable(relGrid));
        specfit::ModelGrid local({localBase.toStdString()},
                                 relGrid.toStdString());
        const auto axT = local.axis("t");
        const auto axG = local.axis("g");
        allOk &= step("local grid opened", axT && axG);
        if (!axT || !axG) return 1;

        // A point deliberately off-node, so the read exercises a full
        // interpolation hypercube rather than a single corner.
        const double teff = 0.5 * (axT->values[0] + axT->values[axT->values.size() - 1]) + 137.0;
        const double logg = 0.5 * (axG->values[0] + axG->values[axG->values.size() - 1]) + 0.031;

        local.set_wavelength_window(4000.0, 4400.0);
        QElapsedTimer t;
        t.start();
        const auto refSpec = local.load_spectrum(teff, logg, 0.0, -1.0, 0.0,
                                                 0.0, 0.0, 0.0);
        std::printf("%-28s %lld points [%lld ms]\n", "local load_spectrum",
                    (long long)refSpec.lambda.size(), (long long)t.elapsed());

        const QString sshBase =
            QStringLiteral("ssh://gridtest") +
            (remoteBase.startsWith(QLatin1Char('/')) ? remoteBase
                                                     : QLatin1Char('/') + remoteBase);
        std::printf("Loading remote %s/%s\n", qPrintable(sshBase),
                    qPrintable(relGrid));
        specfit::ModelGrid remote({sshBase.toStdString()},
                                  relGrid.toStdString());
        remote.set_wavelength_window(4000.0, 4400.0);
        t.restart();
        const auto gotSpec = remote.load_spectrum(teff, logg, 0.0, -1.0, 0.0,
                                                  0.0, 0.0, 0.0);
        const qint64 firstMs = t.elapsed();
        std::printf("%-28s %lld points [%lld ms, %d files fetched]\n",
                    "streamed load_spectrum",
                    (long long)gotSpec.lambda.size(), (long long)firstMs,
                    provider->filesFetched());

        allOk &= step("same sample count",
                      refSpec.lambda.size() == gotSpec.lambda.size());
        bool identical = refSpec.lambda.size() == gotSpec.lambda.size();
        double maxDiff = 0.0;
        for (Eigen::Index i = 0; identical && i < refSpec.lambda.size(); ++i) {
            maxDiff = std::max(maxDiff,
                               std::abs(refSpec.flux[i] - gotSpec.flux[i]));
            if (refSpec.lambda[i] != gotSpec.lambda[i] ||
                refSpec.flux[i] != gotSpec.flux[i])
                identical = false;
        }
        allOk &= step("flux bit-identical", identical,
                      QStringLiteral("max |diff| = %1").arg(maxDiff));

        // Second read of the same corners must come from the disk cache.
        const int fetchedBefore = provider->filesFetched();
        specfit::ModelGrid remote2({sshBase.toStdString()},
                                   relGrid.toStdString());
        remote2.set_wavelength_window(4000.0, 4400.0);
        t.restart();
        (void)remote2.load_spectrum(teff, logg, 0.0, -1.0, 0.0, 3.0, 0.0, 0.0);
        std::printf("%-28s [%lld ms]\n", "cached re-read", (long long)t.elapsed());
        allOk &= step("no refetch from cache",
                      provider->filesFetched() == fetchedBefore,
                      QStringLiteral("%1 cache hits")
                          .arg(provider->cacheHits()));
    } catch (const std::exception& e) {
        allOk &= step("no exception", false, QString::fromUtf8(e.what()));
    }

    specfit::set_grid_data_provider(nullptr);
    RemoteHostRegistry::instance().setHosts({});
    std::printf("\n%s\n", allOk ? "ALL PASSED" : "FAILURES ABOVE");
    return allOk ? 0 : 1;
}

int runRemoteFitTest(int argc, char** argv)
{
    if (argc < 6 || std::strcmp(argv[1], "--remote-fittest") != 0) return -1;

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    app.setOrganizationName(QStringLiteral("ASTRA-remotetest"));

    const QString dest       = QString::fromLocal8Bit(argv[2]);
    const QString gridBase   = QString::fromLocal8Bit(argv[3]);
    const QString relGrid    = QString::fromLocal8Bit(argv[4]);
    const QString spectrum   = QString::fromLocal8Bit(argv[5]);
    const bool    useSlurm   = argc >= 7 &&
                               QString::fromLocal8Bit(argv[6]) == QLatin1String("slurm");
    const QString partition  = argc >= 8 ? QString::fromLocal8Bit(argv[7]) : QString();
    const QString workDir    = argc >= 9 ? QString::fromLocal8Bit(argv[8]) : QString();

    RemoteHost host;
    host.id   = QStringLiteral("fittest");
    host.name = QStringLiteral("fittest");
    host.destination   = dest;
    host.gridBasePaths = {gridBase};
    host.useForFitting = true;
    host.workDir       = workDir;
    host.type = useSlurm ? RemoteHost::Type::Slurm : RemoteHost::Type::Plain;
    host.slurm.partition   = partition;
    host.slurm.cpusPerTask = 8;
    host.slurm.timeLimit   = QStringLiteral("01:00:00");
    RemoteHostRegistry::instance().setHosts({host});

    // "abort" as the spectrum argument's neighbour: request a cancel a few
    // seconds in, so the abort path gets exercised against a real job.
    const bool testAbort = qEnvironmentVariableIsSet("ASTRA_FITTEST_ABORT");

    astra::fitting::SpectralFitJob job;
    job.backend       = QStringLiteral("GAEL");
    job.executionHost = host.id;
    // A long jitter ensemble when testing the abort, so there is something
    // to interrupt; otherwise keep the test short.
    job.contJitterK   = testAbort ? 400 : 0;
    job.workerThreads = 8;

    astra::fitting::StellarComponent comp;
    comp.gridPath = relGrid;
    comp.teff = 35000; comp.logg = 5.5; comp.he = -1.0; comp.vsini = 5.0;
    job.components.append(comp);

    astra::fitting::SpectrumFile sf;
    sf.filename   = spectrum;
    sf.spectype   = QStringLiteral("ASCII_with_3_columns");
    sf.spectrumId = QStringLiteral("spec-under-test");
    sf.resSlope   = 0.37037;
    astra::fitting::Observation obs;
    obs.waveCut = {3600.0, 5250.0};
    obs.anchors = {{3000.0, 3850.0, 50.0}, {3850.0, 5250.0, 75.0}};
    obs.files.append(sf);
    job.observations.append(obs);

    RemoteFitService service(nullptr);

    QElapsedTimer clock;
    clock.start();
    auto onLog = [](const QString& l) {
        std::printf("  [log] %s\n", qPrintable(l));
        std::fflush(stdout);
    };
    double lastFrac = -1.0;
    auto onProgress = [&](const astra::fitting::FitProgressInfo& p) {
        if (p.fraction >= 0.0 && p.fraction - lastFrac < 0.05 &&
            p.fraction < 1.0)
            return;
        lastFrac = p.fraction;
        std::printf("  [progress] %5.1f%%  %s%s%s\n",
                    100.0 * (p.fraction < 0 ? 0.0 : p.fraction),
                    qPrintable(p.stage),
                    p.detail.isEmpty() ? "" : "  --  ",
                    qPrintable(p.detail));
        std::fflush(stdout);
    };

    auto shouldAbort = [&]() -> bool {
        return testAbort && clock.elapsed() > 3000;
    };
    const auto result = service.runJob(job, {}, onLog, onProgress, shouldAbort);

    bool allOk = true;
    if (testAbort) {
        allOk &= step("fit reported aborted", result.aborted,
                      result.errorMessage);
        allOk &= step("abort is not a success", !result.success);
        std::printf("%-28s %lld s\n", "wall clock",
                    (long long)(clock.elapsed() / 1000));
        RemoteHostRegistry::instance().setHosts({});
        std::printf("\n%s\n", allOk ? "ALL PASSED" : "FAILURES ABOVE");
        return allOk ? 0 : 1;
    }
    allOk &= step("fit succeeded", result.success, result.errorMessage);
    if (result.success) {
        std::printf("%-28s %.4f\n", "chi2", result.finalChi2);
        std::printf("%-28s %d\n", "iterations", result.iterations);
        allOk &= step("one component returned", result.components.size() == 1);
        if (!result.components.isEmpty()) {
            const auto& c = result.components.first();
            allOk &= step("teff fitted", !c.teff.isEmpty(),
                          c.teff.isEmpty()
                              ? QString()
                              : QStringLiteral("%1 +- %2")
                                    .arg(c.teff[0].value, 0, 'f', 1)
                                    .arg(c.teff[0].error, 0, 'f', 1));
            allOk &= step("logg fitted", !c.logg.isEmpty(),
                          c.logg.isEmpty()
                              ? QString()
                              : QStringLiteral("%1 +- %2")
                                    .arg(c.logg[0].value, 0, 'f', 3)
                                    .arg(c.logg[0].error, 0, 'f', 3));
        }
        allOk &= step("spectrum returned", result.spectra.size() == 1);
        if (!result.spectra.isEmpty()) {
            const auto& s0 = result.spectra.first();
            allOk &= step("spectrum id resolved",
                          s0.spectrumId == QLatin1String("spec-under-test"),
                          s0.spectrumId);
            allOk &= step("model arrays present",
                          !s0.lambda.isEmpty() && s0.model.size() == s0.lambda.size(),
                          QStringLiteral("%1 points").arg(s0.lambda.size()));
            allOk &= step("continuum anchors present", !s0.contX.isEmpty(),
                          QStringLiteral("%1 anchors").arg(s0.contX.size()));
        }
    }
    std::printf("%-28s %lld s\n", "wall clock", (long long)(clock.elapsed() / 1000));

    RemoteHostRegistry::instance().setHosts({});
    std::printf("\n%s\n", allOk ? "ALL PASSED" : "FAILURES ABOVE");
    return allOk ? 0 : 1;
}

namespace {

/*  A minimal project/star/spectrum so a harvested fit has somewhere to go.
 *  Ids are fixed, so the second process addresses the same rows.            */
struct SeedIds { QString projectId, starId, spectrumId; };

SeedIds seedDatabase(DatabaseManager* dbm, const QString& spectrumFile)
{
    SeedIds ids;
    ids.projectId  = QStringLiteral("detachtest-project");
    ids.starId     = QStringLiteral("detachtest-star");
    ids.spectrumId = QStringLiteral("detachtest-spectrum");

    auto existing = dbm->loadSpectra(ids.starId);
    if (!existing.empty()) return ids;

    auto project = std::make_shared<Project>(QStringLiteral("Detach test"));
    project->setId(ids.projectId);
    dbm->saveProject(project);

    auto star = std::make_shared<Star>();
    star->setId(ids.starId);
    star->setSourceId(QStringLiteral("detach test star"));
    dbm->saveStar(ids.projectId, star);

    auto spectrum = std::make_shared<Spectrum>();
    spectrum->setId(ids.spectrumId);
    spectrum->setFile(spectrumFile);
    dbm->saveSpectrum(ids.starId, spectrum);
    return ids;
}

astra::fitting::SpectralFitJob buildDetachJob(const QString& relGrid,
                                              const QString& spectrum,
                                              const QString& spectrumId,
                                              const QString& hostId,
                                              int jitter)
{
    astra::fitting::SpectralFitJob job;
    job.backend       = QStringLiteral("GAEL");
    job.executionHost = hostId;
    job.contJitterK   = jitter;
    job.workerThreads = 4;

    astra::fitting::StellarComponent comp;
    comp.gridPath = relGrid;
    comp.teff = 35000; comp.logg = 5.5; comp.he = -1.0; comp.vsini = 5.0;
    job.components.append(comp);

    astra::fitting::SpectrumFile sf;
    sf.filename   = spectrum;
    sf.spectype   = QStringLiteral("ASCII_with_3_columns");
    sf.spectrumId = spectrumId;
    sf.resSlope   = 0.37037;
    astra::fitting::Observation obs;
    obs.waveCut = {3600.0, 5250.0};
    obs.anchors = {{3000.0, 3850.0, 50.0}, {3850.0, 5250.0, 75.0}};
    obs.files.append(sf);
    job.observations.append(obs);
    return job;
}

} // namespace

int runRemoteDetachTest(int argc, char** argv)
{
    if (argc < 3 || std::strcmp(argv[1], "--remote-detach") != 0) return -1;

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    app.setOrganizationName(QStringLiteral("ASTRA-remotetest"));
    AppPaths::initialize();

    const QString phase = QString::fromLocal8Bit(argv[2]);
    DatabaseManager dbm;

    if (phase == QLatin1String("start")) {
        if (argc < 7) {
            std::fprintf(stderr, "start needs <dest> <gridbase> <grid> <spectrum>\n");
            return 2;
        }
        const QString dest     = QString::fromLocal8Bit(argv[3]);
        const QString gridBase = QString::fromLocal8Bit(argv[4]);
        const QString relGrid  = QString::fromLocal8Bit(argv[5]);
        const QString spectrum = QString::fromLocal8Bit(argv[6]);
        const bool useSlurm = argc >= 8 &&
            QString::fromLocal8Bit(argv[7]) == QLatin1String("slurm");

        RemoteHost host;
        host.id = QStringLiteral("detachtest");
        host.name = QStringLiteral("detachtest");
        host.destination = dest;
        host.gridBasePaths = {gridBase};
        host.useForFitting = true;
        host.type = useSlurm ? RemoteHost::Type::Slurm : RemoteHost::Type::Plain;
        if (argc >= 9) host.slurm.partition = QString::fromLocal8Bit(argv[8]);
        if (argc >= 10) host.workDir = QString::fromLocal8Bit(argv[9]);
        host.slurm.cpusPerTask = 4;
        host.slurm.timeLimit = QStringLiteral("01:00:00");
        RemoteHostRegistry::instance().setHosts({host});

        const auto ids = seedDatabase(&dbm, spectrum);
        // Long enough that it is certainly still going when this process
        // exits, which is the situation being reproduced.
        auto job = buildDetachJob(relGrid, spectrum, ids.spectrumId, host.id, 900);

        RemoteFitService service(&dbm);
        // Launch and walk away: run the fit on a worker thread and quit as
        // soon as the run row exists, exactly like a session that was closed.
        std::atomic<bool> launched{false};
        auto fut = QtConcurrent::run([&] {
            RemoteFitService::Context ctx;
            ctx.projectId = ids.projectId;
            ctx.starId    = ids.starId;
            auto onLog = [&](const QString& l) {
                std::printf("  [log] %s\n", qPrintable(l));
                std::fflush(stdout);
                if (l.contains(QLatin1String("Running on")) ||
                    l.contains(QLatin1String("Submitted to")))
                    launched.store(true);
            };
            service.runJob(job, ctx, onLog, {}, [&] {
                // Stop waiting once the row is written; the remote job stays.
                return false;
            });
        });

        for (int i = 0; i < 1800 && !launched.load(); ++i)
            QThread::msleep(100);
        const bool ok = launched.load();
        step("launched and recorded", ok);
        if (!ok) return 1;

        // Leave the run behind exactly as a killed session would.
        std::printf("\nLaunched; exiting without waiting (simulating a "
                    "closed ASTRA).\n");
        std::fflush(stdout);
        std::_Exit(ok ? 0 : 1);
    }

    if (phase != QLatin1String("finish") && phase != QLatin1String("stop")) {
        std::fprintf(stderr, "phase must be start, finish or stop\n");
        return 2;
    }

    // ── the next session ────────────────────────────────────────────────
    const auto pending = dbm.loadActiveRemoteFitRuns();
    bool allOk = step("run survived the restart", pending.size() == 1,
                      QStringLiteral("%1 unfinished run(s) in the database")
                          .arg(pending.size()));
    if (pending.empty()) return 1;
    const QString runId = QString::fromStdString(pending.front().id.toStdString());

    RemoteFitService service(&dbm);
    QEventLoop loop;
    bool harvested = false, storedOk = false;
    QObject::connect(&service, &RemoteFitService::reattachedRunHarvested,
                     [&](const QString&, const QString&, bool stored) {
                         harvested = true;
                         storedOk = stored;
                         loop.quit();
                     });

    service.reattachAll();
    allOk &= step("run adopted", !service.runs().isEmpty(),
                  QStringLiteral("%1 run(s) being watched")
                      .arg(service.runs().size()));

    if (phase == QLatin1String("stop")) {
        // Give the adoption a moment to attach, then stop it the way the
        // Remote Fits window does.
        QTimer::singleShot(4000, [&] {
            std::printf("  requesting stop ...\n");
            std::fflush(stdout);
            service.requestStop(runId);
        });
    }

    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(20 * 60 * 1000);
    loop.exec();

    allOk &= step("run settled", harvested);

    const auto after = dbm.loadRemoteFitRun(runId);
    const QString finalState = after ? after->state : QStringLiteral("(gone)");
    if (phase == QLatin1String("stop")) {
        allOk &= step("ended as stopped", finalState == QLatin1String("aborted"),
                      finalState);
    } else {
        allOk &= step("ended as collected",
                      finalState == QLatin1String("harvested"), finalState);
        allOk &= step("result stored against the star", storedOk);
        const auto fits = dbm.loadSpectralFits(QStringLiteral("detachtest-spectrum"));
        allOk &= step("fit row written", !fits.empty(),
                      QStringLiteral("%1 fit(s) on the spectrum").arg(fits.size()));
    }
    allOk &= step("nothing left unfinished",
                  dbm.loadActiveRemoteFitRuns().empty());

    RemoteHostRegistry::instance().setHosts({});
    std::printf("\n%s\n", allOk ? "ALL PASSED" : "FAILURES ABOVE");
    return allOk ? 0 : 1;
}

int runRemoteGridListTest(int argc, char** argv)
{
    if (argc < 4 || std::strcmp(argv[1], "--remote-gridlist") != 0) return -1;

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    app.setOrganizationName(QStringLiteral("ASTRA-remotetest"));

    const QString dest     = QString::fromLocal8Bit(argv[2]);
    const QString gridBase = QString::fromLocal8Bit(argv[3]);

    RemoteHost host;
    host.id = QStringLiteral("gridlist");
    host.name = QStringLiteral("gridlist");
    host.destination = dest;
    host.gridBasePaths = {gridBase};
    host.useGridsForStreaming = true;
    RemoteHostRegistry::instance().setHosts({host});

    bool allOk = true;

    const QStringList paths =
        RemoteHostRegistry::instance().streamingBasePaths();
    allOk &= step("host offers a streaming base path", paths.size() == 1,
                  paths.value(0));

    // Exactly what FitComponentsWidget builds for each component.
    GridSelectorWidget selector;
    selector.setBasePaths(paths);
    selector.refresh();

    QElapsedTimer t;
    t.start();
    while (!selector.hasSelection() && t.elapsed() < 120000) {
        app.processEvents(QEventLoop::AllEvents, 50);
        QThread::msleep(20);
    }

    allOk &= step("selector found remote grids", selector.hasSelection(),
                  QStringLiteral("after %1 ms").arg(t.elapsed()));
    if (!selector.hasSelection()) {
        RemoteHostRegistry::instance().setHosts({});
        std::printf("\nFAILURES ABOVE\n");
        return 1;
    }

    const QString base = selector.selectedBasePath();
    const QString rel  = selector.selectedRelativePath();
    std::printf("%-28s %s\n", "selected base", qPrintable(base));
    std::printf("%-28s %s\n", "selected grid", qPrintable(rel));

    allOk &= step("base path is the remote host",
                  isRemoteGridUrl(base), base);
    allOk &= step("relative grid path is relative",
                  !rel.isEmpty() && !rel.startsWith(QLatin1Char('/')), rel);

    if (const auto g = selector.selectedGrid(); g)
        allOk &= step("display names the host",
                      g->displayName.contains(QLatin1String("@ gridlist")),
                      g->displayName);

    // The pair the fit would carry has to open a real grid; this is the same
    // resolution a job does through GAEL's base_paths.
    QTemporaryDir cacheDir;
    auto cache = std::make_shared<GridDiskCache>(cacheDir.path(),
                                                 qint64(4) << 30);
    auto provider = std::make_shared<SshGridProvider>(cache);
    specfit::set_grid_data_provider(provider);
    try {
        specfit::ModelGrid grid({base.toStdString()}, rel.toStdString());
        allOk &= step("that pair opens the grid", grid.axis("t") != nullptr,
                      QStringLiteral("%1 axes").arg(grid.axes().size()));
    } catch (const std::exception& e) {
        allOk &= step("that pair opens the grid", false,
                      QString::fromUtf8(e.what()));
    }
    specfit::set_grid_data_provider(nullptr);

    RemoteHostRegistry::instance().setHosts({});
    std::printf("\n%s\n", allOk ? "ALL PASSED" : "FAILURES ABOVE");
    return allOk ? 0 : 1;
}

} // namespace astra::remote
