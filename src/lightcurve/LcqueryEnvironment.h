#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QVector>

/**
 * Bootstraps a self-contained Python environment for the bundled
 * lightcurvequery tool so users (especially of the AppImage) do not have to
 * clone the repository or install Python packages by hand.
 *
 * An AppImage is mounted read-only at a path that changes every launch, so we
 * can neither run pip inside it nor persist a stable script path that points
 * into it. The fix is to "unpack" the bundled scripts into a writable per-user
 * directory and build a virtualenv next to them:
 *
 *   <AppData>/lightcurvequery/   - unpacked copy of the bundled scripts
 *   <AppData>/lcquery-venv/      - virtualenv with requirements.txt installed
 *
 * On success the resolved interpreter and script paths are meant to be written
 * back into AppSettings (lcqueryPython / lcqueryScript) by the caller.
 *
 * Provisioning runs a small sequence of child processes (python -m venv, then
 * pip) and streams their output via logLine(). It is safe to use the static
 * query helpers (isProvisioned(), bundleAvailable(), …) without an instance.
 */
class LcqueryEnvironment : public QObject
{
    Q_OBJECT
public:
    explicit LcqueryEnvironment(QObject* parent = nullptr);
    ~LcqueryEnvironment() override;

    // ── Writable (per-user) target locations ─────────────────────────────
    static QString installDir();        // <AppData>/lightcurvequery
    static QString venvDir();           // <AppData>/lcquery-venv
    static QString venvPython();        // venvDir/bin/python3  (Scripts/python.exe on Windows)
    static QString scriptPath();        // installDir/lightcurvequery.py
    static QString requirementsPath();  // installDir/requirements.txt

    // ── Read-only source bundle shipped with the application ─────────────
    /// Directory containing the bundled lightcurvequery sources (AppImage /
    /// installed tree / dev build). Empty if it cannot be located.
    static QString bundledSourceDir();
    /// True if bundledSourceDir() actually contains lightcurvequery.py.
    static bool    bundleAvailable();

    /// True once both the venv interpreter and the unpacked script exist.
    static bool isProvisioned();

    bool isRunning() const;

    /// Unpack the bundled scripts, then build the venv on top of `systemPython`
    /// (e.g. "python3") and pip-install the requirements. Emits stage()/logLine()
    /// while running and finished(ok, message) when done.
    void provision(const QString& systemPython);
    void cancel();

signals:
    void stage(const QString& description);   // high-level step description
    void logLine(const QString& line);        // a line of child stdout/stderr
    void finished(bool ok, const QString& message);

private slots:
    void onReadyRead();
    void onStepFinished(int code, QProcess::ExitStatus status);
    void onErrorOccurred(QProcess::ProcessError err);

private:
    struct Command {
        QString     stage;
        QString     program;
        QStringList args;
    };

    void runNext();
    void fail(const QString& message);
    void finishOk();

    /// Recursively copy the bundle into installDir(), skipping caches / VCS /
    /// large output dirs. Returns false and sets *err on failure.
    static bool unpackBundle(QString* err);

    QProcess*        _proc = nullptr;
    QVector<Command> _queue;
    int              _index = 0;
    QByteArray       _lineBuf;
    bool             _cancelled = false;
};
