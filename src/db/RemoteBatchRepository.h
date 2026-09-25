#ifndef REMOTEBATCHREPOSITORY_H
#define REMOTEBATCHREPOSITORY_H

#include <QString>
#include <optional>
#include <vector>

class DBAccess;

// ─────────────────────────────────────────────────────────────────────────────
// Persistence for batched remote fitting (see remote/RemoteBatchService.h).
//
// A batch is one submission to a host: a staged directory of fits and one
// Slurm array (or one detached runner pool on a plain host) that works
// through them in tasks. An item is one fit inside it. Both outlive the
// session that created them - a campaign runs for days - so everything a
// later session needs to find a batch again, tell which of its fits finished,
// and hand each result to the mass-fit attempt that asked for it is stored
// here before anything is submitted.
//
// Items stay after they are consumed: their measured wall time and peak
// memory are what the host's tier suggestions and duration estimates learn
// from.
// ─────────────────────────────────────────────────────────────────────────────

struct RemoteBatchRow {
    QString id;               ///< uuid, also the remote batch directory name
    QString hostId;
    QString hostName;
    QString remoteDir;
    QString scheduler;        ///< "slurm" or "plain"
    int     tier = 0;
    QString memory;           ///< the tier's memory as submitted
    QString timeLimit;        ///< the tier's wall time as submitted
    int     taskCount = 0;
    QString slurmJobId;       ///< the array's job id
    qint64  remotePid = 0;    ///< the runner pool on a plain host
    QString state;            ///< RemoteBatchService::BatchState label
    QString bundleVersion;
    QString createdAt;
    QString updatedAt;
    QString error;
};

struct RemoteBatchItemRow {
    QString id;               ///< uuid, also the remote item directory name
    QString itemKey;          ///< who asked: "<attempt id>:<ordinal>"
    QString batchId;          ///< empty while still queued locally
    QString hostId;
    QString projectId;
    QString starId;
    QString massFitAttemptId;
    QString state;            ///< RemoteBatchService::ItemState label
    int     tier = 0;
    int     retries = 0;
    int     nSpectra = 0;
    int     taskIndex = -1;
    QString spectrumIdsJson;  ///< submitted spectrum ids, in submission order
    QString jobJson;          ///< the SpectralFitJob as submitted
    QString resultPath;       ///< local copy of the result, once harvested
    QString error;
    double    wallSeconds  = -1.0;
    double    cpuSeconds   = -1.0;
    long long peakRssBytes = -1;
    QString createdAt;
    QString updatedAt;
};

/// One finished fit's measured cost, for learning a host's tiers.
struct RemoteFitUsage {
    int       nSpectra = 0;
    int       tier = 0;
    double    wallSeconds = -1.0;
    long long peakRssBytes = -1;
};

class RemoteBatchRepository {
public:
    explicit RemoteBatchRepository(DBAccess& db);

    bool saveBatch(const RemoteBatchRow& row);
    bool updateBatchState(const QString& id, const QString& state,
                          const QString& error = {});
    std::optional<RemoteBatchRow> loadBatch(const QString& id);
    /// Batches not yet settled, oldest first: what a starting session
    /// resumes watching.
    std::vector<RemoteBatchRow> loadActiveBatches();
    /// Every batch ever recorded, settled or not.
    int countBatches();

    bool saveItem(const RemoteBatchItemRow& row);
    std::optional<RemoteBatchItemRow> loadItem(const QString& id);
    /// The newest item filed under `itemKey`, if any.
    std::optional<RemoteBatchItemRow> loadItemByKey(const QString& itemKey);
    std::vector<RemoteBatchItemRow> loadItemsForBatch(const QString& batchId);
    /// Items that were harvested but whose result nobody collected yet.
    std::vector<RemoteBatchItemRow> loadUnconsumedItems();

    /// The most recent measured fits on a host, newest first.
    std::vector<RemoteFitUsage> loadUsage(const QString& hostId, int limit);

private:
    DBAccess& _db;
};

#endif // REMOTEBATCHREPOSITORY_H
