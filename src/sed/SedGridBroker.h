#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QProcess>

/// Serves streamed (ssh://) model grids to a running sedfit process.
///
/// sedfit is a separate process and cannot use the SshGridProvider GAEL
/// gets in-process, so with SEDFIT_GRID_BROKER=stdio it asks its parent for
/// remote grid files over stdout/stdin (protocol in SEDplusplus
/// src/gridio.hpp). This object sits on the process: it splits sedfit's
/// stdout into log lines (re-emitted as output()) and grid requests, answers
/// requests through the process-wide grid provider on a worker thread, and
/// writes the reply to sedfit's stdin. Files land in the same disk cache the
/// spectroscopic fits use.
class SedGridBroker : public QObject
{
    Q_OBJECT
public:
    /// Attaches to `proc` (not yet started) and takes over its stdout.
    explicit SedGridBroker(QProcess* proc, QObject* parent = nullptr);

    /// Puts SEDFIT_GRID_BROKER=stdio into `env`.
    static void enableIn(QProcessEnvironment& env);

    /// Emits any trailing stdout text that did not end in a newline, and how
    /// many streamed grid files the run used.
    void flush();

signals:
    /// sedfit stdout text that is not a grid request (complete lines).
    void output(const QString& text);

private:
    void onReadyRead();
    void handleRequest(const QByteArray& line);
    void reply(const QByteArray& line);

    QPointer<QProcess> _proc;
    QByteArray         _buf;
    int                _served = 0;
};
