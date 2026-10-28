#include "db/StarRepository.h"
#include "db/DBAccess.h"
#include "db/SqlValue.h"
#include "fitting/ElementAbundances.h"
#include "core/Star.h"
#include "core/Project.h"
#include <QSqlQuery>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include "app/DataStore.h"
#include "app/Logger.h"
#include "catalog/StarMatching.h"
#include "lightcurve/Photometry.h"
#include "spectra/Spectrum.h"
#include "rv/RadialVelocity.h"
#include <QFile>
#include <QHash>
#include <QTextStream>
#include <QtConcurrent>

#include <algorithm>
#include <utility>

StarRepository::StarRepository(DBAccess& db) : _db(db) {}

// Galactic kinematics bindings shared by saveStar() and updateStarRow().
// All values use the NaN→NULL sentinel convention.
static void bindGalacticFields(QSqlQuery& query, const Star& star)
{
    query.bindValue(":gal_u", SqlValue::fromDouble(star.getGalU()));
    query.bindValue(":gal_e_u", SqlValue::fromDouble(star.getGalEU()));
    query.bindValue(":gal_e_u_up", SqlValue::fromDouble(star.getGalEUUp()));
    query.bindValue(":gal_e_u_down", SqlValue::fromDouble(star.getGalEUDown()));
    query.bindValue(":gal_v", SqlValue::fromDouble(star.getGalV()));
    query.bindValue(":gal_e_v", SqlValue::fromDouble(star.getGalEV()));
    query.bindValue(":gal_e_v_up", SqlValue::fromDouble(star.getGalEVUp()));
    query.bindValue(":gal_e_v_down", SqlValue::fromDouble(star.getGalEVDown()));
    query.bindValue(":gal_w", SqlValue::fromDouble(star.getGalW()));
    query.bindValue(":gal_e_w", SqlValue::fromDouble(star.getGalEW()));
    query.bindValue(":gal_e_w_up", SqlValue::fromDouble(star.getGalEWUp()));
    query.bindValue(":gal_e_w_down", SqlValue::fromDouble(star.getGalEWDown()));
    query.bindValue(":gal_x", SqlValue::fromDouble(star.getGalX()));
    query.bindValue(":gal_e_x", SqlValue::fromDouble(star.getGalEX()));
    query.bindValue(":gal_e_x_up", SqlValue::fromDouble(star.getGalEXUp()));
    query.bindValue(":gal_e_x_down", SqlValue::fromDouble(star.getGalEXDown()));
    query.bindValue(":gal_y", SqlValue::fromDouble(star.getGalY()));
    query.bindValue(":gal_e_y", SqlValue::fromDouble(star.getGalEY()));
    query.bindValue(":gal_e_y_up", SqlValue::fromDouble(star.getGalEYUp()));
    query.bindValue(":gal_e_y_down", SqlValue::fromDouble(star.getGalEYDown()));
    query.bindValue(":gal_z", SqlValue::fromDouble(star.getGalZ()));
    query.bindValue(":gal_e_z", SqlValue::fromDouble(star.getGalEZ()));
    query.bindValue(":gal_e_z_up", SqlValue::fromDouble(star.getGalEZUp()));
    query.bindValue(":gal_e_z_down", SqlValue::fromDouble(star.getGalEZDown()));
    query.bindValue(":gal_p_thin", SqlValue::fromDouble(star.getGalPThin()));
    query.bindValue(":gal_e_p_thin", SqlValue::fromDouble(star.getGalEPThin()));
    query.bindValue(":gal_p_thick", SqlValue::fromDouble(star.getGalPThick()));
    query.bindValue(":gal_e_p_thick", SqlValue::fromDouble(star.getGalEPThick()));
    query.bindValue(":gal_p_halo", SqlValue::fromDouble(star.getGalPHalo()));
    query.bindValue(":gal_e_p_halo", SqlValue::fromDouble(star.getGalEPHalo()));
    query.bindValue(":gal_jz", SqlValue::fromDouble(star.getGalJz()));
    query.bindValue(":gal_e_jz", SqlValue::fromDouble(star.getGalEJz()));
    query.bindValue(":gal_e_jz_up", SqlValue::fromDouble(star.getGalEJzUp()));
    query.bindValue(":gal_e_jz_down", SqlValue::fromDouble(star.getGalEJzDown()));
    query.bindValue(":gal_ecc", SqlValue::fromDouble(star.getGalEcc()));
    query.bindValue(":gal_e_ecc", SqlValue::fromDouble(star.getGalEEcc()));
    query.bindValue(":gal_e_ecc_up", SqlValue::fromDouble(star.getGalEEccUp()));
    query.bindValue(":gal_e_ecc_down", SqlValue::fromDouble(star.getGalEEccDown()));
}

// The 24 elements contribute three columns each, so the SQL for them is
// generated from the element table rather than written out; the fragments are
// spliced into the statements below and cached because they never change.
static const QString& abundanceColumnList()
{
    static const QString s = [] {
        QStringList cols;
        for (const auto& el : astra::elements::all())
            cols << QString("abund_%1, e_abund_%1, abund_%1_limit").arg(el.dbSuffix);
        return cols.join(", ") + ", ";
    }();
    return s;
}

static const QString& abundancePlaceholderList()
{
    static const QString s = [] {
        QStringList ph;
        for (const auto& el : astra::elements::all())
            ph << QString(":abund_%1, :e_abund_%1, :abund_%1_limit").arg(el.dbSuffix);
        return ph.join(", ") + ", ";
    }();
    return s;
}

static const QString& abundanceAssignmentList()
{
    static const QString s = [] {
        QStringList assign;
        for (const auto& el : astra::elements::all())
            assign << QString("abund_%1 = :abund_%1, e_abund_%1 = :e_abund_%1, "
                              "abund_%1_limit = :abund_%1_limit").arg(el.dbSuffix);
        return assign.join(", ") + ", ";
    }();
    return s;
}

// Preparing either of the wide star statements costs far more than running it:
// each is roughly 6 kB of SQL with 229 placeholders, so sqlite3_prepare_v2()
// dominates any bulk write. Measured over an 88 k-star project, re-preparing
// per star costs 28 s where a reused statement costs 3 s.
//
// Reuse is safe here because Qt resets a prepared query on every exec() and
// both statements below rebind every placeholder on every call, so nothing
// leaks from one star into the next.
namespace {

class PreparedStatement {
public:
    // Re-prepares only when the connection underneath changed: another
    // thread's connection (each instance is thread_local, so this is really
    // just the first call) or a reopened database, which bumps the epoch.
    QSqlQuery& get(DBAccess& access, const QString& sql)
    {
        const QSqlDatabase db = access.threadConnection();
        const quint64 epoch = access.connectionEpoch();

        if (!_prepared || _owner != &access || _epoch != epoch
            || _connection != db.connectionName()) {
            _query = QSqlQuery(db);
            _query.prepare(sql);
            _owner = &access;
            _connection = db.connectionName();
            _epoch = epoch;
            _prepared = true;
        }
        return _query;
    }

private:
    QSqlQuery      _query;
    const DBAccess* _owner = nullptr;
    QString        _connection;
    quint64        _epoch = 0;
    bool           _prepared = false;
};

}  // namespace

// Element abundance bindings shared by saveStar() and updateStarRow(); NaN
// stores as NULL like every other optional double.
static void bindAbundanceFields(QSqlQuery& query, const Star& star)
{
    // Going through the getters would allocate the star's lazy per-element
    // storage for every star saved, so a star without abundances just binds
    // the empty row directly.
    const bool any = star.hasAbundances();
    const auto& elements = astra::elements::all();
    for (int i = 0; i < elements.size(); ++i) {
        const QString& suffix = elements[i].dbSuffix;
        query.bindValue(":abund_" + suffix,
                        any ? SqlValue::fromDouble(star.getAbundance(i))
                            : QVariant());
        query.bindValue(":e_abund_" + suffix,
                        any ? SqlValue::fromDouble(star.getEAbundance(i))
                            : QVariant());
        query.bindValue(":abund_" + suffix + "_limit",
                        any ? star.getAbundanceLimit(i) : 0);
    }
}

size_t StarRepository::getStarCountForProject(const QString& projectId)
{
    QSqlQuery query(_db.threadConnection());
    query.prepare("SELECT COUNT(*) FROM stars WHERE project_id = :project_id");
    query.bindValue(":project_id", projectId);
    
    if (!query.exec() || !query.next()) {
        LOG_ERROR("Stars", QString("Failed to get star count: %1").arg(query.lastError().text()));
        return 0;
    }
    
    return query.value(0).toULongLong();
}

bool StarRepository::saveStars(const QString& projectId, const std::vector<std::shared_ptr<Star>>& stars)
{
    QSqlDatabase db = _db.threadConnection();      
    db.transaction();

    try {
        for (const auto& star : stars) {
            if (!saveStar(projectId, star)) {
                db.rollback();
                return false;
            }
        }
        db.commit();
        return true;
    } catch (...) {
        db.rollback();
        return false;
    }
}

bool StarRepository::moveStarsToProject(const std::vector<QString>& starIds,
                                        const QString& targetProjectId)
{
    if (starIds.empty() || targetProjectId.isEmpty())
        return false;

    QSqlDatabase db = _db.threadConnection();
    db.transaction();

    QSqlQuery q(db);
    q.prepare("UPDATE stars SET project_id = :pid WHERE id = :sid");
    for (const auto& id : starIds) {
        q.bindValue(":pid", targetProjectId);
        q.bindValue(":sid", id);
        if (!q.exec()) {
            LOG_WARNING("DB", QString("moveStarsToProject failed for %1: %2")
                                  .arg(id, q.lastError().text()));
            db.rollback();
            return false;
        }
    }
    db.commit();
    return true;
}

// ── Copying stars ─────────────────────────────────────────────────────────────
//
// A copy used to go through StarPackage: serialise the selection with every
// spectrum, fit and lightcurve loaded into memory, parse that buffer back into
// a second set of objects, then save those star by star. A large selection
// held its data twice over and could exhaust memory before the first row was
// written. Here no star is ever loaded: rows are duplicated inside SQLite with
// INSERT ... SELECT, ids are swapped through a temporary old -> new map, and
// the data files are copied on disk (QFile::copy clones them where the file
// system can).

namespace {

// A version-4 UUID in the form DBAccess::generateUUID() produces.
const QString kSqlUuid = QStringLiteral(
    "lower(hex(randomblob(4))) || '-' || lower(hex(randomblob(2))) || '-4' || "
    "substr(lower(hex(randomblob(2))), 2) || '-' || "
    "substr('89ab', 1 + (abs(random()) % 4), 1) || "
    "substr(lower(hex(randomblob(2))), 2) || '-' || lower(hex(randomblob(6)))");

// Stars copied per round. Bounds the id map and the file list held at once.
constexpr int kCopyChunk = 250;

/// A table of a star's tree. Its rows are the ones whose `parentCol` points at
/// a copied row of kind `parentKind`; `remap` names further columns that
/// refer into the tree and follow it to the copy (a reference outside the
/// copied stars is kept as it is).
struct CopyTable {
    QString                        table;
    QString                        kind;
    QString                        parentCol;
    QString                        parentKind;
    QList<QPair<QString, QString>> remap;   // column, kind
};

// Parents before children: each table's map is built from its parent's.
const QList<CopyTable>& copyTables()
{
    static const QList<CopyTable> t = {
        {"spectra",            "spectrum",          "star_id",       "star",       {}},
        {"spectral_fits",      "spectral_fit",      "spectrum_id",   "spectrum",   {}},
        {"photometry",         "photometry",        "star_id",       "star",       {}},
        {"photometric_points", "photometric_point", "photometry_id", "photometry", {}},
        {"lightcurves",        "lightcurve",        "photometry_id", "photometry", {}},
        {"lc_fits",            "lc_fit",            "lightcurve_id", "lightcurve", {}},
        {"sed_models",         "sed_model",         "photometry_id", "photometry", {}},
        {"rv_curves",          "rv_curve",          "star_id",       "star",       {}},
        {"rv_points",          "rv_point",          "curve_id",      "rv_curve",
         {{"spectrum_id", "spectrum"}, {"spectral_fit_id", "spectral_fit"}}},
        {"rv_fits",            "rv_fit",            "curve_id",      "rv_curve",   {}},
        {"periodograms",       "periodogram",       "star_id",       "star",       {}},
        {"rv_periodograms",    "rv_periodogram",    "curve_id",      "rv_curve",
         {{"star_id", "star"}}},
    };
    return t;
}

/// A data-file column and where the copy's file belongs. `select` lists the
/// copied rows of `kind` as (id, current path, ...) and `path` turns one
/// result row into the file's canonical location, the one the repository
/// would write (a later save of the copy then rewrites that file instead of
/// deleting it in favour of its own).
struct FileColumn {
    QString table;
    QString column;
    QString select;
    std::function<QString(const QString& base, const QSqlQuery& q)> path;
};

const QList<FileColumn>& fileColumns()
{
    // %1 is the copied rows' kind in the map; every select starts from it.
    static const QString from =
        QStringLiteral(" FROM temp.astra_copy_map m JOIN %1 x ON x.id = m.new ");
    static const QList<FileColumn> f = {
        {"spectra", "data_file",
         "SELECT x.id, x.data_file, x.star_id" + from.arg("spectra") +
             "WHERE m.kind = 'spectrum'",
         [](const QString& b, const QSqlQuery& q) {
             return DataStore::spectrumPath(b, q.value(2).toString(), q.value(0).toString());
         }},
        {"spectral_fits", "model_data_file",
         "SELECT x.id, x.model_data_file, s.star_id, x.spectrum_id" +
             from.arg("spectral_fits") +
             "JOIN spectra s ON s.id = x.spectrum_id WHERE m.kind = 'spectral_fit'",
         [](const QString& b, const QSqlQuery& q) {
             return DataStore::spectralFitPath(b, q.value(2).toString(),
                                               q.value(3).toString(), q.value(0).toString());
         }},
        {"photometry", "photometric_points_file",
         "SELECT x.id, x.photometric_points_file, x.star_id" + from.arg("photometry") +
             "WHERE m.kind = 'photometry'",
         [](const QString& b, const QSqlQuery& q) {
             return DataStore::photometricPointsPath(b, q.value(2).toString(),
                                                     q.value(0).toString());
         }},
        {"photometry", "sed_points_file",
         "SELECT x.id, x.sed_points_file, x.star_id" + from.arg("photometry") +
             "WHERE m.kind = 'photometry'",
         [](const QString& b, const QSqlQuery& q) {
             return DataStore::sedPhotometryPointsPath(b, q.value(2).toString(),
                                                       q.value(0).toString());
         }},
        {"lightcurves", "data_file",
         "SELECT x.id, x.data_file, p.star_id, x.photometry_id, x.source" +
             from.arg("lightcurves") +
             "JOIN photometry p ON p.id = x.photometry_id WHERE m.kind = 'lightcurve'",
         [](const QString& b, const QSqlQuery& q) {
             return DataStore::lightcurvePath(b, q.value(2).toString(),
                                              q.value(3).toString(), q.value(4).toString());
         }},
        {"lc_fits", "data_file",
         "SELECT x.id, x.data_file, p.star_id, l.photometry_id" + from.arg("lc_fits") +
             "JOIN lightcurves l ON l.id = x.lightcurve_id "
             "JOIN photometry p ON p.id = l.photometry_id WHERE m.kind = 'lc_fit'",
         [](const QString& b, const QSqlQuery& q) {
             return DataStore::lcFitPath(b, q.value(2).toString(),
                                         q.value(3).toString(), q.value(0).toString());
         }},
        {"sed_models", "model_data_file",
         "SELECT x.id, x.model_data_file, p.star_id, x.photometry_id" +
             from.arg("sed_models") +
             "JOIN photometry p ON p.id = x.photometry_id WHERE m.kind = 'sed_model'",
         [](const QString& b, const QSqlQuery& q) {
             return DataStore::sedModelPath(b, q.value(2).toString(),
                                            q.value(3).toString(), q.value(0).toString());
         }},
        {"periodograms", "data_file",
         "SELECT x.id, x.data_file, x.star_id" + from.arg("periodograms") +
             "WHERE m.kind = 'periodogram'",
         [](const QString& b, const QSqlQuery& q) {
             return DataStore::periodogramPath(b, q.value(2).toString(), q.value(0).toString());
         }},
        {"rv_periodograms", "data_file",
         "SELECT x.id, x.data_file, c.star_id" + from.arg("rv_periodograms") +
             "JOIN rv_curves c ON c.id = x.curve_id WHERE m.kind = 'rv_periodogram'",
         [](const QString& b, const QSqlQuery& q) {
             return DataStore::periodogramPath(b, q.value(2).toString(),
                                               "rv_" + q.value(0).toString());
         }},
    };
    return f;
}

struct FileJob {
    QString table, column, id, src, dst;
    bool    failed = false;
};

QString quoted(const QString& identifier)
{
    return '"' + QString(identifier).replace('"', "\"\"") + '"';
}

/// INSERT INTO table SELECT ... for the copied rows of `kind`: every column
/// is taken over as it is unless `exprFor` gives it another expression over
/// the source row `src` and its map entry `m`.
QString copyInsertSql(const QString& table, const QStringList& columns,
                      const QString& kind, const QHash<QString, QString>& exprFor)
{
    QStringList cols, exprs;
    for (const QString& c : columns) {
        cols << quoted(c);
        exprs << exprFor.value(c, "src." + quoted(c));
    }
    return QStringLiteral(
               "INSERT INTO %1 (%2) SELECT %3 FROM temp.astra_copy_map m "
               "JOIN %1 src ON src.id = m.old WHERE m.kind = '%4'")
        .arg(quoted(table), cols.join(", "), exprs.join(", "), kind);
}

QString mappedExpr(const QString& column, const QString& kind, bool keepUnmapped)
{
    const QString lookup =
        QStringLiteral("(SELECT new FROM temp.astra_copy_map WHERE kind = '%1' "
                       "AND old = src.%2)").arg(kind, quoted(column));
    return keepUnmapped ? QStringLiteral("COALESCE(%1, src.%2)").arg(lookup, quoted(column))
                        : lookup;
}

}   // namespace

StarRepository::CopyResult
StarRepository::copyStarsToProject(const std::vector<QString>& starIds,
                                   const QString& targetProjectId,
                                   const CopyProgress& progress)
{
    CopyResult r;
    if (starIds.empty() || targetProjectId.isEmpty()) {
        r.error = QStringLiteral("nothing to copy");
        return r;
    }
    auto report = [&progress](int pct, const QString& phase) {
        if (progress) progress(pct, phase);
    };

    QSqlDatabase db  = _db.threadConnection();
    const QString base = QFileInfo(_db.databasePath()).absolutePath() + "/data";

    // Columns per table, read from the schema so a column a later migration
    // adds is copied without anyone having to list it here. A table created
    // lazily (rv_periodograms) may not exist yet and is skipped.
    QHash<QString, QStringList> columns;
    {
        QSqlQuery q(db);
        QStringList tables{"stars"};
        for (const CopyTable& t : copyTables()) tables << t.table;
        for (const QString& t : tables) {
            if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(quoted(t)))) continue;
            QStringList cols;
            while (q.next()) cols << q.value(1).toString();
            if (!cols.isEmpty()) columns.insert(t, cols);
        }
    }

    auto fail = [&r, &db](const QString& what, const QSqlQuery& q) {
        r.error = QStringLiteral("%1: %2").arg(what, q.lastError().text());
        LOG_WARNING("DB", "copyStarsToProject: " + r.error);
        return false;
    };

    QSqlQuery q(db);
    q.exec("DROP TABLE IF EXISTS temp.astra_copy_map");
    q.exec("DROP TABLE IF EXISTS temp.astra_copy_src");
    if (!q.exec("CREATE TEMP TABLE astra_copy_map (kind TEXT NOT NULL, old TEXT NOT NULL, "
                "new TEXT NOT NULL, PRIMARY KEY (kind, old))") ||
        !q.exec("CREATE TEMP TABLE astra_copy_src (id TEXT PRIMARY KEY)")) {
        fail("temporary tables", q);
        return r;
    }

    // The statements of one round, built once.
    QStringList mapSql, insertSql;
    mapSql << QStringLiteral(
                  "INSERT INTO temp.astra_copy_map (kind, old, new) SELECT 'star', s.id, %1 "
                  "FROM temp.astra_copy_src c JOIN stars s ON s.id = c.id").arg(kSqlUuid);
    insertSql << copyInsertSql("stars", columns.value("stars"), "star",
                               {{"id", "m.new"}, {"project_id", ":target"}});
    for (const CopyTable& t : copyTables()) {
        if (!columns.contains(t.table)) continue;
        mapSql << QStringLiteral(
                      "INSERT INTO temp.astra_copy_map (kind, old, new) "
                      "SELECT '%1', c.id, %2 FROM temp.astra_copy_map p "
                      "JOIN %3 c ON c.%4 = p.old WHERE p.kind = '%5'")
                      .arg(t.kind, kSqlUuid, quoted(t.table), quoted(t.parentCol), t.parentKind);
        QHash<QString, QString> expr{{"id", "m.new"},
                                     {t.parentCol, mappedExpr(t.parentCol, t.parentKind, false)}};
        for (const auto& [col, kind] : t.remap)
            if (columns.value(t.table).contains(col))
                expr.insert(col, mappedExpr(col, kind, true));
        insertSql << copyInsertSql(t.table, columns.value(t.table), t.kind, expr);
    }

    if (!db.transaction()) {
        r.error = QStringLiteral("could not start a transaction: %1")
                      .arg(db.lastError().text());
        return r;
    }

    QHash<QString, QString> newIdOf;     // old star id -> copy
    bool ok = true;
    const int total = int(starIds.size());

    for (int from = 0; ok && from < total; from += kCopyChunk) {
        const int to = std::min(total, from + kCopyChunk);
        report(int(100.0 * from / total),
               QStringLiteral("Copying stars %1-%2 of %3").arg(from + 1).arg(to).arg(total));

        ok = q.exec("DELETE FROM temp.astra_copy_map") && q.exec("DELETE FROM temp.astra_copy_src");
        if (!ok) { fail("clearing the id map", q); break; }

        QSqlQuery add(db);
        add.prepare("INSERT OR IGNORE INTO temp.astra_copy_src (id) VALUES (?)");
        for (int i = from; ok && i < to; ++i) {
            add.bindValue(0, starIds[size_t(i)]);
            if (!add.exec()) ok = fail("listing the stars", add);
        }

        // Ids first (every table's map is drawn from its parent's), then the
        // rows themselves, which look their parents' new ids up in that map.
        for (const QString& sql : mapSql)
            if (ok && !q.exec(sql)) ok = fail("mapping ids", q);
        for (const QString& sql : insertSql) {
            if (!ok) break;
            QSqlQuery ins(db);
            ins.prepare(sql);
            if (sql.contains(":target")) ins.bindValue(":target", targetProjectId);
            if (!ins.exec()) ok = fail("copying rows", ins);
        }
        if (!ok) break;

        if (q.exec("SELECT old, new FROM temp.astra_copy_map WHERE kind = 'star'"))
            while (q.next()) newIdOf.insert(q.value(0).toString(), q.value(1).toString());

        // The copied rows still name the originals' files. Give each its own
        // file at the place the repository would put it.
        std::vector<FileJob> jobs;
        for (const FileColumn& f : fileColumns()) {
            if (!columns.value(f.table).contains(f.column)) continue;
            if (!q.exec(f.select)) { ok = fail("listing data files", q); break; }
            while (q.next()) {
                const QString src = q.value(1).toString();
                if (src.isEmpty()) continue;
                jobs.push_back({f.table, f.column, q.value(0).toString(), src,
                                f.path(base, q)});
            }
        }
        if (!ok) break;

        QtConcurrent::blockingMap(jobs, [](FileJob& j) {
            if (!QFile::exists(j.src)) {     // the original has lost it too
                j.dst.clear();
                return;
            }
            DataStore::ensureDirForFile(j.dst);
            QFile::remove(j.dst);
            j.failed = !QFile::copy(j.src, j.dst);
            // A fit's shared base file goes along, under the same name (the
            // name is the hash of its contents). Fits of one spectrum share
            // it, so another job may have copied it already.
            if (!j.failed && j.table == QLatin1String("spectral_fits")) {
                const QString base = SpectralFit::basePathOf(j.src);
                if (!base.isEmpty()) {
                    const QString to = QFileInfo(j.dst).absolutePath() + '/' +
                                       QFileInfo(base).fileName();
                    j.failed = !QFile::exists(to) && !QFile::copy(base, to) &&
                               !QFile::exists(to);
                }
            }
        });

        QHash<QString, QSqlQuery> updates;   // one prepared UPDATE per column
        for (const FileJob& j : jobs) {
            if (j.failed) {
                r.error = QStringLiteral("could not copy %1 to %2").arg(j.src, j.dst);
                ok = false;
                break;
            }
            const QString key = j.table + '.' + j.column;
            auto it = updates.find(key);
            if (it == updates.end()) {
                it = updates.insert(key, QSqlQuery(db));
                it->prepare(QStringLiteral("UPDATE %1 SET %2 = ? WHERE id = ?")
                                .arg(quoted(j.table), quoted(j.column)));
            }
            QSqlQuery& up = *it;
            up.bindValue(0, j.dst.isEmpty() ? QVariant() : QVariant(j.dst));
            up.bindValue(1, j.id);
            if (!up.exec()) { ok = fail("pointing a copy at its file", up); break; }
        }
    }

    if (ok && !db.commit()) {
        r.error = QStringLiteral("commit failed: %1").arg(db.lastError().text());
        ok = false;
    }
    if (!ok) {
        db.rollback();
        // Whatever files were copied belong to rows that no longer exist.
        for (const QString& id : std::as_const(newIdOf))
            DataStore::removeStarData(base, id);
        newIdOf.clear();
    }
    q.exec("DROP TABLE IF EXISTS temp.astra_copy_map");
    q.exec("DROP TABLE IF EXISTS temp.astra_copy_src");

    r.ok = ok;
    r.newStarIds.reserve(starIds.size());
    for (const QString& id : starIds) r.newStarIds.push_back(newIdOf.value(id));
    report(100, QStringLiteral("Done"));
    return r;
}

// %1 / %2 are the generated element abundance columns and placeholders.
static const QString& saveStarSql()
{
    static const QString s = QString(R"(
        INSERT OR REPLACE INTO stars (
            id, project_id, alias, source_id, source_id_norm, tic, jname,
            ra, dec, pmra, pmdec, e_pmra, e_pmdec, plx, e_plx,
            pmra_pmdec_corr, plx_pmdec_corr, plx_pmra_corr,
            gmag, e_gmag, bp, e_bp, rp, e_rp, bp_rp,
            spec_class, teff, e_teff, logg, e_logg, he, e_he,
            e_teff_up, e_teff_down, e_logg_up, e_logg_down, e_he_up, e_he_down,
            teff_limit, logg_limit, he_limit,
            logp, deltaRV, e_deltaRV, rv_avg, e_rv_avg, rv_med, e_rv_med,
            n_spectra, n_fit_spectra,
            rv_timespan, rv_npoints, rv_k, rv_e_k,
            rv_period, rv_e_period, rv_gamma, rv_e_gamma,
            rv_e_k_up, rv_e_k_down, rv_e_period_up, rv_e_period_down,
            rv_e_gamma_up, rv_e_gamma_down,
            rv_k2, rv_e_k2, rv_e_k2_up, rv_e_k2_down,
            rv_ecc, rv_phi, rv_t0, rv_chi2, rv_rms,
            sed_mass1, sed_e_mass1, sed_radius1, sed_e_radius1,
            sed_lum1, sed_e_lum1,
            sed_mass2, sed_e_mass2, sed_radius2, sed_e_radius2,
            sed_lum2, sed_e_lum2,
            sed_e_mass1_up, sed_e_mass1_down,
            sed_e_radius1_up, sed_e_radius1_down,
            sed_e_lum1_up, sed_e_lum1_down,
            sed_e_mass2_up, sed_e_mass2_down,
            sed_e_radius2_up, sed_e_radius2_down,
            sed_e_lum2_up, sed_e_lum2_down,
            phot_period, phot_e_period, phot_incl, phot_e_incl,
            phot_q, phot_e_q,
            phot_e_period_up, phot_e_period_down,
            phot_e_incl_up, phot_e_incl_down,
            phot_e_q_up, phot_e_q_down,
            has_tess, has_gaia, has_ztf, has_atlas, has_blackgem,
            tess_crowdsap, phot_peaks_json,
            comp_mass_min, comp_e_mass_min, comp_mass_true, comp_e_mass_true,
            comp_e_mass_min_up, comp_e_mass_min_down,
            comp_e_mass_true_up, comp_e_mass_true_down,
            gal_u, gal_e_u, gal_e_u_up, gal_e_u_down,
            gal_v, gal_e_v, gal_e_v_up, gal_e_v_down,
            gal_w, gal_e_w, gal_e_w_up, gal_e_w_down,
            gal_x, gal_e_x, gal_e_x_up, gal_e_x_down,
            gal_y, gal_e_y, gal_e_y_up, gal_e_y_down,
            gal_z, gal_e_z, gal_e_z_up, gal_e_z_down,
            gal_p_thin, gal_e_p_thin, gal_p_thick, gal_e_p_thick,
            gal_p_halo, gal_e_p_halo,
            gal_jz, gal_e_jz, gal_e_jz_up, gal_e_jz_down,
            gal_ecc, gal_e_ecc, gal_e_ecc_up, gal_e_ecc_down,
            %1
            bibcodes
        ) VALUES (
            :id, :project_id, :alias, :source_id, :source_id_norm, :tic, :jname,
            :ra, :dec, :pmra, :pmdec, :e_pmra, :e_pmdec, :plx, :e_plx,
            :pmra_pmdec_corr, :plx_pmdec_corr, :plx_pmra_corr,
            :gmag, :e_gmag, :bp, :e_bp, :rp, :e_rp, :bp_rp,
            :spec_class, :teff, :e_teff, :logg, :e_logg, :he, :e_he,
            :e_teff_up, :e_teff_down, :e_logg_up, :e_logg_down, :e_he_up, :e_he_down,
            :teff_limit, :logg_limit, :he_limit,
            :logp, :deltaRV, :e_deltaRV, :rv_avg, :e_rv_avg, :rv_med, :e_rv_med,
            :n_spectra, :n_fit_spectra,
            :rv_timespan, :rv_npoints, :rv_k, :rv_e_k,
            :rv_period, :rv_e_period, :rv_gamma, :rv_e_gamma,
            :rv_e_k_up, :rv_e_k_down, :rv_e_period_up, :rv_e_period_down,
            :rv_e_gamma_up, :rv_e_gamma_down,
            :rv_k2, :rv_e_k2, :rv_e_k2_up, :rv_e_k2_down,
            :rv_ecc, :rv_phi, :rv_t0, :rv_chi2, :rv_rms,
            :sed_mass1, :sed_e_mass1, :sed_radius1, :sed_e_radius1,
            :sed_lum1, :sed_e_lum1,
            :sed_mass2, :sed_e_mass2, :sed_radius2, :sed_e_radius2,
            :sed_lum2, :sed_e_lum2,
            :sed_e_mass1_up, :sed_e_mass1_down,
            :sed_e_radius1_up, :sed_e_radius1_down,
            :sed_e_lum1_up, :sed_e_lum1_down,
            :sed_e_mass2_up, :sed_e_mass2_down,
            :sed_e_radius2_up, :sed_e_radius2_down,
            :sed_e_lum2_up, :sed_e_lum2_down,
            :phot_period, :phot_e_period, :phot_incl, :phot_e_incl,
            :phot_q, :phot_e_q,
            :phot_e_period_up, :phot_e_period_down,
            :phot_e_incl_up, :phot_e_incl_down,
            :phot_e_q_up, :phot_e_q_down,
            :has_tess, :has_gaia, :has_ztf, :has_atlas, :has_blackgem,
            :tess_crowdsap, :phot_peaks_json,
            :comp_mass_min, :comp_e_mass_min, :comp_mass_true, :comp_e_mass_true,
            :comp_e_mass_min_up, :comp_e_mass_min_down,
            :comp_e_mass_true_up, :comp_e_mass_true_down,
            :gal_u, :gal_e_u, :gal_e_u_up, :gal_e_u_down,
            :gal_v, :gal_e_v, :gal_e_v_up, :gal_e_v_down,
            :gal_w, :gal_e_w, :gal_e_w_up, :gal_e_w_down,
            :gal_x, :gal_e_x, :gal_e_x_up, :gal_e_x_down,
            :gal_y, :gal_e_y, :gal_e_y_up, :gal_e_y_down,
            :gal_z, :gal_e_z, :gal_e_z_up, :gal_e_z_down,
            :gal_p_thin, :gal_e_p_thin, :gal_p_thick, :gal_e_p_thick,
            :gal_p_halo, :gal_e_p_halo,
            :gal_jz, :gal_e_jz, :gal_e_jz_up, :gal_e_jz_down,
            :gal_ecc, :gal_e_ecc, :gal_e_ecc_up, :gal_e_ecc_down,
            %2
            :bibcodes
        )
    )")
        .arg(abundanceColumnList(), abundancePlaceholderList());
    return s;
}

bool StarRepository::saveStar(const QString& projectId, std::shared_ptr<Star> star)
{
    // Generate UUID if star doesn't have one
    if (star->getId().isEmpty()) {
        star->setId(_db.generateUUID());
    }

    static thread_local PreparedStatement stmt;

    QSqlQuery& query = stmt.get(_db, saveStarSql());

    query.bindValue(":id", star->getId());
    query.bindValue(":project_id", projectId);
    query.bindValue(":alias", star->getAlias());
    query.bindValue(":source_id", star->getSourceId());
    // Kept in lockstep with source_id so findMatchingStarId can match on an
    // indexed equality instead of a suffix LIKE.
    query.bindValue(":source_id_norm",
                    StarMatching::normalizeSourceId(star->getSourceId()));
    query.bindValue(":tic", star->getTic());
    query.bindValue(":jname", star->getJName());
    
    query.bindValue(":ra", star->getRa());
    query.bindValue(":dec", star->getDec());
    query.bindValue(":pmra", star->getPmra());
    query.bindValue(":pmdec", star->getPmdec());
    query.bindValue(":e_pmra", star->getEPmra());
    query.bindValue(":e_pmdec", star->getEPmdec());
    query.bindValue(":plx", star->getPlx());
    query.bindValue(":e_plx", star->getEPlx());
    query.bindValue(":pmra_pmdec_corr", star->getPmraPmdecCorr());
    query.bindValue(":plx_pmdec_corr", star->getPlxPmdecCorr());
    query.bindValue(":plx_pmra_corr", star->getPlxPmraCorr());
    
    query.bindValue(":gmag", star->getGmag());
    query.bindValue(":e_gmag", star->getEGmag());
    query.bindValue(":bp", star->getBp());
    query.bindValue(":e_bp", star->getEBp());
    query.bindValue(":rp", star->getRp());
    query.bindValue(":e_rp", star->getERp());
    query.bindValue(":bp_rp", star->getBpRp());
    
    query.bindValue(":spec_class", star->getSpecClass());
    query.bindValue(":teff", star->getTeff());
    query.bindValue(":e_teff", star->getETeff());
    query.bindValue(":logg", star->getLogg());
    query.bindValue(":e_logg", star->getELogg());
    query.bindValue(":he", star->getHe());
    query.bindValue(":e_he", star->getEHe());
    query.bindValue(":e_teff_up", SqlValue::fromDouble(star->getETeffUp()));
    query.bindValue(":e_teff_down", SqlValue::fromDouble(star->getETeffDown()));
    query.bindValue(":e_logg_up", SqlValue::fromDouble(star->getELoggUp()));
    query.bindValue(":e_logg_down", SqlValue::fromDouble(star->getELoggDown()));
    query.bindValue(":e_he_up", SqlValue::fromDouble(star->getEHeUp()));
    query.bindValue(":e_he_down", SqlValue::fromDouble(star->getEHeDown()));
    query.bindValue(":teff_limit", star->getTeffLimit());
    query.bindValue(":logg_limit", star->getLoggLimit());
    query.bindValue(":he_limit", star->getHeLimit());

    query.bindValue(":logp", star->getLogP());
    query.bindValue(":deltaRV", star->getDeltaRV());
    query.bindValue(":e_deltaRV", star->getEDeltaRV());
    query.bindValue(":rv_avg", star->getRVAvg());
    query.bindValue(":e_rv_avg", star->getERVAvg());
    query.bindValue(":rv_med", star->getRVMed());
    query.bindValue(":e_rv_med", star->getERVMed());

    query.bindValue(":n_spectra", star->getNSpectra());
    query.bindValue(":n_fit_spectra", star->getNFitSpectra());

    query.bindValue(":rv_timespan", star->getRVTimespan());
    query.bindValue(":rv_npoints", star->getRVNPoints());
    query.bindValue(":rv_k", star->getRVK());
    query.bindValue(":rv_e_k", star->getRVEK());
    query.bindValue(":rv_period", star->getRVPeriod());
    query.bindValue(":rv_e_period", star->getRVEPeriod());
    query.bindValue(":rv_gamma", star->getRVGamma());
    query.bindValue(":rv_e_gamma", star->getRVEGamma());
    query.bindValue(":rv_e_k_up", SqlValue::fromDouble(star->getRVEKUp()));
    query.bindValue(":rv_e_k_down", SqlValue::fromDouble(star->getRVEKDown()));
    query.bindValue(":rv_k2", SqlValue::fromDouble(star->getRVK2()));
    query.bindValue(":rv_e_k2", SqlValue::fromDouble(star->getRVEK2()));
    query.bindValue(":rv_e_k2_up", SqlValue::fromDouble(star->getRVEK2Up()));
    query.bindValue(":rv_e_k2_down", SqlValue::fromDouble(star->getRVEK2Down()));
    query.bindValue(":rv_e_period_up", SqlValue::fromDouble(star->getRVEPeriodUp()));
    query.bindValue(":rv_e_period_down", SqlValue::fromDouble(star->getRVEPeriodDown()));
    query.bindValue(":rv_e_gamma_up", SqlValue::fromDouble(star->getRVEGammaUp()));
    query.bindValue(":rv_e_gamma_down", SqlValue::fromDouble(star->getRVEGammaDown()));
    query.bindValue(":rv_ecc", star->getRVEcc());
    query.bindValue(":rv_phi", star->getRVPhi());
    query.bindValue(":rv_t0", star->getRVT0());
    query.bindValue(":rv_chi2", star->getRVChi2());
    query.bindValue(":rv_rms", star->getRVRms());

    query.bindValue(":sed_mass1", star->getSedMass1());
    query.bindValue(":sed_e_mass1", star->getSedEMass1());
    query.bindValue(":sed_radius1", star->getSedRadius1());
    query.bindValue(":sed_e_radius1", star->getSedERadius1());
    query.bindValue(":sed_lum1", star->getSedLum1());
    query.bindValue(":sed_e_lum1", star->getSedELum1());
    query.bindValue(":sed_mass2", star->getSedMass2());
    query.bindValue(":sed_e_mass2", star->getSedEMass2());
    query.bindValue(":sed_radius2", star->getSedRadius2());
    query.bindValue(":sed_e_radius2", star->getSedERadius2());
    query.bindValue(":sed_lum2", star->getSedLum2());
    query.bindValue(":sed_e_lum2", star->getSedELum2());
    query.bindValue(":sed_e_mass1_up", SqlValue::fromDouble(star->getSedEMass1Up()));
    query.bindValue(":sed_e_mass1_down", SqlValue::fromDouble(star->getSedEMass1Down()));
    query.bindValue(":sed_e_radius1_up", SqlValue::fromDouble(star->getSedERadius1Up()));
    query.bindValue(":sed_e_radius1_down", SqlValue::fromDouble(star->getSedERadius1Down()));
    query.bindValue(":sed_e_lum1_up", SqlValue::fromDouble(star->getSedELum1Up()));
    query.bindValue(":sed_e_lum1_down", SqlValue::fromDouble(star->getSedELum1Down()));
    query.bindValue(":sed_e_mass2_up", SqlValue::fromDouble(star->getSedEMass2Up()));
    query.bindValue(":sed_e_mass2_down", SqlValue::fromDouble(star->getSedEMass2Down()));
    query.bindValue(":sed_e_radius2_up", SqlValue::fromDouble(star->getSedERadius2Up()));
    query.bindValue(":sed_e_radius2_down", SqlValue::fromDouble(star->getSedERadius2Down()));
    query.bindValue(":sed_e_lum2_up", SqlValue::fromDouble(star->getSedELum2Up()));
    query.bindValue(":sed_e_lum2_down", SqlValue::fromDouble(star->getSedELum2Down()));

    query.bindValue(":phot_period", star->getPhotPeriod());
    query.bindValue(":phot_e_period", star->getPhotEPeriod());
    query.bindValue(":phot_incl", star->getPhotIncl());
    query.bindValue(":phot_e_incl", star->getPhotEIncl());
    query.bindValue(":phot_q", star->getPhotQ());
    query.bindValue(":phot_e_q", star->getPhotEQ());
    query.bindValue(":phot_e_period_up", SqlValue::fromDouble(star->getPhotEPeriodUp()));
    query.bindValue(":phot_e_period_down", SqlValue::fromDouble(star->getPhotEPeriodDown()));
    query.bindValue(":phot_e_incl_up", SqlValue::fromDouble(star->getPhotEInclUp()));
    query.bindValue(":phot_e_incl_down", SqlValue::fromDouble(star->getPhotEInclDown()));
    query.bindValue(":phot_e_q_up", SqlValue::fromDouble(star->getPhotEQUp()));
    query.bindValue(":phot_e_q_down", SqlValue::fromDouble(star->getPhotEQDown()));

    query.bindValue(":has_tess", star->getHasTess() ? 1 : 0);
    query.bindValue(":has_gaia", star->getHasGaia() ? 1 : 0);
    query.bindValue(":has_ztf", star->getHasZtf() ? 1 : 0);
    query.bindValue(":has_atlas", star->getHasAtlas() ? 1 : 0);
    query.bindValue(":has_blackgem", star->getHasBlackgem() ? 1 : 0);

    query.bindValue(":tess_crowdsap", star->getTessCrowdsap());
    // Without this bind the placeholder is NULL and the INSERT OR REPLACE wipes
    // any periodogram peaks saved via DatabaseManager::saveStarPhotPeaks().
    query.bindValue(":phot_peaks_json", star->getPhotPeaksJson());

    query.bindValue(":comp_mass_min", star->getCompMassMin());
    query.bindValue(":comp_e_mass_min", star->getECompMassMin());
    query.bindValue(":comp_mass_true", star->getCompMassTrue());
    query.bindValue(":comp_e_mass_true", star->getECompMassTrue());
    query.bindValue(":comp_e_mass_min_up", SqlValue::fromDouble(star->getECompMassMinUp()));
    query.bindValue(":comp_e_mass_min_down", SqlValue::fromDouble(star->getECompMassMinDown()));
    query.bindValue(":comp_e_mass_true_up", SqlValue::fromDouble(star->getECompMassTrueUp()));
    query.bindValue(":comp_e_mass_true_down", SqlValue::fromDouble(star->getECompMassTrueDown()));

    bindGalacticFields(query, *star);
    bindAbundanceFields(query, *star);

    // Convert bibcodes to JSON array
    QJsonArray bibcodesArray;
    for (const auto& bibcode : star->getBibcodes()) {
        bibcodesArray.append(bibcode);
    }
    query.bindValue(":bibcodes", QJsonDocument(bibcodesArray).toJson(QJsonDocument::Compact));

    if (!query.exec()) {
        LOG_ERROR("Stars", QString("Failed to save star: %1").arg(query.lastError().text()));
        return false;
    }

    return true;
}

bool StarRepository::updateStar(const QString& projectId, std::shared_ptr<Star> star)
{
    // Simply use saveStar with INSERT OR REPLACE
    return saveStar(projectId, star);
}

bool StarRepository::deleteStar(const QString& projectId, const QString& starId)
{
    // Everything hanging off a star has to go with it. The schema declares
    // ON DELETE CASCADE on most of these tables, but no connection ever runs
    // PRAGMA foreign_keys=ON and SQLite defaults it off, so no cascade has ever
    // fired: every child row has to be deleted explicitly here. Anything left
    // behind is unreachable - nothing joins back to a deleted star - so it just
    // grows the database and can collide with a re-imported star of the same id.
    //
    // The on-disk blobs need no separate handling: DataStore::removeStarData
    // drops the star's whole directory, periodograms and lightcurve models
    // included.
    DataStore::removeStarData(QFileInfo(_db.databasePath()).absolutePath() + "/data", starId);

    QSqlDatabase db = _db.threadConnection();

    // Some of these tables are created lazily the first time their repository
    // writes (rv_periodograms is), so a database that has never held a
    // periodogram does not have one. Ask the schema rather than assuming, or a
    // missing table fails the statement and rolls the whole delete back.
    QSet<QString> tables;
    {
        QSqlQuery q(db);
        if (q.exec("SELECT name FROM sqlite_master WHERE type = 'table'"))
            while (q.next()) tables.insert(q.value(0).toString());
    }

    // One transaction: a partial delete is what leaves orphans in the first place.
    const bool inTransaction = db.transaction();

    auto exec = [&db, &tables](const QString& table, const QString& where,
                               const QString& key, const QString& value) {
        if (!tables.contains(table)) return true;
        const QString sql = QStringLiteral("DELETE FROM %1 WHERE %2").arg(table, where);
        QSqlQuery q(db);
        q.prepare(sql);
        q.bindValue(key, value);
        if (!q.exec()) {
            qWarning() << "deleteStar:" << sql << q.lastError().text();
            return false;
        }
        return true;
    };

    auto idsFrom = [&db](const QString& sql, const QString& key,
                         const QString& value) {
        QStringList ids;
        QSqlQuery q(db);
        q.prepare(sql);
        q.bindValue(key, value);
        if (q.exec())
            while (q.next()) ids << q.value(0).toString();
        return ids;
    };

    bool ok = true;

    // ── Photometry: SED models, lightcurves, their fits and models, points ──
    for (const QString& photometryId :
         idsFrom("SELECT id FROM photometry WHERE star_id = :star_id",
                 ":star_id", starId)) {
        ok &= exec("sed_models", "photometry_id = :id", ":id", photometryId);

        // No lightcurve_models delete here: no schema, migration or repository
        // in the project ever creates that table. The statement that used to
        // stand here referenced a table that has never existed.
        for (const QString& lightcurveId :
             idsFrom("SELECT id FROM lightcurves WHERE photometry_id = :id",
                     ":id", photometryId)) {
            ok &= exec("lc_fits", "lightcurve_id = :id", ":id", lightcurveId);
        }

        ok &= exec("lightcurves", "photometry_id = :id", ":id", photometryId);
        ok &= exec("photometric_points", "photometry_id = :id", ":id", photometryId);
    }
    ok &= exec("photometry", "star_id = :star_id", ":star_id", starId);

    // ── Spectra and their fits ──────────────────────────────────────────────
    for (const QString& spectrumId :
         idsFrom("SELECT id FROM spectra WHERE star_id = :star_id",
                 ":star_id", starId)) {
        ok &= exec("spectral_fits", "spectrum_id = :id", ":id", spectrumId);
    }
    ok &= exec("spectra", "star_id = :star_id", ":star_id", starId);

    // ── Radial velocities: points, fits and per-curve periodograms ──────────
    for (const QString& curveId :
         idsFrom("SELECT id FROM rv_curves WHERE star_id = :star_id",
                 ":star_id", starId)) {
        ok &= exec("rv_points", "curve_id = :id", ":id", curveId);
        ok &= exec("rv_fits", "curve_id = :id", ":id", curveId);
        ok &= exec("rv_periodograms", "curve_id = :id", ":id", curveId);
    }
    ok &= exec("rv_curves", "star_id = :star_id", ":star_id", starId);

    // ── Periodograms, mass-fit bookkeeping and adopted remote runs ──────────
    ok &= exec("periodograms", "star_id = :star_id", ":star_id", starId);
    ok &= exec("periodogram_store", "star_id = :star_id", ":star_id", starId);
    ok &= exec("mass_fit_attempts", "star_id = :star_id", ":star_id", starId);
    ok &= exec("mass_fit_run_stars", "star_id = :star_id", ":star_id", starId);
    ok &= exec("remote_fit_runs", "star_id = :star_id", ":star_id", starId);

    // ── The star itself ─────────────────────────────────────────────────────
    QSqlQuery query(db);
    query.prepare("DELETE FROM stars WHERE id = :id AND project_id = :project_id");
    query.bindValue(":id", starId);
    query.bindValue(":project_id", projectId);
    ok &= query.exec();

    if (inTransaction) {
        if (ok) db.commit();
        else    db.rollback();
    }
    return ok;
}

bool StarRepository::importCSV(const QString& filepath, std::shared_ptr<Project> project)
{
    // TODO: Implement CSV import
    Q_UNUSED(filepath)
    Q_UNUSED(project)
    return true;
}

bool StarRepository::updateSpectraCounts(const QString& starId, int nSpectra,
                                        int nFitSpectra)
{
    if (starId.isEmpty()) return false;

    QSqlQuery query(_db.threadConnection());
    query.prepare(QStringLiteral(
        "UPDATE stars SET n_spectra = :n, n_fit_spectra = :nfit "
        "WHERE id = :id"));
    query.bindValue(":n", nSpectra);
    query.bindValue(":nfit", nFitSpectra);
    query.bindValue(":id", starId);

    if (!query.exec()) {
        LOG_ERROR("Stars", QString("Failed to update spectrum counts for star "
                                   "%1: %2")
                               .arg(starId, query.lastError().text()));
        return false;
    }
    return true;
}

// %1 is the generated element abundance assignment list.
static const QString& updateStarRowSql()
{
    static const QString s = QString(R"(
        UPDATE stars SET
            alias = :alias, source_id = :source_id,
            source_id_norm = :source_id_norm, tic = :tic, jname = :jname,
            ra = :ra, dec = :dec, pmra = :pmra, pmdec = :pmdec,
            e_pmra = :e_pmra, e_pmdec = :e_pmdec, plx = :plx, e_plx = :e_plx,
            pmra_pmdec_corr = :pmra_pmdec_corr, plx_pmdec_corr = :plx_pmdec_corr,
            plx_pmra_corr = :plx_pmra_corr,
            gmag = :gmag, e_gmag = :e_gmag, bp = :bp, e_bp = :e_bp,
            rp = :rp, e_rp = :e_rp, bp_rp = :bp_rp,
            spec_class = :spec_class, teff = :teff, e_teff = :e_teff,
            logg = :logg, e_logg = :e_logg, he = :he, e_he = :e_he,
            e_teff_up = :e_teff_up, e_teff_down = :e_teff_down,
            e_logg_up = :e_logg_up, e_logg_down = :e_logg_down,
            e_he_up = :e_he_up, e_he_down = :e_he_down,
            teff_limit = :teff_limit, logg_limit = :logg_limit, he_limit = :he_limit,
            logp = :logp, deltaRV = :deltaRV, e_deltaRV = :e_deltaRV,
            rv_avg = :rv_avg, e_rv_avg = :e_rv_avg, rv_med = :rv_med, e_rv_med = :e_rv_med,
            n_spectra = :n_spectra, n_fit_spectra = :n_fit_spectra,
            rv_timespan = :rv_timespan, rv_npoints = :rv_npoints,
            rv_k = :rv_k, rv_e_k = :rv_e_k,
            rv_period = :rv_period, rv_e_period = :rv_e_period,
            rv_gamma = :rv_gamma, rv_e_gamma = :rv_e_gamma,
            rv_e_k_up = :rv_e_k_up, rv_e_k_down = :rv_e_k_down,
            rv_k2 = :rv_k2, rv_e_k2 = :rv_e_k2,
            rv_e_k2_up = :rv_e_k2_up, rv_e_k2_down = :rv_e_k2_down,
            rv_e_period_up = :rv_e_period_up, rv_e_period_down = :rv_e_period_down,
            rv_e_gamma_up = :rv_e_gamma_up, rv_e_gamma_down = :rv_e_gamma_down,
            rv_ecc = :rv_ecc, rv_phi = :rv_phi, rv_t0 = :rv_t0,
            rv_chi2 = :rv_chi2, rv_rms = :rv_rms,
            sed_mass1 = :sed_mass1, sed_e_mass1 = :sed_e_mass1,
            sed_radius1 = :sed_radius1, sed_e_radius1 = :sed_e_radius1,
            sed_lum1 = :sed_lum1, sed_e_lum1 = :sed_e_lum1,
            sed_mass2 = :sed_mass2, sed_e_mass2 = :sed_e_mass2,
            sed_radius2 = :sed_radius2, sed_e_radius2 = :sed_e_radius2,
            sed_lum2 = :sed_lum2, sed_e_lum2 = :sed_e_lum2,
            sed_e_mass1_up = :sed_e_mass1_up, sed_e_mass1_down = :sed_e_mass1_down,
            sed_e_radius1_up = :sed_e_radius1_up, sed_e_radius1_down = :sed_e_radius1_down,
            sed_e_lum1_up = :sed_e_lum1_up, sed_e_lum1_down = :sed_e_lum1_down,
            sed_e_mass2_up = :sed_e_mass2_up, sed_e_mass2_down = :sed_e_mass2_down,
            sed_e_radius2_up = :sed_e_radius2_up, sed_e_radius2_down = :sed_e_radius2_down,
            sed_e_lum2_up = :sed_e_lum2_up, sed_e_lum2_down = :sed_e_lum2_down,
            phot_period = :phot_period, phot_e_period = :phot_e_period,
            phot_incl = :phot_incl, phot_e_incl = :phot_e_incl,
            phot_q = :phot_q, phot_e_q = :phot_e_q,
            phot_e_period_up = :phot_e_period_up, phot_e_period_down = :phot_e_period_down,
            phot_e_incl_up = :phot_e_incl_up, phot_e_incl_down = :phot_e_incl_down,
            phot_e_q_up = :phot_e_q_up, phot_e_q_down = :phot_e_q_down,
            comp_mass_min = :comp_mass_min, comp_e_mass_min = :comp_e_mass_min,
            comp_mass_true = :comp_mass_true, comp_e_mass_true = :comp_e_mass_true,
            comp_e_mass_min_up = :comp_e_mass_min_up,
            comp_e_mass_min_down = :comp_e_mass_min_down,
            comp_e_mass_true_up = :comp_e_mass_true_up,
            comp_e_mass_true_down = :comp_e_mass_true_down,
            gal_u = :gal_u, gal_e_u = :gal_e_u,
            gal_e_u_up = :gal_e_u_up, gal_e_u_down = :gal_e_u_down,
            gal_v = :gal_v, gal_e_v = :gal_e_v,
            gal_e_v_up = :gal_e_v_up, gal_e_v_down = :gal_e_v_down,
            gal_w = :gal_w, gal_e_w = :gal_e_w,
            gal_e_w_up = :gal_e_w_up, gal_e_w_down = :gal_e_w_down,
            gal_x = :gal_x, gal_e_x = :gal_e_x,
            gal_e_x_up = :gal_e_x_up, gal_e_x_down = :gal_e_x_down,
            gal_y = :gal_y, gal_e_y = :gal_e_y,
            gal_e_y_up = :gal_e_y_up, gal_e_y_down = :gal_e_y_down,
            gal_z = :gal_z, gal_e_z = :gal_e_z,
            gal_e_z_up = :gal_e_z_up, gal_e_z_down = :gal_e_z_down,
            gal_p_thin = :gal_p_thin, gal_e_p_thin = :gal_e_p_thin,
            gal_p_thick = :gal_p_thick, gal_e_p_thick = :gal_e_p_thick,
            gal_p_halo = :gal_p_halo, gal_e_p_halo = :gal_e_p_halo,
            gal_jz = :gal_jz, gal_e_jz = :gal_e_jz,
            gal_e_jz_up = :gal_e_jz_up, gal_e_jz_down = :gal_e_jz_down,
            gal_ecc = :gal_ecc, gal_e_ecc = :gal_e_ecc,
            gal_e_ecc_up = :gal_e_ecc_up, gal_e_ecc_down = :gal_e_ecc_down,
            has_tess = :has_tess, has_gaia = :has_gaia, has_ztf = :has_ztf,
            has_atlas = :has_atlas, has_blackgem = :has_blackgem,
            %1
            bibcodes = :bibcodes
        WHERE id = :id AND project_id = :project_id
    )")
        .arg(abundanceAssignmentList());
    return s;
}

bool StarRepository::updateStarRow(const QString& projectId, std::shared_ptr<Star> star)
{
    if (!star || star->getId().isEmpty()) return false;

    static thread_local PreparedStatement stmt;

    QSqlQuery& query = stmt.get(_db, updateStarRowSql());

    query.bindValue(":id", star->getId());
    query.bindValue(":project_id", projectId);
    query.bindValue(":alias", star->getAlias());
    query.bindValue(":source_id", star->getSourceId());
    // Kept in lockstep with source_id so findMatchingStarId can match on an
    // indexed equality instead of a suffix LIKE.
    query.bindValue(":source_id_norm",
                    StarMatching::normalizeSourceId(star->getSourceId()));
    query.bindValue(":tic", star->getTic());
    query.bindValue(":jname", star->getJName());

    query.bindValue(":ra", star->getRa());
    query.bindValue(":dec", star->getDec());
    query.bindValue(":pmra", star->getPmra());
    query.bindValue(":pmdec", star->getPmdec());
    query.bindValue(":e_pmra", star->getEPmra());
    query.bindValue(":e_pmdec", star->getEPmdec());
    query.bindValue(":plx", star->getPlx());
    query.bindValue(":e_plx", star->getEPlx());
    query.bindValue(":pmra_pmdec_corr", star->getPmraPmdecCorr());
    query.bindValue(":plx_pmdec_corr", star->getPlxPmdecCorr());
    query.bindValue(":plx_pmra_corr", star->getPlxPmraCorr());

    query.bindValue(":gmag", star->getGmag());
    query.bindValue(":e_gmag", star->getEGmag());
    query.bindValue(":bp", star->getBp());
    query.bindValue(":e_bp", star->getEBp());
    query.bindValue(":rp", star->getRp());
    query.bindValue(":e_rp", star->getERp());
    query.bindValue(":bp_rp", star->getBpRp());

    query.bindValue(":spec_class", star->getSpecClass());
    query.bindValue(":teff", star->getTeff());
    query.bindValue(":e_teff", star->getETeff());
    query.bindValue(":logg", star->getLogg());
    query.bindValue(":e_logg", star->getELogg());
    query.bindValue(":he", star->getHe());
    query.bindValue(":e_he", star->getEHe());
    query.bindValue(":e_teff_up", SqlValue::fromDouble(star->getETeffUp()));
    query.bindValue(":e_teff_down", SqlValue::fromDouble(star->getETeffDown()));
    query.bindValue(":e_logg_up", SqlValue::fromDouble(star->getELoggUp()));
    query.bindValue(":e_logg_down", SqlValue::fromDouble(star->getELoggDown()));
    query.bindValue(":e_he_up", SqlValue::fromDouble(star->getEHeUp()));
    query.bindValue(":e_he_down", SqlValue::fromDouble(star->getEHeDown()));
    query.bindValue(":teff_limit", star->getTeffLimit());
    query.bindValue(":logg_limit", star->getLoggLimit());
    query.bindValue(":he_limit", star->getHeLimit());

    query.bindValue(":logp", star->getLogP());
    query.bindValue(":deltaRV", star->getDeltaRV());
    query.bindValue(":e_deltaRV", star->getEDeltaRV());
    query.bindValue(":rv_avg", star->getRVAvg());
    query.bindValue(":e_rv_avg", star->getERVAvg());
    query.bindValue(":rv_med", star->getRVMed());
    query.bindValue(":e_rv_med", star->getERVMed());

    query.bindValue(":n_spectra", star->getNSpectra());
    query.bindValue(":n_fit_spectra", star->getNFitSpectra());

    query.bindValue(":rv_timespan", star->getRVTimespan());
    query.bindValue(":rv_npoints", star->getRVNPoints());
    query.bindValue(":rv_k", star->getRVK());
    query.bindValue(":rv_e_k", star->getRVEK());
    query.bindValue(":rv_period", star->getRVPeriod());
    query.bindValue(":rv_e_period", star->getRVEPeriod());
    query.bindValue(":rv_gamma", star->getRVGamma());
    query.bindValue(":rv_e_gamma", star->getRVEGamma());
    query.bindValue(":rv_e_k_up", SqlValue::fromDouble(star->getRVEKUp()));
    query.bindValue(":rv_e_k_down", SqlValue::fromDouble(star->getRVEKDown()));
    query.bindValue(":rv_k2", SqlValue::fromDouble(star->getRVK2()));
    query.bindValue(":rv_e_k2", SqlValue::fromDouble(star->getRVEK2()));
    query.bindValue(":rv_e_k2_up", SqlValue::fromDouble(star->getRVEK2Up()));
    query.bindValue(":rv_e_k2_down", SqlValue::fromDouble(star->getRVEK2Down()));
    query.bindValue(":rv_e_period_up", SqlValue::fromDouble(star->getRVEPeriodUp()));
    query.bindValue(":rv_e_period_down", SqlValue::fromDouble(star->getRVEPeriodDown()));
    query.bindValue(":rv_e_gamma_up", SqlValue::fromDouble(star->getRVEGammaUp()));
    query.bindValue(":rv_e_gamma_down", SqlValue::fromDouble(star->getRVEGammaDown()));
    query.bindValue(":rv_ecc", star->getRVEcc());
    query.bindValue(":rv_phi", star->getRVPhi());
    query.bindValue(":rv_t0", star->getRVT0());
    query.bindValue(":rv_chi2", star->getRVChi2());
    query.bindValue(":rv_rms", star->getRVRms());

    query.bindValue(":sed_mass1", star->getSedMass1());
    query.bindValue(":sed_e_mass1", star->getSedEMass1());
    query.bindValue(":sed_radius1", star->getSedRadius1());
    query.bindValue(":sed_e_radius1", star->getSedERadius1());
    query.bindValue(":sed_lum1", star->getSedLum1());
    query.bindValue(":sed_e_lum1", star->getSedELum1());
    query.bindValue(":sed_mass2", star->getSedMass2());
    query.bindValue(":sed_e_mass2", star->getSedEMass2());
    query.bindValue(":sed_radius2", star->getSedRadius2());
    query.bindValue(":sed_e_radius2", star->getSedERadius2());
    query.bindValue(":sed_lum2", star->getSedLum2());
    query.bindValue(":sed_e_lum2", star->getSedELum2());
    query.bindValue(":sed_e_mass1_up", SqlValue::fromDouble(star->getSedEMass1Up()));
    query.bindValue(":sed_e_mass1_down", SqlValue::fromDouble(star->getSedEMass1Down()));
    query.bindValue(":sed_e_radius1_up", SqlValue::fromDouble(star->getSedERadius1Up()));
    query.bindValue(":sed_e_radius1_down", SqlValue::fromDouble(star->getSedERadius1Down()));
    query.bindValue(":sed_e_lum1_up", SqlValue::fromDouble(star->getSedELum1Up()));
    query.bindValue(":sed_e_lum1_down", SqlValue::fromDouble(star->getSedELum1Down()));
    query.bindValue(":sed_e_mass2_up", SqlValue::fromDouble(star->getSedEMass2Up()));
    query.bindValue(":sed_e_mass2_down", SqlValue::fromDouble(star->getSedEMass2Down()));
    query.bindValue(":sed_e_radius2_up", SqlValue::fromDouble(star->getSedERadius2Up()));
    query.bindValue(":sed_e_radius2_down", SqlValue::fromDouble(star->getSedERadius2Down()));
    query.bindValue(":sed_e_lum2_up", SqlValue::fromDouble(star->getSedELum2Up()));
    query.bindValue(":sed_e_lum2_down", SqlValue::fromDouble(star->getSedELum2Down()));

    query.bindValue(":phot_period", star->getPhotPeriod());
    query.bindValue(":phot_e_period", star->getPhotEPeriod());
    query.bindValue(":phot_incl", star->getPhotIncl());
    query.bindValue(":phot_e_incl", star->getPhotEIncl());
    query.bindValue(":phot_q", star->getPhotQ());
    query.bindValue(":phot_e_q", star->getPhotEQ());
    query.bindValue(":phot_e_period_up", SqlValue::fromDouble(star->getPhotEPeriodUp()));
    query.bindValue(":phot_e_period_down", SqlValue::fromDouble(star->getPhotEPeriodDown()));
    query.bindValue(":phot_e_incl_up", SqlValue::fromDouble(star->getPhotEInclUp()));
    query.bindValue(":phot_e_incl_down", SqlValue::fromDouble(star->getPhotEInclDown()));
    query.bindValue(":phot_e_q_up", SqlValue::fromDouble(star->getPhotEQUp()));
    query.bindValue(":phot_e_q_down", SqlValue::fromDouble(star->getPhotEQDown()));

    query.bindValue(":comp_mass_min", star->getCompMassMin());
    query.bindValue(":comp_e_mass_min", star->getECompMassMin());
    query.bindValue(":comp_mass_true", star->getCompMassTrue());
    query.bindValue(":comp_e_mass_true", star->getECompMassTrue());
    query.bindValue(":comp_e_mass_min_up", SqlValue::fromDouble(star->getECompMassMinUp()));
    query.bindValue(":comp_e_mass_min_down", SqlValue::fromDouble(star->getECompMassMinDown()));
    query.bindValue(":comp_e_mass_true_up", SqlValue::fromDouble(star->getECompMassTrueUp()));
    query.bindValue(":comp_e_mass_true_down", SqlValue::fromDouble(star->getECompMassTrueDown()));

    bindGalacticFields(query, *star);
    bindAbundanceFields(query, *star);

    query.bindValue(":has_tess", star->getHasTess() ? 1 : 0);
    query.bindValue(":has_gaia", star->getHasGaia() ? 1 : 0);
    query.bindValue(":has_ztf", star->getHasZtf() ? 1 : 0);
    query.bindValue(":has_atlas", star->getHasAtlas() ? 1 : 0);
    query.bindValue(":has_blackgem", star->getHasBlackgem() ? 1 : 0);

    QJsonArray bibcodesArray;
    for (const auto& bibcode : star->getBibcodes()) {
        bibcodesArray.append(bibcode);
    }
    query.bindValue(":bibcodes", QJsonDocument(bibcodesArray).toJson(QJsonDocument::Compact));

    if (!query.exec()) {
        LOG_ERROR("Stars", QString("Failed to update star row: %1").arg(query.lastError().text()));
        return false;
    }

    return true;
}

QString StarRepository::findMatchingStarId(const QString& projectId,
                                             const QString& sourceId,
                                             const QString& alias,
                                             const QString& tic,
                                             const QString& jname,
                                             double ra, double dec,
                                             double toleranceArcsec)
{
    QSqlDatabase db = _db.threadConnection();

    // ── 1. Exact source_id match (most reliable) ────────────────
    if (!sourceId.isEmpty()) {
        QSqlQuery q(db);
        q.prepare(R"(
            SELECT id FROM stars
            WHERE project_id = :pid AND source_id = :sid
            LIMIT 1
        )");
        q.bindValue(":pid", projectId);
        q.bindValue(":sid", sourceId);
        if (q.exec() && q.next())
            return q.value(0).toString();

        // Either side may carry the catalogue prefix: the row can hold
        // "Gaia DR3 1234567890" while the caller passes the bare number, or
        // the other way round. Both sides are reduced to the catalogue number
        // and compared through idx_stars_source_id_norm. This replaces an
        // earlier `source_id LIKE '%' || :num`, which no index can serve and
        // which cost a full scan of the project - 86 ms per probe on an 88 k
        // star project, so hours across a catalogue-sized import.
        const QString norm = StarMatching::normalizeSourceId(sourceId);
        if (!norm.isEmpty()) {
            q.prepare(R"(
                SELECT id FROM stars
                WHERE project_id = :pid AND source_id_norm = :norm
                LIMIT 1
            )");
            q.bindValue(":pid", projectId);
            q.bindValue(":norm", norm);
            if (q.exec() && q.next())
                return q.value(0).toString();
        }
    }

    // ── 2. Exact TIC match ──────────────────────────────────────
    if (!tic.isEmpty()) {
        QSqlQuery q(db);
        q.prepare(R"(
            SELECT id FROM stars
            WHERE project_id = :pid AND tic = :tic
            LIMIT 1
        )");
        q.bindValue(":pid", projectId);
        q.bindValue(":tic", tic);
        if (q.exec() && q.next())
            return q.value(0).toString();
    }

    // ── 3. Exact J-name match ───────────────────────────────────
    if (!jname.isEmpty()) {
        QSqlQuery q(db);
        q.prepare(R"(
            SELECT id FROM stars
            WHERE project_id = :pid AND jname = :jname
            LIMIT 1
        )");
        q.bindValue(":pid", projectId);
        q.bindValue(":jname", jname);
        if (q.exec() && q.next())
            return q.value(0).toString();
    }

    // ── 4. Alias match (case-insensitive) ───────────────────────
    if (!alias.isEmpty()) {
        QSqlQuery q(db);
        // COLLATE NOCASE rather than LOWER() on both sides: the function call
        // made the column non-indexable and forced a full scan. SQLite's
        // NOCASE and LOWER() are both ASCII-only, so the matching semantics
        // are unchanged; idx_stars_alias_nocase carries the same collation.
        q.prepare(R"(
            SELECT id FROM stars
            WHERE project_id = :pid AND alias = :alias COLLATE NOCASE
            LIMIT 1
        )");
        q.bindValue(":pid", projectId);
        q.bindValue(":alias", alias);
        if (q.exec() && q.next())
            return q.value(0).toString();
    }

    // ── 5. Positional match (ra/dec within the caller's radius) ─
    //    Only if we have valid coordinates.
    if (!std::isnan(ra) && !std::isnan(dec)) {
        if (toleranceArcsec <= 0.0)
            return QString();
        const double TOLERANCE_DEG = toleranceArcsec / 3600.0;

        // Use a bounding box for the SQL filter (fast), then refine with
        // proper spherical distance. The cos(dec) factor for RA is applied
        // in the app-side check.
        double decLo = dec - TOLERANCE_DEG;
        double decHi = dec + TOLERANCE_DEG;
        // RA box is wider near poles; use a generous factor
        double cosDec = std::cos(dec * M_PI / 180.0);
        double raMargin = (cosDec > 0.01) ? TOLERANCE_DEG / cosDec : 360.0;
        double raLo = ra - raMargin;
        double raHi = ra + raMargin;

        QSqlQuery q(db);

        // Handle RA wraparound at 0/360
        if (raLo < 0.0 || raHi > 360.0) {
            // Wraparound - use OR
            double raLoW = (raLo < 0.0) ? raLo + 360.0 : raLo;
            double raHiW = (raHi > 360.0) ? raHi - 360.0 : raHi;
            q.prepare(R"(
                SELECT id, ra, dec FROM stars
                WHERE project_id = :pid
                  AND dec BETWEEN :decLo AND :decHi
                  AND (ra >= :raLoW OR ra <= :raHiW)
                  AND ra IS NOT NULL AND dec IS NOT NULL
            )");
            q.bindValue(":pid", projectId);
            q.bindValue(":decLo", decLo);
            q.bindValue(":decHi", decHi);
            q.bindValue(":raLoW", raLoW);
            q.bindValue(":raHiW", raHiW);
        } else {
            q.prepare(R"(
                SELECT id, ra, dec FROM stars
                WHERE project_id = :pid
                  AND dec BETWEEN :decLo AND :decHi
                  AND ra BETWEEN :raLo AND :raHi
                  AND ra IS NOT NULL AND dec IS NOT NULL
            )");
            q.bindValue(":pid", projectId);
            q.bindValue(":decLo", decLo);
            q.bindValue(":decHi", decHi);
            q.bindValue(":raLo", raLo);
            q.bindValue(":raHi", raHi);
        }

        if (q.exec()) {
            QString bestId;
            double bestDist = TOLERANCE_DEG;

            while (q.next()) {
                double dbRa  = q.value("ra").toDouble();
                double dbDec = q.value("dec").toDouble();

                // Simple small-angle distance
                double dRa  = (ra - dbRa) * cosDec;
                double dDec = dec - dbDec;
                double dist = std::sqrt(dRa * dRa + dDec * dDec);

                if (dist < bestDist) {
                    bestDist = dist;
                    bestId   = q.value("id").toString();
                }
            }

            if (!bestId.isEmpty())
                return bestId;
        }
    }

    return QString();  // no match found
}
