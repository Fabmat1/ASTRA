#include "LightcurveFetchService.h"

#include "controllers/ApplicationController.h"
#include "db/DatabaseManager.h"
#include "models/Photometry.h"
#include "models/Star.h"
#include "utils/AppPaths.h"
#include "utils/AppSettings.h"
#include "utils/Logger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <cmath>
#include <limits>

namespace {

// Keep at most this much terminal scrollback per session.
constexpr int kMaxBufferBytes = 2 * 1024 * 1024;

// Keep at most this many finished sessions around for later inspection.
constexpr int kMaxFinishedSessions = 200;

double readCrowdsapFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return std::numeric_limits<double>::quiet_NaN();
    QTextStream s(&f);
    const QString line = s.readLine().trimmed();
    bool ok = false;
    const double v = line.toDouble(&ok);
    return ok ? v : std::numeric_limits<double>::quiet_NaN();
}

} // namespace

LightcurveFetchService::LightcurveFetchService(ApplicationController* controller,
                                               QObject*               parent)
    : QObject(parent)
    , _controller(controller)
{
    if (AppSettings* settings = _controller ? _controller->settings() : nullptr) {
        connect(settings, &AppSettings::lcquerySettingsChanged, this, [this] {
            _availability = Availability::Unknown;
            checkAvailabilityAsync();
        });
    }
}

LightcurveFetchService::~LightcurveFetchService() = default;

QString LightcurveFetchService::workingDir() const
{
    return QDir(AppPaths::root()).absoluteFilePath("lcquery");
}

QString LightcurveFetchService::stateLabel(State s)
{
    switch (s) {
        case State::Queued:    return tr("Queued");
        case State::Running:   return tr("Running");
        case State::Finished:  return tr("Finished");
        case State::Failed:    return tr("Failed");
        case State::Cancelled: return tr("Cancelled");
    }
    return {};
}

// ── Session lookup ─────────────────────────────────────────────────

LightcurveFetchService::Session* LightcurveFetchService::find(const QString& id)
{
    for (auto& s : _sessions)
        if (s->info.id == id) return s.get();
    return nullptr;
}

const LightcurveFetchService::Session*
LightcurveFetchService::find(const QString& id) const
{
    for (const auto& s : _sessions)
        if (s->info.id == id) return s.get();
    return nullptr;
}

QList<LightcurveFetchService::SessionInfo> LightcurveFetchService::sessions() const
{
    QList<SessionInfo> out;
    out.reserve(int(_sessions.size()));
    for (const auto& s : _sessions)
        out.append(s->info);
    return out;
}

LightcurveFetchService::SessionInfo
LightcurveFetchService::sessionInfo(const QString& id, bool* found) const
{
    if (const Session* s = find(id)) {
        if (found) *found = true;
        return s->info;
    }
    if (found) *found = false;
    return {};
}

QString LightcurveFetchService::sessionForStar(const QString& starId) const
{
    QString latest;
    for (auto it = _sessions.rbegin(); it != _sessions.rend(); ++it) {
        const auto& s = *it;
        if (s->info.starId != starId) continue;
        if (s->info.state == State::Running || s->info.state == State::Queued)
            return s->info.id;
        if (latest.isEmpty()) latest = s->info.id;
    }
    return latest;
}

QByteArray LightcurveFetchService::sessionBuffer(const QString& id) const
{
    const Session* s = find(id);
    return s ? s->buffer : QByteArray{};
}

bool LightcurveFetchService::isSessionActive(const QString& id) const
{
    const Session* s = find(id);
    return s && (s->info.state == State::Running || s->info.state == State::Queued);
}

bool LightcurveFetchService::hasActiveSessions() const
{
    return runningCount() + queuedCount() > 0;
}

int LightcurveFetchService::runningCount() const
{
    int n = 0;
    for (const auto& s : _sessions)
        if (s->info.state == State::Running) ++n;
    return n;
}

int LightcurveFetchService::queuedCount() const
{
    int n = 0;
    for (const auto& s : _sessions)
        if (s->info.state == State::Queued) ++n;
    return n;
}

// ── Queueing ───────────────────────────────────────────────────────

QString LightcurveFetchService::enqueue(std::shared_ptr<Star>             star,
                                        const QString&                    projectId,
                                        const LightcurveFetcher::Options& opt,
                                        bool                              reattempt)
{
    if (!star || star->getSourceId().isEmpty())
        return {};

    if (!hasActiveSessions()) {
        _waveDone  = 0;
        _waveTotal = 0;
    }

    // Drop stale finished sessions for the same star - the new run supersedes
    // them - and cap overall history growth.
    for (auto it = _sessions.begin(); it != _sessions.end();) {
        const State st = (*it)->info.state;
        if ((*it)->info.starId == star->getId() &&
            st != State::Running && st != State::Queued)
            it = _sessions.erase(it);
        else
            ++it;
    }
    int finished = 0;
    for (const auto& s : _sessions)
        if (s->info.state != State::Running && s->info.state != State::Queued)
            ++finished;
    for (auto it = _sessions.begin();
         finished > kMaxFinishedSessions && it != _sessions.end();) {
        const State st = (*it)->info.state;
        if (st != State::Running && st != State::Queued) {
            it = _sessions.erase(it);
            --finished;
        } else {
            ++it;
        }
    }

    auto s = std::make_unique<Session>();
    s->info.id        = QString::number(++_seq);
    s->info.starId    = star->getId();
    s->info.gaiaId    = star->getSourceId();
    s->info.starLabel = star->getAlias().isEmpty() ? star->getSourceId()
                                                   : star->getAlias();
    s->info.createdAt = QDateTime::currentDateTime();
    s->star           = std::move(star);
    s->projectId      = projectId;
    s->opt            = opt;
    s->reattempt      = reattempt;

    const QString id = s->info.id;
    _sessions.push_back(std::move(s));
    ++_waveTotal;

    LOG_INFO("LCQuery", QString("Queued fetch session %1 for Gaia %2")
                            .arg(id, _sessions.back()->info.gaiaId));

    emit sessionsChanged();
    emitProgress();
    pumpQueue();
    return id;
}

int LightcurveFetchService::enqueueBatch(
    const std::vector<std::shared_ptr<Star>>& stars,
    const QString&                            projectId,
    const LightcurveFetcher::Options&         opt,
    int                                       parallelWorkers)
{
    setMaxParallel(parallelWorkers);
    int n = 0;
    for (const auto& star : stars) {
        if (!star || star->getSourceId().isEmpty()) continue;
        // Skip stars that already have an active session.
        const QString existing = sessionForStar(star->getId());
        if (!existing.isEmpty() && isSessionActive(existing)) continue;
        if (!enqueue(star, projectId, opt, false).isEmpty()) ++n;
    }
    return n;
}

void LightcurveFetchService::setMaxParallel(int n)
{
    _maxParallel = qBound(1, n, 4);
    pumpQueue();
}

// ── Cancellation ───────────────────────────────────────────────────

void LightcurveFetchService::cancelSession(const QString& id)
{
    Session* s = find(id);
    if (!s) return;

    if (s->info.state == State::Queued) {
        s->cancelRequested = true;
        s->info.state      = State::Cancelled;
        s->info.summary    = tr("Cancelled before start.");
        appendNote(s, tr("- cancelled before start -"));
        ++_waveDone;
        emit sessionFinished(s->info.id, false, s->info.summary);
        emit sessionsChanged();
        emitProgress();
        if (!hasActiveSessions()) emit allFinished(_waveDone, _waveTotal);
    } else if (s->info.state == State::Running && s->fetcher) {
        s->cancelRequested = true;
        appendNote(s, tr("- cancellation requested -"));
        s->fetcher->cancel();
    }
}

void LightcurveFetchService::cancelAll()
{
    // Cancel queued sessions first so the queue doesn't refill the workers.
    QStringList ids;
    for (const auto& s : _sessions)
        if (s->info.state == State::Queued) ids << s->info.id;
    for (const auto& s : _sessions)
        if (s->info.state == State::Running) ids << s->info.id;
    for (const QString& id : ids)
        cancelSession(id);
}

// ── Availability probing ───────────────────────────────────────────

void LightcurveFetchService::configureFetcher(LightcurveFetcher* f) const
{
    f->setWorkingDir(workingDir());
    if (AppSettings* settings = _controller ? _controller->settings() : nullptr) {
        if (!settings->lcqueryPython().isEmpty())
            f->setPython(settings->lcqueryPython());
        if (!settings->lcqueryScript().isEmpty())
            f->setScript(settings->lcqueryScript());
        f->setAtlasToken(settings->atlasToken());
        f->setBlackgemScript(settings->blackgemScript());
    }
}

void LightcurveFetchService::recheckAvailability()
{
    if (_availability != Availability::Checking)
        _availability = Availability::Unknown;
    checkAvailabilityAsync();
}

void LightcurveFetchService::checkAvailabilityAsync()
{
    if (_availability == Availability::Ok) {
        emit availabilityChecked(true, {});
        return;
    }
    if (_availability == Availability::Bad) {
        emit availabilityChecked(false, _availabilityMsg);
        return;
    }
    if (_availability == Availability::Checking)
        return; // answer comes when the running probe finishes

    if (!_probe) {
        _probe = new LightcurveFetcher(this);
        connect(_probe, &LightcurveFetcher::availabilityChecked,
                this, [this](bool ok, const QString& msg) {
            _availability    = ok ? Availability::Ok : Availability::Bad;
            _availabilityMsg = msg;
            emit availabilityChecked(ok, msg);
            if (ok)
                pumpQueue();
            else
                failQueued(msg);
        });
    }
    configureFetcher(_probe);
    _availability = Availability::Checking;
    _probe->checkAvailableAsync();
}

void LightcurveFetchService::failQueued(const QString& reason)
{
    bool any = false;
    for (auto& sp : _sessions) {
        Session* s = sp.get();
        if (s->info.state != State::Queued) continue;
        s->info.state   = State::Failed;
        s->info.summary = reason.section('\n', 0, 0);
        appendNote(s, tr("[availability] %1").arg(reason));
        ++_waveDone;
        emit sessionFinished(s->info.id, false, s->info.summary);
        any = true;
    }
    if (any) {
        emit sessionsChanged();
        emitProgress();
        if (!hasActiveSessions()) emit allFinished(_waveDone, _waveTotal);
    }
}

// ── Running ────────────────────────────────────────────────────────

void LightcurveFetchService::pumpQueue()
{
    if (_availability == Availability::Bad) {
        failQueued(_availabilityMsg);
        return;
    }
    if (queuedCount() == 0) return;
    if (_availability != Availability::Ok) {
        checkAvailabilityAsync();
        return;
    }

    while (runningCount() < _maxParallel) {
        Session* next = nullptr;
        for (auto& s : _sessions)
            if (s->info.state == State::Queued) { next = s.get(); break; }
        if (!next) break;
        startSession(next);
    }
}

void LightcurveFetchService::startSession(Session* s)
{
    s->fetcher = new LightcurveFetcher(this);
    configureFetcher(s->fetcher);
    s->fetcher->setSkipPreflightCheck(true);

    const QString id = s->info.id;

    connect(s->fetcher, &LightcurveFetcher::rawOutput,
            this, [this, id](const QByteArray& chunk) {
        if (Session* s = find(id)) appendOutput(s, chunk);
    });
    connect(s->fetcher, &LightcurveFetcher::finished,
            this, [this, id](int code, bool ok) {
        if (Session* s = find(id)) finalizeSession(s, code, ok);
    });
    connect(s->fetcher, &LightcurveFetcher::failed,
            this, [this, id](const QString& reason) {
        Session* s = find(id);
        if (!s || s->info.state != State::Running) return;
        appendNote(s, tr("[fail] %1").arg(reason));
        s->info.summary = reason.section('\n', 0, 0);
        // If the process is gone (or never started) no finished() will come.
        if (!s->fetcher || !s->fetcher->isRunning())
            finalizeSession(s, -1, false);
    });

    if (s->reattempt)
        clearReattemptFiles(s);

    s->info.state = State::Running;
    appendNote(s, tr("Fetching %1 for Gaia DR3 %2%3")
                      .arg(s->opt.sources.join(", "),
                           s->info.gaiaId,
                           s->reattempt ? tr("  (reattempt mode)") : QString()));

    emit sessionStarted(id);
    emit sessionsChanged();
    emitProgress();

    s->fetcher->start(s->info.gaiaId, s->opt);
}

void LightcurveFetchService::clearReattemptFiles(Session* s)
{
    const auto expected = s->fetcher->expectedOutputFiles(s->info.gaiaId);
    for (const QString& src : s->opt.sources) {
        const QString path = expected.value(src);
        if (!path.isEmpty() && QFile::exists(path)) {
            if (QFile::remove(path))
                appendNote(s, tr("[reattempt] removed %1").arg(path));
            else
                appendNote(s, tr("[reattempt] WARNING: could not remove %1").arg(path));
        }
    }

    // Per-source preview / aux files, so stale ones don't get shown for
    // sources whose fetch fails.
    const QString prevDir = QDir(workingDir()).absoluteFilePath(
        QString("lightcurves/%1").arg(s->info.gaiaId));
    QStringList auxFiles;
    if (s->opt.sources.contains("TESS"))
        auxFiles << "tess_preview.png" << "tess_crowdsap.txt";
    if (s->opt.sources.contains("ZTF"))
        auxFiles << "ztf_preview.png";
    for (const QString& f : auxFiles) {
        const QString p = QDir(prevDir).absoluteFilePath(f);
        if (QFile::exists(p)) QFile::remove(p);
    }
}

void LightcurveFetchService::finalizeSession(Session* s, int exitCode, bool ok)
{
    if (s->info.state != State::Running) return;

    s->info.exitCode = exitCode;
    s->info.ok       = ok;
    s->info.state    = s->cancelRequested ? State::Cancelled
                     : ok                 ? State::Finished
                                          : State::Failed;

    appendNote(s, ok ? tr("- lightcurvequery finished successfully -")
                     : tr("- lightcurvequery exited with code %1 -").arg(exitCode));

    // Import whatever output files exist - even after a failed or cancelled
    // run partial results may be usable (mirrors the old in-dialog behaviour).
    const QString importSummary = importResults(s);

    if (s->info.summary.isEmpty() || !importSummary.isEmpty())
        s->info.summary = !importSummary.isEmpty()
            ? importSummary
            : (ok ? tr("No data was produced.")
                  : tr("Failed (exit %1).").arg(exitCode));
    if (s->info.state == State::Cancelled && importSummary.isEmpty())
        s->info.summary = tr("Cancelled.");

    if (s->fetcher) {
        s->fetcher->deleteLater();
        s->fetcher = nullptr;
    }

    ++_waveDone;
    emit sessionFinished(s->info.id, ok, s->info.summary);
    emit sessionsChanged();
    emitProgress();

    pumpQueue();
    if (!hasActiveSessions())
        emit allFinished(_waveDone, _waveTotal);
}

// ── Output buffering ───────────────────────────────────────────────

void LightcurveFetchService::appendOutput(Session* s, const QByteArray& chunk)
{
    s->buffer += chunk;
    if (s->buffer.size() > kMaxBufferBytes) {
        // Drop the oldest half, cutting at a line boundary where possible.
        int cut = s->buffer.size() / 2;
        const int nl = s->buffer.indexOf('\n', cut);
        if (nl >= 0) cut = nl + 1;
        s->buffer.remove(0, cut);
    }
    emit sessionOutput(s->info.id, chunk);
}

void LightcurveFetchService::appendNote(Session* s, const QString& text)
{
    appendOutput(s, (text + '\n').toUtf8());
}

// ── Result import (moved from LightcurveFetchDialog) ───────────────

QString LightcurveFetchService::importResults(Session* s)
{
    auto star = s->star;
    if (!star) return {};

    DatabaseManager* dbm = _controller ? _controller->databaseManager() : nullptr;

    LightcurveFetcher probe; // only used for path resolution
    probe.setWorkingDir(workingDir());
    const auto expected = probe.expectedOutputFiles(s->info.gaiaId);

    auto phot = star->getPhotometry();
    if (!phot) {
        phot = std::make_shared<Photometry>();
        star->setPhotometry(phot);
    }

    const bool haveCoords =
        Star::isSet(star->getRa()) && Star::isSet(star->getDec());
    if (!haveCoords)
        appendNote(s, tr("Warning: star has no RA/Dec - BJDs will not be computed."));

    QStringList imported, empty;
    int totalPoints = 0;

    for (auto it = expected.cbegin(); it != expected.cend(); ++it) {
        const QString& source = it.key();
        const QString& path   = it.value();
        if (!QFile::exists(path)) continue;

        TimeScale ts  = TimeScale::Unknown;
        auto      pts = LightcurveFetcher::parseOutputFile(path, source, &ts);
        if (pts.empty()) { empty << source; continue; }

        const bool nativeIsBjd =
            (ts == TimeScale::BJD  ||
             ts == TimeScale::BTJD ||
             ts == TimeScale::BKJD ||
             ts == TimeScale::GaiaTCB);

        if (!nativeIsBjd && haveCoords && dbm) {
            auto inst = dbm->resolveInstrumentString(source);
            if (!inst) {
                appendNote(s, tr("[%1] no instrument record found - BJD not computed")
                                  .arg(source));
            } else {
                int converted = 0;
                for (auto& pt : pts) {
                    if (pt.time.hasBjd()) continue;
                    pt.time.setAutoConvertInfo(inst, star->getRa(), star->getDec());
                    if (pt.time.bjd().has_value()) ++converted;
                }
                appendNote(s, tr("[%1] computed BJD for %2 / %3 points")
                                  .arg(source).arg(converted).arg(int(pts.size())));
            }
        }

        QString verb;
        if (s->reattempt) {
            phot->addLightcurve(source, pts);
            verb = tr("replaced (reattempt)");
        } else {
            const auto result = phot->mergeLightcurve(source, pts);
            switch (result) {
                case Photometry::MergeResult::Identical: verb = tr("identical"); break;
                case Photometry::MergeResult::Replaced:  verb = tr("replaced");  break;
                case Photometry::MergeResult::Merged:    verb = tr("merged");    break;
                case Photometry::MergeResult::Added:     verb = tr("added");     break;
            }
        }
        appendNote(s, tr("[%1] %2 points %3").arg(source).arg(pts.size()).arg(verb));

        if (dbm && !dbm->saveLightcurveForStar(star->getId(), source, phot.get()))
            appendNote(s, tr("[%1] WARNING: failed to save to database").arg(source));

        imported << QString("%1 (%2)").arg(source).arg(pts.size());
        totalPoints += int(pts.size());
    }

    // TESS crowding metric, if the helper script produced one.
    {
        const QString crowdFile =
            QDir(workingDir()).absoluteFilePath(
                QString("lightcurves/%1/tess_crowdsap.txt").arg(s->info.gaiaId));
        if (QFile::exists(crowdFile)) {
            const double v = readCrowdsapFile(crowdFile);
            if (!std::isnan(v)) {
                star->setTessCrowdsap(v);
                if (dbm) dbm->saveStarTessCrowdsap(star->getId(), v);
                appendNote(s, tr("[TESS] CROWDSAP = %1").arg(v, 0, 'f', 3));
            } else {
                appendNote(s, tr("[TESS] tess_crowdsap.txt present but could not be parsed"));
            }
        }
    }

    if (!empty.isEmpty())
        appendNote(s, tr("(No data for: %1)").arg(empty.join(", ")));

    if (imported.isEmpty())
        return {};

    emit starLightcurvesUpdated(star->getId());
    return tr("Imported %1 points: %2").arg(totalPoints).arg(imported.join(", "));
}

void LightcurveFetchService::emitProgress()
{
    emit progressChanged(_waveDone, _waveTotal, runningCount());
}
