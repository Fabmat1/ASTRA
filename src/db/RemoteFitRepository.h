#ifndef REMOTEFITREPOSITORY_H
#define REMOTEFITREPOSITORY_H

#include <QString>
#include <optional>
#include <vector>

class DBAccess;

// ─────────────────────────────────────────────────────────────────────────────
// Persistence for fits running on remote machines.
//
// A remote fit outlives the dialog that started it and, on a cluster, this
// ASTRA session: the job keeps running while the user closes the program or
// the laptop loses its network. These rows are what lets a later session find
// those runs again, resume monitoring them, and harvest their results.
//
// The job is stored as an opaque JSON blob (see FitTypesJson) for the same
// reason the mass fitter stores plans that way: the fitting types can grow
// without a schema migration.
// ─────────────────────────────────────────────────────────────────────────────

struct RemoteFitRunRow {
    QString id;               ///< uuid, also the remote job directory name
    QString hostId;           ///< RemoteHost::id
    QString hostName;         ///< copy, so an error can name a deleted host
    QString projectId;
    QString starId;
    QString massFitAttemptId; ///< empty for single-star fits
    QString createdAt;        ///< ISO 8601
    QString updatedAt;
    QString state;            ///< see RemoteRunState in RemoteFitService.h
    QString scheduler;        ///< "plain" or "slurm"
    QString slurmJobId;
    qint64  remotePid = 0;
    QString remoteDir;
    QString jobJson;          ///< the SpectralFitJob as submitted
    QString spectrumIdsJson;  ///< submitted spectrum ids, in submission order
    QString bundleVersion;    ///< worker version that ran it
    QString error;
};

class RemoteFitRepository {
public:
    explicit RemoteFitRepository(DBAccess& db);

    bool save(const RemoteFitRunRow& row);
    bool updateState(const QString& id, const QString& state,
                     const QString& error = {});
    std::optional<RemoteFitRunRow> load(const QString& id);
    /// Runs that have not reached a terminal state, oldest first. These are
    /// the ones a starting session has to pick back up.
    std::vector<RemoteFitRunRow> loadActive();
    std::vector<RemoteFitRunRow> loadForProject(const QString& projectId);
    bool remove(const QString& id);

private:
    DBAccess& _db;
};

#endif // REMOTEFITREPOSITORY_H
