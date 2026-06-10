#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <memory>
#include <vector>

#include "utils/LightcurveFetcher.h"

class Star;
class ApplicationController;

/**
 * Application-level manager for lightcurvequery fetch sessions.
 *
 * Each session fetches the lightcurves of a single star with its own
 * LightcurveFetcher (child process). Sessions keep running when the
 * LightcurveFetchDialog that started them is closed; the dialog (or the
 * sessions overview) can re-attach later and replay the buffered terminal
 * output. Result parsing and database import also live here so they happen
 * regardless of any open UI.
 *
 * Sessions are queued and at most maxParallel() of them run concurrently,
 * which is how the batch "Fetch Lightcurves" action fans out over the
 * selected stars without hammering the remote services.
 */
class LightcurveFetchService : public QObject
{
    Q_OBJECT
public:
    enum class State { Queued, Running, Finished, Failed, Cancelled };

    struct SessionInfo {
        QString   id;
        QString   starId;
        QString   gaiaId;
        QString   starLabel;     // alias or gaia id, for display
        State     state    = State::Queued;
        bool      ok       = false;
        int       exitCode = 0;
        QString   summary;       // human-readable result line
        QDateTime createdAt;
    };

    explicit LightcurveFetchService(ApplicationController* controller,
                                    QObject*               parent = nullptr);
    ~LightcurveFetchService() override;

    QString workingDir() const;

    /// Queue a fetch for one star. Returns the new session id.
    QString enqueue(std::shared_ptr<Star>             star,
                    const QString&                    projectId,
                    const LightcurveFetcher::Options& opt,
                    bool                              reattempt = false);

    /// Queue fetches for many stars and raise the worker count. Returns the
    /// number of sessions actually queued (stars without Gaia id are skipped).
    int enqueueBatch(const std::vector<std::shared_ptr<Star>>& stars,
                     const QString&                            projectId,
                     const LightcurveFetcher::Options&         opt,
                     int                                       parallelWorkers);

    void cancelSession(const QString& id);
    void cancelAll();

    int  maxParallel() const { return _maxParallel; }
    void setMaxParallel(int n);

    QList<SessionInfo> sessions() const;
    SessionInfo        sessionInfo(const QString& id, bool* found = nullptr) const;
    /// Active (running/queued) session for a star, else its most recent one.
    QString            sessionForStar(const QString& starId) const;
    QByteArray         sessionBuffer(const QString& id) const;
    bool               isSessionActive(const QString& id) const;

    bool hasActiveSessions() const;
    int  runningCount() const;
    int  queuedCount() const;

    /// Progress of the current "wave" of sessions (resets when a new session
    /// is queued while everything was idle).
    int  waveDone() const  { return _waveDone; }
    int  waveTotal() const { return _waveTotal; }

    /// Async environment probe; result is cached until lcquery settings
    /// change. Always answers via availabilityChecked().
    void checkAvailabilityAsync();
    /// Drop the cached probe result and check again (e.g. after env setup).
    void recheckAvailability();

    static QString stateLabel(State s);

signals:
    void sessionStarted(const QString& id);
    void sessionOutput(const QString& id, const QByteArray& chunk);
    void sessionFinished(const QString& id, bool ok, const QString& summary);
    /// List membership / state of any session changed.
    void sessionsChanged();
    void progressChanged(int done, int total, int running);
    void allFinished(int done, int total);
    void availabilityChecked(bool ok, const QString& message);
    /// Emitted after fetched lightcurves were imported for a star.
    void starLightcurvesUpdated(const QString& starId);

private:
    struct Session {
        SessionInfo                info;
        std::shared_ptr<Star>      star;
        QString                    projectId;
        LightcurveFetcher::Options opt;
        bool                       reattempt       = false;
        bool                       cancelRequested = false;
        LightcurveFetcher*         fetcher         = nullptr;
        QByteArray                 buffer;
    };

    enum class Availability { Unknown, Checking, Ok, Bad };

    Session*       find(const QString& id);
    const Session* find(const QString& id) const;

    void configureFetcher(LightcurveFetcher* f) const;
    void pumpQueue();
    void startSession(Session* s);
    void finalizeSession(Session* s, int exitCode, bool ok);
    void failQueued(const QString& reason);
    void appendNote(Session* s, const QString& text);
    void appendOutput(Session* s, const QByteArray& chunk);
    void clearReattemptFiles(Session* s);
    QString importResults(Session* s);
    void emitProgress();

    ApplicationController* _controller = nullptr;

    std::vector<std::unique_ptr<Session>> _sessions;
    int _maxParallel = 1;
    int _seq         = 0;

    int _waveDone  = 0;
    int _waveTotal = 0;

    Availability       _availability = Availability::Unknown;
    QString            _availabilityMsg;
    LightcurveFetcher* _probe = nullptr;
};
