// Data files: the .asd files that rows name travel with the rows.
//
// A row is applied as soon as it arrives; its files go on a want list and
// follow. A file is rewritten in place under the same name, so "the row did
// not change" says nothing about its content: the flush hashes the files of
// every row it examines.

#include <doctest.h>

#include "sync_test_support.h"

#include "spectra/Spectrum.h"
#include "sync/BlobIndex.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QThread>

using namespace synctest;

namespace {

void writeFile(const QString& path, const QByteArray& content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    REQUIRE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content);
}

QByteArray readFile(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray("<missing>");
}

// The modification time is what tells a rewrite from "still the same"; make
// sure a rewrite within the same test gets a different one.
void tick() { QThread::msleep(15); }

const QString kSpectrum = "data/stars/S1/spectra/SP7.asd";

// A spectrum row with its data file, the way a save leaves them: file first,
// then the row.
void saveSpectrum(Device& d, const QByteArray& content)
{
    writeFile(d.root + "/" + kSpectrum, content);
    REQUIRE(d.run(QStringLiteral("INSERT OR REPLACE INTO spectra (id, star_id, data_file) "
                                 "VALUES ('SP7', 'S1', '%1/%2')").arg(d.root, kSpectrum)));
}

}   // namespace

TEST_SUITE("sync")
{

TEST_CASE("blob index: safe paths, hashing, and what counts as changed")
{
    CHECK(BlobIndex::isSafeRelative("data/stars/S1/spectra/a.asd"));
    CHECK_FALSE(BlobIndex::isSafeRelative("../outside.asd"));
    CHECK_FALSE(BlobIndex::isSafeRelative("data/../../etc/passwd"));
    CHECK_FALSE(BlobIndex::isSafeRelative("/etc/passwd"));
    CHECK_FALSE(BlobIndex::isSafeRelative("data//x"));
    CHECK_FALSE(BlobIndex::isSafeRelative(""));
    CHECK_FALSE(BlobIndex::isSafeRelative("astra.db"));
    CHECK_FALSE(BlobIndex::isSafeRelative(".sync/trash/x"));

    World world;
    auto a = world.device();
    BlobIndex index(a->db, a->root);
    CHECK(index.relative(a->root + "/data/x.asd") == "data/x.asd");
    CHECK(index.relative("/somewhere/else/x.asd").isEmpty());

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    writeFile(a->root + "/data/old.asd", "as it was when paired");
    writeFile(a->root + "/data/new.asd", "written since");

    // Seen for the first time: older than the baseline is "as it was",
    // newer is a change.
    CHECK(index.check("data/old.asd", now + 60000) == BlobIndex::Change::Unchanged);
    CHECK(index.check("data/new.asd", now - 60000) == BlobIndex::Change::Changed);
    CHECK(index.check("data/none.asd", now) == BlobIndex::Change::Missing);

    // Known from here on: only the content decides.
    CHECK(index.check("data/new.asd", now - 60000) == BlobIndex::Change::Unchanged);
    tick();
    writeFile(a->root + "/data/new.asd", "written since");           // same bytes again
    CHECK(index.check("data/new.asd", now + 60000) == BlobIndex::Change::Unchanged);
    tick();
    writeFile(a->root + "/data/new.asd", "other content");
    CHECK(index.check("data/new.asd", now + 60000) == BlobIndex::Change::Changed);

    const BlobRef ref = index.describe("data/new.asd");
    REQUIRE(ref.isValid());
    CHECK(ref.size == 13);
    CHECK(ref.hash == BlobIndex::hashFile(a->root + "/data/new.asd"));
    CHECK(ref.hash.size() == 32);
}

TEST_CASE("a new row brings its data file along")
{
    World world;
    auto a = world.device();
    auto b = world.device();

    saveSpectrum(*a, "spectrum arrays v1");
    const ApplyResult atB = send(*a, *b);
    CHECK(atB.rowsInserted == 1);
    CHECK(atB.filesWanted == 1);

    // The row is there at once, naming the file under B's own root; the file
    // follows.
    CHECK(b->one("SELECT data_file FROM spectra WHERE id = 'SP7'") == b->root + "/" + kSpectrum);
    CHECK_FALSE(QFile::exists(b->root + "/" + kSpectrum));
    CHECK(b->engine->wantedFileCount() == 1);

    CHECK(fetchFiles(*a, *b) == 1);
    CHECK(readFile(b->root + "/" + kSpectrum) == "spectrum arrays v1");
    CHECK(b->engine->wantedFileCount() == 0);

    // The file that arrived is not a local change: nothing goes back.
    settle({a.get(), b.get()});
    CHECK(a->engine->wantedFileCount() == 0);
    CHECK(b->engine->wantedFileCount() == 0);
    CHECK(a->digest() == b->digest());
}

TEST_CASE("a file rewritten in place is a change even when the row is not")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    saveSpectrum(*a, "spectrum arrays v1");
    send(*a, *b);
    fetchFiles(*a, *b);

    // Saved again with the same content: nothing to send.
    tick();
    saveSpectrum(*a, "spectrum arrays v1");
    FlushResult same;
    REQUIRE(SyncStore(a->db, a->root).flushAll(&same));
    CHECK(same.filesChanged == 0);
    CHECK(same.entries() == 0);

    // Saved again with new content. The row is identical.
    tick();
    saveSpectrum(*a, "spectrum arrays v2, longer");
    FlushResult changed;
    REQUIRE(SyncStore(a->db, a->root).flushAll(&changed));
    CHECK(changed.filesChanged == 1);
    CHECK(a->one("SELECT cell FROM sync_clock WHERE tbl = 'spectra' AND pk = 'SP7' "
                 "AND cell LIKE '~%'") == "~data_file");

    const ApplyResult atB = send(*a, *b);
    CHECK(atB.filesWanted == 1);
    // Until the new content is here B keeps the old file, whole.
    CHECK(readFile(b->root + "/" + kSpectrum) == "spectrum arrays v1");
    CHECK(fetchFiles(*a, *b) == 1);
    CHECK(readFile(b->root + "/" + kSpectrum) == "spectrum arrays v2, longer");

    // Rewritten on both sides: the later one ends up everywhere.
    tick();
    saveSpectrum(*a, "from A");
    REQUIRE(SyncStore(a->db, a->root).flushAll());
    tick();
    saveSpectrum(*b, "from B, later");
    for (int round = 0; round < 3; ++round) {
        send(*a, *b); fetchFiles(*a, *b);
        send(*b, *a); fetchFiles(*b, *a);
    }
    CHECK(readFile(a->root + "/" + kSpectrum) == "from B, later");
    CHECK(readFile(b->root + "/" + kSpectrum) == "from B, later");
}

TEST_CASE("a file that changed again before it was fetched is not delivered stale")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    saveSpectrum(*a, "first");
    send(*a, *b);                               // B wants "first"
    REQUIRE(b->engine->wantedFileCount() == 1);

    tick();
    saveSpectrum(*a, "second");                 // rewritten before B fetched
    const BlobRef stale = b->engine->wantedFiles(10).first();
    QByteArray data;
    CHECK_FALSE(a->engine->readFile(stale, &data));

    // B stops asking for what no longer exists; the new content comes with
    // its own change.
    CHECK(fetchFiles(*a, *b) == 0);
    CHECK(b->engine->wantedFileCount() == 0);
    send(*a, *b);
    CHECK(fetchFiles(*a, *b) == 1);
    CHECK(readFile(b->root + "/" + kSpectrum) == "second");
}

TEST_CASE("damaged or misdirected files are refused")
{
    World world;
    auto a = world.device();
    BlobRef ref;
    ref.relPath = "data/stars/S1/spectra/x.asd";
    ref.size    = 5;
    ref.hash    = QCryptographicHash::hash("hello", QCryptographicHash::Blake2b_256);

    QString error;
    CHECK_FALSE(a->engine->storeFile(ref, "hellp", &error));         // wrong content
    CHECK_FALSE(a->engine->storeFile(ref, "hello!", &error));        // wrong size
    CHECK_FALSE(QFile::exists(a->root + "/" + ref.relPath));
    CHECK(a->engine->storeFile(ref, "hello", &error));
    CHECK(readFile(a->root + "/" + ref.relPath) == "hello");

    BlobRef outside = ref;
    outside.relPath = "../escaped.asd";
    CHECK_FALSE(a->engine->storeFile(outside, "hello", &error));
    outside.relPath = "astra.db";
    CHECK_FALSE(a->engine->storeFile(outside, "hello", &error));
    QByteArray data;
    outside.relPath = "../../etc/hostname";
    CHECK_FALSE(a->engine->readFile(outside, &data));
}

TEST_CASE("a fit's shared base file travels with the fit")
{
    World world;
    auto a = world.device();
    auto b = world.device();

    // Two fits of one spectrum share their wavelength and data arrays in a
    // base file that no row names.
    const QString dir = "data/stars/S1/spectra";
    QStringList fitFiles;
    int n = 0;
    for (double scale : {1.0, 1.01}) {
        SpectralFit fit;
        fit.modelWavelengths = {4000.0, 4000.5, 4001.0};
        fit.rebinnedFluxes   = {1.0, 0.9, 1.1};
        fit.modelFluxes      = {1.0 * scale, 0.95 * scale, 1.05 * scale};
        fit.rebinnedSigmas   = {0.01, 0.02, 0.01};
        const QString rel = QStringLiteral("%1/fit_SP1_NF%2.asd").arg(dir).arg(++n);
        QDir().mkpath(a->root + "/" + dir);
        REQUIRE(fit.saveDataToFile(a->root + "/" + rel));
        REQUIRE(a->run(QStringLiteral("INSERT INTO spectral_fits (id, spectrum_id, model_data_file) "
                                      "VALUES ('NF%1', 'SP1', '%2/%3')").arg(n).arg(a->root, rel)));
        fitFiles << rel;
    }
    const QString baseAtA = SpectralFit::basePathOf(a->root + "/" + fitFiles.first());
    REQUIRE_FALSE(baseAtA.isEmpty());

    const ApplyResult atB = send(*a, *b);
    CHECK(atB.rowsInserted == 2);
    CHECK(atB.filesWanted == 3);                // two fits and the one base
    CHECK(fetchFiles(*a, *b) == 3);

    for (const QString& rel : fitFiles) {
        SpectralFit loaded;
        REQUIRE(loaded.loadDataFromFile(b->root + "/" + rel));
        CHECK(loaded.rebinnedFluxes == std::vector<double>{1.0, 0.9, 1.1});
    }
    CHECK(QDir(b->root + "/" + dir).entryList({"fitbase_*.asd"}, QDir::Files).size() == 1);
}

TEST_CASE("deletes: files go to the trash, a star takes its folder and local rows")
{
    World world;
    auto a = world.device();
    auto b = world.device();
    saveSpectrum(*a, "spectrum arrays");
    send(*a, *b);
    fetchFiles(*a, *b);
    REQUIRE(QFile::exists(b->root + "/" + kSpectrum));

    // Things on B that belong to the star but are not synced.
    writeFile(b->root + "/data/stars/S1/periodograms/p.asd", "local periodogram");
    REQUIRE(b->run("CREATE TABLE IF NOT EXISTS periodograms (id TEXT PRIMARY KEY, star_id TEXT)"));
    REQUIRE(b->run("INSERT INTO periodograms (id, star_id) VALUES ('PG1', 'S1')"));
    REQUIRE(b->run("INSERT INTO periodogram_store (kind, owner_id, star_id) VALUES ('lc', 'S1', 'S1')"));

    // One spectrum deleted on A.
    REQUIRE(a->run("DELETE FROM spectra WHERE id = 'SP7'"));
    const ApplyResult one = send(*a, *b);
    CHECK(one.rowsDeleted == 1);
    CHECK_FALSE(QFile::exists(b->root + "/" + kSpectrum));
    const QString day = QDateTime::currentDateTimeUtc().toString("yyyyMMdd");
    CHECK(readFile(b->root + "/.sync/trash/" + day + "/" + kSpectrum) == "spectrum arrays");
    CHECK(b->one("SELECT COUNT(*) FROM sync_history WHERE tbl = 'spectra' AND pk = 'SP7'") == "1");

    // The whole star deleted on A, the way the application does it: children
    // first, then the star.
    for (const char* sql : {"DELETE FROM spectral_fits WHERE spectrum_id = 'SP1'",
                            "DELETE FROM spectra WHERE star_id = 'S1'",
                            "DELETE FROM stars WHERE id = 'S1'"})
        REQUIRE(a->run(sql));
    send(*a, *b);
    CHECK(b->one("SELECT COUNT(*) FROM stars WHERE id = 'S1'") == "0");
    CHECK(b->one("SELECT COUNT(*) FROM spectra WHERE star_id = 'S1'") == "0");
    CHECK_FALSE(QDir(b->root + "/data/stars/S1").exists());
    CHECK(QFile::exists(b->root + "/.sync/trash/" + day + "/data/stars/S1/periodograms/p.asd"));
    CHECK(b->one("SELECT COUNT(*) FROM periodograms") == "0");
    CHECK(b->one("SELECT COUNT(*) FROM periodogram_store") == "0");
    CHECK(a->digest() == b->digest());

    // Trash older than the retention goes.
    BlobIndex index(b->db, b->root);
    CHECK(index.purgeTrash(30) == 0);
    QDir().mkpath(b->root + "/.sync/trash/20200101/data");
    CHECK(index.purgeTrash(30) == 1);
    CHECK_FALSE(QDir(b->root + "/.sync/trash/20200101").exists());
}

}   // TEST_SUITE
