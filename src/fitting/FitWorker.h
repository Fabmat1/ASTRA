#pragma once

#include <QObject>
#include <QThread>
#include <atomic>
#include <memory>
#include "FitTypes.h"
#include "IFitBackend.h"

namespace astra::fitting {

// Runs one SpectralFitJob on a worker thread. Emits signals from inside
// run(); connect Qt::QueuedConnection so the GUI thread receives them safely.
class FitWorker : public QObject {
    Q_OBJECT
public:
    explicit FitWorker(QObject* parent = nullptr);
    ~FitWorker() override;

    void start(const SpectralFitJob& job);
    void requestAbort();
    bool isRunning() const { return _running; }

signals:
    void logMessage(const QString& line);
    void progress(const QString& stage, double pct);
    void finished(const astra::fitting::SpectralFitResult& result);
    void failed(const QString& errorMessage);

private:
    void runOnThread();

    QThread*          _thread = nullptr;
    SpectralFitJob    _job;
    std::atomic<bool> _running  { false };
    std::atomic<bool> _abortReq { false };
};

} // namespace astra::fitting