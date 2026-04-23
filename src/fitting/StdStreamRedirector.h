#pragma once

#include <QString>
#include <atomic>
#include <functional>
#include <thread>

namespace astra::fitting {

// Redirect the process-wide stdout/stderr to a pipe and feed each line
// into a user callback. UNIX-only; on other platforms it is a no-op.
//
// The callback runs on an internal reader thread, so emit Qt signals from
// it via Qt::QueuedConnection (the auto-connection picks that automatically
// because the calling thread is not the receiver's thread).
class StdStreamRedirector
{
public:
    using LineFn = std::function<void(const QString&)>;

    explicit StdStreamRedirector(LineFn onLine);
    ~StdStreamRedirector();

    StdStreamRedirector(const StdStreamRedirector&)            = delete;
    StdStreamRedirector& operator=(const StdStreamRedirector&) = delete;

    void start();
    void stop();
    bool active() const { return _started; }

private:
    void readerLoop();

    LineFn            _onLine;
    int               _readFd      = -1;
    int               _savedStdout = -1;
    int               _savedStderr = -1;
    std::atomic<bool> _running     { false };
    std::thread       _reader;
    bool              _started     = false;
};

} // namespace astra::fitting