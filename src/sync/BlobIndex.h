#pragma once

// The data files that go with synced rows.
//
// Spectra, fits, lightcurves and SED models keep their arrays in .asd files
// below the data root, and the rows name those files in path columns. A file
// is rewritten in place under the same name, so a row can change content
// without changing at all. sync_blobs remembers size, modification time and
// content hash of every file sync has looked at; a file whose hash moved is
// a change of its row (a clock entry for the cell "~<column>").
//
// Files travel separately from rows: a row arrives with the size and hash of
// its files, and what is missing or different here goes on a want list that
// the transport works off.

#include <QByteArray>
#include <QList>
#include <QSqlDatabase>
#include <QString>

namespace astra::sync {

/// Prefix of the cell name that stands for a path column's file content.
inline const QString kBlobCellPrefix = QStringLiteral("~");

/// One data file as the sender has it.
struct BlobRef {
    QString    column;     ///< path column of the row; empty for a file a file depends on
    QString    relPath;    ///< below the data root, '/' separated
    qint64     size = 0;
    QByteArray hash;       ///< BLAKE2b-256 of the content

    bool isValid() const { return !relPath.isEmpty() && !hash.isEmpty(); }
};

class BlobIndex {
public:
    BlobIndex(const QSqlDatabase& db, const QString& dataRoot);

    const QString& root() const { return _root; }

    /// Path below the data root, or empty when `absolutePath` is not below it.
    QString relative(const QString& absolutePath) const;
    QString absolute(const QString& relPath) const;
    /// False for anything that could lead out of the data root.
    static bool isSafeRelative(const QString& relPath);

    static QByteArray hashFile(const QString& absolutePath, qint64* size = nullptr);

    /// What the file is now: size and hash, taken from the index when size
    /// and modification time still match, hashed (and indexed) otherwise.
    /// Invalid when there is no such file.
    BlobRef describe(const QString& relPath);

    enum class Change { Missing, Unchanged, Changed };
    /// Compares the file with what the index knew and brings the index up to
    /// date. A file seen for the first time counts as unchanged when it was
    /// last written before `baselineMs` (it is as it was when the databases
    /// were paired), as changed otherwise.
    Change check(const QString& relPath, qint64 baselineMs);

    /// Files the fit file needs besides itself (its shared base), relative.
    QStringList dependencies(const QString& relPath) const;

    // ── receiving ───────────────────────────────────────────────────────
    /// Notes that the file is needed, unless the local one already is it.
    /// True when it was put on the want list.
    bool want(const BlobRef& ref, const QString& fromDevice);
    QList<BlobRef> wanted(int limit, const QString& fromDevice = {}) const;
    int wantedCount() const;

    /// Sender side: the file's content, if it still is what `ref` says.
    /// `actual` receives what the file is now when it is something else.
    bool read(const BlobRef& ref, QByteArray* data, BlobRef* actual = nullptr);
    /// Receiver side: checks size and hash, writes the file through a staging
    /// file and a rename, updates the index and the want list.
    bool store(const BlobRef& ref, const QByteArray& data, QString* error = nullptr);
    /// The sender no longer has that content: stop asking for it.
    void forget(const BlobRef& ref);

    /// Moves a data file (or a whole directory) out of the way into the
    /// trash below the root, where it stays until it is purged.
    bool moveToTrash(const QString& relPath);
    /// Deletes trash older than `days`. Returns how many entries went.
    int purgeTrash(int days);

private:
    QSqlDatabase _db;
    QString      _root;
};

}   // namespace astra::sync
