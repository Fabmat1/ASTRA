#pragma once

/*  Python for the AI chat's run_python tool, and the managed environment it
 *  runs in.
 *
 *  The managed environment is a virtualenv at <data root>/ai/python-venv
 *  with numpy, scipy, matplotlib, astropy, pandas and zstandard, built on
 *  request from the settings page. A user can point ASTRA at their own
 *  interpreter instead. Code runs in a scratch directory per conversation
 *  with ASTRA's Python package on the path and the data root and MCP socket
 *  in the environment; it is NOT sandboxed, which is why running it is an
 *  Execute action that asks the user first unless auto mode is on.         */

#include <QObject>
#include <QProcess>
#include <QStringList>

namespace astra::ai {

class AiSettings;

class PythonEnvironment : public QObject {
    Q_OBJECT
public:
    explicit PythonEnvironment(QObject* parent = nullptr);

    static QString venvDir();
    static QString venvPython();
    static QStringList packages();
    /// The interpreter run_python uses: the user's choice, the managed venv,
    /// or a python3 on PATH. Empty when there is none.
    static QString interpreter(const AiSettings* settings);
    /// The directory holding ASTRA's `astra` Python package.
    static QString packageDir();

    /// Builds or updates the managed venv; progress through logLine().
    void provision();
    bool running() const { return _proc != nullptr; }

signals:
    void logLine(const QString& line);
    void finished(bool ok, const QString& message);

private:
    void runStep(int i);
    QProcess* _proc = nullptr;
    QList<QStringList> _steps;
};

} // namespace astra::ai
