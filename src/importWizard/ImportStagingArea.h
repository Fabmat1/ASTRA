#ifndef IMPORTSTAGINGAREA_H
#define IMPORTSTAGINGAREA_H
#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QMutex>

#include <memory>
#include <vector>

class Star;
class DatabaseManager;

class ImportStagingArea
{
public:
    ImportStagingArea() = default;
    ~ImportStagingArea() = default;

    // Non-copyable (mutex)
    ImportStagingArea(const ImportStagingArea&) = delete;
    ImportStagingArea& operator=(const ImportStagingArea&) = delete;

    // ── Working set management ──────────────────────────────────
    void addStar(std::shared_ptr<Star> star, bool isNew);

    // Pull child data from DB for stars already in the working set.
    // Each method skips stars that already have the data loaded.
    void pullStarsFromDB(DatabaseManager* dbm, const QString& projectId,
                         const QStringList& starIds);
    void pullSpectraFromDB(DatabaseManager* dbm);
    void pullFitsFromDB(DatabaseManager* dbm);
    void pullRVFromDB(DatabaseManager* dbm);

    // ── Queries ─────────────────────────────────────────────────
    std::shared_ptr<Star> getStar(const QString& starId) const;
    std::vector<std::shared_ptr<Star>> allStars() const;
    bool hasStar(const QString& starId) const;
    bool isEmpty() const;

    // ── Tracking: call these when tasks create new child objects ─
    void markStarDirty(const QString& starId);
    void markSpectrumNew(const QString& spectrumId);
    void markFitNew(const QString& fitId);
    void markRVCurveNew(const QString& curveId);

    void markSEDModelNew(const QString& modelId);
    int  newSEDModelCount() const;
    void pullPhotometryFromDB(DatabaseManager* dbm);

    // ── Counts ──────────────────────────────────────────────────
    int totalStarCount() const;
    int newStarCount() const;
    int newSpectrumCount() const;
    int newFitCount() const;
    int newRVCurveCount() const;

    // ── Lifecycle ───────────────────────────────────────────────
    void clear();
    bool commitAll(DatabaseManager* dbm, const QString& projectId);

private:
    void deduplicateStars();
    mutable QMutex _mutex;

    // The working set: starId → Star object (shared_ptr)
    QHash<QString, std::shared_ptr<Star>> _workingStars;

    // Tracking sets
    QSet<QString> _newStarIds;       // stars that don't exist in DB yet
    QSet<QString> _dirtyStarIds;     // existing stars whose row fields changed
    QSet<QString> _newSpectrumIds;   // spectra created during this wizard run
    QSet<QString> _newFitIds;        // spectral fits created during this wizard run
    QSet<QString> _newRVCurveIds;    // RV curves created during this wizard run
    QSet<QString> _newSEDModelIds;

};

#endif // IMPORTSTAGINGAREA_H