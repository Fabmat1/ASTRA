#include "FitWorker.h"
#include "FitBackendRegistry.h"
#include "StdStreamRedirector.h"

namespace astra::fitting {

FitWorker::FitWorker(QObject* parent) : QObject(parent) {}

FitWorker::~FitWorker()
{
    if (_thread) {
        _thread->quit();
        _thread->wait();
    }
}

void FitWorker::start(const SpectralFitJob& job)
{
    if (_running) return;
    _job      = job;
    _abortReq = false;
    _running  = true;

    _thread = QThread::create([this]{ runOnThread(); });
    connect(_thread, &QThread::finished, _thread, &QObject::deleteLater);
    connect(_thread, &QThread::finished, this, [this]{
        _running = false;
        _thread  = nullptr;
    });
    _thread->start();
}

void FitWorker::requestAbort() { _abortReq = true; }

void FitWorker::runOnThread()
{
    auto backend = FitBackendRegistry::instance().create(_job.backend);
    if (!backend) {
        emit failed(QStringLiteral("Unknown backend: %1").arg(_job.backend));
        return;
    }

    auto logFn = [this](const QString& s) { emit logMessage(s); };
    auto progFn = [this](const FitProgressInfo& info) { emit progress(info); };
    auto abortFn = [this]() { return _abortReq.load(); };

    // Convention: fraction < 0 → indeterminate. The dialog flips the progress
    // bar to busy mode until a backend reports a real fraction (or until we
    // send the final 100% pulse below).
    {
        FitProgressInfo start;
        start.stage = QStringLiteral("Starting %1…").arg(_job.backend);
        emit progress(start);
    }

    StdStreamRedirector redirector(logFn);
    redirector.start();

    SpectralFitResult result;
    bool gotResult = false;
    QString errMsg;
    try {
        result    = backend->run(_job, logFn, progFn, abortFn);
        gotResult = true;
    } catch (const std::exception& e) {
        errMsg = QString::fromUtf8(e.what());
    } catch (...) {
        errMsg = QStringLiteral("Unknown exception");
    }

    redirector.stop();   // flush any tail output before we emit anything

    if (gotResult && result.aborted) {
        emit aborted();
        return;
    }

    {
        FitProgressInfo done;
        done.stage      = QStringLiteral("Finished");
        done.fraction   = 1.0;
        done.etaSeconds = 0.0;
        emit progress(done);
    }

    if (!gotResult)             emit failed(errMsg);
    else if (!result.success)   emit failed(result.errorMessage);
    else                        emit finished(result);
}

} // namespace astra::fitting