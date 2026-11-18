#include "sync/BlobIndex.h"

#include "app/Logger.h"
#include "spectra/Spectrum.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace astra::sync {

namespace {

const QString kSyncDir  = QStringLiteral(".sync");
const QString kTrashDir = QStringLiteral(".sync/trash");

qint64 mtimeMs(const QFileInfo& info)
{
    return info.lastModified().toMSecsSinceEpoch();
}

}   // namespace

BlobIndex::BlobIndex(const QSqlDatabase& db, const QString& dataRoot)
    : _db(db), _root(QDir::cleanPath(dataRoot))
{
}

QString BlobIndex::relative(const QString& absolutePath) const
{
    const QString prefix = _root + QLatin1Char('/');
    if (_root.isEmpty() || !absolutePath.startsWith(prefix)) return {};
    const QString rel = absolutePath.mid(prefix.size());
    return isSafeRelative(rel) ? rel : QString();
}

QString BlobIndex::absolute(const QString& relPath) const
{
    return _root + QLatin1Char('/') + relPath;
}

bool BlobIndex::isSafeRelative(const QString& relPath)
{
    if (relPath.isEmpty() || relPath.startsWith(QLatin1Char('/')) ||
        relPath.contains(QLatin1Char('\\')) || relPath.contains(QChar(0)))
        return false;
    for (const QString& part : relPath.split(QLatin1Char('/')))
        if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String(".."))
            return false;
    // Sync's own area and the database are never data files.
    return !relPath.startsWith(kSyncDir + QLatin1Char('/')) &&
           !relPath.startsWith(QLatin1String("astra.db"));
}

QByteArray BlobIndex::hashFile(const QString& absolutePath, qint64* size)
{
    QFile f(absolutePath);
    if (!f.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash h(QCryptographicHash::Blake2b_256);
    if (!h.addData(&f)) return {};
    if (size) *size = f.size();
    return h.result();
}

BlobRef BlobIndex::describe(const QString& relPath)
{
    BlobRef ref;
    if (!isSafeRelative(relPath)) return ref;
    const QFileInfo info(absolute(relPath));
    if (!info.isFile()) return ref;

    QSqlQuery q(_db);
    q.prepare(QStringLiteral("SELECT size, mtime, hash FROM sync_blobs WHERE relpath = ?"));
    q.addBindValue(relPath);
    if (q.exec() && q.next() && q.value(0).toLongLong() == info.size() &&
        q.value(1).toLongLong() == mtimeMs(info)) {
        ref.relPath = relPath;
        ref.size    = info.size();
        ref.hash    = q.value(2).toByteArray();
        return ref;
    }
    q.finish();

    qint64 size = 0;
    const QByteArray hash = hashFile(info.absoluteFilePath(), &size);
    if (hash.isEmpty()) return ref;

    QSqlQuery put(_db);
    put.prepare(QStringLiteral("INSERT OR REPLACE INTO sync_blobs (relpath, size, mtime, hash) "
                               "VALUES (?, ?, ?, ?)"));
    put.addBindValue(relPath);
    put.addBindValue(size);
    put.addBindValue(mtimeMs(info));
    put.addBindValue(hash);
    put.exec();

    ref.relPath = relPath;
    ref.size    = size;
    ref.hash    = hash;
    return ref;
}

BlobIndex::Change BlobIndex::check(const QString& relPath, qint64 baselineMs)
{
    if (!isSafeRelative(relPath)) return Change::Missing;
    const QFileInfo info(absolute(relPath));
    if (!info.isFile()) return Change::Missing;

    bool known = false;
    QByteArray before;
    {
        QSqlQuery q(_db);
        q.prepare(QStringLiteral("SELECT size, mtime, hash FROM sync_blobs WHERE relpath = ?"));
        q.addBindValue(relPath);
        if (q.exec() && q.next()) {
            known = true;
            if (q.value(0).toLongLong() == info.size() && q.value(1).toLongLong() == mtimeMs(info))
                return Change::Unchanged;
            before = q.value(2).toByteArray();
        }
    }

    const BlobRef now = describe(relPath);          // hashes and indexes
    if (!now.isValid()) return Change::Missing;
    if (known) return now.hash == before ? Change::Unchanged : Change::Changed;
    return mtimeMs(info) <= baselineMs ? Change::Unchanged : Change::Changed;
}

QStringList BlobIndex::dependencies(const QString& relPath) const
{
    // Only fit files have one: the base file shared by the fits of a spectrum.
    if (!QFileInfo(relPath).fileName().startsWith(QLatin1String("fit_"))) return {};
    const QString base = relative(SpectralFit::basePathOf(absolute(relPath)));
    return base.isEmpty() ? QStringList() : QStringList{base};
}

bool BlobIndex::want(const BlobRef& ref, const QString& fromDevice)
{
    if (!ref.isValid() || !isSafeRelative(ref.relPath)) return false;
    const BlobRef have = describe(ref.relPath);
    if (have.isValid() && have.hash == ref.hash) {
        forget(ref);
        return false;
    }
    {
        QSqlQuery known(_db);
        known.prepare(QStringLiteral("SELECT 1 FROM sync_blob_wanted WHERE relpath = ? AND hash = ?"));
        known.addBindValue(ref.relPath);
        known.addBindValue(ref.hash);
        if (known.exec() && known.next()) return false;      // already asked for
    }
    QSqlQuery q(_db);
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO sync_blob_wanted (relpath, size, hash, peer) "
                             "VALUES (?, ?, ?, ?)"));
    q.addBindValue(ref.relPath);
    q.addBindValue(ref.size);
    q.addBindValue(ref.hash);
    q.addBindValue(fromDevice);
    return q.exec();
}

QList<BlobRef> BlobIndex::wanted(int limit, const QString& fromDevice) const
{
    QList<BlobRef> out;
    QSqlQuery q(_db);
    if (fromDevice.isEmpty()) {
        q.prepare(QStringLiteral("SELECT relpath, size, hash FROM sync_blob_wanted "
                                 "ORDER BY relpath LIMIT ?"));
    } else {
        q.prepare(QStringLiteral("SELECT relpath, size, hash FROM sync_blob_wanted "
                                 "WHERE peer = ? ORDER BY relpath LIMIT ?"));
        q.addBindValue(fromDevice);
    }
    q.addBindValue(limit);
    if (q.exec())
        while (q.next()) {
            BlobRef ref;
            ref.relPath = q.value(0).toString();
            ref.size    = q.value(1).toLongLong();
            ref.hash    = q.value(2).toByteArray();
            out << ref;
        }
    return out;
}

int BlobIndex::wantedCount() const
{
    QSqlQuery q(_db);
    return q.exec(QStringLiteral("SELECT COUNT(*) FROM sync_blob_wanted")) && q.next()
               ? q.value(0).toInt() : 0;
}

bool BlobIndex::read(const BlobRef& ref, QByteArray* data, BlobRef* actual)
{
    if (!isSafeRelative(ref.relPath)) return false;
    QFile f(absolute(ref.relPath));
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = f.readAll();
    const QByteArray hash =
        QCryptographicHash::hash(bytes, QCryptographicHash::Blake2b_256);
    if (hash != ref.hash) {
        // Rewritten since the row was sent; the newer content follows with
        // its own clock entry.
        if (actual) {
            actual->relPath = ref.relPath;
            actual->size    = bytes.size();
            actual->hash    = hash;
        }
        return false;
    }
    *data = bytes;
    return true;
}

bool BlobIndex::store(const BlobRef& ref, const QByteArray& data, QString* error)
{
    auto fail = [error](const QString& what) {
        if (error) *error = what;
        LOG_ERROR("Sync", what);
        return false;
    };
    if (!ref.isValid() || !isSafeRelative(ref.relPath))
        return fail(QStringLiteral("refused data file path: ") + ref.relPath);
    if (data.size() != ref.size ||
        QCryptographicHash::hash(data, QCryptographicHash::Blake2b_256) != ref.hash)
        return fail(QStringLiteral("data file arrived damaged: ") + ref.relPath);

    const QString target = absolute(ref.relPath);
    if (!QDir().mkpath(QFileInfo(target).absolutePath()))
        return fail(QStringLiteral("cannot create the folder of ") + ref.relPath);

    // QSaveFile writes beside the target and renames, so a reader sees the
    // old file or the new one, never half of it.
    QSaveFile f(target);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        return fail(QStringLiteral("cannot write ") + ref.relPath + QStringLiteral(": ") +
                    f.errorString());

    describe(ref.relPath);          // index it as it is now
    forget(ref);
    return true;
}

void BlobIndex::forget(const BlobRef& ref)
{
    QSqlQuery q(_db);
    q.prepare(QStringLiteral("DELETE FROM sync_blob_wanted WHERE relpath = ? AND hash = ?"));
    q.addBindValue(ref.relPath);
    q.addBindValue(ref.hash);
    q.exec();
}

bool BlobIndex::moveToTrash(const QString& relPath)
{
    if (!isSafeRelative(relPath)) return false;
    const QString source = absolute(relPath);
    if (!QFileInfo::exists(source)) return true;

    // A folder goes file by file, so its files land beside the ones that
    // went there before it (a star's folder after single files of the star).
    if (QFileInfo(source).isDir()) {
        bool ok = true;
        const QDir dir(source);
        for (const QString& name : dir.entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot))
            ok = moveToTrash(relPath + QLatin1Char('/') + name) && ok;
        return QDir().rmdir(source) && ok;
    }

    // One folder per day keeps purging simple and names from clashing rarely;
    // a clash gets a counter.
    const QString day = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd"));
    QString target = absolute(kTrashDir + QLatin1Char('/') + day + QLatin1Char('/') + relPath);
    for (int n = 1; QFileInfo::exists(target); ++n)
        target = absolute(kTrashDir + QLatin1Char('/') + day + QLatin1Char('/') + relPath) +
                 QLatin1Char('.') + QString::number(n);
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) return false;
    if (!QFile::rename(source, target)) return false;

    QSqlQuery q(_db);
    q.prepare(QStringLiteral("DELETE FROM sync_blobs WHERE relpath = ? OR relpath LIKE ? ESCAPE '\\'"));
    q.addBindValue(relPath);
    QString like = relPath;
    like.replace(QLatin1Char('\\'), QLatin1String("\\\\"))
        .replace(QLatin1Char('%'), QLatin1String("\\%"))
        .replace(QLatin1Char('_'), QLatin1String("\\_"));
    q.addBindValue(like + QStringLiteral("/%"));
    q.exec();
    return true;
}

bool BlobIndex::restoreFromTrash(const QString& relPath)
{
    if (!isSafeRelative(relPath)) return false;
    const QString target = absolute(relPath);
    if (QFileInfo::exists(target)) return false;
    QDir trash(absolute(kTrashDir));
    QStringList days = trash.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (auto day = days.crbegin(); day != days.crend(); ++day) {
        const QString source = trash.filePath(*day + QLatin1Char('/') + relPath);
        if (!QFileInfo(source).isFile()) continue;
        return QDir().mkpath(QFileInfo(target).absolutePath()) && QFile::rename(source, target);
    }
    return false;
}

int BlobIndex::purgeTrash(int days)
{
    QDir trash(absolute(kTrashDir));
    if (!trash.exists()) return 0;
    const QString cutoff =
        QDateTime::currentDateTimeUtc().addDays(-days).toString(QStringLiteral("yyyyMMdd"));
    int removed = 0;
    for (const QString& day : trash.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (day < cutoff && QDir(trash.filePath(day)).removeRecursively()) ++removed;
    return removed;
}

}   // namespace astra::sync
