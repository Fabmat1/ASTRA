#pragma once

// Lightweight INFO-level scope timer for ad-hoc profiling.
//
// Unlike LOG_FUNCTION (which logs at DEBUG and is usually filtered out), this
// always logs at INFO so timings show up in the normal log/console. Intended
// for temporary instrumentation while hunting UI-thread bottlenecks.
//
//   void foo() {
//       PERF_SCOPE("Perf.LC", "LCPanel::populate");   // logs total on scope exit
//       ...
//   }
//
// Or time a sub-section explicitly:
//
//   { PerfTimer t("Perf.LC", "rebuildSeriesCache");
//     rebuildSeriesCache(); }
//
// Use PERF_MARK(timer, "label") to log an intermediate split without ending.

#include "utils/Logger.h"

#include <QElapsedTimer>
#include <QString>
#include <utility>

class PerfTimer
{
public:
    PerfTimer(QString category, QString label)
        : _category(std::move(category)), _label(std::move(label))
    {
        _timer.start();
    }

    ~PerfTimer()
    {
        if (!_reported) report("");
    }

    double elapsedMs() const { return _timer.nsecsElapsed() / 1.0e6; }

    // Log an intermediate split (keeps timing running).
    void mark(const QString& note)
    {
        Logger::instance()->info(
            _category,
            QString("[perf] %1 · %2: %3 ms")
                .arg(_label, note)
                .arg(elapsedMs(), 0, 'f', 2),
            nullptr, 0, "PerfTimer");
    }

    // End early with an explicit suffix (e.g. point counts).
    void report(const QString& suffix)
    {
        _reported = true;
        Logger::instance()->info(
            _category,
            QString("[perf] %1: %2 ms%3")
                .arg(_label)
                .arg(elapsedMs(), 0, 'f', 2)
                .arg(suffix.isEmpty() ? QString() : ("  (" + suffix + ")")),
            nullptr, 0, "PerfTimer");
    }

private:
    QString       _category;
    QString       _label;
    QElapsedTimer _timer;
    bool          _reported = false;
};

#define PERF_CONCAT_(a, b) a##b
#define PERF_CONCAT(a, b)  PERF_CONCAT_(a, b)
#define PERF_SCOPE(category, label) PerfTimer PERF_CONCAT(_perf_, __LINE__)(category, label)
