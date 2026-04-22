#include "FitWorker.h"
#include "FitBackendRegistry.h"

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
    _job       = job;
    _abortReq  = false;
    _running   = true;

    _thread = QThread::create([this]{ runOnThread(); });
    connect(_thread, &QThread::finished, _thread, &QObject::deleteLater);
    connect(_thread, &QThread::finished, this,    [this]{
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

    auto logFn = [this](const QString& s) {
        emit logMessage(s);
    };
    auto progFn = [this](const QString& stage, double p) {
        emit progress(stage, p);
    };
    auto abortFn = [this]() { return _abortReq.load(); };

    try {
        auto result = backend->run(_job, logFn, progFn, abortFn);
        if (!result.success) emit failed(result.errorMessage);
        else                 emit finished(result);
    } catch (const std::exception& e) {
        emit failed(QString::fromUtf8(e.what()));
    } catch (...) {
        emit failed("Unknown exception");
    }
}

} // namespace astra::fitting