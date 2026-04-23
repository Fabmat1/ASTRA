#include "StdStreamRedirector.h"

#include <QByteArray>
#include <cstdio>
#include <utility>

#if defined(Q_OS_UNIX) || defined(__unix__) || defined(__APPLE__)
  #include <unistd.h>
  #include <fcntl.h>
  #define ASTRA_HAVE_PIPE_REDIRECT 1
#else
  #define ASTRA_HAVE_PIPE_REDIRECT 0
#endif

namespace astra::fitting {

StdStreamRedirector::StdStreamRedirector(LineFn onLine)
    : _onLine(std::move(onLine)) {}

StdStreamRedirector::~StdStreamRedirector() { stop(); }

void StdStreamRedirector::start()
{
#if ASTRA_HAVE_PIPE_REDIRECT
    if (_started) return;

    int fds[2];
    if (pipe(fds) != 0) return;

    std::fflush(stdout);
    std::fflush(stderr);

    _savedStdout = ::dup(fileno(stdout));
    _savedStderr = ::dup(fileno(stderr));
    if (_savedStdout < 0 || _savedStderr < 0) {
        ::close(fds[0]); ::close(fds[1]);
        return;
    }

    // Point stdout/stderr at the pipe write end, then drop our extra ref.
    ::dup2(fds[1], fileno(stdout));
    ::dup2(fds[1], fileno(stderr));
    ::close(fds[1]);

    // Force line buffering on the now-non-tty FDs so DIGGA's printf/cout
    // output shows up promptly instead of being held by full buffering.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    _readFd  = fds[0];
    _running = true;
    _reader  = std::thread([this]{ readerLoop(); });
    _started = true;
#endif
}

void StdStreamRedirector::stop()
{
#if ASTRA_HAVE_PIPE_REDIRECT
    if (!_started) return;

    std::fflush(stdout);
    std::fflush(stderr);

    // Restore the originals. dup2() drops the previous reference, which
    // is the pipe write end → the reader will EOF once the buffer drains.
    if (_savedStdout >= 0) {
        ::dup2(_savedStdout, fileno(stdout));
        ::close(_savedStdout);
        _savedStdout = -1;
    }
    if (_savedStderr >= 0) {
        ::dup2(_savedStderr, fileno(stderr));
        ::close(_savedStderr);
        _savedStderr = -1;
    }

    _running = false;
    if (_reader.joinable()) _reader.join();

    if (_readFd >= 0) { ::close(_readFd); _readFd = -1; }
    _started = false;
#endif
}

void StdStreamRedirector::readerLoop()
{
#if ASTRA_HAVE_PIPE_REDIRECT
    char       buf[4096];
    QByteArray pending;

    while (_running) {
        ssize_t n = ::read(_readFd, buf, sizeof(buf));
        if (n > 0) {
            pending.append(buf, int(n));
            int idx;
            while ((idx = pending.indexOf('\n')) >= 0) {
                QByteArray line = pending.left(idx);
                pending.remove(0, idx + 1);
                if (line.endsWith('\r')) line.chop(1);
                if (_onLine) _onLine(QString::fromUtf8(line));
            }
        } else if (n == 0) {
            break;                     // EOF: write end fully closed
        } else if (errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
    if (!pending.isEmpty() && _onLine)
        _onLine(QString::fromUtf8(pending));
#endif
}

} // namespace astra::fitting