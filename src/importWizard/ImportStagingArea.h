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
class LightcurvePoint;
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

    void markLightcurveDirty(const QString& starId);       // ← NEW

    // ── Counts ──────────────────────────────────────────────────
    int totalStarCount() const;
    int newStarCount() const;
    int newSpectrumCount() const;
    int newFitCount() const;
    int newRVCurveCount() const;
    int dirtyLightcurveStarCount() const;                   // ← NEW

    // ── Lifecycle ───────────────────────────────────────────────
    void clear();
    using ProgressCallback = std::function<void(int done, int total)>;

    bool commitAll(DatabaseManager* dbm,
                const QString& projectId,
                ProgressCallback progress = {});

    // ── Lightcurve staging (with merge logic) ───────────────────
    bool stageLightcurve(const QString& starId,
                         const QString& instrument,
                         const std::vector<LightcurvePoint>& points);


private:
    void deduplicateStars();
    mutable QMutex _mutex;

    QHash<QString, std::shared_ptr<Star>> _workingStars;

    QSet<QString> _newStarIds;
    QSet<QString> _dirtyStarIds;
    QSet<QString> _newSpectrumIds;
    QSet<QString> _newFitIds;
    QSet<QString> _newRVCurveIds;
    QSet<QString> _newSEDModelIds;
    QSet<QString> _dirtyLightcurveStarIds;                  // ← NEW
};

#endif // IMPORTSTAGINGAREA_H