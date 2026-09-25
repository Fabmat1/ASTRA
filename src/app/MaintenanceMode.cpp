#include "app/MaintenanceMode.h"

#include "app/AppPaths.h"
#include "core/Instrument.h"
#include "core/Project.h"
#include "db/DatabaseManager.h"
#include "massfit/MassFitPlan.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#ifdef __linux__
#include <unistd.h>
#endif

namespace {

struct Row {
    QString id, starId, instrumentId, modeKey, origin, dataFile;
    double  mjd = 0.0;
};

struct Match {
    Row    imported;
    Row    archive;
    double deltaSec = 0.0;
};

QString arg(int argc, char** argv, const char* name, const QString& def = {})
{
    for (int i = 2; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return QString::fromLocal8Bit(argv[i + 1]);
    return def;
}

bool flag(int argc, char** argv, const char* name)
{
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

/*  Another ASTRA window on this machine, as far as Linux lets us tell.
 *  Elsewhere the check is skipped and --apply relies on the user.           */
QStringList otherAstraProcesses()
{
    QStringList found;
#ifdef __linux__
    const QString self = QString::number(getpid());
    for (const QString& pid : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs)) {
        bool num = false;
        pid.toLongLong(&num);
        if (!num || pid == self) continue;
        const QString exe = QFileInfo(QStringLiteral("/proc/%1/exe").arg(pid)).symLinkTarget();
        if (QFileInfo(exe).fileName() != QLatin1String("ASTRA")) continue;
        QFile cmd(QStringLiteral("/proc/%1/cmdline").arg(pid));
        if (!cmd.open(QIODevice::ReadOnly)) continue;
        // Our own helper modes (askpass, self-tests, this one) do not count:
        // they are the ones started with a --option.
        const QList<QByteArray> args = cmd.readAll().split('\0');
        if (args.size() > 1 && args[1].startsWith("--")) continue;
        found << pid;
    }
#endif
    return found;
}

bool exec(QSqlQuery& q, const QString& sql)
{
    if (q.exec(sql)) return true;
    std::fprintf(stderr, "SQL failed: %s\n  %s\n", qPrintable(q.lastError().text()),
                 qPrintable(sql.left(200)));
    return false;
}

int replaceImportedDuplicates(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: --replace-imported-duplicates <project> "
                             "[--instrument NAME] [--mode KEY] [--tolerance S] "
                             "[--backup DIR] [--apply] [--force]\n");
        return 2;
    }
    const QString projectArg = QString::fromLocal8Bit(argv[2]);
    const QString instArg    = arg(argc, argv, "--instrument");
    const QString modeArg    = arg(argc, argv, "--mode");
    const double  tolSec     = arg(argc, argv, "--tolerance", QStringLiteral("60")).toDouble();
    const bool    apply      = flag(argc, argv, "--apply");
    const bool    force      = flag(argc, argv, "--force");
    QString backupDir        = arg(argc, argv, "--backup");

    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ASTRA"));
    app.setOrganizationName(QStringLiteral("ASTRA"));
    AppPaths::initialize();

    DatabaseManager dbm;
    std::shared_ptr<Project> project;
    for (const auto& p : dbm.loadProjects())
        if (p->getId() == projectArg || p->getName() == projectArg) project = p;
    if (!project) {
        std::fprintf(stderr, "no project \"%s\" in %s\n", qPrintable(projectArg),
                     qPrintable(AppPaths::database()));
        return 1;
    }

    QSet<QString> instIds;
    QHash<QString, QString> instNames;
    for (const auto& inst : dbm.getAllInstruments()) {
        instNames.insert(inst->getId(), inst->getName());
        if (!instArg.isEmpty() && inst->getName().compare(instArg, Qt::CaseInsensitive) == 0)
            instIds.insert(inst->getId());
    }
    if (!instArg.isEmpty() && instIds.isEmpty()) {
        std::fprintf(stderr, "no instrument named \"%s\"\n", qPrintable(instArg));
        return 1;
    }

    // A connection of our own for the scan and the backup; deletions go
    // through DatabaseManager, the same path the UI uses.
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                QStringLiteral("maintenance"));
    db.setDatabaseName(AppPaths::database());
    if (!db.open()) {
        std::fprintf(stderr, "cannot open %s\n", qPrintable(AppPaths::database()));
        return 1;
    }

    std::map<QString, std::vector<Row>> groups;   // star|instrument|mode
    {
        QSqlQuery q(db);
        q.prepare(QStringLiteral(
            "SELECT sp.id, sp.star_id, coalesce(sp.instrument_id,''), "
            "coalesce(sp.mode_key,''), sp.mjd, coalesce(sp.origin,''), "
            "coalesce(sp.data_file,'') FROM spectra sp JOIN stars s ON s.id = sp.star_id "
            "WHERE s.project_id = :p"));
        q.bindValue(QStringLiteral(":p"), project->getId());
        if (!q.exec()) {
            std::fprintf(stderr, "scan failed: %s\n", qPrintable(q.lastError().text()));
            return 1;
        }
        while (q.next()) {
            Row r;
            r.id = q.value(0).toString();
            r.starId = q.value(1).toString();
            r.instrumentId = q.value(2).toString();
            r.modeKey = q.value(3).toString();
            r.mjd = q.value(4).toDouble();
            r.origin = q.value(5).toString();
            r.dataFile = q.value(6).toString();
            if (!instIds.isEmpty() && !instIds.contains(r.instrumentId)) continue;
            if (!modeArg.isEmpty() && r.modeKey != modeArg) continue;
            groups[r.starId + '|' + r.instrumentId + '|' + r.modeKey].push_back(r);
        }
    }

    // Nearest archive exposure within the tolerance, each claimed once.
    std::vector<Match> matches;
    std::vector<Row> unmatched;
    std::map<QString, std::pair<int, int>> perMode;   // label -> (imported, matched)
    for (auto& [key, rows] : groups) {
        std::vector<Row> archive, imported;
        for (const auto& r : rows) (r.origin.isEmpty() ? imported : archive).push_back(r);
        if (imported.empty()) continue;
        const QString label = QStringLiteral("%1 / %2")
                                  .arg(instNames.value(imported.front().instrumentId,
                                                       QStringLiteral("unknown")),
                                       imported.front().modeKey);
        perMode[label].first += int(imported.size());
        std::vector<bool> claimed(archive.size(), false);
        for (const auto& im : imported) {
            int best = -1;
            double bestD = 1e300;
            if (im.mjd > 0.0)
                for (size_t k = 0; k < archive.size(); ++k) {
                    if (claimed[k] || archive[k].mjd <= 0.0) continue;
                    const double d = std::abs(archive[k].mjd - im.mjd) * 86400.0;
                    if (d < bestD) { bestD = d; best = int(k); }
                }
            if (best >= 0 && bestD <= tolSec) {
                claimed[size_t(best)] = true;
                matches.push_back({ im, archive[size_t(best)], bestD });
                ++perMode[label].second;
            } else {
                unmatched.push_back(im);
            }
        }
    }

    std::printf("Project %s: %zu file-imported spectra considered\n",
                qPrintable(project->getName()),
                size_t(matches.size() + unmatched.size()));
    for (const auto& [label, c] : perMode)
        std::printf("  %-28s %6d imported, %6d have an archive exposure within %.0f s\n",
                    qPrintable(label), c.first, c.second, tolSec);
    double worst = 0.0;
    for (const auto& m : matches) worst = std::max(worst, m.deltaSec);
    std::printf("  largest epoch difference among matches: %.1f s\n", worst);

    if (!apply) {
        std::printf("\nDry run. Add --apply --backup <dir> to replace them.\n");
        return 0;
    }
    if (matches.empty()) { std::printf("Nothing to replace.\n"); return 0; }
    if (backupDir.isEmpty()) {
        std::fprintf(stderr, "--apply needs --backup <dir>\n");
        return 2;
    }
    if (const QStringList others = otherAstraProcesses(); !others.isEmpty() && !force) {
        std::fprintf(stderr, "ASTRA is running (pid %s). Close it first, or pass "
                             "--force.\n", qPrintable(others.join(QLatin1Char(' '))));
        return 1;
    }

    // ── Backup ───────────────────────────────────────────────────────────
    QDir().mkpath(backupDir + QLatin1String("/files"));
    const QString rowsDb = QDir(backupDir).filePath(QStringLiteral("rows.sqlite"));
    QSqlQuery q(db);
    if (!exec(q, QStringLiteral("ATTACH DATABASE '%1' AS bk").arg(rowsDb))) return 1;
    for (const char* t : { "spectra", "spectral_fits", "rv_points" })
        if (!exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS bk.%1 AS SELECT * FROM main.%1 WHERE 0")
                         .arg(QLatin1String(t))))
            return 1;
    exec(q, QStringLiteral("CREATE TABLE IF NOT EXISTS bk.replaced (imported_id TEXT, "
                           "archive_id TEXT, star_id TEXT, delta_seconds REAL, "
                           "replaced_at TEXT)"));
    exec(q, QStringLiteral("CREATE TEMP TABLE dup (id TEXT PRIMARY KEY, archive_id TEXT, "
                           "star_id TEXT, delta REAL)"));
    db.transaction();
    {
        QSqlQuery ins(db);
        ins.prepare(QStringLiteral("INSERT OR IGNORE INTO dup VALUES (?, ?, ?, ?)"));
        for (const auto& m : matches) {
            ins.addBindValue(m.imported.id);
            ins.addBindValue(m.archive.id);
            ins.addBindValue(m.imported.starId);
            ins.addBindValue(m.deltaSec);
            ins.exec();
        }
    }
    db.commit();
    const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    bool ok = exec(q, QStringLiteral("INSERT INTO bk.spectra SELECT * FROM spectra WHERE id IN (SELECT id FROM dup)")) &&
              exec(q, QStringLiteral("INSERT INTO bk.spectral_fits SELECT * FROM spectral_fits WHERE spectrum_id IN (SELECT id FROM dup)")) &&
              exec(q, QStringLiteral("INSERT INTO bk.rv_points SELECT * FROM rv_points WHERE spectrum_id IN (SELECT id FROM dup)")) &&
              exec(q, QStringLiteral("INSERT INTO bk.replaced SELECT id, archive_id, star_id, delta, '%1' FROM dup").arg(now));
    if (!ok) return 1;

    // The blobs deleteSpectrum removes: the spectrum's own and its fits'.
    QStringList blobs;
    exec(q, QStringLiteral("SELECT data_file FROM spectra WHERE id IN (SELECT id FROM dup) "
                           "UNION ALL SELECT model_data_file FROM spectral_fits "
                           "WHERE spectrum_id IN (SELECT id FROM dup)"));
    while (q.next()) {
        const QString f = q.value(0).toString();
        if (!f.isEmpty() && QFileInfo::exists(f)) blobs << f;
    }
    int copied = 0;
    for (const QString& f : blobs) {
        const QString star = QFileInfo(f).dir().dirName() == QLatin1String("spectra")
                                 ? QFileInfo(QFileInfo(f).dir().absolutePath()).dir().dirName()
                                 : QStringLiteral("misc");
        const QString dst = QDir(backupDir).filePath(QStringLiteral("files/%1/%2")
                                                         .arg(star, QFileInfo(f).fileName()));
        QDir().mkpath(QFileInfo(dst).absolutePath());
        if (QFileInfo::exists(dst) || QFile::copy(f, dst)) ++copied;
        else { std::fprintf(stderr, "could not back up %s\n", qPrintable(f)); return 1; }
    }

    QHash<QString, QStringList> rvByspectrum;
    exec(q, QStringLiteral("SELECT spectrum_id, id FROM rv_points WHERE spectrum_id IN (SELECT id FROM dup)"));
    while (q.next()) rvByspectrum[q.value(0).toString()] << q.value(1).toString();
    exec(q, QStringLiteral("DETACH DATABASE bk"));
    std::printf("Backed up %zu spectra with their fits and RV points to %s "
                "(%d files)\n", matches.size(), qPrintable(backupDir), copied);

    // ── Replace ──────────────────────────────────────────────────────────
    std::map<QString, std::vector<const Match*>> byStar;
    for (const auto& m : matches) byStar[m.imported.starId].push_back(&m);
    int deleted = 0, rvDeleted = 0, failed = 0;
    for (const auto& [starId, list] : byStar) {
        dbm.beginTransaction();
        for (const Match* m : list) {
            for (const QString& rv : rvByspectrum.value(m->imported.id))
                if (dbm.deleteRadialVelocityPoint(rv)) ++rvDeleted;
            if (dbm.deleteSpectrum(m->imported.id)) ++deleted;
            else ++failed;
        }
        int n = 0, nf = 0;
        if (dbm.spectraCounts(starId, &n, &nf)) dbm.updateSpectraCounts(starId, n, nf);
        dbm.commitTransaction();
    }
    std::printf("Replaced %d spectra (%d failed), removed %d RV points taken "
                "from them, across %zu stars.\n", deleted, failed, rvDeleted, byStar.size());
    return failed == 0 ? 0 : 1;
}

} // namespace

int validatePlan(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: --validate-plan <plan.json>\n");
        return 2;
    }
    QCoreApplication app(argc, argv);
    QFile f(QString::fromLocal8Bit(argv[2]));
    if (!f.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read %s\n", argv[2]);
        return 1;
    }
    const auto plan = astra::massfit::MassFitPlan::fromJsonString(QString::fromUtf8(f.readAll()));
    std::printf("%s: %d mode(s), %d setup(s), %d node(s)\n\n%s\n",
                qPrintable(plan.name), int(plan.modes.size()), int(plan.setups.size()),
                int(plan.nodes.size()), qPrintable(astra::massfit::describeTree(plan)));
    const QStringList problems = astra::massfit::validate(plan);
    for (const QString& p : problems) std::printf("problem: %s\n", qPrintable(p));
    std::printf("%s\n", problems.isEmpty() ? "valid" : "INVALID");
    return problems.isEmpty() ? 0 : 1;
}

int runMaintenanceMode(int argc, char** argv)
{
    if (argc >= 2 && std::strcmp(argv[1], "--validate-plan") == 0)
        return validatePlan(argc, argv);
    if (argc >= 2 && std::strcmp(argv[1], "--replace-imported-duplicates") == 0)
        return replaceImportedDuplicates(argc, argv);
    return -1;
}
